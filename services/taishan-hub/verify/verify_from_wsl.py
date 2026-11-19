"""WSL / Windows 侧验收：从局域网订阅泰山派 Hub 的 MQTT 主题。

凭据不落盘：运行时通过 SSH 一次性读取到内存变量，进程退出即消失。
验收内容（protocol/topics-v1.md §3）：
  1. 能连上 192.168.0.156:1883 并鉴权成功
  2. status 为 online 且未过期
  3. metrics 携带真实 CPU/内存/温度/磁盘/服务
  4. 订阅 20 s，统计两条主题的到达次数与间隔

用法：
  WSL：      python3 verify_from_wsl.py
  Windows：  python verify_from_wsl.py     （自动借用 WSL 的 SSH 密钥）
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt

BROKER = "192.168.0.156"
PORT = 1883
SSH_TARGET = "lckfb@192.168.0.156"
SUB_SECONDS = 20

state: dict[str, list] = {"status": [], "metrics": []}
now_msgs: dict[str, dict] = {}


def _ssh_argv() -> list[str]:
    """Windows 上没有密钥，但 WSL 里有；自动选能用的那条路。"""
    if sys.platform == "win32" and shutil.which("wsl.exe"):
        return ["wsl.exe", "-d", "Ubuntu-24.04", "--", "ssh", "-o", "BatchMode=yes",
                SSH_TARGET, "cat /etc/taishan-hub/credentials.env"]
    return ["ssh", "-o", "BatchMode=yes", SSH_TARGET, "cat /etc/taishan-hub/credentials.env"]


def read_credentials() -> tuple[str, str]:
    proc = subprocess.run(
        _ssh_argv(), capture_output=True, timeout=25,
    )
    if proc.returncode != 0:
        raise SystemExit(
            "读取凭据失败（rc=%d）。WSL 里手动试一次：\n  ssh %s 'cat /etc/taishan-hub/credentials.env'\nstderr: %s"
            % (proc.returncode, SSH_TARGET, (proc.stderr or b"").decode("utf-8", "replace").strip())
        )
    raw = proc.stdout.decode("utf-8", "replace")
    values = {}
    for line in raw.splitlines():
        line = line.strip().lstrip("﻿")
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        values[key.strip()] = val.strip()
    return values["ESP32_USER"], values["ESP32_PASS"]


def on_connect(client, userdata, flags, reason_code, properties=None):
    rc = getattr(reason_code, "value", reason_code)
    print(f"[connect] rc={rc}")
    client.subscribe("hub/v1/devices/#")


def on_message(client, userdata, msg):
    topic = msg.topic
    kind = topic.rsplit("/", 1)[-1]
    if kind not in state:
        return
    now = time.monotonic()
    state[kind].append(now)
    now_msgs[kind] = json.loads(msg.payload)
    if len(state[kind]) == 1:
        print(f"[first {kind}] retained={msg.retain} topic={topic}")


def is_expired(doc: dict) -> bool:
    ts = datetime.strptime(doc["timestamp_utc"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    return (datetime.now(timezone.utc) - ts).total_seconds() > doc["valid_for_seconds"]


def main() -> int:
    # Windows 控制台默认 GBK，中文输出会炸；统一转 UTF-8。
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    username, password = read_credentials()

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="wsl-verify",
        protocol=mqtt.MQTTv311,
    )
    client.username_pw_set(username, password)
    client.on_connect = on_connect
    client.on_message = on_message

    print(f"[connect] {BROKER}:{PORT} as {username}")
    client.connect(BROKER, PORT, keepalive=20)
    client.loop_start()
    time.sleep(SUB_SECONDS)
    client.loop_stop()
    client.disconnect()

    ok = True
    for kind in ("status", "metrics"):
        stamps = state[kind]
        if not stamps:
            print(f"[FAIL] {kind}: 一条消息都没收到")
            ok = False
            continue
        gaps = [round(b - a, 2) for a, b in zip(stamps, stamps[1:])]
        print(f"[{kind}] 收到 {len(stamps)} 条，间隔={gaps}")
        if len(stamps) < 2:
            print(f"[FAIL] {kind}: 消息太少，无法判断节奏")
            ok = False

    doc = now_msgs.get("status")
    if doc:
        payload = doc["payload"]
        expired = is_expired(doc)
        print(f"[status] state={payload['state']} clock_synced={payload['clock_synced']} "
              f"ips={payload['ips']} ts={doc['timestamp_utc']} expired={expired}")
        if payload["state"] != "online":
            print("[FAIL] status 不是 online")
            ok = False
        if expired:
            print("[FAIL] status 已过期")
            ok = False

    doc = now_msgs.get("metrics")
    if doc:
        p = doc["payload"]
        vals = {
            "cpu%": p["cpu"]["usage_percent"],
            "mem%": p["memory"]["used_percent"],
            "soc°C": p["temperature"]["soc_celsius"],
            "disk%": p["disk"]["used_percent"],
            "mosquitto": p["services"]["mosquitto"],
        }
        print(f"[metrics] {vals} expired={is_expired(doc)}")
        for key, value in vals.items():
            if value is None:
                print(f"[FAIL] metrics.{key} 为空")
                ok = False

    print("\n结果：", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
