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


# ---------------------------------------------------------------- 温度

def read_temperatures() -> dict[str, object]:
    """普通权限拿不到 CPU 封装温度（需要内核驱动或管理员 + WMI 热区）。

    协议 §4.2 明确「没有温度传感器用 null 或不出现，不要用 0 冒充」，
    所以这里如实返回空，让屏幕显示 "--"。
    """
    return {"soc_celsius": None, "max_celsius": None, "zones": {}}


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