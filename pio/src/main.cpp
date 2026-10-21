#include <Arduino.h>
/*
 * ============================================================
 *  新启年工作室广播器  NewEra-Broadcast  v13(管理后台版)
 * ============================================================
 *  功能:
 *    1. 上电广播 NewEraAdmin 管理热点(开放网络, 192.168.4.1),
 *       手机连上自动弹出 MD3 风格管理后台(captive portal)
 *    2. WiFi 干扰: 注入 N 个假信标(默认 200 个, 前缀+序号)
 *       - 无人连接时: 1/6/11(或 1/4/6/9/11/13)分组跨信道轮换
 *       - 有设备连接管理后台时: 锁定当前信道, 全部压在管理信道上
 *         密集爆发(STA 不跟随跳频, 跳频会断连, 故自适应锁信道)
 *    3. 蓝牙干扰: 轮换广播同名设备(开启需重启, 避 coex 断言)
 *    4. 后台可调: 总开关 / WiFi 开关 / 蓝牙开关 / 前缀 / 数量 /
 *       信道策略 / TX 功率 / 重启 / 恢复默认, 全部持久化 NVS
 *    5. 性能: 2Mbps 强制空口速率 + 19.5dBm 满功率 + 失败重试
 *
 *  硬件: ESP32-S3, 双 USB(原生 COM5 + CH343 COM4), 16MB Flash
 *  署名: NewEraStudio
 * ============================================================
 */

#include <string.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <BLEDevice.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>

// -------------------- 常量 --------------------
#define MAXN          500            // 广播名上限
#define AP_CH         1              // 管理热点固定信道(锁台时必须锁这里, 否则 STA 失联)
static const char* AP_SSID   = "NewEraAdmin";   // 管理热点名
static const char* FW_VER    = "v13";
static const char* STUDIO    = "NewEraStudio";
static const int   BLE_DWELL_MS = 250;          // 每个蓝牙名停留时长

// -------------------- 设置(运行时 + NVS) --------------------
static bool     g_master = true;     // 总开关
static bool     g_wifiOn = true;     // WiFi 干扰
static bool     g_bleOn  = false;    // 蓝牙干扰(重启生效)
static bool     g_bleStarted = false;// 本次启动是否真初始化了 BLE
static String   g_prefix = "新启年工作室牛逼";
static int      g_count  = 200;      // 广播数量
static int      g_chMode = 3;        // 3={1,6,11}  6={1,4,6,9,11,13}
static int      g_power  = 78;       // 0.25dBm 单位 (78=19.5dBm)
static String   g_apPass = "";       // 管理热点密码(空=开放, 重启生效)

// -------------------- 广播数据 --------------------
static String  g_names[MAXN];
static uint8_t g_wifiMac[MAXN][6];
static uint8_t g_bleMac[MAXN][6];
static int     g_macReady = 0;       // MAC 已生成到第几个(高水位)

// -------------------- 诊断 --------------------
static uint32_t g_txOk = 0, g_txErr = 0, g_chOk = 0, g_chErr = 0, g_chSw = 0;
static unsigned long g_chUs = 0;
static int      g_txLastErr = 0;
static uint8_t  g_failStreak = 0;     // 注入连续失败计数(退避用)
static unsigned long g_cycleMs = 0, g_cycleCnt = 0;
static uint32_t g_txRate = 0;        // 最近统计窗口的 TX 速率
static bool     g_stationMode = false; // 有设备连入管理热点 -> 锁信道

// BLE 诊断
static uint32_t g_gapEvt = 0;
static int      g_gapLastEvt = -1;
static int           g_bleIdx = 0;
static unsigned long g_bleLast = 0;
static bool          g_bleAdvOn = false;

static Preferences prefs;
static WebServer    server(80);
static DNSServer    dnsServer;

// ==================== 名字/MAC 生成 ====================

static void generateNames() {
  for (int i = 0; i < g_count; i++) g_names[i] = g_prefix + String(i + 1);
  if (g_macReady < g_count) {
    for (int i = g_macReady; i < g_count; i++) {
      g_wifiMac[i][0] = 0x02 | (uint8_t)random(0x00, 0x40);   // 本地管理单播
      for (int j = 1; j < 6; j++) g_wifiMac[i][j] = (uint8_t)random(256);
      g_bleMac[i][0]  = 0xC0 | (uint8_t)random(0x00, 0x40);   // 随机静态
      for (int j = 1; j < 6; j++) g_bleMac[i][j] = (uint8_t)random(256);
    }
    g_macReady = g_count;
  }
}

// ==================== WiFi 信标注入 ====================

