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
import os
import subprocess
import sys
import time
from typing import NamedTuple, Optional

if sys.platform != "win32":  # pragma: no cover - 只在 Windows 上跑
    raise RuntimeError("本模块只能在 Windows 上运行")

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
#   - 网络速率同理。
# 但「格式化性能计数器」每次要 5 s（Win32_PerfFormattedData 会遍历所有进程），
# 撑不住 10 s 的刷新周期。所以改成采「累计值 + 自己差分」：
#   - 进程：Get-Process 的 CPU（累计 CPU 秒）与 WorkingSet64，约 240 ms；
#   - 网络：netstat -e 的累计收发字节，约 26 ms（比 Get-NetAdapterStatistics 的
#     2.6 s 便宜两个数量级，且输出里的 "Bytes"/"字节" 行两种语言都能认）。
# 差分由调用方做：同一份原始快照 + 上次采样时刻。
_PS_PROCS = (
    "$p = Get-Process -ErrorAction SilentlyContinue; "
    "[pscustomobject]@{ count = $p.Count; items = @($p | "
    "Where-Object { $_.CPU -ne $null } | "
    "Select-Object Name,CPU,WorkingSet64) } | ConvertTo-Json -Compress -Depth 3"
)


def _run_powershell(script: str, timeout: float = 8.0) -> Optional[str]:
    try:
        proc = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive",
             "-ExecutionPolicy", "Bypass", "-Command", script],
            capture_output=True, text=True, timeout=timeout, check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    return proc.stdout


def read_net_totals() -> tuple[Optional[int], Optional[int]]:
    """netstat -e 的累计收发字节。取不到返回 (None, None)。

    坑：带 CREATE_NO_WINDOW 启动时，netstat 不会输出英文，而是按系统 OEM
    代码页输出（中文机器是 GBK 的「字节」行）。按 UTF-8 解会变成乱码，
    匹配不上任何行——所以这里用 encoding="oem"。
    """
    try:
        proc = subprocess.run(
            ["netstat.exe", "-e"],
            capture_output=True, text=True, timeout=5, check=False,
            encoding="oem", errors="replace",
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError):
        return None, None
    if proc.returncode != 0:
        return None, None

    for line in proc.stdout.splitlines():
        tokens = line.split()
        # 英文系统 "Bytes 123 456"，中文系统 "字节 123 456"；
        # 只认这一行，避免把 Unicast packets 之类也当成字节数
        if len(tokens) >= 3 and tokens[0] in ("Bytes", "字节"):
            try:
                return int(tokens[1]), int(tokens[2])
            except ValueError:
                return None, None
    return None, None


def sample_procnet() -> dict[str, object]:
    """采一份原始快照（累计值，不是速率）。失败的部分保持空/None。"""
    result: dict[str, object] = {
        "processes": {},          # name -> {"cpu_seconds": float, "memory_bytes": int}
        "process_count": None,
        "rx_total": None,
        "tx_total": None,
        "at": time.monotonic(),
    }

    out = _run_powershell(_PS_PROCS)
    if out is not None:
        import json
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
                procs[name] = {
                    "cpu_seconds": float(cpu),
                    "memory_bytes": int(mem) if isinstance(mem, (int, float)) else 0,
                }
            result["processes"] = procs
            # 总数用 Get-Process 的总数量，而不是「有 CPU 值的那部分」：
            # 受保护进程读不到 CPU，但它们确实在跑，资源管理器也算进总数
            count = data.get("count")
            result["process_count"] = (
                int(count) if isinstance(count, (int, float)) else len(procs)
            )

    rx, tx = read_net_totals()
    result["rx_total"] = rx
    result["tx_total"] = tx
    return result


def derive_procnet(prev: Optional[dict[str, object]],
                   cur: dict[str, object],
                   cores: int,
                   top_n: int = 3) -> dict[str, object]:
    """把两份累计快照差分成「进程 CPU% / 内存」和「网络收发速率」。

    首次采样（没有 prev）时，CPU% 与速率一律给 None —— 屏幕上显示 "--"，
    不拿 0 冒充「这一刻真的没跑东西」。
    """
    empty = {
        "processes": [],
        "process_count": cur.get("process_count"),
        "network": {"rx_bytes_per_sec": None, "tx_bytes_per_sec": None},
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
            "memory_bytes": info.get("memory_bytes"),
        })

    # 按内存占用降序（同内存时 CPU 高的在前）
    rows.sort(key=lambda r: (
        float(r["memory_bytes"] or 0),
        float(r["cpu_percent"] or 0),
    ), reverse=True)

    network = {"rx_bytes_per_sec": None, "tx_bytes_per_sec": None}
    rx_now, tx_now = cur.get("rx_total"), cur.get("tx_total")
    rx_before, tx_before = prev.get("rx_total"), prev.get("tx_total")
    if None not in (rx_now, tx_now, rx_before, tx_before):
        network["rx_bytes_per_sec"] = max(int(rx_now) - int(rx_before), 0) // int(elapsed)
        network["tx_bytes_per_sec"] = max(int(tx_now) - int(tx_before), 0) // int(elapsed)

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