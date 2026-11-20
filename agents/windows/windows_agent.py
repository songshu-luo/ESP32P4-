"""Windows 指标 Agent —— 主循环。

与泰山派 Agent（services/taishan-hub/agent/agent.py）同一套协议、同一套信封，
只是采集端换成了 Win32 API。协议见 protocol/topics-v1.md：

  - 每 2 s 发布 hub/v1/devices/windows-main/metrics（不 retained）
  - 每 5 s 发布 hub/v1/devices/windows-main/status（retained）
  - LWT：异常断开时 Broker 代发 state=offline
  - 正常退出时显式补发一条 offline（systemd/任务计划程序停止属于正常断开，
    Broker 不会触发 LWT）

为什么没有 systemd：Windows 上用「任务计划程序」的开机启动项，
所以日志写到 %ProgramData%\\taishan-hub\\logs\\，不用 journald。
"""

from __future__ import annotations

import json
import logging
import logging.handlers
import signal
import socket
import sys
import time
from datetime import datetime, timezone
from typing import Any, Optional

try:
    import paho.mqtt.client as mqtt
except ImportError:  # pragma: no cover
    sys.stderr.write("缺少 paho-mqtt。请执行：pip install paho-mqtt\n")
    raise

from config import PROGRAMDATA, AgentConfig, ConfigError, load_config
from metrics import (
    CpuSample,
    clock_synced,
    cpu_usage_percent,
    derive_procnet,
    local_ip_addresses,
    read_core_count,
    read_cpu_sample,
    read_disk,
    read_disks,
    read_gpu,
    read_memory,
    read_physical_cores,
    read_services,
    read_temperatures,
    read_uptime_seconds,
    sample_procnet,
)

LOG = logging.getLogger("windows-agent")

# paho 1.x / 2.x 回调签名不同；两台机器上装的版本可能不一样。
_PAHO_V2 = hasattr(mqtt, "CallbackAPIVersion")

LOG_DIR = PROGRAMDATA / "taishan-hub" / "logs"
LOG_FILE = LOG_DIR / "windows-agent.log"


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class MetricCache:
    """按各自节奏刷新的指标缓存，发布时合并成一帧。

    CPU 使用率必须靠两次采样的差值，所以第一次 refresh 之后
    第二个 tick 才有数——这期间 usage_percent 是 None，屏幕显示 "--"。
    """

    def __init__(self, cfg: AgentConfig) -> None:
        self._cfg = cfg
        self._last: dict[str, float] = {}
        self._cpu_prev: Optional[CpuSample] = None
        # 物理核数不会变，开机读一次就够；读不到时退化成 None，屏幕上只显示线程数
        self._physical_cores: Optional[int] = read_physical_cores()
        # 进程 CPU 与网速都是累计值差分，必须留着上一份原始快照
        self._procnet_prev: Optional[dict[str, Any]] = None
        self._cache: dict[str, Any] = {
            "cpu": {"usage_percent": None, "load1": None, "load5": None,
                    "load15": None, "cores": read_core_count(),
                    "physical_cores": self._physical_cores},
            "memory": {},
            "temperature": {},
            "disk": {},
            "services": {},
            # GPU 每 5 s 刷一次；没有独显时这里会一直是全 null 的占位
            "gpu": {},
            # 进程 / 网速（10 s）、分区列表（30 s）
            "processes": [],
            "process_count": None,
            "network": {"rx_bytes_per_sec": None, "tx_bytes_per_sec": None},
            "disks": [],
        }

    def _due(self, key: str, interval: float, now: float) -> bool:
        if now - self._last.get(key, -1e9) >= interval:
            self._last[key] = now
            return True
        return False

    def refresh(self, now: float) -> None:
        cfg = self._cfg

        if self._due("cpu", cfg.cpu_interval, now):
            cur = read_cpu_sample()
            usage = cpu_usage_percent(self._cpu_prev, cur)
            self._cache["cpu"] = {
                "usage_percent": usage,
                # Windows 没有 loadavg；协议允许字段为 null，不能拿 0 冒充
                "load1": None, "load5": None, "load15": None,
                "cores": read_core_count(),
                "physical_cores": self._physical_cores,
            }
            self._cpu_prev = cur

        if self._due("memory", cfg.memory_interval, now):
            self._cache["memory"] = read_memory()

        if self._due("temperature", cfg.temperature_interval, now):
            self._cache["temperature"] = read_temperatures()

        if self._due("disk", cfg.disk_interval, now):
            self._cache["disk"] = read_disk(cfg.disk_path)

        if self._due("services", cfg.services_interval, now):
            services = read_services(cfg.service_names)
            # 一个都没查到时给空对象；屏幕会显示「未上报」，不会显示 0 个在跑
            self._cache["services"] = services

        if self._due("gpu", cfg.gpu_interval, now):
            self._cache["gpu"] = read_gpu()

        if self._due("procnet", cfg.procnet_interval, now):
            sample = sample_procnet()
            derived = derive_procnet(
                self._procnet_prev, sample, read_core_count() or 1
            )
            self._cache["processes"] = derived["processes"]
            self._cache["process_count"] = derived["process_count"]
            self._cache["network"] = derived["network"]
            self._procnet_prev = sample

        if self._due("disks", cfg.disks_interval, now):
            self._cache["disks"] = read_disks()

    def snapshot(self) -> dict[str, Any]:
        payload = dict(self._cache)
        payload["uptime_seconds"] = read_uptime_seconds()
        return payload

    def prime_cpu(self) -> None:
        """先采一次基线，否则第一条 metrics 的 usage_percent 永远是 None。"""
        self._cpu_prev = read_cpu_sample()