static void sendBeacon(const String& name, const uint8_t* mac, uint8_t channel) {
  uint8_t pkt[160];
  size_t len = 0;

  // ---- MAC 头(24 字节) ----
  pkt[len++] = 0x80; pkt[len++] = 0x00;            // 帧控制: Beacon
  pkt[len++] = 0x00; pkt[len++] = 0x00;            // Duration
  for (int i = 0; i < 6; i++) pkt[len++] = 0xFF;   // 地址1 = 广播
  for (int i = 0; i < 6; i++) pkt[len++] = mac[i]; // 地址2 = 源地址
  for (int i = 0; i < 6; i++) pkt[len++] = mac[i]; // 地址3 = BSSID
  pkt[len++] = 0x00; pkt[len++] = 0x00;            // 序列号(驱动覆写)

  // ---- 固定参数(12 字节) ----
  for (int i = 0; i < 8; i++) pkt[len++] = 0x00;   // 时间戳
  pkt[len++] = 0x64; pkt[len++] = 0x00;            // 信标间隔 100TU
  pkt[len++] = 0x01; pkt[len++] = 0x00;            // 能力信息: ESS, 开放网络

  // ---- SSID ----
  int sl = name.length();
  if (sl > 32) sl = 32;
  pkt[len++] = 0x00;
  pkt[len++] = (uint8_t)sl;
  memcpy(pkt + len, name.c_str(), sl);
  len += sl;

  // ---- 支持速率 ----
  static const uint8_t rates[] = {0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C};
  pkt[len++] = 0x01;
  pkt[len++] = sizeof(rates);
  memcpy(pkt + len, rates, sizeof(rates));
  len += sizeof(rates);

  // ---- DS 参数(信道) ----
  pkt[len++] = 0x03;
  pkt[len++] = 0x01;
  pkt[len++] = channel;

  int r = esp_wifi_80211_tx(WIFI_IF_AP, pkt, (int)len, true);
  if (r != ESP_OK && !g_stationMode) {   // 非锁台: 稍等再吃一次队列空隙(压榨吞吐)
    delay(1);
    r = esp_wifi_80211_tx(WIFI_IF_AP, pkt, (int)len, true);
  }
  if (r == ESP_OK) { g_txOk++; g_failStreak = 0; }
  else {
    g_txErr++; g_txLastErr = r;
    // 锁台时连续失败大幅退避: 队列让给 WebServer/DHCP 的响应帧
    if (g_stationMode && ++g_failStreak >= 3) { delay(5); g_failStreak = 0; }
  }
}

static const uint8_t CH3[] = {1, 6, 11};
static const uint8_t CH6[] = {1, 4, 6, 9, 11, 13};
static uint8_t g_curCh = 1;

static void setChannel(uint8_t ch) {
  if (ch == g_curCh) return;         // 只在真正换台时调用(每帧都调会拖死 WiFi 任务)
  unsigned long t0 = micros();
  esp_err_t rc = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  g_chUs += micros() - t0;
  if (rc == ESP_OK) { g_chOk++; g_curCh = ch; g_chSw++; }
  else g_chErr++;
}

static void wifiTick() {
  if (!g_master || !g_wifiOn) return;

  // 有设备连管理热点 -> 锁信道单点爆发(跳频会断连)
  int stations = WiFi.softAPgetStationNum();
  if (stations > 0 && !g_stationMode) g_stationMode = true;
  if (stations == 0 && g_stationMode) g_stationMode = false;

  unsigned long cyc0 = millis();
  if (g_stationMode) {
    // 锁到 softAP 信道。注意: 有 STA 时 esp_wifi_set_channel 返回非 ESP_OK, 但射频实际
    // 已切到 AP_CH(手机 RSSI -5 实证), 强制同步 g_curCh, 否则日志/跳频恢复逻辑都会被假失败骗
    if (g_curCh != AP_CH) {
      esp_wifi_set_channel(AP_CH, WIFI_SECOND_CHAN_NONE);
      g_curCh = AP_CH; g_chSw++;
    }
    // 锁台降载: 每轮只发 1/4 名字(4 轮覆盖全部) + 帧间均匀让位。
    // 集中 delay 不够: DHCP offer/HTTP 响应排在 TX 队列里被洪流挤到超时(手机实测 40s 拿不到 IP)
    int quarter = (g_count + 3) / 4;
    int start = (int)((g_cycleCnt % 4) * (unsigned long)quarter);
    for (int k = 0; k < quarter; k++) {
      int idx = start + k;
      if (idx >= g_count) idx -= g_count;
      sendBeacon(g_names[idx], g_wifiMac[idx], AP_CH);
      yield();
      delay(2);                      // 帧间让位: 协议栈出站帧插进队列空隙
    }
    delay(50);
  } else {
    const uint8_t* chs = (g_chMode == 6) ? CH6 : CH3;
    int nch = (g_chMode == 6) ? 6 : 3;
    int gs  = (g_count + nch - 1) / nch;
    for (int g = 0; g < nch; g++) {
      setChannel(chs[g]);
      int start = g * gs;
      int end = start + gs;
      if (end > g_count) end = g_count;
      int off = (int)((g_cycleCnt + g * 17) % (unsigned long)(end - start));
      for (int k = 0; k < end - start; k++) {
        int i = start + (off + k) % (end - start);
        sendBeacon(g_names[i], g_wifiMac[i], chs[g]);
        yield();
      }
    }
  }
  g_cycleMs = millis() - cyc0;
  g_cycleCnt++;
}

// ==================== 蓝牙广播(运行时开关, 开启需重启) ====================

static esp_ble_adv_params_t g_advParams = {
  .adv_int_min       = 0x20,
  .adv_int_max       = 0x30,
  .adv_type          = ADV_TYPE_IND,
  .own_addr_type     = BLE_ADDR_TYPE_RANDOM,
  .peer_addr         = {0},
  .peer_addr_type    = BLE_ADDR_TYPE_PUBLIC,
  .channel_map       = ADV_CHNL_ALL,
  .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void bleConfigAndStart() {
  const char* name = g_names[g_bleIdx].c_str();
  int nl = (int)strlen(name);
  if (nl > 27) nl = 27;              // 广播数据上限 31 字节

  uint8_t data[31];
  int len = 0;
  data[len++] = (uint8_t)(nl + 1);
  data[len++] = 0x09;                // 完整设备名
  memcpy(data + len, name, nl);
  len += nl;

  esp_ble_gap_set_rand_addr(g_bleMac[g_bleIdx]);
  esp_ble_gap_config_adv_data_raw(data, len);
}

static void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  // 蓝牙任务上下文, 严禁 Serial.printf(堵输出锁 -> 看门狗重置), 只计数
  g_gapEvt++;
  g_gapLastEvt = (int)event;
  switch (event) {
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
      g_bleAdvOn = false;
      bleConfigAndStart();
      break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
      esp_ble_gap_start_advertising(&g_advParams);
      break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      g_bleAdvOn = true;
      break;
    default:
      break;
  }
}

