"""泰山派指标 Agent —— 主循环。

职责（protocol/topics-v1.md）：
  - 连接本地 Mosquitto（每设备独立账号，ACL 只能写自己的主题）
  - 设置 LWT：断线时 Broker 代发 state=offline
  - 每 2 s 发布 metrics，每 5 s 发布 status
  - 时间戳用 UTC ISO-8601，带 valid_for_seconds

设计约束：
  - 启动不阻塞：连不上 Broker 就退避重试，进程本身不退出
  - 不产生陈旧数据：采集失败时字段是 None，不补 0
  - 日志写 stdout，交给 systemd 的 journald 统一管理
"""

from __future__ import annotations

import json
import logging
import signal
import sys
import time
from datetime import datetime, timezone
from typing import Any, Optional

try:
    import paho.mqtt.client as mqtt
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "缺少 paho-mqtt。请安装：sudo apt install python3-paho-mqtt\n"
    )
    raise

from config import AgentConfig, ConfigError, load_config
from metrics import (
    clock_synced,
    cpu_usage_percent,
    local_ip_addresses,
    read_core_count,
    read_cpu_sample,
    read_disk,
    read_fan,
    read_loadavg,
    read_memory,
    read_services,
    read_temperatures,
    read_uptime_seconds,
)

LOG = logging.getLogger("taishan-agent")

