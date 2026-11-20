"""Windows 指标 Agent —— 配置加载。

凭据不进仓库：从 %ProgramData%\\taishan-hub\\windows-agent.env 读，
由安装脚本把 Hub 的 credentials.env 里的 WINDOWS_AGENT_USER/PASS 搬进来。

查找顺序（后面的覆盖前面的）：
  1. 同目录 windows-agent.env        —— 开发调试用
  2. %ProgramData%\\taishan-hub\\windows-agent.env —— 正式部署用
  3. 真实环境变量                    —— 临时覆盖用
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

ENV_FILENAME = "windows-agent.env"

PROGRAMDATA = Path(os.environ.get("ProgramData", r"C:\ProgramData"))
DEPLOY_ENV_PATH = PROGRAMDATA / "taishan-hub" / ENV_FILENAME
DEV_ENV_PATH = Path(__file__).resolve().parent / ENV_FILENAME


class ConfigError(RuntimeError):
    """配置缺失或非法。"""


def _read_env_file(path: Path) -> dict[str, str]:
    """解析 KEY=VALUE；忽略空行与 # 注释。"""
    values: dict[str, str] = {}
    try:
        with open(path, "r", encoding="utf-8-sig") as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                values[key.strip()] = value.strip().strip('"').strip("'")
    except FileNotFoundError:
        return {}
    return values


@dataclass(frozen=True)
class AgentConfig:
    device_id: str
    broker_host: str
    broker_port: int
    username: str
    password: str
    agent_version: str = "0.1.0"
    schema_version: int = 3

    # 采样/发布节奏，与 protocol/topics-v1.md §4.2 一致
    cpu_interval: float = 2.0
    memory_interval: float = 2.0
    temperature_interval: float = 30.0
    disk_interval: float = 30.0
    services_interval: float = 10.0
    # nvidia-smi 每次要起一个进程（60~120 ms），比读 /proc 贵，所以放慢到 5 s
    gpu_interval: float = 5.0
    # 进程 CPU / 网速的累计值差分：一条 240 ms 的 Get-Process，10 s 一次足够
    procnet_interval: float = 10.0
    disks_interval: float = 30.0
    metrics_interval: float = 2.0
    status_interval: float = 5.0
    loop_tick: float = 0.5

    metrics_valid_for: int = 8
    status_valid_for: int = 15

    disk_path: str = "C:\\"
    service_names: tuple[str, ...] = ()

    @property
    def status_topic(self) -> str:
        return f"hub/v1/devices/{self.device_id}/status"

    @property
    def metrics_topic(self) -> str:
        return f"hub/v1/devices/{self.device_id}/metrics"


def _parse_services(raw: str) -> tuple[str, ...]:
    return tuple(s.strip() for s in raw.split(",") if s.strip())


# 允许用真实环境变量临时覆盖配置项（调试时不改文件）。
_OVERRIDABLE = (
    "WINDOWS_DEVICE_ID", "MOSQUITTO_HOST", "MOSQUITTO_PORT",
    "WINDOWS_AGENT_USER", "WINDOWS_AGENT_PASS",
    "WINDOWS_DISK_PATH", "WINDOWS_SERVICES",
)


def load_config(env_path: "Path | str | None" = None) -> AgentConfig:
    values: dict[str, str] = {}
    candidates = [DEV_ENV_PATH, DEPLOY_ENV_PATH]
    if env_path is not None:
        candidates = [Path(env_path)]
    for path in candidates:
        values.update(_read_env_file(path))
    for key in _OVERRIDABLE:
        if os.environ.get(key):
            values[key] = os.environ[key]

    def required(key: str) -> str:
        value = values.get(key, "")
        if not value:
            raise ConfigError(
                f"配置项缺失：{key}\n"
                f"  检查这一文件是否存在且包含该键：{candidates[-1]}"
            )
        return value

    try:
        port = int(values.get("MOSQUITTO_PORT", "1883"))
    except ValueError as exc:
        raise ConfigError(f"MOSQUITTO_PORT 不是整数：{values.get('MOSQUITTO_PORT')!r}") from exc

    return AgentConfig(
        device_id=values.get("WINDOWS_DEVICE_ID", "windows-main"),
        broker_host=required("MOSQUITTO_HOST"),
        broker_port=port,
        username=required("WINDOWS_AGENT_USER"),
        password=required("WINDOWS_AGENT_PASS"),
        disk_path=values.get("WINDOWS_DISK_PATH", "C:\\"),
        service_names=_parse_services(values.get("WINDOWS_SERVICES", "W32Time")),
    )