static void bleTick() {
  if (!g_master || !g_bleOn || !g_bleStarted) return;

  unsigned long now = millis();
  if (now - g_bleLast < (unsigned long)BLE_DWELL_MS) return;
  g_bleLast = now;

  g_bleIdx++;
  if (g_bleIdx >= g_count) g_bleIdx = 0;

  if (g_bleAdvOn) esp_ble_gap_stop_advertising();
  else bleConfigAndStart();
}

// ==================== 管理后台(HTTP + captive portal) ====================

static const char INDEX_HTML[] PROGMEM = R"HTMLDOC(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>NewEraAdmin · 广播管理后台</title>
<style>
:root{
  --primary:#D0BCFF;--on-primary:#381E72;--primary-container:#4F378B;--on-primary-container:#EADDFF;
  --secondary-container:#4A4458;--on-secondary-container:#E8DEF8;
  --error:#F2B8B5;
  --surface:#1C1B1F;--on-surface:#E6E1E5;--surface-variant:#49454F;--on-surface-variant:#CAC4D0;
  --surface-container-lowest:#0F0D13;--surface-container-low:#1D1B20;--surface-container:#211F26;
  --surface-container-high:#2B2930;--surface-container-highest:#36343B;
  --outline:#938F99;--outline-variant:#49454F;
  --inverse-surface:#E6E1E5;--inverse-on-surface:#313033;
  --shadow:rgba(0,0,0,.5);
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;padding:0}
body{background:var(--surface);color:var(--on-surface);
  font-family:Roboto,"Noto Sans SC","PingFang SC","Microsoft YaHei",system-ui,sans-serif;
  font-size:14px;line-height:1.45;min-height:100vh}
.wrap{max-width:520px;margin:0 auto;padding:0 16px 140px}
/* ---------- 顶部应用栏 ---------- */
.appbar{position:sticky;top:0;z-index:10;background:var(--surface-container);
  display:flex;align-items:center;gap:12px;padding:10px 8px 10px 16px;box-shadow:0 1px 3px var(--shadow)}
.appbar .logo{width:40px;height:40px;border-radius:12px;background:var(--primary-container);
  color:var(--on-primary-container);display:grid;place-items:center;flex:none}
.appbar .logo svg{width:22px;height:22px}
.appbar .grow{flex:1;min-width:0}
.appbar h1{margin:0;font-size:20px;font-weight:500;line-height:1.2}
.appbar p{margin:2px 0 0;font-size:12px;color:var(--on-surface-variant)}
.iconbtn{width:40px;height:40px;border-radius:20px;border:0;background:transparent;
  color:var(--on-surface-variant);display:grid;place-items:center;cursor:pointer;position:relative;flex:none}
.iconbtn svg{width:22px;height:22px}
.iconbtn::after{content:"";position:absolute;inset:0;border-radius:inherit;background:currentColor;opacity:0;transition:opacity .15s}
.iconbtn:hover::after{opacity:.08}.iconbtn:active::after{opacity:.12}
/* ---------- 卡片 ---------- */
.card{background:var(--surface-container-low);border-radius:16px;padding:16px;margin-top:16px}
.card h2{margin:0 0 4px;font-size:16px;font-weight:500;display:flex;align-items:center;gap:8px}
.card h2 svg{width:20px;height:20px;color:var(--primary)}
.row{display:flex;align-items:center;gap:16px;padding:8px 0;min-height:48px}
.row .grow{flex:1;min-width:0}
.row .t{font-size:16px}
.row .d{font-size:12px;color:var(--on-surface-variant);margin-top:2px}
.divider{height:1px;background:var(--outline-variant);border:0;margin:10px 0}
.lab{font-size:12px;font-weight:500;color:var(--on-surface-variant);letter-spacing:.4px;margin:14px 0 8px}
.lab .val{float:right;color:var(--primary);font-weight:500;font-variant-numeric:tabular-nums}
.chips{display:flex;gap:8px;flex-wrap:wrap}
/* ---------- MD3 开关 ---------- */
.m3-switch{width:52px;height:32px;border-radius:16px;border:2px solid var(--outline);
  background:var(--surface-container-highest);position:relative;cursor:pointer;flex:none;
  padding:0;transition:background .2s,border-color .2s}
.m3-switch .handle{position:absolute;top:50%;left:6px;transform:translateY(-50%);
  width:16px;height:16px;border-radius:50%;background:var(--outline);
  display:grid;place-items:center;transition:all .2s cubic-bezier(.2,0,0,1)}
.m3-switch .handle svg{width:14px;height:14px;color:var(--on-primary-container);opacity:0;transition:opacity .15s}
.m3-switch[aria-checked="true"]{background:var(--primary);border-color:var(--primary)}
.m3-switch[aria-checked="true"] .handle{width:24px;height:24px;left:20px;background:var(--primary-container)}
.m3-switch[aria-checked="true"] .handle svg{opacity:1}
/* ---------- MD3 纸 chip ---------- */
.chip{height:32px;border-radius:8px;border:1px solid var(--outline);background:transparent;
  color:var(--on-surface-variant);font-size:14px;font-weight:500;padding:0 12px;
  display:inline-flex;align-items:center;gap:6px;cursor:pointer;position:relative;
  font-family:inherit;transition:background .15s,color .15s,border-color .15s}