# paho-mqtt 1.x 与 2.x 的回调 API 不同；apt 的 Ubuntu 24.04 包里是 1.6.1，
# 但开发机可能是 2.x。两边都支持，避免部署时再改代码。
_PAHO_V2 = hasattr(mqtt, "CallbackAPIVersion")


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class MetricCache:
    """按各自节奏更新的指标缓存，发布时合并成一帧。

    每个字段记录上次刷新时刻；循环 tick 只负责合并，采集节奏由 interval 决定。
    """

    def __init__(self, cfg: AgentConfig) -> None:
        self._cfg = cfg
        self._last: dict[str, float] = {}
        self._cpu_prev = None
        self._cpu_prev_at = 0.0
        self._cache: dict[str, Any] = {
            "cpu": {"usage_percent": None, "load1": None, "load5": None, "load15": None,
                    "cores": read_core_count()},
            "memory": {},
            "temperature": {},
            "disk": {},
            "services": {},
            "fan": {},
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
            load = read_loadavg()
            usage = cpu_usage_percent(self._cpu_prev, cur) if self._cpu_prev_at else None
            self._cache["cpu"] = {
                "usage_percent": usage,
                "load1": load["load1"],
                "load5": load["load5"],
                "load15": load["load15"],
                "cores": read_core_count(),
            }
            self._cpu_prev, self._cpu_prev_at = cur, now

        if self._due("memory", cfg.memory_interval, now):
            self._cache["memory"] = read_memory()

        if self._due("temperature", cfg.temperature_interval, now):
            self._cache["temperature"] = read_temperatures()

        if self._due("disk", cfg.disk_interval, now):
            self._cache["disk"] = read_disk(cfg.disk_path)

        if self._due("services", cfg.services_interval, now):
            self._cache["services"] = read_services(cfg.service_names)
            self._cache["fan"] = read_fan()

    def snapshot(self) -> dict[str, Any]:
        payload = dict(self._cache)
        payload["uptime_seconds"] = read_uptime_seconds()
        return payload

    def prime_cpu(self, now: float) -> None:
        """先采一次 CPU 基线，否则第一条消息使用率永远是 None。"""
        self._cpu_prev = read_cpu_sample()
        self._cpu_prev_at = now


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

        # LWT：Broker 发现连接异常断开时，代发这条离线消息（retained）。
        lwt = self._envelope(state="offline", valid_for=cfg.status_valid_for, payload={
            "state": "offline",
            "agent_version": cfg.agent_version,
            "hostname": self._hostname(),
            "ips": [],
            "uptime_seconds": 0,
            "clock_synced": False,
        })
        client.will_set(cfg.status_topic, json.dumps(lwt), qos=1, retain=True)

        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect
        return client

    @staticmethod
    def _hostname() -> str:
        import socket
        try:
            return socket.gethostname()
        except OSError:
            return "unknown"

    def _on_connect(self, client, userdata, flags, reason_code, properties=None) -> None:
        rc = getattr(reason_code, "value", reason_code)
        if rc != 0:
            LOG.error("broker 拒绝连接：rc=%s（检查用户名密码与 ACL）", rc)
            return
        LOG.info("已连接 %s:%s", self._cfg.broker_host, self._cfg.broker_port)
        # 连上立刻发一次 status，让订阅方不必等 5 s。
        self._publish_status()

    def _on_disconnect(self, client, userdata, *args) -> None:
        LOG.warning("与 broker 断开，paho 会自动重连")

    # ---------- 消息构造 ----------

    def _envelope(self, *, state: str, valid_for: int, payload: dict[str, Any]) -> dict[str, Any]:
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
        envelope = self._envelope(
            state="online",
            valid_for=cfg.status_valid_for,
            payload={
                "state": "online",
                "agent_version": cfg.agent_version,
                "hostname": self._hostname(),
                "ips": local_ip_addresses(),
                "uptime_seconds": read_uptime_seconds(),
                "clock_synced": clock_synced(),
            },
        )
        self._publish(cfg.status_topic, envelope, retain=True)

    def _publish_metrics(self) -> None:
        cfg = self._cfg
        envelope = self._envelope(
            state="online",
            valid_for=cfg.metrics_valid_for,
            payload=self._cache.snapshot(),
        )
        self._publish(cfg.metrics_topic, envelope, retain=False)

    # ---------- 主循环 ----------

    def run(self) -> int:
        cfg = self._cfg
        now = time.monotonic()
        self._cache.prime_cpu(now)
        self._cache.refresh(now)

        # 先起网络循环（后台线程），主线程只管节奏。
        self._client.connect_async(cfg.broker_host, cfg.broker_port, keepalive=30)
        self._client.loop_start()

        next_metrics = now
        next_status = now
        while self._running:
            now = time.monotonic()
            self._cache.refresh(now)

            if self._client.is_connected():
                # 发布节奏由 protocol/topics-v1.md §4.2 规定：
                # metrics 2 s，status 5 s。不能跟着 loop_tick 走。
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
        """主动关闭时显式发一条 offline。

        LWT 只在非正常断开时由 Broker 代发；systemd stop / 重启属于「正常断开」，
        Broker 不会补发离线消息，retained 的 status 会一直停在 online。
        订阅方虽然有 valid_for_seconds 兜底，但让状态立刻正确更好。
        """
        cfg = self._cfg
        if not self._client.is_connected():
            return
        envelope = self._envelope(
            state="offline",
            valid_for=cfg.status_valid_for,
            payload={
                "state": "offline",
                "agent_version": cfg.agent_version,
                "hostname": self._hostname(),
                "ips": [],
                "uptime_seconds": 0,
                "clock_synced": False,
            },
        )
        info = self._client.publish(cfg.status_topic, json.dumps(envelope), qos=1, retain=True)
        try:
            # loop_start() 的后台线程负责收发；这里只等这一帧确认送达。
            # paho 1.6.x 的 wait_for_publish() 不接受 timeout 参数，2.x 才接受。
            try:
                info.wait_for_publish(timeout=3)
            except TypeError:
                info.wait_for_publish()
        except (RuntimeError, ValueError):
            # 等不到就退化为「让 LWT/有效期兜底」，不阻塞关停。
            LOG.warning("offline 消息未能确认送达")


def main() -> int:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        cfg = load_config()
    except ConfigError as exc:
        LOG.error("%s", exc)
        return 2

    agent = Agent(cfg)
    signal.signal(signal.SIGTERM, agent.stop)
    signal.signal(signal.SIGINT, agent.stop)
    LOG.info("启动：device=%s broker=%s:%s",
             cfg.device_id, cfg.broker_host, cfg.broker_port)
    return agent.run()


if __name__ == "__main__":
    sys.exit(main())
