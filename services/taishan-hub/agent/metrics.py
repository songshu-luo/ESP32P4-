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


# ---------------------------------------------------------------- 任务状态检查（schema v5）
#
# 五项固定检查，供屏幕右半列「任务状态」卡使用。每条独立求值、独立兜底：
# 某一项采集失败只把自己标成 down，不能把整帧 metrics 打掉。
# 状态取值：ok / warn / down（协议 §4.2 v5）。

_DOCKER_CACHE_TTL = 15.0
_docker_cache_at: float = -1e9
_docker_cache_rows: Optional[list[tuple[str, str]]] = None


def _docker_ps() -> Optional[list[tuple[str, str]]]:
    """`docker ps` 快照 → [(容器名, Status)]；读不到 docker 时返回 None。

    None 与空列表含义不同：None 是「读不到」（daemon 没起 / 不在 docker 组），
    空列表是「docker 正常但一个运行中的容器都没有」。
    """
    import subprocess

    try:
        # 用 "|" 做分隔符，不用 "\t"：docker 的 --format 只对 {{}} 内的模板求值，
        # 模板外的 \t 在不同版本上可能原样输出；容器名和 Status 里都不会出现 "|"。
        proc = subprocess.run(
            ["docker", "ps", "--format", "{{.Names}}|{{.Status}}"],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    rows: list[tuple[str, str]] = []
    for line in proc.stdout.splitlines():
        name, sep, status = line.partition("|")
        name = name.strip()
        if name:
            rows.append((name, status.strip() if sep else ""))
    return rows


def read_docker_snapshot(ttl: float = _DOCKER_CACHE_TTL) -> Optional[list[tuple[str, str]]]:
    """带 TTL 的 docker 快照。

    `docker ps` 要起进程（约 30–60 ms），不能跟着 5 s 的检查节奏跑；
    docker 本身的状态变化也远慢于 5 s，所以 15 s 缓存足够。
    """
    global _docker_cache_at, _docker_cache_rows

    now = monotonic()
    if now - _docker_cache_at < ttl:
        return _docker_cache_rows
    rows = _docker_ps()
    _docker_cache_at = now
    _docker_cache_rows = rows
    return rows


def _process_exists(name: str) -> bool:
    """按 /proc/<pid>/comm 找进程，不调 ps。comm 最多 15 字节，mihomo 够用。"""
    try:
        pids = os.listdir("/proc")
    except OSError:
        return False
    for pid in pids:
        if not pid.isdigit():
            continue
        if (_read_text(f"/proc/{pid}/comm") or "").strip() == name:
            return True
    return False


def _hex_ipv4(raw: str) -> str:
    """/proc/net/tcp 的地址是小端 8 位十六进制，例如 0100007F → 127.0.0.1。"""
    if len(raw) != 8:
        return ""
    try:
        octets = [int(raw[i:i + 2], 16) for i in (6, 4, 2, 0)]
    except ValueError:
        return ""
    return ".".join(str(o) for o in octets)


def _tcp_entries() -> list[tuple[str, int, str, int, str]]:
    """解析 /proc/net/tcp → [(本地IP, 本地端口, 对端IP, 对端端口, 状态)]。"""
    text = _read_text("/proc/net/tcp")
    if not text:
        return []
    entries: list[tuple[str, int, str, int, str]] = []
    for line in text.splitlines()[1:]:
        parts = line.split()
        if len(parts) < 4:
            continue
        local, remote, state = parts[1], parts[2], parts[3]
        lip, _, lport = local.partition(":")
        rip, _, rport = remote.partition(":")
        try:
            entries.append((_hex_ipv4(lip), int(lport, 16),
                            _hex_ipv4(rip), int(rport, 16), state))
        except ValueError:
            continue
    return entries


def _tcp_listening(host: str, port: int) -> bool:
    """有 LISTEN（0A）状态且端口匹配的套接字。

    绑在 0.0.0.0 上的监听同样能通过 127.0.0.1 访问（mihomo 两种绑定方式都见过），
    所以通配地址也算数；只要求端口一致，避免把别的服务当成代理。
    """
    return any(lport == port and state == "0A" and lip in (host, "0.0.0.0")
               for lip, lport, _rip, _rport, state in _tcp_entries())


def _tcp_established_from(local_port: int, peer_ip: str) -> bool:
    """本地端口有来自 peer_ip 的 ESTABLISHED 连接。"""
    return any(lport == local_port and rip == peer_ip and state == "01"
               for _lip, lport, rip, _rport, state in _tcp_entries())


def _tailscale_backend_state() -> Optional[str]:
    """`tailscale status --json` 的 BackendState；CLI 不可用或未运行时为 None。"""
    import json
    import subprocess

    try:
        proc = subprocess.run(
            ["tailscale", "status", "--json"],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    try:
        data = json.loads(proc.stdout)
    except ValueError:
        return None
    state = data.get("BackendState")
    return state if isinstance(state, str) else None


def _check(check_id: str, label: str, state: str, detail: str) -> dict[str, str]:
    return {"id": check_id, "label": label, "state": state, "detail": detail}


def _read_checks_cloud(
    *,
    docker_ttl: float = _DOCKER_CACHE_TTL,
    tailscale_service: str = "tailscaled",
    newapi_port: int = 3000,
) -> list[dict[str, str]]:
    """云服务器版五项任务状态：1Panel / OpenResty / Docker / Tailscale / New API。

    和泰山派同一套协议：顺序固定、每项独立 try/except、始终返回 5 条。
    """
    checks: list[dict[str, str]] = []
    rows = None
    try:
        rows = read_docker_snapshot(docker_ttl)
    except Exception:  # noqa: BLE001
        rows = None

    def container_state(keyword: str) -> str | None:
        """按容器名关键字找状态：'up' 视为在跑，healthy 另外标注。"""
        if not rows:
            return None
        for name, status in rows:
            if keyword.lower() in name.lower():
                return (status or "").lower()
        return None

    # 1) 1Panel：systemd 单元或同名容器任一在跑（本机是容器化部署）
    try:
        unit_state = read_services(("1panel",)).get("1panel", "unknown")
        cstate = container_state("1panel")
        if unit_state == "active" or (cstate is not None and "up" in cstate):
            checks.append(_check("1panel", "1Panel", "ok", "在线"))
        else:
            checks.append(_check("1panel", "1Panel", "down", "未运行"))
    except Exception:  # noqa: BLE001
        checks.append(_check("1panel", "1Panel", "down", "未检测到"))

    # 2) OpenResty：单元或容器任一在跑
    try:
        unit_state = read_services(("openresty",)).get("openresty", "unknown")
        cstate = container_state("openresty")
        if unit_state == "active" or (cstate is not None and "up" in cstate):
            checks.append(_check("openresty", "OpenResty", "ok", "在线"))
        else:
            checks.append(_check("openresty", "OpenResty", "down", "未运行"))
    except Exception:  # noqa: BLE001
        checks.append(_check("openresty", "OpenResty", "down", "未检测到"))

    # 3) Docker：守护进程在跑，且 docker ps 读得到
    try:
        unit_state = read_services(("docker",)).get("docker", "unknown")
        if unit_state == "active" and rows is not None:
            checks.append(_check("docker", "Docker", "ok", "在线"))
        elif unit_state == "active":
            checks.append(_check("docker", "Docker", "warn", "未就绪"))
        else:
            checks.append(_check("docker", "Docker", "down", "未运行"))
    except Exception:  # noqa: BLE001
        checks.append(_check("docker", "Docker", "down", "未检测到"))

    # 4) Tailscale：tailscaled 在跑，且 BackendState == Running
    try:
        unit_state = read_services((tailscale_service,)).get(tailscale_service, "unknown")
        backend = _tailscale_backend_state() if unit_state == "active" else None
        if unit_state == "active" and backend == "Running":
            checks.append(_check("tailscale", "Tailscale", "ok", "在线"))
        elif unit_state == "active":
            checks.append(_check("tailscale", "Tailscale", "warn", "未连接"))
        else:
            checks.append(_check("tailscale", "Tailscale", "down", "未运行"))
    except Exception:  # noqa: BLE001
        checks.append(_check("tailscale", "Tailscale", "down", "未检测到"))

    # 5) New API：容器在跑 + 3000 端口在监听
    try:
        cstate = container_state("new-api")
        listening = _tcp_listening("127.0.0.1", newapi_port)
        if cstate is not None and "up" in cstate and listening:
            checks.append(_check("newapi", "New API", "ok", "在线"))
        elif cstate is not None and "up" in cstate:
            checks.append(_check("newapi", "New API", "warn", "未监听"))
        else:
            checks.append(_check("newapi", "New API", "down", "未运行"))
    except Exception:  # noqa: BLE001
        checks.append(_check("newapi", "New API", "down", "未检测到"))

    return checks


def read_checks(
    *,
    profile: str = "taishan",
    docker_ttl: float = _DOCKER_CACHE_TTL,
    xianyu_container: str = "ydisks-xianyu-helper-app-1",
    proxy_process: str = "mihomo",
    proxy_host: str = "127.0.0.1",
    proxy_port: int = 7890,
    mqtt_port: int = 1883,
    esp32_ip: str = "192.168.0.100",
    tailscale_service: str = "tailscaled",
    camera_service: str = "rkaiq_3A",
) -> list[dict[str, str]]:
    """五项任务状态，顺序固定（屏幕按顺序显示）。

    每项都包在 try/except 里：单项异常只影响自己，返回的列表长度始终是 5。
    profile=cloud 时换成云服务器那一套检查项（见 _read_checks_cloud）。
    """
    if (profile or "").strip().lower() == "cloud":
        return _read_checks_cloud(docker_ttl=docker_ttl,
                                  tailscale_service=tailscale_service)

    checks: list[dict[str, str]] = []

    # 1) 闲鱼助手：容器在跑，且有 healthy 健康检查结果
    try:
        rows = read_docker_snapshot(docker_ttl)
        status = next((s for n, s in rows or [] if n == xianyu_container), None)
        if rows is None:
            checks.append(_check("xianyu", "闲鱼助手", "down", "未检测到"))
        elif status is None:
            checks.append(_check("xianyu", "闲鱼助手", "down", "未运行"))
        else:
            lowered = status.lower()
            if "health: starting" in lowered:
                checks.append(_check("xianyu", "闲鱼助手", "warn", "运行中"))
            elif "unhealthy" in lowered:
                checks.append(_check("xianyu", "闲鱼助手", "warn", "不健康"))
            elif "healthy" in lowered:
                checks.append(_check("xianyu", "闲鱼助手", "ok", "在线"))
            else:
                # 没配 healthcheck：容器活着，但协议要求「运行且健康」，
                # 无法证明健康就降一档，不能说成在线。
                checks.append(_check("xianyu", "闲鱼助手", "warn", "运行中"))
    except Exception:  # noqa: BLE001 - 单项失败不能拖垮整帧
        checks.append(_check("xianyu", "闲鱼助手", "down", "未检测到"))

    # 2) 网络代理：mihomo 进程存在 + 7890 正在监听
    try:
        running = _process_exists(proxy_process)
        listening = _tcp_listening(proxy_host, proxy_port)
        if running and listening:
            checks.append(_check("proxy", "网络代理", "ok", "在线"))
        elif running:
            checks.append(_check("proxy", "网络代理", "warn", "未监听"))
        else:
            checks.append(_check("proxy", "网络代理", "down", "未运行"))
    except Exception:  # noqa: BLE001
        checks.append(_check("proxy", "网络代理", "down", "未检测到"))

    # 3) ESP32 通信：1883 上有来自终端 IP 的 ESTABLISHED 连接
    try:
        if _tcp_established_from(mqtt_port, esp32_ip):
            checks.append(_check("esp32", "ESP32通信", "ok", "在线"))
        else:
            checks.append(_check("esp32", "ESP32通信", "down", "未连接"))
    except Exception:  # noqa: BLE001
        checks.append(_check("esp32", "ESP32通信", "down", "未连接"))

    # 4) Tailscale：tailscaled 在跑，且 BackendState == Running
    try:
        unit_state = read_services((tailscale_service,)).get(tailscale_service, "unknown")
        backend = _tailscale_backend_state() if unit_state == "active" else None
        if unit_state == "active" and backend == "Running":
            checks.append(_check("tailscale", "Tailscale", "ok", "在线"))
        elif unit_state == "active":
            checks.append(_check("tailscale", "Tailscale", "warn", "未连接"))
        else:
            checks.append(_check("tailscale", "Tailscale", "down", "未运行"))
    except Exception:  # noqa: BLE001
        checks.append(_check("tailscale", "Tailscale", "down", "未检测到"))

    # 5) 摄像头：/dev/video* 存在 + rkaiq_3A 服务在跑
    try:
        import glob

        has_dev = bool(glob.glob("/dev/video*"))
        unit_state = read_services((camera_service,)).get(camera_service, "unknown")
        if has_dev and unit_state == "active":
            checks.append(_check("camera", "摄像头", "ok", "在线"))
        elif not has_dev:
            checks.append(_check("camera", "摄像头", "down", "未检测到"))
        else:
            checks.append(_check("camera", "摄像头", "warn", "未运行"))
    except Exception:  # noqa: BLE001
        checks.append(_check("camera", "摄像头", "down", "未检测到"))

    return checks


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