.chip .ck{width:0;height:16px;overflow:hidden;transition:width .15s;flex:none}
.chip[aria-pressed="true"]{background:var(--secondary-container);color:var(--on-secondary-container);border-color:transparent}
.chip[aria-pressed="true"] .ck{width:16px}
/* ---------- MD3 滑块 ---------- */
.m3-slider{-webkit-appearance:none;appearance:none;width:100%;height:24px;background:transparent;cursor:pointer;display:block}
.m3-slider::-webkit-slider-runnable-track{height:4px;border-radius:2px;
  background:linear-gradient(to right,var(--primary) var(--pct,50%),var(--surface-variant) var(--pct,50%))}
.m3-slider::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;
  background:var(--primary);margin-top:-8px;border:0}
.m3-slider::-moz-range-track{height:4px;border-radius:2px;background:var(--surface-variant)}
.m3-slider::-moz-range-progress{height:4px;border-radius:2px;background:var(--primary)}
.m3-slider::-moz-range-thumb{width:20px;height:20px;border:0;border-radius:50%;background:var(--primary)}
/* ---------- MD3 文本框 ---------- */
.m3-tf{position:relative;display:block;margin-top:14px}
.m3-tf input{width:100%;height:52px;background:transparent;border:1px solid var(--outline);
  border-radius:8px;color:var(--on-surface);font-size:16px;font-family:inherit;padding:0 14px;outline:none;transition:border-color .15s}
.m3-tf input:focus{border-color:var(--primary);border-width:2px;padding:0 13px}
.m3-tf span{position:absolute;left:10px;top:16px;font-size:16px;color:var(--on-surface-variant);
  background:var(--surface-container-low);padding:0 4px;pointer-events:none;transition:all .15s}
.m3-tf input:focus+span,.m3-tf input:not(:placeholder-shown)+span{top:-8px;font-size:12px;color:var(--primary)}
.hint{font-size:12px;color:var(--on-surface-variant);margin-top:10px;line-height:1.6}
.hint .val{color:var(--primary);font-weight:500}
.cnt{float:right;font-variant-numeric:tabular-nums}
.cnt.over{color:var(--error)}
/* ---------- 状态行 ---------- */
.stat{display:flex;justify-content:space-between;align-items:center;padding:7px 0;font-size:14px;gap:12px}
.graphbox{margin:8px 0 2px}
.graphbox canvas{width:100%;height:56px;display:block}
.stat span{color:var(--on-surface-variant);flex:none}
.stat b{font-weight:500;color:var(--primary);font-variant-numeric:tabular-nums;text-align:right}
/* ---------- 按钮 / FAB ---------- */
.m3-btn{height:40px;border-radius:20px;border:0;font-size:14px;font-weight:500;font-family:inherit;
  padding:0 24px;display:inline-flex;align-items:center;gap:8px;cursor:pointer;position:relative;overflow:hidden}
.m3-btn.text{background:transparent;color:var(--primary);padding:0 12px}
.m3-btn::after{content:"";position:absolute;inset:0;background:currentColor;opacity:0;transition:opacity .15s}
.m3-btn:hover::after{opacity:.08}.m3-btn:active::after{opacity:.12}
.fab{position:fixed;right:16px;bottom:24px;height:56px;border-radius:16px;
  background:var(--primary-container);color:var(--on-primary-container);border:0;
  display:inline-flex;align-items:center;gap:10px;padding:0 20px;font-size:14px;font-weight:500;
  font-family:inherit;box-shadow:0 3px 8px var(--shadow);cursor:pointer;z-index:20}
.fab svg{width:20px;height:20px}
footer{text-align:center;color:var(--on-surface-variant);font-size:12px;padding:28px 0 8px;line-height:1.6}
/* ---------- Snackbar / 对话框 ---------- */
.snackbar{position:fixed;left:16px;right:16px;bottom:96px;background:var(--inverse-surface);
  color:var(--inverse-on-surface);border-radius:8px;padding:14px 16px;font-size:14px;
  box-shadow:0 3px 8px var(--shadow);transform:translateY(20px);opacity:0;transition:all .25s;
  z-index:30;pointer-events:none;max-width:488px;margin:0 auto}
