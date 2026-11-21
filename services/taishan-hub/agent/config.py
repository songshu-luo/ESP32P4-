"""泰山派指标 Agent —— 配置加载。

所有可变项来自 /etc/taishan-hub/credentials.env 与 /etc/taishan-hub/agent.env，
不写死在代码里（用户规则：no hardcoded values）。
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field

DEFAULT_CREDENTIALS_PATH = "/etc/taishan-hub/credentials.env"
DEFAULT_AGENT_ENV_PATH = "/etc/taishan-hub/agent.env"


class ConfigError(RuntimeError):
    """配置缺失或非法。"""


def _read_env_file(path: str) -> dict[str, str]:
    """解析 KEY=VALUE 形式的 env 文件；忽略空行与 # 注释。"""
    values: dict[str, str] = {}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                values[key.strip()] = value.strip().strip('"').strip("'")
    except FileNotFoundError as exc:
        raise ConfigError(f"找不到配置文件：{path}") from exc
    return values


@dataclass(frozen=True)
class AgentConfig:
    device_id: str
    broker_host: str
    broker_port: int
    username: str
    password: str
    agent_version: str = "0.1.0"
    schema_version: int = 5  # v5：metrics 增加 checks[]（任务状态）

    # 采样/发布节奏（秒）。数值来自 protocol/topics-v1.md §4.2。
    cpu_interval: float = 2.0
    memory_interval: float = 2.0
    temperature_interval: float = 5.0
    disk_interval: float = 30.0
    services_interval: float = 5.0
    metrics_interval: float = 2.0
    status_interval: float = 5.0
    loop_tick: float = 0.5
    # v5 任务状态：5 s 刷新一次；内部 docker ps 另有 15 s 缓存
    checks_interval: float = 5.0
    docker_cache_ttl: float = 15.0
    # 任务状态检查项按设备类型分组：taishan（闲鱼/代理/ESP32/Tailscale/摄像头）
    # 或 cloud（1Panel/OpenResty/Docker/Tailscale/New API）
    checks_profile: str = "taishan"

    metrics_valid_for: int = 8
    status_valid_for: int = 15

    service_names: tuple[str, ...] = field(default_factory=lambda: ("mosquitto",))
    disk_path: str = "/"
    # 需要上报的挂载点（屏幕磁盘卡逐行显示）；留空则只报 disk_path 一个
    disk_paths: tuple[str, ...] = field(default_factory=lambda: ("/",))

    @property
    def status_topic(self) -> str:
        return f"hub/v1/devices/{self.device_id}/status"

    @property
    def metrics_topic(self) -> str:
        return f"hub/v1/devices/{self.device_id}/metrics"


def load_config(
    credentials_path: str = DEFAULT_CREDENTIALS_PATH,
    agent_env_path: str = DEFAULT_AGENT_ENV_PATH,
) -> AgentConfig:
    """合并两个 env 文件；agent.env 优先（便于单独调参而不动凭据）。"""
    values = _read_env_file(credentials_path)
    values.update(_read_env_file(agent_env_path))

    def required(key: str) -> str:
        value = values.get(key, "")
        if not value:
            raise ConfigError(f"配置项缺失：{key}（检查 {credentials_path} / {agent_env_path}）")
        return value

    services_raw = values.get("HUB_SERVICES", "mosquitto")
    services = tuple(s.strip() for s in services_raw.split(",") if s.strip())

    disk_paths_raw = values.get("HUB_DISK_PATHS", "")
    disk_paths = tuple(s.strip() for s in disk_paths_raw.split(",") if s.strip())
    disk_path = values.get("HUB_DISK_PATH", "/")

    return AgentConfig(
        device_id=values.get("HUB_DEVICE_ID", "taishan-3m"),
        broker_host=values.get("MOSQUITTO_HOST", "127.0.0.1"),
        broker_port=int(values.get("MOSQUITTO_PORT", "1883")),
        username=values.get("HUB_AGENT_USER") or required("TAISHAN_AGENT_USER"),
        password=values.get("HUB_AGENT_PASS") or required("TAISHAN_AGENT_PASS"),
        service_names=services or ("mosquitto",),
        disk_path=disk_path,
        disk_paths=disk_paths or (disk_path,),
        checks_profile=values.get("HUB_CHECKS_PROFILE", "taishan").strip() or "taishan",
    )


def env_flag(name: str, default: bool = False) -> bool:
    """读一个布尔环境变量（systemd 里用 Environment= 传）。"""
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")