class Agent:
    def __init__(self, cfg: AgentConfig) -> None:
        self._cfg = cfg
        self._running = True
        self._seq = 0
        self._cache = MetricCache(cfg)
        self._client = self._build_client()

    # ---------- MQTT ----------

    def _build_client(self) -> mqtt.Client:
        cfg = self._cfg
        # client_id 固定，避免每次重连在 broker 上留一堆 session
        if _PAHO_V2:
            client = mqtt.Client(
                callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
                client_id=f"{cfg.device_id}-agent",
                protocol=mqtt.MQTTv311,
            )
        else:
            client = mqtt.Client(
                client_id=f"{cfg.device_id}-agent",
                protocol=mqtt.MQTTv311,
            )
        client.username_pw_set(cfg.username, cfg.password)

        lwt = self._envelope(valid_for=cfg.status_valid_for, payload={
            "state": "offline",
            "agent_version": cfg.agent_version,
            "hostname": _hostname(),
            "ips": [],
            "uptime_seconds": 0,
            "clock_synced": False,
        })
        client.will_set(cfg.status_topic, json.dumps(lwt), qos=1, retain=True)

        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect
        return client

    def _on_connect(self, client, userdata, flags, reason_code, properties=None) -> None:
        rc = getattr(reason_code, "value", reason_code)
        if rc != 0:
            LOG.error("broker 拒绝连接：rc=%s（检查用户名密码与 ACL）", rc)
            return
        LOG.info("已连接 %s:%s", self._cfg.broker_host, self._cfg.broker_port)
        self._publish_status()

    def _on_disconnect(self, client, userdata, *args) -> None:
        LOG.warning("与 broker 断开，paho 会自动重连")

    # ---------- 消息构造 ----------

    def _envelope(self, *, valid_for: int, payload: dict[str, Any]) -> dict[str, Any]:
        self._seq += 1
        return {
            "schema_version": self._cfg.schema_version,
            "device_id": self._cfg.device_id,
            "timestamp_utc": utc_now_iso(),
            "message_id": f"{self._cfg.device_id}-{self._seq}",
            "valid_for_seconds": valid_for,
            "payload": payload,
        }

    def _publish(self, topic: str, envelope: dict[str, Any], *, retain: bool) -> None:
        info = self._client.publish(topic, json.dumps(envelope), qos=1, retain=retain)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            LOG.warning("发布失败 topic=%s rc=%s", topic, info.rc)

    def _publish_status(self) -> None:
        cfg = self._cfg
        self._publish(cfg.status_topic, self._envelope(
            valid_for=cfg.status_valid_for,
            payload={
                "state": "online",
                "agent_version": cfg.agent_version,
                "hostname": _hostname(),
                "ips": local_ip_addresses(cfg.broker_host),
                "uptime_seconds": read_uptime_seconds(),
                "clock_synced": clock_synced(),
            },
        ), retain=True)

    def _publish_metrics(self) -> None:
        cfg = self._cfg
        self._publish(cfg.metrics_topic, self._envelope(
            valid_for=cfg.metrics_valid_for,
            payload=self._cache.snapshot(),
        ), retain=False)

    # ---------- 主循环 ----------

    def run(self) -> int:
        cfg = self._cfg
        now = time.monotonic()
        self._cache.prime_cpu()
        self._cache.refresh(now)

        self._client.connect_async(cfg.broker_host, cfg.broker_port, keepalive=30)
        self._client.loop_start()

        next_metrics = now
        next_status = now
        while self._running:
            now = time.monotonic()
            self._cache.refresh(now)

            if self._client.is_connected():
                if now >= next_metrics:
                    self._publish_metrics()
                    next_metrics = now + cfg.metrics_interval
                if now >= next_status:
                    self._publish_status()
                    next_status = now + cfg.status_interval
            time.sleep(cfg.loop_tick)

        self._publish_offline()
        self._client.loop_stop()
        self._client.disconnect()
        return 0

    def stop(self, *_args) -> None:
        LOG.info("收到退出信号，准备断开")
        self._running = False

    def _publish_offline(self) -> None:
        cfg = self._cfg
        if not self._client.is_connected():
            return
        info = self._client.publish(cfg.status_topic, json.dumps(self._envelope(
            valid_for=cfg.status_valid_for,
            payload={
                "state": "offline",
                "agent_version": cfg.agent_version,
                "hostname": _hostname(),
                "ips": [],
                "uptime_seconds": 0,
                "clock_synced": False,
            },
        )), qos=1, retain=True)
        try:
            try:
                info.wait_for_publish(timeout=3)
            except TypeError:  # paho 1.6.x 不接受 timeout
                info.wait_for_publish()
        except (RuntimeError, ValueError):
            LOG.warning("offline 消息未能确认送达，交给 LWT 与有效期兜底")