.snackbar.show{transform:none;opacity:1}
.snackbar.err{background:#8C1D18;color:#F9DEDC}
.scrim{position:fixed;inset:0;background:rgba(0,0,0,.6);display:none;place-items:center;z-index:40;padding:24px}
.scrim.show{display:grid}
.dialog{background:var(--surface-container-high);border-radius:28px;padding:24px;max-width:320px;width:100%;
  box-shadow:0 6px 16px var(--shadow)}
.dialog h3{margin:0 0 16px;font-size:22px;font-weight:400}
.dialog p{margin:0 0 24px;font-size:14px;color:var(--on-surface-variant)}
.dialog .acts{display:flex;justify-content:flex-end;gap:8px}
</style>
</head>
<body>
<header class="appbar">
  <div class="logo"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M2.8 9.2a14.5 14.5 0 0 1 18.4 0M5.8 12.6a9.5 9.5 0 0 1 12.4 0M8.7 15.9a4.8 4.8 0 0 1 6.6 0"/><circle cx="12" cy="19.3" r="1.15" fill="currentColor" stroke="none"/></svg></div>
  <div class="grow"><h1>NewEraAdmin</h1><p>广播管理后台 · NewEraStudio</p></div>
  <button class="iconbtn" id="btn-reset" title="恢复出厂"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18M8 6V4.8A1.8 1.8 0 0 1 9.8 3h4.4A1.8 1.8 0 0 1 16 4.8V6M5.5 6l1 14a2 2 0 0 0 2 1.9h7a2 2 0 0 0 2-1.9l1-14"/><path d="M10 11v6M14 11v6"/></svg></button>
  <button class="iconbtn" id="btn-reboot" title="重启设备"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12A7.5 7.5 0 1 1 12 4.5"/><path d="M15 4.5L12.3 2.6M15 4.5L12.3 6.4"/></svg></button>
</header>
<main class="wrap">

  <section class="card">
    <h2><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M5 19.5v-6M12 19.5V5M19 19.5v-9"/></svg>运行状态</h2>
    <div class="stat"><span>广播状态</span><b id="st-master">—</b></div>
    <div class="stat"><span>注入速率</span><b id="st-rate">—</b></div>
    <div class="graphbox"><canvas id="txg"></canvas></div>
    <div class="stat"><span>累计发送</span><b id="st-total">—</b></div>
    <div class="stat"><span>在线设备</span><b id="st-cli">—</b></div>
    <div class="stat"><span>信道策略</span><b id="st-ch">—</b></div>
    <div class="stat"><span>可用内存</span><b id="st-heap">—</b></div>
    <div class="stat"><span>运行时长</span><b id="st-up">—</b></div>
  </section>

  <section class="card">
    <div class="row">
      <div class="grow"><div class="t">广播总开关</div><div class="d">关闭后停止一切广播，管理后台仍可访问</div></div>
      <button class="m3-switch" id="sw-master" role="switch" aria-checked="true" aria-label="总开关"><span class="handle"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg></span></button>
    </div>
  </section>

  <section class="card">
    <h2><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M2.8 9.2a14.5 14.5 0 0 1 18.4 0M5.8 12.6a9.5 9.5 0 0 1 12.4 0M8.7 15.9a4.8 4.8 0 0 1 6.6 0"/><circle cx="12" cy="19.3" r="1.15" fill="currentColor" stroke="none"/></svg>WiFi 干扰</h2>
    <div class="row">
      <div class="grow"><div class="t">信标注入</div><div class="d">向周围广播大量同名热点</div></div>
      <button class="m3-switch" id="sw-wifi" role="switch" aria-checked="true" aria-label="WiFi开关"><span class="handle"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg></span></button>
    </div>
    <hr class="divider">
    <div class="lab">广播数量</div>
    <div class="chips">
      <button class="chip" data-g="cnt" data-v="100"><svg class="ck" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg>100</button>
      <button class="chip" data-g="cnt" data-v="200"><svg class="ck" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg>200</button>
      <button class="chip" data-g="cnt" data-v="300"><svg class="ck" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg>300</button>
      <button class="chip" data-g="cnt" data-v="500"><svg class="ck" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg>500</button>
    </div>
    <div class="lab">信道策略</div>
    <div class="chips">
      <button class="chip" data-g="chm" data-v="3"><svg class="ck" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg>3 信道 · 集中</button>
      <button class="chip" data-g="chm" data-v="6"><svg class="ck" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg>6 信道 · 分散</button>
    </div>
    <hr class="divider">
    <div class="lab">发射功率 <b class="val" id="pwr-v">19.5 dBm</b></div>
    <input type="range" class="m3-slider" id="pwr" min="8" max="19.5" step="0.5" value="19.5">
  </section>

  <section class="card">
    <h2><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2.2v19.6M12 2.2l5 4.8-5 5M12 12.2l-5 4.8 5 5"/></svg>蓝牙干扰</h2>
    <div class="row">
      <div class="grow"><div class="t">广播同名设备</div><div class="d">轮换广播与 WiFi 同名的蓝牙设备</div></div>
      <button class="m3-switch" id="sw-ble" role="switch" aria-checked="false" aria-label="蓝牙开关"><span class="handle"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg></span></button>
    </div>
    <div class="hint">切换蓝牙开关会自动重启设备（约 2 秒），用于避开 WiFi/BT 共存初始化顺序限制。</div>
  </section>

  <section class="card">
    <h2><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M14.8 5.2l4 4L8 20H4v-4L14.8 5.2z"/><path d="M13.2 6.8l4 4"/></svg>广播前缀</h2>
    <label class="m3-tf"><input id="prefix" placeholder=" " maxlength="40" autocomplete="off" spellcheck="false"><span>前缀名称</span></label>
    <div class="hint"><span class="cnt" id="pfx-cnt">0/26</span>最长 26 字节（约 8 个汉字），保存后立即生效<br>预览：<b class="val" id="pv">—</b></div>
  </section>

  <section class="card">
    <h2><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><rect x="5" y="10.5" width="14" height="10" rx="2.2"/><path d="M8.2 10.5V7.8a3.8 3.8 0 0 1 7.6 0v2.7"/></svg>管理热点密码</h2>
    <label class="m3-tf"><input id="apass" type="password" placeholder=" " maxlength="63" autocomplete="new-password" spellcheck="false"><span>连接密码</span></label>
    <div class="chips">
      <button class="chip" id="apw-clear" aria-pressed="false"><svg class="ck" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg>清除密码（开放网络）</button>
    </div>
    <div class="hint"><span id="apw-state">当前：开放网络</span><br>8~63 字符；留空保存 = 不修改；设置后需重启生效</div>
  </section>

  <footer>NewEraStudio · NewEra-Broadcast v13 · ESP32-S3<br>管理热点 NewEraAdmin · 192.168.4.1</footer>
</main>
<button class="fab" id="fab"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 12.6l5 5L19.5 7"/></svg>保存</button>
<div class="snackbar" id="snack"></div>
<div class="scrim" id="dlg"><div class="dialog"><h3 id="dlg-t"></h3><p id="dlg-b"></p><div class="acts"><button class="m3-btn text" id="dlg-no">取消</button><button class="m3-btn text" id="dlg-ok">确定</button></div></div></div>
<script>
const $=s=>document.querySelector(s),$$=s=>document.querySelectorAll(s);
let selCount=200,selCh=3,selPower=78;
const txHist=[];
function drawTx(){
  const c=$('#txg');if(!c)return;
  const dpr=window.devicePixelRatio||1,W=c.clientWidth||300,H=56;
  c.width=W*dpr;c.height=H*dpr;const x=c.getContext('2d');x.setTransform(dpr,0,0,dpr,0,0);
  x.clearRect(0,0,W,H);
  const n=txHist.length;if(n<2)return;
  const mx=(Math.max.apply(null,txHist)*1.15)||1;
  const pt=i=>[i/(n-1)*W,H-4-(txHist[i]/mx)*(H-12)];
  const g=x.createLinearGradient(0,0,0,H);
  g.addColorStop(0,'rgba(208,188,255,.35)');g.addColorStop(1,'rgba(208,188,255,0)');
  x.beginPath();x.moveTo(pt(0)[0],pt(0)[1]);
  for(let i=1;i<n;i++)x.lineTo(pt(i)[0],pt(i)[1]);
  x.lineTo(W,H);x.lineTo(0,H);x.closePath();x.fillStyle=g;x.fill();
  x.beginPath();x.moveTo(pt(0)[0],pt(0)[1]);
  for(let i=1;i<n;i++)x.lineTo(pt(i)[0],pt(i)[1]);
  x.strokeStyle='#D0BCFF';x.lineWidth=2;x.lineJoin='round';x.stroke();
}
function snack(msg,err){const s=$('#snack');s.textContent=msg;s.classList.toggle('err',!!err);
  s.classList.add('show');clearTimeout(s._t);s._t=setTimeout(()=>s.classList.remove('show'),3000);}
function dlg(title,body,onOk){$('#dlg-t').textContent=title;$('#dlg-b').textContent=body;
  $('#dlg-ok').onclick=()=>{$('#dlg').classList.remove('show');onOk&&onOk();};
  $('#dlg').classList.add('show');}
$('#dlg-no').onclick=()=>$('#dlg').classList.remove('show');
function fmtUp(s){const h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=s%60;
  return (h?h+' 时 ':'')+String(m).padStart(2,'0')+' 分 '+String(x).padStart(2,'0')+' 秒';}
function utf8len(s){let n=0;for(const c of s){const k=c.codePointAt(0);n+=k<0x80?1:k<0x800?2:k<0x10000?3:4;}return n;}
function updPfxCnt(){const n=utf8len($('#prefix').value);$('#pfx-cnt').textContent=n+'/26';
  $('#pfx-cnt').classList.toggle('over',n>26);}
async function refresh(){
  try{
    const r=await fetch('/api/status',{cache:'no-store'});const j=await r.json();
    $('#sw-master').setAttribute('aria-checked',j.master?'true':'false');
    $('#sw-wifi').setAttribute('aria-checked',j.wifi?'true':'false');
    $('#sw-ble').setAttribute('aria-checked',j.ble?'true':'false');
    $('#st-master').textContent=!j.master?'已停止':((j.wifi?'WiFi 广播中':'WiFi 已暂停')+(j.ble?' · 蓝牙广播中':''));
    $('#st-rate').textContent=j.txrate.toLocaleString()+' 帧/秒';
    txHist.push(j.txrate);if(txHist.length>40)txHist.shift();drawTx();
    $('#st-total').textContent=j.txtotal.toLocaleString();
    $('#st-cli').textContent=j.clients+' 台';
    $('#st-ch').textContent=(j.chmode==6?'6 信道':'3 信道')+' · 当前 '+j.channel+(j.station?' · 已锁定':'');
    $('#st-heap').textContent=Math.round(j.heap/1024)+' KB';
    $('#st-up').textContent=fmtUp(j.uptime);
    if(document.activeElement!==$('#prefix')){$('#prefix').value=j.prefix;updPfxCnt();}
    $('#pv').textContent=(j.prefix||'')+'1';
    $('#apw-state').textContent=j.apw?'当前：已设置密码（重启生效后替换）':'当前：开放网络';
    selCount=j.count;selCh=j.chmode;selPower=j.power;
    $$('.chip[data-g="cnt"]').forEach(c=>c.setAttribute('aria-pressed',+c.dataset.v===j.count));
    $$('.chip[data-g="chm"]').forEach(c=>c.setAttribute('aria-pressed',+c.dataset.v===j.chmode));
    const db=j.power/4;$('#pwr').value=db;$('#pwr-v').textContent=db.toFixed(1)+' dBm';
    $('#pwr').style.setProperty('--pct',((db-8)/11.5*100)+'%');
  }catch(e){}
}
setInterval(refresh,2000);refresh();
window.addEventListener('resize',drawTx);
$$('.m3-switch').forEach(s=>s.onclick=()=>s.setAttribute('aria-checked',
  s.getAttribute('aria-checked')==='true'?'false':'true'));
$$('.chip').forEach(c=>c.onclick=()=>{const g=c.dataset.g;
  $$('.chip[data-g="'+g+'"]').forEach(x=>x.setAttribute('aria-pressed','false'));
  c.setAttribute('aria-pressed','true');
  if(g==='cnt')selCount=+c.dataset.v;else if(g==='chm')selCh=+c.dataset.v;});
$('#apw-clear').onclick=()=>{  // 清除密码 chip 可反复开关
  $('#apw-clear').setAttribute('aria-pressed',
    $('#apw-clear').getAttribute('aria-pressed')==='true'?'false':'true');};
$('#pwr').addEventListener('input',e=>{const db=+e.target.value;selPower=Math.round(db*4);
  $('#pwr-v').textContent=db.toFixed(1)+' dBm';
  e.target.style.setProperty('--pct',((db-8)/11.5*100)+'%');});
$('#prefix').addEventListener('input',()=>{updPfxCnt();$('#pv').textContent=($('#prefix').value||'')+'1';});
$('#fab').onclick=async()=>{
  const n=utf8len($('#prefix').value);
  if(n===0||n>26){snack('前缀长度需为 1~26 字节',true);return;}
  const body=new URLSearchParams({
    master:$('#sw-master').getAttribute('aria-checked')==='true'?1:0,
    wifi:$('#sw-wifi').getAttribute('aria-checked')==='true'?1:0,
    ble:$('#sw-ble').getAttribute('aria-checked')==='true'?1:0,
    prefix:$('#prefix').value,count:selCount,chmode:selCh,power:selPower,
    appass:$('#apass').value,
    apwclr:$('#apw-clear').getAttribute('aria-pressed')==='true'?1:0});
  try{
    const r=await fetch('/api/save',{method:'POST',body});const j=await r.json();
    if(j.ok){snack(j.msg||'已保存');
      $('#apw-clear').setAttribute('aria-pressed','false');$('#apass').value='';
      if(j.restart)setTimeout(()=>dlg('需要重启','部分设置需要重启设备后生效，是否立即重启？',reboot),400);
      refresh();}
    else snack(j.msg||'保存失败',true);
  }catch(e){snack('网络错误',true);}
};
async function post(u){try{await fetch(u,{method:'POST'});}catch(e){}}
function reboot(){post('/api/reboot');snack('正在重启…');setTimeout(()=>location.reload(),3500);}
$('#btn-reboot').onclick=()=>dlg('重启设备','管理后台将中断约 2 秒，确定重启？',reboot);
$('#btn-reset').onclick=()=>dlg('恢复出厂','将清空全部设置（前缀/数量/功率/密码等）并重启，确定？',()=>{
  post('/api/reset');snack('正在恢复出厂并重启…');setTimeout(()=>location.reload(),3500);});
</script>
</body>
</html>
)HTMLDOC";

