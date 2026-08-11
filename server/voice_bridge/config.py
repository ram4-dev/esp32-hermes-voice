from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


def _required(name: str, env: dict[str, str]) -> str:
    value = env.get(name, "").strip()
    if not value:
        raise ValueError(f"{name} is required")
    return value


def _tokens(raw: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        device_id, separator, token = item.partition(":")
        if not separator or not device_id.strip() or not token.strip():
            raise ValueError(
                "VOICE_DEVICE_TOKENS must use device-id:token pairs separated by commas"
            )
        result[device_id.strip()] = token.strip()
    if not result:
        raise ValueError("VOICE_DEVICE_TOKENS must contain at least one device")
    return result


@dataclass(frozen=True, slots=True)
class Settings:
    database_path: Path
    audio_dir: Path
    device_tokens: dict[str, str]
    stt_base_url: str
    stt_api_key: str
    stt_model: str
    stt_language: str
    hermes_base_url: str
    hermes_api_key: str
    hermes_model: str
    max_audio_bytes: int = 1_100_000
    max_audio_seconds: int = 30
    display_answer_chars: int = 1200
    upstream_timeout_seconds: float = 180.0

    @classmethod
    def from_env(cls, source: dict[str, str] | None = None) -> Settings:
        env = dict(os.environ if source is None else source)
        data_dir = Path(env.get("VOICE_DATA_DIR", "./data")).resolve()
        return cls(
            database_path=Path(
                env.get("VOICE_DATABASE_PATH", str(data_dir / "voice_bridge.sqlite3"))
            ).resolve(),
            audio_dir=Path(env.get("VOICE_AUDIO_DIR", str(data_dir / "audio"))).resolve(),
            device_tokens=_tokens(_required("VOICE_DEVICE_TOKENS", env)),
            stt_base_url=_required("STT_BASE_URL", env).rstrip("/"),
            stt_api_key=_required("STT_API_KEY", env),
            stt_model=env.get("STT_MODEL", "whisper-1").strip() or "whisper-1",
            stt_language=env.get("STT_LANGUAGE", "es").strip(),
            hermes_base_url=_required("HERMES_BASE_URL", env).rstrip("/"),
            hermes_api_key=_required("HERMES_API_KEY", env),
            hermes_model=env.get("HERMES_MODEL", "hermes-agent").strip()
            or "hermes-agent",
            max_audio_bytes=int(env.get("VOICE_MAX_AUDIO_BYTES", "1100000")),
            max_audio_seconds=int(env.get("VOICE_MAX_AUDIO_SECONDS", "30")),
            display_answer_chars=int(env.get("VOICE_DISPLAY_ANSWER_CHARS", "1200")),
            upstream_timeout_seconds=float(
                env.get("VOICE_UPSTREAM_TIMEOUT_SECONDS", "180")
            ),
        )