def _hostname() -> str:
    try:
        return socket.gethostname()
    except OSError:
        return "unknown"


def _setup_logging() -> None:
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
    LOG.setLevel(logging.INFO)

    console = logging.StreamHandler(sys.stdout)
    console.setFormatter(fmt)
    LOG.addHandler(console)

    # 任务计划程序启动时没有控制台，日志只落在文件里。
    # 1 MB × 3 个文件，够查最近几天的断线。
    try:
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        fh = logging.handlers.RotatingFileHandler(
            LOG_FILE, maxBytes=1024 * 1024, backupCount=3, encoding="utf-8"
        )
        fh.setFormatter(fmt)
        LOG.addHandler(fh)
    except OSError as exc:
        LOG.warning("无法写日志文件 %s：%s（只输出到控制台）", LOG_FILE, exc)


def main() -> int:
    _setup_logging()
    try:
        cfg = load_config()
    except ConfigError as exc:
        LOG.error("%s", exc)
        return 2

    LOG.info("启动：device=%s broker=%s:%s python=%s",
             cfg.device_id, cfg.broker_host, cfg.broker_port,
             ".".join(map(str, sys.version_info[:3])))

    agent = Agent(cfg)
    # Ctrl+C 与「停止任务」都走同一条退出路径，保证 offline 发得出去
    signal.signal(signal.SIGINT, agent.stop)
    signal.signal(signal.SIGTERM, agent.stop)
    return agent.run()


if __name__ == "__main__":
    sys.exit(main())