// ---- JSON 工具 ----
static void sendJson(int code, const String& body) {
  server.send(code, "application/json", body);
}

static void handleStatus() {
  char buf[520];
  snprintf(buf, sizeof(buf),
    "{\"ok\":1,\"master\":%d,\"wifi\":%d,\"ble\":%d,\"started\":%d,"
    "\"prefix\":\"%s\",\"count\":%d,\"chmode\":%d,\"power\":%d,\"apw\":%d,"
    "\"uptime\":%lu,\"txrate\":%lu,\"txtotal\":%lu,\"txerr\":%lu,"
    "\"clients\":%d,\"channel\":%d,\"station\":%d,\"heap\":%lu,"
    "\"ap\":\"%s\",\"version\":\"%s\",\"studio\":\"%s\"}",
    g_master ? 1 : 0, g_wifiOn ? 1 : 0, g_bleOn ? 1 : 0, g_bleStarted ? 1 : 0,
    g_prefix.c_str(), g_count, g_chMode, g_power, g_apPass.length() ? 1 : 0,
    millis() / 1000, (unsigned long)g_txRate, (unsigned long)g_txOk, (unsigned long)g_txErr,
    WiFi.softAPgetStationNum(), (int)g_curCh, g_stationMode ? 1 : 0,
    (unsigned long)ESP.getFreeHeap(), AP_SSID, FW_VER, STUDIO);
  sendJson(200, buf);
}

