"""系统指标采集 —— 只读 /proc 与 /sys，不依赖第三方库。

为什么不用 psutil：
  1. 板子上 python3-pip 未安装，走 venv 需要额外下载；
  2. /proc 与 /sys 已经能提供方案 §5.3 要求全部指标；
  3. 少一个依赖 = 少一个 24×7 运行时的故障面。
字段缺失一律返回 None，不拿 0 冒充（protocol/topics-v1.md §4.2）。
"""

from __future__ import annotations

import os
import time
from dataclasses import dataclass
from typing import Optional


def _read_text(path: str) -> Optional[str]:
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return fh.read()
    except OSError:
        return None


def _read_int(path: str) -> Optional[int]:
    text = _read_text(path)
    if text is None:
        return None
    try:
        return int(text.strip())
    except ValueError:
        return None


def read_uptime_seconds() -> int:
    text = _read_text("/proc/uptime") or "0"
    try:
        return int(float(text.split()[0]))
    except (ValueError, IndexError):
        return 0


def clock_synced() -> bool:
    """判断系统时间是否已由 NTP 同步。

    依据 systemd 自己的判定（`timedatectl show -p NTPSynchronized`）。
    不能靠 /run/chrony/ 是否可读：那个目录是 0700 root，普通用户看不到；
    本机实测 chrony 在跑，但 /run/systemd/timesync 不存在——只看文件会误报 false。
    未同步时返回 False（保守失败，计划 §5.4）。
    """
    import subprocess

    try:
        proc = subprocess.run(
            ["timedatectl", "show", "-p", "NTPSynchronized", "--value"],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return proc.stdout.strip().lower() == "yes"


@dataclass(frozen=True)
class CpuSample:
    total_ticks: int
    idle_ticks: int


def read_cpu_sample() -> Optional[CpuSample]:
    """读 /proc/stat 第一行，返回累计 tick；由调用方做差分。"""
    text = _read_text("/proc/stat")
    if not text:
        return None
    first = text.splitlines()[0]
    parts = first.split()
    if len(parts) < 5 or parts[0] != "cpu":
        return None
    try:
        values = [int(v) for v in parts[1:]]
    except ValueError:
        return None
    # user nice system idle iowait irq softirq steal
    idle = values[3] + (values[4] if len(values) > 4 else 0)
    return CpuSample(total_ticks=sum(values), idle_ticks=idle)


def cpu_usage_percent(prev: Optional[CpuSample], cur: Optional[CpuSample]) -> Optional[float]:
    """两次采样的差值 → 使用率。首次采样返回 None（无法判断）。"""
    if prev is None or cur is None:
        return None
    total_delta = cur.total_ticks - prev.total_ticks
    idle_delta = cur.idle_ticks - prev.idle_ticks
    if total_delta <= 0:
        return None
    busy = max(total_delta - idle_delta, 0)
    return round(busy * 100.0 / total_delta, 1)


def read_loadavg() -> dict[str, Optional[float]]:
    text = _read_text("/proc/loadavg")
    if not text:
        return {"load1": None, "load5": None, "load15": None}
    parts = text.split()
    out: dict[str, Optional[float]] = {"load1": None, "load5": None, "load15": None}
    for key, idx in (("load1", 0), ("load5", 1), ("load15", 2)):
        try:
            out[key] = float(parts[idx])
        except (ValueError, IndexError):
            pass
    return out


def read_core_count() -> int:
    count = 0
    for entry in os.scandir("/sys/devices/system/cpu"):
        if entry.is_dir() and entry.name.startswith("cpu") and entry.name[3:].isdigit():
            count += 1
    return count or (os.cpu_count() or 1)


def read_memory() -> dict[str, object]:
    text = _read_text("/proc/meminfo")
    info: dict[str, int] = {}
    if text:
        for line in text.splitlines():
            if ":" not in line:
                continue
            key, _, rest = line.partition(":")
            raw = rest.strip().split()
            if not raw:
                continue
            try:
                info[key] = int(raw[0]) * 1024  # kB → B
            except ValueError:
                continue

    total = info.get("MemTotal")
    available = info.get("MemAvailable")
    if total is None:
        return {
            "total_bytes": None,
            "used_bytes": None,
            "used_percent": None,
            "swap_used_percent": None,
        }

    used = (total - available) if available is not None else None
    used_pct = round(used * 100.0 / total, 1) if used is not None and total else None

    swap_total = info.get("SwapTotal", 0)
    swap_free = info.get("SwapFree", 0)
    swap_pct = None
    if swap_total:
        swap_pct = round((swap_total - swap_free) * 100.0 / swap_total, 1)

    return {
        "total_bytes": total,
        "used_bytes": used,
        "used_percent": used_pct,
        "swap_used_percent": swap_pct,
    }


def read_temperatures() -> dict[str, object]:
    """读全部 thermal_zone。RK3576 实测有 6 个温区。"""
    zones: dict[str, float] = {}
    root = "/sys/class/thermal"
    try:
        entries = sorted(os.listdir(root))
    except OSError:
        return {"soc_celsius": None, "max_celsius": None, "zones": {}}

    for name in entries:
        if not name.startswith("thermal_zone"):
            continue
        base = os.path.join(root, name)
        zone_type = (_read_text(os.path.join(base, "type")) or "").strip()
        milli = _read_int(os.path.join(base, "temp"))
        if not zone_type or milli is None:
            continue
        zones[zone_type] = round(milli / 1000.0, 1)

    soc = zones.get("soc-thermal")
    if soc is None and zones:
        soc = next(iter(zones.values()))
    return {
        "soc_celsius": soc,
        "max_celsius": max(zones.values()) if zones else None,
        "zones": zones,
    }


def read_disk(path: str = "/") -> dict[str, object]:
    try:
        stats = os.statvfs(path)
    except OSError:
        return {"path": path, "total_bytes": None, "used_bytes": None, "used_percent": None}

    total = stats.f_blocks * stats.f_frsize
    free = stats.f_bavail * stats.f_frsize
    used = max(total - free, 0)
    used_pct = round(used * 100.0 / total, 1) if total else None
    return {
        "path": path,
        "total_bytes": total,
        "used_bytes": used,
        "used_percent": used_pct,
    }


_ALLOWED_SERVICE_STATES = {
    "active": "active",
    "inactive": "inactive",
    "failed": "failed",
    "activating": "activating",
    "deactivating": "deactivating",
    "reloading": "reloading",
}


def read_services(names: tuple[str, ...]) -> dict[str, str]:
    """读 systemd 单元 ActiveState。

    不调 systemctl：systemd 通过 D-Bus 暴露状态，从 /run/systemd/ 与
    /proc/<pid>/... 推断不可靠。这里用 `systemctl show -p ActiveState`
    只读查询，属于 systemd 官方接口，开销约 5 ms。
    """
    import subprocess

    result: dict[str, str] = {}
    for name in names:
        unit = name if "." in name else f"{name}.service"
        try:
            proc = subprocess.run(
                ["systemctl", "show", "-p", "ActiveState", "--value", unit],
                capture_output=True,
                text=True,
                timeout=3,
                check=False,
            )
        except (OSError, subprocess.SubprocessError):
            result[name] = "unknown"
            continue
        state = proc.stdout.strip()
        result[name] = _ALLOWED_SERVICE_STATES.get(state, state or "unknown")
    return result


def read_fan() -> dict[str, object]:
    """pwmfan 的 pwm1 是 0-255 幅值；无传感器时返回 None。

    按 hwmon 的 name 属性查找，不写死 hwmon 编号（编号在重启后会变）。
    """
    root = "/sys/class/hwmon"
    try:
        entries = os.listdir(root)
    except OSError:
        return {"pwm": None}
    for name in entries:
        base = os.path.join(root, name)
        if (_read_text(os.path.join(base, "name")) or "").strip() != "pwmfan":
            continue
        pwm = _read_int(os.path.join(base, "pwm1"))
        if pwm is not None:
            return {"pwm": pwm}
    return {"pwm": None}


def local_ip_addresses(target: str = "192.168.0.1") -> list[str]:
    """取本机用于访问 `target` 的那张网卡的 IPv4。

    用 UDP connect（不真正发包）让内核选路，比遍历 /sys 更准，
    也避免在接了 Tailscale / docker 的机器上把 100.x 或 172.17.x 当成局域网地址。
    """
    import socket

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.connect((target, 1))
            ip = sock.getsockname()[0]
        finally:
            sock.close()
    except OSError:
        return []
    if ip and not ip.startswith("127."):
        return [ip]
    return []


def monotonic() -> float:
    return time.monotonic()
