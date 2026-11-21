"""Windows 指标采集 —— 只用 ctypes 调 Win32 API，不依赖第三方库。

为什么不用 psutil（与泰山派 Agent 同样的理由）：
  1. 用户机器上没有 psutil，也不想为 24×7 运行的后台程序引入 pip 依赖；
  2. GetSystemTimes / GlobalMemoryStatusEx / GetDiskFreeSpaceExW 已经能覆盖
     protocol/topics-v1.md §4.2 要求的指标；
  3. 少一个依赖 = 少一个开机自启失败的故障面。

字段缺失一律返回 None，不拿 0 冒充（protocol/topics-v1.md §4.2）。
温度尤其如此：普通权限读不到 CPU 传感器，宁可给 null 让屏幕显示 "--"，
也不要编一个数字上去。
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import logging
import os
import re
import subprocess
import sys
import time
from typing import NamedTuple, Optional

if sys.platform != "win32":  # pragma: no cover - 只在 Windows 上跑
    raise RuntimeError("本模块只能在 Windows 上运行")

# 挂在 windows_agent 的 logger 下：它的 handler / level 由 _setup_logging 配置，
# 直接用 __name__（"metrics"）会走 root，最后只有 lastResort 打到 stderr，
# 日志文件里看不到
LOG = logging.getLogger("windows-agent.metrics")

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# 不声明返回类型的话 ctypes 按 c_int 处理，64 位返回值会被截断：
# GetTickCount64 开机 24.8 天后（2^31 ms）就会翻成负数。
kernel32.GetTickCount64.restype = ctypes.c_ulonglong
kernel32.GetTickCount64.argtypes = []


class _FileTime(ctypes.Structure):
    _fields_ = [("dwLowDateTime", wt.DWORD), ("dwHighDateTime", wt.DWORD)]

    @property
    def value(self) -> int:
        return (self.dwHighDateTime << 32) | self.dwLowDateTime


class _MemoryStatusEx(ctypes.Structure):
    _fields_ = [
        ("dwLength", wt.DWORD),
        ("dwMemoryLoad", wt.DWORD),
        ("ullTotalPhys", ctypes.c_ulonglong),
        ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong),
        ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong),
        ("ullAvailVirtual", ctypes.c_ulonglong),
        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


class CpuSample(NamedTuple):
    idle: int
    kernel: int
    user: int


# ---------------------------------------------------------------- CPU

def read_cpu_sample() -> Optional[CpuSample]:
    """GetSystemTimes 返回的是累计时间（100 ns 单位），由调用方做差分。

    注意 kernel 里**已经包含** idle，所以总时间是 kernel + user，
    busy 是 kernel + user - idle。这里写反过的人很多，结果会算出 100% 以上。
    """
    idle, kern, user = _FileTime(), _FileTime(), _FileTime()
    if not kernel32.GetSystemTimes(
        ctypes.byref(idle), ctypes.byref(kern), ctypes.byref(user)
    ):
        return None
    return CpuSample(idle=idle.value, kernel=kern.value, user=user.value)


def cpu_usage_percent(prev: Optional[CpuSample], cur: Optional[CpuSample]) -> Optional[float]:
    """两次采样的差值 → 使用率。首次采样返回 None（无法判断）。"""
    if prev is None or cur is None:
        return None
    total = (cur.kernel - prev.kernel) + (cur.user - prev.user)
    idle = cur.idle - prev.idle
    if total <= 0:
        return None
    busy = max(total - idle, 0)
    return round(busy * 100.0 / total, 1)


def read_core_count() -> int:
    return os.cpu_count() or 1


# ---------------------------------------------------------------- 内存

def read_memory() -> dict[str, object]:
    """Windows 没有独立的 swap；页面文件（pagefile）里混着物理内存，
    拿它算「交换使用率」是错的。按协议「字段缺失给 null」，swap 一律 None。

    load 百分比直接取 dwMemoryLoad，和 total/avail 算出来的一致，
    但它的口径是系统自己算的，避免我们自己四舍五入产生偏差。
    """
    stat = _MemoryStatusEx()
    stat.dwLength = ctypes.sizeof(stat)
    if not kernel32.GlobalMemoryStatusEx(ctypes.byref(stat)):
        return {"total_bytes": None, "used_bytes": None,
                "used_percent": None, "swap_used_percent": None}

    total = int(stat.ullTotalPhys)
    avail = int(stat.ullAvailPhys)
    used = max(total - avail, 0)
    return {
        "total_bytes": total,
        "used_bytes": used,
        "used_percent": round(used * 100.0 / total, 1) if total else None,
        "swap_used_percent": None,
    }


# ---------------------------------------------------------------- 磁盘

def read_disk(path: str) -> dict[str, object]:
    free = ctypes.c_ulonglong()
    total = ctypes.c_ulonglong()
    total_free = ctypes.c_ulonglong()
    ok = kernel32.GetDiskFreeSpaceExW(
        ctypes.c_wchar_p(path),
        ctypes.byref(free),
        ctypes.byref(total),
        ctypes.byref(total_free),
    )
    if not ok:
        return {"path": path, "total_bytes": None,
                "used_bytes": None, "used_percent": None}

    total_b = int(total.value)
    # 用「当前用户可用」算已用，和资源管理器显示的剩余空间一致
    used_b = max(total_b - int(free.value), 0)
    return {
        "path": path,
        "total_bytes": total_b,
        "used_bytes": used_b,
        "used_percent": round(used_b * 100.0 / total_b, 1) if total_b else None,
    }


_DRIVE_FIXED = 3


def read_disks(max_count: int = 4) -> list[dict[str, object]]:
    """列出本机固定磁盘分区（C:、D:…），最多 max_count 个。

    只认 DRIVE_FIXED：U 盘、光驱、网络盘会随插拔变化，
    屏幕上「共 N 个分区」跟着跳来跳去反而不可信。
    """
    mask = kernel32.GetLogicalDrives()
    if not mask:
        return []

    found: list[dict[str, object]] = []
    for i in range(26):
        if not (mask >> i) & 1:
            continue
        root = f"{chr(ord('A') + i)}:\\"
        if kernel32.GetDriveTypeW(ctypes.c_wchar_p(root)) != _DRIVE_FIXED:
            continue
        info = read_disk(root)
        if info.get("total_bytes"):
            found.append(info)
        if len(found) >= max_count:
            break
    return found


# ---------------------------------------------------------------- 温度

def read_temperatures() -> dict[str, object]:
    """普通权限拿不到 CPU 封装温度（需要内核驱动或管理员 + WMI 热区）。

    协议 §4.2 明确「没有温度传感器用 null 或不出现，不要用 0 冒充」，
    所以这里如实返回空，让屏幕显示 "--"。
    """
    return {"soc_celsius": None, "max_celsius": None, "zones": {}}


# ---------------------------------------------------------------- GPU（NVIDIA）

# 只查这一组字段。笔记本的 fan.speed 恒为 [N/A]（风扇由 EC 统一控制，
# NVML 不暴露），查了只会拿到空值，干脆不放进帧里，避免下游误判成 0 RPM。
_SMI_FIELDS = "name,utilization.gpu,temperature.gpu,memory.used,memory.total"


def _gpu_empty() -> dict[str, object]:
    """没有独显 / nvidia-smi 不可用时返回全 None。

    协议 §4.2：字段缺失一律给 null，不拿 0 冒充。
    """
    return {
        "name": None,
        "usage_percent": None,
        "temp_celsius": None,
        "memory_used_bytes": None,
        "memory_total_bytes": None,
    }


def _optional(value: str, cast):
    """把 nvidia-smi 的 'N/A' / '[N/A]' / 空串统一变成 None。"""
    try:
        return cast(value)
    except (TypeError, ValueError):
        return None


def read_gpu() -> dict[str, object]:
    """读 NVIDIA 独显的利用率、温度与显存。

    为什么用 nvidia-smi 而不是 NVML：
      1. 它随显卡驱动安装、固定在 System32，不需要额外 pip 依赖；
      2. NVML 的 ctypes 绑定要维护一堆结构体和版本差异，收益不大；
      3. 笔记本 GPU 风扇本来就读不到（见上面 _SMI_FIELDS 的说明）。

    代价是每次调用要起一个进程（实测 60~120 ms），所以调用方把刷新周期
    设在 5 s，而不是跟 CPU 一样 2 s。

    只取第一块卡：本机只有一块 RTX 4060 Laptop；Intel 核显不归 nvidia-smi 管。
    """
    import shutil

    exe = shutil.which("nvidia-smi")
    if exe is None:
        fallback = r"C:\Windows\System32\nvidia-smi.exe"
        if os.path.isfile(fallback):
            exe = fallback
    if exe is None:
        return _gpu_empty()

    try:
        proc = subprocess.run(
            [exe, f"--query-gpu={_SMI_FIELDS}", "--format=csv,noheader,nounits"],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError):
        return _gpu_empty()

    if proc.returncode != 0:
        return _gpu_empty()

    lines = [line for line in proc.stdout.splitlines() if line.strip()]
    if not lines:
        return _gpu_empty()

    # 显卡名里可能带逗号，所以从右侧切 4 次，前段整体当名字
    parts = [p.strip() for p in lines[0].rsplit(",", 4)]
    if len(parts) != 5:
        return _gpu_empty()

    name = parts[0]
    usage = _optional(parts[1], float)
    temp = _optional(parts[2], float)
    used_mib = _optional(parts[3], float)
    total_mib = _optional(parts[4], float)

    return {
        "name": None if name in ("", "[N/A]", "N/A") else name,
        "usage_percent": usage,
        "temp_celsius": temp,
        "memory_used_bytes": int(used_mib * 1024 * 1024) if used_mib is not None else None,
        "memory_total_bytes": int(total_mib * 1024 * 1024) if total_mib is not None else None,
    }


# ---------------------------------------------------------------- CPU 拓扑

# GetLogicalProcessorInformation（老接口）的条目长度不固定：ProcessorCore 条目
# 是 16 字节，Cache 条目更大，按固定步长遍历会数错（实测把 10 核读成 30）。
# 所以用 Ex 版本：每条记录自带 Size 字段（前 4 字节 Relationship，后 4 字节 Size），
# 按 Size 跳着走就永远不会错位。
_RELATION_PROCESSOR_CORE = 0


def read_physical_cores() -> Optional[int]:
    """物理核数（例如 10），用来在屏幕上写「10 核 / 16 线程」。

    不依赖 WMI/语言：只调一次 kernel32，开机读一次即可（核数不会变）。
    """
    size = wt.DWORD(0)
    kernel32.GetLogicalProcessorInformationEx(0, None, ctypes.byref(size))
    if size.value == 0:
        return None

    buf = (ctypes.c_byte * size.value)()
    if not kernel32.GetLogicalProcessorInformationEx(0, buf, ctypes.byref(size)):
        return None

    cores = 0
    offset = 0
    total = int(size.value)
    while offset + 8 <= total:
        relationship = ctypes.c_uint.from_buffer(buf, offset).value
        entry_size = ctypes.c_uint.from_buffer(buf, offset + 4).value
        if relationship == _RELATION_PROCESSOR_CORE:
            cores += 1
        if entry_size <= 0:
            break
        offset += entry_size

    return cores or None


# ---------------------------------------------------------------- 进程 / 网络

# 这两类数据没有便宜的纯 ctypes 路径：
#   - 每进程 CPU 要用 PDH / NtQuerySystemInformation，自己实现容易出错；
#   - 分网卡收发字节只有 NetTCPIP 模块（Get-NetAdapterStatistics）能给出。
# 但「格式化性能计数器」每次要 5 s（Win32_PerfFormattedData 会遍历所有进程），
# 撑不住 10 s 的刷新周期。所以一律采「累计值 + 自己差分」：
#   - 进程：Get-Process 的 CPU（累计 CPU 秒）与 WorkingSet64，约 240 ms；
#   - 网络：一次 PowerShell 同时取分网卡累计字节、链路信息与默认出口。
#     Get-NetAdapterStatistics 实测约 2.6 s，因此 30 s 才跑一次并缓存
#     （见 collect_network_interfaces）。
_PS_PROCS = (
    "$p = Get-Process -ErrorAction SilentlyContinue; "
    "[pscustomobject]@{ count = $p.Count; items = @($p | "
    "Where-Object { $_.CPU -ne $null } | "
    "Select-Object Name,CPU,PrivateMemorySize64) } | ConvertTo-Json -Compress -Depth 3"
)

# 任务管理器「内存」列是“私有工作集”。Get-Process 的 WorkingSet64 含共享页，
# 同名多个进程会把同一批 DLL 重复计算（豆包 16 个进程能虚高近 1 GB）；而
# PrivateMemorySize64 是提交量，又比实际驻留大约高 20%。这里用性能计数器取
# 真正的私有工作集；一次约 3~4 s，所以 30 s 刷新一次并缓存。
_PS_PRIVATE_WS = (
    "$ErrorActionPreference = 'SilentlyContinue'; "
    "$s = Get-Counter '\\Process(*)\\Working set - Private' -ErrorAction SilentlyContinue; "
    "[pscustomobject]@{ samples = @($s.CounterSamples | ForEach-Object { "
    "[pscustomobject]@{ name = $_.InstanceName; bytes = [double]$_.CookedValue } }) } | "
    "ConvertTo-Json -Compress -Depth 3"
)
_PRIVATE_WS_TTL = 30.0
_PRIVATE_WS = {"at": 0.0, "map": {}}


def collect_private_ws() -> dict[str, int]:
    """按进程名聚合私有工作集（字节），30 s 缓存。失败时沿用上一轮结果。"""
    now = time.monotonic()
    if now - float(_PRIVATE_WS["at"]) < _PRIVATE_WS_TTL:
        return dict(_PRIVATE_WS["map"])

    out = _run_powershell(_PS_PRIVATE_WS, timeout=12.0)
    mapping: dict[str, int] = {}
    if out:
        try:
            data = json.loads(out)
        except ValueError:
            data = None
        if isinstance(data, dict):
            for item in data.get("samples") or []:
                name = str(item.get("name") or "")
                # 性能计数器的实例名一律小写，Get-Process 是原始大小写，
                # 所以统一转小写当键；"_total" 也是小写，别只按 "_Total" 过滤。
                base = name.split("#")[0].strip().lower()
                if not base or base.startswith("_total") or base == "idle":
                    continue
                b = item.get("bytes")
                if isinstance(b, (int, float)) and b > 0:
                    mapping[base] = mapping.get(base, 0) + int(b)
    if mapping:
        _PRIVATE_WS["at"] = now
        _PRIVATE_WS["map"] = mapping
    return mapping or dict(_PRIVATE_WS["map"])



def _powershell_exe() -> str:
    """绝对路径优先：计划任务的 PATH 里不一定有 WindowsPowerShell\\v1.0。"""
    p = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                     "System32", "WindowsPowerShell", "v1.0", "powershell.exe")
    return p if os.path.exists(p) else "powershell.exe"


def _run_powershell(script: str, timeout: float = 8.0) -> Optional[str]:
    try:
        proc = subprocess.run(
            [_powershell_exe(), "-NoProfile", "-NonInteractive",
             "-ExecutionPolicy", "Bypass", "-Command", script],
            capture_output=True, text=True, timeout=timeout, check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    return proc.stdout


def _run_powershell_bytes(script: str, timeout: float) -> Optional[bytes]:
    """先拿原始字节，由调用方决定编码。

    带 CREATE_NO_WINDOW 时不一定有控制台，脚本里设置 [Console]::OutputEncoding
    可能失败；此时 PowerShell 按 OEM 代码页（中文机器是 GBK）输出。
    解码时先按 UTF-8 严格解，失败再按 OEM 解，两条路都能拿到正确文本。
    """
    try:
        proc = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive",
             "-ExecutionPolicy", "Bypass", "-Command", script],
            capture_output=True, timeout=timeout, check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    return proc.stdout


def _decode_powershell_output(raw: bytes) -> str:
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("oem", errors="replace")


# ---------------------------------------------------------------- 分网卡网络

# 一次子进程同时取三样东西：
#   1. Get-NetAdapterStatistics 的累计收发字节（速率靠两次采样差分）；
#   2. Get-NetAdapter 的 ifIndex / 别名 / 物理介质 / 状态 / 链路速率；
#   3. Find-NetRoute 到公网地址的出口 ifIndex（「当前上网卡」）。
# Status / PhysicalMediaType 都是枚举，ConvertTo-Json 在 Windows PowerShell 5.1
# 里会把枚举序列化成数字，所以显式转成字符串；LinkSpeed 是 "1 Gbps" 这种文本。
# 中文网卡名要求输出走 UTF-8：设置 OutputEncoding 在无控制台时可能抛异常，
# 用 try/catch 兜住（拿原始字节 + 双编码解码再兜一层，见 _decode_powershell_output）。
_PS_NET = (
    "try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }; "
    "$ErrorActionPreference = 'SilentlyContinue'; "
    "$stats = @(Get-NetAdapterStatistics | Select-Object Name,ReceivedBytes,SentBytes); "
    "$adapters = @(Get-NetAdapter | Select-Object ifIndex,Name,"
    "@{Name='PhysicalMediaType';Expression={[string]$_.PhysicalMediaType}},"
    "@{Name='Status';Expression={[string]$_.Status}},LinkSpeed); "
    "$active = $null; "
    "foreach ($r in @(Find-NetRoute -RemoteIPAddress '223.5.5.5' -ErrorAction SilentlyContinue)) { "
    "if ($null -ne $r.ifIndex) { $active = [int]$r.ifIndex; break }; "
    "if ($null -ne $r.InterfaceIndex) { $active = [int]$r.InterfaceIndex; break } }; "
    "[pscustomobject]@{ stats = $stats; adapters = $adapters; active = $active } | "
    "ConvertTo-Json -Compress -Depth 4"
)

# PowerShell 要 2.6 s，30 s 刷新一次；速率至少隔 5 s 才有意义（差分噪声）
_NET_REFRESH_SECONDS = 30.0
_NET_MIN_SAMPLE_SECONDS = 5.0

# PhysicalMediaType → 协议里的类型；Wireless WAN / Bluetooth 等一律不认
_NET_PHYSICAL_TYPES = {"802.3": "wired", "native 802.11": "wifi"}
# 名字里带这些字样的都是虚拟/隧道网卡，即使介质报 802.3 也不能当成上网卡
_NET_VIRTUAL_HINTS = (
    "vmware", "hyper-v", "vethernet", "tailscale",
    "loopback", "bluetooth", "tap",
)

_LINK_SPEED_RE = re.compile(r"([\d.]+)\s*([KMGT]?)bps", re.IGNORECASE)
_LINK_SPEED_UNITS = {"": 1e-6, "K": 1e-3, "M": 1.0, "G": 1e3, "T": 1e6}

_NET_STATE: dict[str, object] = {
    "next_try": float("-inf"),  # 下次允许跑 PowerShell 的时刻（30 s 节流）
    "at": float("-inf"),        # 上次成功采样累计计数器的 monotonic 时刻
    "counters": {},             # 别名 -> (rx 累计字节, tx 累计字节)
    "rates": {},                # 别名 -> (rx B/s, tx B/s)
    "result": {
        "rx_bytes_per_sec": None, "tx_bytes_per_sec": None,
        "interfaces": [], "active_id": None, "active_type": None,
    },
}
_NET_WARNED = False        # 一次故障期内只记一条 warning，避免每 30 s 刷屏


def _net_empty() -> dict[str, object]:
    """采集失败的降级值：老字段与新字段都给空/null。"""
    return {
        "rx_bytes_per_sec": None,
        "tx_bytes_per_sec": None,
        "interfaces": [],
        "active_id": None,
        "active_type": None,
    }


def _copy_network(result: object) -> dict[str, object]:
    """返回副本，调用方改它不会污染缓存（不可变约定）。"""
    if not isinstance(result, dict):
        return _net_empty()
    raw_rows = result.get("interfaces")
    rows = (
        [dict(item) for item in raw_rows if isinstance(item, dict)]
        if isinstance(raw_rows, list) else []
    )
    return {
        "rx_bytes_per_sec": result.get("rx_bytes_per_sec"),
        "tx_bytes_per_sec": result.get("tx_bytes_per_sec"),
        "interfaces": rows,
        "active_id": result.get("active_id"),
        "active_type": result.get("active_type"),
    }


def _coerce_int(value: object) -> Optional[int]:
    """JSON 数字 / 数字字符串 → int；None、布尔、'N/A' 一律给 None。"""
    if value is None or isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    if isinstance(value, str):
        try:
            return int(float(value.strip()))
        except ValueError:
            return None
    return None


def _link_mbps(value: object) -> Optional[int]:
    """LinkSpeed → Mbps。Get-NetAdapter 给 '1 Gbps' 这类文本；
    直接来自 CIM 时可能是 bits/s 数字，两种都认。"""
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        mbps = float(value) / 1e6
    else:
        match = _LINK_SPEED_RE.search(str(value or ""))
        if match is None:
            return None
        mbps = float(match.group(1)) * _LINK_SPEED_UNITS[match.group(2).upper()]
    speed = int(round(mbps))
    # 0 Mbps 在协议里表示「断开」，所以小数舍成 0 的一律按未知处理
    return speed if speed > 0 else None


def _warn_net_once(exc: BaseException) -> None:
    global _NET_WARNED
    if _NET_WARNED:
        return
    _NET_WARNED = True
    LOG.warning("网卡指标采集失败，本轮 interfaces 为空（恢复前不再重复记录）：%s", exc)


def _read_net_raw() -> dict[str, object]:
    """跑一次 PowerShell；超时 / 无输出 / JSON 不合法都抛异常，由上层降级。"""
    raw = _run_powershell_bytes(_PS_NET, timeout=20.0)
    if raw is None:
        raise RuntimeError("PowerShell 超时、退出码非 0 或没有输出")
    data = json.loads(_decode_powershell_output(raw))
    if not isinstance(data, dict):
        raise ValueError("PowerShell 输出不是 JSON 对象")
    return data


def _rows(raw: object) -> list[dict[str, object]]:
    """ConvertTo-Json 对单元素数组仍给数组，但这里再兜一层单对象情况。"""
    if isinstance(raw, list):
        return [row for row in raw if isinstance(row, dict)]
    if isinstance(raw, dict):
        return [raw]
    return []


def _diff_rates(prev_counters: dict, counters: dict,
                elapsed: Optional[float], prev_rates: dict) -> dict:
    """累计字节差 / 实际间隔 → B/s。

    首次见到这张卡、计数器变小（网卡重启/系统重开）都给 null；
    间隔不足 5 s 时沿用上一次的速率，避免把采样抖动放大成速率尖峰。
    """
    if elapsed is None or elapsed < _NET_MIN_SAMPLE_SECONDS:
        return dict(prev_rates)

    rates: dict[str, tuple[int, int]] = {}
    for name, (rx, tx) in counters.items():
        before = prev_counters.get(name)
        if before is None:
            continue
        d_rx, d_tx = rx - before[0], tx - before[1]
        if d_rx < 0 or d_tx < 0:
            continue
        rates[name] = (int(d_rx / elapsed), int(d_tx / elapsed))
    return rates


def _pick_adapter(candidates: list[dict[str, object]]) -> Optional[dict[str, object]]:
    """同类里优先 Status=Up；全是断开的也保留第一张（由上层标 down / 0 Mbps）。"""
    for row in candidates:
        if row["status"] == "up":
            return row
    return candidates[0] if candidates else None


def _build_network(raw: dict[str, object], now: float, prev_at: float,
                   prev_counters: dict, prev_rates: dict
                   ) -> tuple[dict[str, object], dict, dict]:
    counters: dict[str, tuple[int, int]] = {}
    for row in _rows(raw.get("stats")):
        name = str(row.get("Name") or "").strip()
        rx = _coerce_int(row.get("ReceivedBytes"))
        tx = _coerce_int(row.get("SentBytes"))
        if name and rx is not None and tx is not None:
            counters[name] = (rx, tx)

    by_index: dict[int, str] = {}
    candidates: dict[str, list[dict[str, object]]] = {"wired": [], "wifi": []}
    for row in _rows(raw.get("adapters")):
        name = str(row.get("Name") or "").strip()
        if not name:
            continue
        # 出口可能是虚拟网卡（例如 Tailscale），索引表必须建全，
        # 过滤只影响 interfaces 列表，不影响 active_id 的解析
        index = _coerce_int(row.get("ifIndex"))
        if index is not None:
            by_index[index] = name
        if any(hint in name.lower() for hint in _NET_VIRTUAL_HINTS):
            continue
        media = str(row.get("PhysicalMediaType") or "").strip().lower()
        kind = _NET_PHYSICAL_TYPES.get(media)
        if kind is None:
            continue
        status = "up" if str(row.get("Status") or "").strip().lower() == "up" else "down"
        candidates[kind].append({
            "name": name,
            "status": status,
            "link": row.get("LinkSpeed"),
        })

    elapsed = now - prev_at if prev_counters else None
    rates = _diff_rates(prev_counters, counters, elapsed, prev_rates)

    interfaces: list[dict[str, object]] = []
    for kind in ("wired", "wifi"):          # 有线在前，无线在后
        chosen = _pick_adapter(candidates[kind])
        if chosen is None:                  # 这类网卡一张都没有
            continue
        name = str(chosen["name"])
        rx, tx = rates.get(name, (None, None))
        interfaces.append({
            "id": name,
            "name": name,
            "type": kind,
            "status": chosen["status"],
            # 断开的网卡没有协商速率；给 0 而不是 null，屏幕显示「未连接」
            "link_mbps": 0 if chosen["status"] == "down"
                          else _link_mbps(chosen["link"]),
            "rx_bytes_per_sec": rx,
            "tx_bytes_per_sec": tx,
        })

    active_index = _coerce_int(raw.get("active"))
    active_id = by_index.get(active_index) if active_index is not None else None
    if active_id is None:
        active_type: Optional[str] = None
    else:
        types = {item["id"]: item["type"] for item in interfaces}
        # 出口是虚拟网卡（VPN / Tailscale）时归为 other
        active_type = types.get(active_id, "other")

    # 旧界面的 network.rx/tx 现在表示「当前上网卡」的速率；
    # 出口是虚拟网卡（不在 interfaces 里）或解析失败时给 null，不拿合计冒充
    active_row = next((item for item in interfaces if item["id"] == active_id), None)
    active_rx = active_row["rx_bytes_per_sec"] if active_row else None
    active_tx = active_row["tx_bytes_per_sec"] if active_row else None

    return (
        {
            "rx_bytes_per_sec": active_rx,
            "tx_bytes_per_sec": active_tx,
            "interfaces": interfaces,
            "active_id": active_id,
            "active_type": active_type,
        },
        counters,
        rates,
    )


def collect_network_interfaces(force: bool = False) -> dict[str, object]:
    """分网卡网络指标：物理有线 / 无线各一张 + 当前默认出口。

    约 30 s 刷新一次并缓存（一次 PowerShell 约 2.6 s，不能跟着 10 s 的
    进程采样跑）。任何失败都降级为 interfaces=[] / active_id=null，
    绝不让网络问题把 CPU / GPU 等其它字段一起拖下水。
    """
    global _NET_WARNED

    now = time.monotonic()
    if not force and now < float(_NET_STATE["next_try"]):
        return _copy_network(_NET_STATE["result"])

    prev_at = float(_NET_STATE["at"])
    prev_counters = _NET_STATE["counters"]
    prev_rates = _NET_STATE["rates"]
    if not isinstance(prev_counters, dict):
        prev_counters = {}
    if not isinstance(prev_rates, dict):
        prev_rates = {}

    try:
        raw = _read_net_raw()
        # 计数器是在子进程执行完时才读到的，用完成时刻算间隔，
        # 否则会把 PowerShell 的 2.6 s 算进两次采样之间，速率偏高约 10%
        sampled_at = time.monotonic()
        result, counters, rates = _build_network(
            raw, sampled_at, prev_at, prev_counters, prev_rates
        )
    except Exception as exc:  # noqa: BLE001 - 采集边界，异常不允许冒泡
        # 清掉计数器：恢复后第一轮没有可比基线，给 null 而不是跨故障期算速率
        _NET_STATE["counters"] = {}
        _NET_STATE["rates"] = {}
        _NET_STATE["result"] = _net_empty()
        _warn_net_once(exc)
        return _net_empty()
    finally:
        # 失败要等下一个 30 s 窗口再试，不每轮刷 warning / 起进程
        _NET_STATE["next_try"] = time.monotonic() + _NET_REFRESH_SECONDS

    if prev_counters and sampled_at - prev_at < _NET_MIN_SAMPLE_SECONDS:
        # 间隔太短不重算：沿用上一轮的速率，也不移动差分基线，
        # 这样下一次正常采样仍然覆盖完整窗口
        _NET_STATE["result"] = result
        _NET_WARNED = False
        return _copy_network(result)

    _NET_STATE["at"] = sampled_at
    _NET_STATE["counters"] = counters
    _NET_STATE["rates"] = rates
    _NET_STATE["result"] = result
    _NET_WARNED = False
    return _copy_network(result)


def sample_procnet() -> dict[str, object]:
    """采一份原始快照（进程是累计值；网络是分网卡速率缓存）。失败的部分保持空/None。"""
    result: dict[str, object] = {
        "processes": {},          # name -> {"cpu_seconds": float, "memory_bytes": int}
        "process_count": None,
        "network": _net_empty(),
        "at": time.monotonic(),
    }

    out = _run_powershell(_PS_PROCS)
    if out is not None:
        try:
            data = json.loads(out)
        except ValueError:
            data = None
        if isinstance(data, dict):
            procs: dict[str, dict[str, float]] = {}
            for item in data.get("items") or []:
                if not isinstance(item, dict):
                    continue
                name = str(item.get("Name") or "").strip()
                cpu = item.get("CPU")
                if not name or not isinstance(cpu, (int, float)):
                    continue
                mem = item.get("WorkingSet64")
                mem_bytes = int(mem) if isinstance(mem, (int, float)) else 0
                # 同名多进程必须累加，不能覆盖：Doubao（16 个进程）、svchost（90+）
                # 如果只留最后一个，排行会漏掉真正的大户，屏幕上永远看不到它们。
                row = procs.get(name)
                if row is None:
                    procs[name] = {
                        "cpu_seconds": float(cpu),
                        "memory_bytes": mem_bytes,
                    }
                else:
                    row["cpu_seconds"] += float(cpu)
                    row["memory_bytes"] += mem_bytes
            result["processes"] = procs
            # 总数用 Get-Process 的总数量，而不是「有 CPU 值的那部分」：
            # 受保护进程读不到 CPU，但它们确实在跑，资源管理器也算进总数
            count = data.get("count")
            result["process_count"] = (
                int(count) if isinstance(count, (int, float)) else len(procs)
            )

    # 网络走自己的 30 s 缓存（见 collect_network_interfaces），失败时给空结构
    result["network"] = collect_network_interfaces()
    return result


def derive_procnet(prev: Optional[dict[str, object]],
                   cur: dict[str, object],
                   cores: int,
                   top_n: int = 3) -> dict[str, object]:
    """把两份累计快照差分成「进程 CPU% / 内存」，网络部分直接取当前缓存。

    首次采样（没有 prev）时，CPU% 一律给 None —— 屏幕上显示 "--"，
    不拿 0 冒充「这一刻真的没跑东西」。网络速率不受这份 prev 影响：
    它由 collect_network_interfaces 自己维护差分基线。
    """
    network = _copy_network(cur.get("network"))
    empty = {
        "processes": [],
        "process_count": cur.get("process_count"),
        "network": network,
    }
    if prev is None:
        return empty

    elapsed = float(cur["at"]) - float(prev["at"])
    if elapsed <= 0.05:      # 两次采样几乎同一时刻，差分没有意义
        return empty

    prev_procs = prev.get("processes") or {}
    cur_procs = cur.get("processes") or {}
    if not isinstance(prev_procs, dict) or not isinstance(cur_procs, dict) or not cur_procs:
        return empty

    # 内存优先用私有工作集（和任务管理器同口径），取不到才退回 PrivateMemorySize64
    ws_private = collect_private_ws()

    rows = []
    for name, info in cur_procs.items():
        if not isinstance(info, dict):
            continue
        before = prev_procs.get(name)
        before_cpu = before.get("cpu_seconds") if isinstance(before, dict) else None
        cpu_now = info.get("cpu_seconds")
        if not isinstance(cpu_now, (int, float)):
            continue
        # 同名进程重启会让累计值归零/变小，按 0 处理，不出现负的占用
        delta = 0.0
        if isinstance(before_cpu, (int, float)):
            delta = max(float(cpu_now) - float(before_cpu), 0.0)
        pct = min(delta / elapsed / max(cores, 1) * 100.0, 100.0)
        rows.append({
            "name": name,
            "cpu_percent": round(pct, 1),
            "memory_bytes": ws_private.get(name.lower(), info.get("memory_bytes")),
        })

    # 按内存占用降序（同内存时 CPU 高的在前）
    rows.sort(key=lambda r: (
        float(r["memory_bytes"] or 0),
        float(r["cpu_percent"] or 0),
    ), reverse=True)

    return {
        "processes": rows[:top_n],
        "process_count": cur.get("process_count"),
        "network": network,
    }


# ---------------------------------------------------------------- 服务

# `sc query` 的 STATE 数字在英文/中文系统上都一样，用它判断比解析文本可靠
_SC_STATE = {
    1: "inactive",      # STOPPED
    2: "activating",    # START_PENDING
    3: "deactivating",  # STOP_PENDING
    4: "active",        # RUNNING
    5: "activating",    # CONTINUE_PENDING
    6: "deactivating",  # PAUSE_PENDING
    7: "inactive",      # PAUSED
}


def _service_state(name: str) -> Optional[str]:
    try:
        proc = subprocess.run(
            ["sc.exe", "query", name],
            capture_output=True, text=True, timeout=5, check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError):
        return None
    for line in proc.stdout.splitlines():
        stripped = line.strip()
        if not stripped.upper().startswith("STATE"):
            continue
        # 形如 "STATE              : 4  RUNNING"
        _, _, rest = stripped.partition(":")
        token = rest.split()
        if token and token[0].isdigit():
            return _SC_STATE.get(int(token[0]), "inactive")
    return None


def read_services(names: tuple[str, ...]) -> dict[str, str]:
    """只返回查到的服务；查不到的直接不出现在结果里（协议允许字段缺失）。"""
    result: dict[str, str] = {}
    for name in names:
        state = _service_state(name)
        if state is not None:
            result[name] = state
    return result


# ---------------------------------------------------------------- 其他

def read_uptime_seconds() -> int:
    ms = kernel32.GetTickCount64()
    return int(ms // 1000)


def local_ip_addresses(broker_host: str) -> list[str]:
    """取本机在「通往 broker」的那块网卡上的 IP。

    不遍历所有网卡：Hyper-V / WSL / 虚拟网卡会给出一堆 172.x 地址，
    屏幕上也放不下，而且它们不代表设备在 LAN 上的身份。
    """
    import socket

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # UDP connect 不发包，只是让内核选出出口网卡
        sock.connect((broker_host, 1883))
        ip = sock.getsockname()[0]
    except OSError:
        ip = ""
    finally:
        sock.close()
    return [ip] if ip else []


def clock_synced(max_age_seconds: int = 7 * 24 * 3600) -> bool:
    """判断系统时间是否由 Windows Time 服务同步过。

    不用 `w32tm /query /status`：它的输出随系统语言变化，中文机器上是
    乱码分级文本，解析必错。改读注册表 Config\\LastKnownGoodTime ——
    这是 REG_QWORD 的 FILETIME，与时区/语言无关。

    超过 7 天没同步就按「不可信」处理（保守失败，协议 §5.4）。
    """
    import winreg

    path = r"SYSTEM\CurrentControlSet\Services\W32Time\Config"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path) as key:
            value, typ = winreg.QueryValueEx(key, "LastKnownGoodTime")
    except OSError:
        return False
    if typ != winreg.REG_QWORD or not isinstance(value, int) or value <= 0:
        return False

    now = _FileTime()
    kernel32.GetSystemTimeAsFileTime(ctypes.byref(now))
    age_seconds = (now.value - value) / 1e7
    return 0 <= age_seconds <= max_age_seconds