static void savePrefs() {
  prefs.putBool("mas", g_master);
  prefs.putBool("wif", g_wifiOn);
  prefs.putBool("ble", g_bleOn);
  prefs.putString("pfx", g_prefix);
  prefs.putInt("cnt", g_count);
  prefs.putInt("chm", g_chMode);
  prefs.putInt("pwr", g_power);
  prefs.putString("apw", g_apPass);
}

static void handleSave() {
  bool master = server.arg("master") == "1";
  bool wifi   = server.arg("wifi")   == "1";
  bool ble    = server.arg("ble")    == "1";

  // 前缀清洗: 去首尾空白 + 剔除控制字符/引号/反斜杠(JSON 安全)
  String pfx = server.arg("prefix");
  pfx.trim();
  String clean;
  for (size_t i = 0; i < pfx.length(); i++) {
    char c = pfx[i];
    if ((uint8_t)c < 0x20) continue;
    if (c == '"' || c == '\\') continue;
    clean += c;
  }
  if (clean.length() == 0 || clean.length() > 26) {
    sendJson(200, "{\"ok\":0,\"msg\":\"前缀长度需为 1~26 字节\"}");
    return;
  }

  int count  = server.arg("count").toInt();
  if (count < 10) count = 10;
  if (count > MAXN) count = MAXN;
  int chmode = server.arg("chmode").toInt();
  if (chmode != 6) chmode = 3;
  int power  = server.arg("power").toInt();
  if (power < 32) power = 32;
  if (power > 78) power = 78;

  // 管理热点密码: 空=保持不变; "1"=清除; 其他=新密码(8~63 字节)
  String pw = server.arg("appass");
  pw.trim();
  bool apDirty = false;
  if (server.arg("apwclr") == "1") {
    if (g_apPass.length()) { g_apPass = ""; apDirty = true; }
  } else if (pw.length()) {
    String pwClean;
    for (size_t i = 0; i < pw.length(); i++) {
      char c = pw[i];
      if ((uint8_t)c < 0x20 || c == '"' || c == '\\') continue;
      pwClean += c;
    }
    if (pwClean.length() < 8 || pwClean.length() > 63) {
      sendJson(200, "{\"ok\":0,\"msg\":\"密码长度需为 8~63 字符（留空表示不修改）\"}");
      return;
    }
    if (pwClean != g_apPass) { g_apPass = pwClean; apDirty = true; }
  }

  bool bleTurningOn = (ble && !g_bleOn);
  bool namesDirty = (clean != g_prefix) || (count != g_count);

  g_master = master;
  g_wifiOn = wifi;
  g_bleOn  = ble;
  if (namesDirty) { g_prefix = clean; g_count = count; generateNames(); }
  g_chMode = chmode;
  if (power != g_power) { g_power = power; WiFi.setTxPower((wifi_power_t)g_power); }
  if (!g_bleOn && g_bleAdvOn) esp_ble_gap_stop_advertising();

  savePrefs();

  if (bleTurningOn) {
    sendJson(200, "{\"ok\":1,\"restart\":1,\"msg\":\"蓝牙干扰已开启，重启后生效\"}");
  } else if (apDirty) {
    sendJson(200, "{\"ok\":1,\"restart\":1,\"msg\":\"管理热点密码已更新，重启后生效\"}");
  } else {
    sendJson(200, "{\"ok\":1,\"restart\":0,\"msg\":\"已保存\"}");
  }
}

