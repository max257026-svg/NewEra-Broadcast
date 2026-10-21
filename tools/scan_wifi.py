# -*- coding: utf-8 -*-
"""强制全新 WiFi 扫描(绕开 netsh 缓存), 统计 NewEra-Broadcast 广播的热点。

用法:  python tools/scan_wifi.py [数量]
        python tools/scan_wifi.py 200      # 广播了 200 个时
        python tools/scan_wifi.py          # 默认 200

关键点(踩过的坑):
  - WlanScan 的末参 pReserved 必须是 NULL, 传 True 会返回 87 (ERROR_INVALID_PARAMETER)
  - GUID 用 WlanEnumInterfaces 枚举拿, 不要手工拼字节(GUID 各段字节序不同, 拼错返回 1168)
  - PC 连续多轮 WlanScan 会缓存退化(后几轮掉到 10 个), 是 WLAN 服务行为, 非固件问题;
    每轮之间 sleep 6 秒以上
"""
import ctypes, sys, time
from ctypes import wintypes

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

wlanapi = ctypes.windll.wlanapi

wlanapi.WlanOpenHandle.restype = wintypes.DWORD
wlanapi.WlanCloseHandle.restype = wintypes.DWORD
wlanapi.WlanEnumInterfaces.restype = wintypes.DWORD
wlanapi.WlanScan.restype = wintypes.DWORD
wlanapi.WlanGetAvailableNetworkList.restype = wintypes.DWORD

class DOT11_SSID(ctypes.Structure):
    _fields_ = [("uSSIDLength", wintypes.DWORD), ("ucSSID", ctypes.c_ubyte * 32)]

class WLAN_AVAILABLE_NETWORK(ctypes.Structure):
    _fields_ = [
        ("strProfileName", wintypes.WCHAR * 256),
        ("dot11Ssid", DOT11_SSID),
        ("dot11BssType", ctypes.c_int),
        ("uNumberOfBssids", wintypes.DWORD),
        ("bNetworkConnectable", wintypes.BOOL),
        ("wlanSignalQuality", wintypes.DWORD),
        ("uSecurityEnabled", wintypes.BOOL),
        ("dot11DefaultAuthAlgorithm", ctypes.c_int),
        ("dot11DefaultCipherAlgorithm", ctypes.c_int),
        ("dwFlags", wintypes.DWORD),
        ("dwReserved", wintypes.DWORD),
    ]

class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                ("Data4", ctypes.c_ubyte * 8)]

class WLAN_INTERFACE_INFO(ctypes.Structure):
    _fields_ = [("InterfaceGuid", GUID),
                ("strInterfaceDescription", wintypes.WCHAR * 256),
                ("isState", ctypes.c_int)]

class WLAN_INTERFACE_INFO_LIST(ctypes.Structure):
    _fields_ = [("dwNumberOfItems", wintypes.DWORD), ("dwIndex", wintypes.DWORD),
                ("InterfaceInfo", WLAN_INTERFACE_INFO * 8)]

MAX_NET = 600
class WLAN_AVAILABLE_NETWORK_LIST(ctypes.Structure):
    _fields_ = [("dwNumberOfItems", wintypes.DWORD), ("dwIndex", wintypes.DWORD),
                ("Network", WLAN_AVAILABLE_NETWORK * MAX_NET)]

# ---------- 参数 ----------
PREFIX = "新启年工作室牛逼"
COUNT = int(sys.argv[1]) if len(sys.argv) > 1 else 200
ROUNDS = 3

# 1. 打开句柄
neg = wintypes.DWORD()
h = ctypes.c_void_p()
rc = wlanapi.WlanOpenHandle(2, None, ctypes.byref(neg), ctypes.byref(h))
if rc != 0:
    print("WlanOpenHandle failed:", rc); sys.exit(1)

# 2. 枚举无线接口, 取 GUID
pIfList = ctypes.c_void_p()
rc = wlanapi.WlanEnumInterfaces(h, None, ctypes.byref(pIfList))
if rc != 0:
    print("WlanEnumInterfaces failed:", rc); sys.exit(1)
iflist = ctypes.cast(pIfList, ctypes.POINTER(WLAN_INTERFACE_INFO_LIST)).contents
if iflist.dwNumberOfItems == 0:
    print("no wifi interface"); sys.exit(1)
info = iflist.InterfaceInfo[0]
iface = GUID.from_buffer_copy(bytes(info.InterfaceGuid))
print("iface: %s  GUID: %08X-%04X-%04X..." % (
    info.strInterfaceDescription, iface.Data1, iface.Data2, iface.Data3))

# 3. 多轮强制全新扫描
best = {}
t0 = time.time()
for rnd in range(ROUNDS):
    rc = wlanapi.WlanScan(h, ctypes.byref(iface), None, None, None)   # 末参必须是 NULL
    if rc != 0:
        print("WlanScan round %d rc=%d" % (rnd, rc)); continue
    time.sleep(6)                       # 等扫描完成
    pNet = ctypes.c_void_p()
    rc = wlanapi.WlanGetAvailableNetworkList(h, ctypes.byref(iface), 0, None, ctypes.byref(pNet))
    if rc != 0:
        print("WlanGetAvailableNetworkList round %d rc=%d" % (rnd, rc)); continue
    lst = ctypes.cast(pNet, ctypes.POINTER(WLAN_AVAILABLE_NETWORK_LIST)).contents
    n = min(lst.dwNumberOfItems, MAX_NET)
    seen = set()
    for i in range(n):
        net = lst.Network[i]
        ln = net.dot11Ssid.uSSIDLength
        if ln > 32: ln = 32
        ssid = bytes(net.dot11Ssid.ucSSID[:ln]).decode("utf-8", "ignore")
        if ssid and ssid not in seen:
            seen.add(ssid)
            q = net.wlanSignalQuality
            if q > 100: q = -1
            best[ssid] = max(best.get(ssid, -1), q)
    print("round %d: %d unique BSS, 累计 %d" % (rnd, len(seen), len(best)))
    wlanapi.WlanFreeMemory(pNet)
    if rnd < ROUNDS - 1:
        time.sleep(4)

# 4. 统计
ne = [s for s in best if PREFIX in s]
admin = [s for s in best if s == "NewEraAdmin"]
others = [s for s in best if PREFIX not in s and s != "NewEraAdmin"]

print("\n==== 扫描结果 (%.1fs) ====" % (time.time() - t0))
print("NewEraAdmin 管理热点 : %d" % len(admin))
print("%s 系列: %d / %d" % (PREFIX, len(ne), COUNT))
nums = sorted(int(s.replace(PREFIX, "") or -1) for s in ne)
if nums:
    missing = [x for x in range(1, COUNT + 1) if x not in nums]
    print("编号范围: %d ~ %d" % (nums[0], nums[-1]))
    print("缺失编号(%d 个): %s" % (len(missing), missing if missing else "无, 全覆盖"))
    sigs = [q for s in best if PREFIX in s and best[s] >= 0]
    if sigs:
        print("信号质量: 平均 %d%%, 最高 %d%%, 最低 %d%%" % (
            sum(sigs)//len(sigs), max(sigs), min(sigs)))
print("\n其他真实网络 (%d 个):" % len(others))
for s in sorted(others):
    q = best[s]
    print("   %-30s %s" % (s, ("%d%%" % q) if q >= 0 else "?"))

wlanapi.WlanFreeMemory(pIfList)
wlanapi.WlanCloseHandle(h, None)