static void handleReboot() {
  sendJson(200, "{\"ok\":1,\"msg\":\"restarting\"}");
  delay(400);
  ESP.restart();
}

static void handleReset() {
  prefs.clear();
  sendJson(200, "{\"ok\":1,\"msg\":\"factory reset\"}");
  delay(400);
  ESP.restart();
}

static void setupWeb() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/save",   HTTP_POST, handleSave);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/reset",  HTTP_POST, handleReset);
  // captive portal: 任何未知域名都拉回管理页
  server.onNotFound([]() {
    String uri = server.uri();
    if (uri.startsWith("/api/")) {
      sendJson(404, "{\"ok\":0,\"msg\":\"not found\"}");
    } else {
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.send(302, "text/plain", "Redirecting to NewEraAdmin");
    }
  });
  server.begin();
}

// ==================== 主流程 ====================

void setup() {
  Serial0.begin(115200);       // UART0 -> CH343 桥(COM4, 诊断主通道)
  Serial.begin(115200);        // 原生 USB(COM5)

  prefs.begin("nebcast", false);   // 读写模式
  g_master = prefs.getBool("mas", true);
  g_wifiOn = prefs.getBool("wif", true);
  g_bleOn  = prefs.getBool("ble", false);
  g_prefix = prefs.getString("pfx", "新启年工作室牛逼");
  g_count  = prefs.getInt("cnt", 200);
  g_chMode = prefs.getInt("chm", 3);
  g_power  = prefs.getInt("pwr", 78);
  g_apPass = prefs.getString("apw", "");
  if (g_count < 10) g_count = 10;
  if (g_count > MAXN) g_count = MAXN;
  if (g_chMode != 6) g_chMode = 3;
  if (g_power < 32) g_power = 32;
  if (g_power > 78) g_power = 78;

  randomSeed(esp_random());
  generateNames();

  // 蓝牙必须先于 WiFi 初始化, 否则 coex_enable() abort
  if (g_bleOn) {
    BLEDevice::init("NewEraAdmin");
    esp_ble_gap_register_callback(gapCallback);
    g_bleStarted = true;
    bleConfigAndStart();
  }

  WiFi.mode(WIFI_AP);
  WiFi.setTxPower((wifi_power_t)g_power);
  WiFi.softAP(AP_SSID, g_apPass.length() ? g_apPass.c_str() : nullptr, AP_CH, false, 4);
  esp_wifi_config_80211_tx_rate(WIFI_IF_AP, WIFI_PHY_RATE_2M_L);   // 2Mbps 长前导
  g_curCh = AP_CH;

  setupWeb();
  dnsServer.start(53, "*", WiFi.softAPIP());    // captive portal DNS

  Serial0.println("===========================================");
  Serial0.printf("NewEra-Broadcast %s | %s\n", FW_VER, STUDIO);
  Serial0.printf("SSID=%s  IP=%s  count=%d  chmode=%d  power=%.1fdBm  ble=%d  apw=%s\n",
                 AP_SSID, WiFi.softAPIP().toString().c_str(),
                 g_count, g_chMode, g_power / 4.0, g_bleOn ? 1 : 0,
                 g_apPass.length() ? "on" : "off");
  Serial0.println("管理后台: http://192.168.4.1/  (MD3)");
  Serial0.println("===========================================");
  Serial.printf("NewEra-Broadcast %s started, IP http://192.168.4.1/\n", FW_VER);
}

void loop() {
  server.handleClient();
  dnsServer.processNextRequest();
  wifiTick();
  bleTick();

  static unsigned long lastLog = 0;
  static unsigned long lastTx = 0;
  if (millis() - lastLog > 5000) {
    unsigned long now = millis();
    unsigned long dt = now - lastLog;
    g_txRate = (g_txOk - lastTx) * 1000 / (dt ? dt : 1);
    lastLog = now; lastTx = g_txOk;

    char buf[300];
    snprintf(buf, sizeof(buf),
      "[alive] tx_ok=%lu tx_err=%lu(last=0x%x) tx/s=%lu ch_sw=%lu ch_us=%lu cur=%d cyc=%lu cyc_ms=%lu sta=%d%s%s\n",
      (unsigned long)g_txOk, (unsigned long)g_txErr, (unsigned)g_txLastErr,
      (unsigned long)g_txRate, g_chSw, g_chUs, (int)g_curCh,
      g_cycleCnt, g_cycleMs, WiFi.softAPgetStationNum(),
      g_stationMode ? " [LOCKED]" : "", (!g_master || !g_wifiOn) ? " [PAUSED]" : "");
    Serial0.print(buf);
    Serial.print(buf);
  }
}
