from __future__ import annotations

import io
import time
import wave
from pathlib import Path

from fastapi.testclient import TestClient

from voice_bridge.config import Settings
from voice_bridge.main import create_app


class FakeSTT:
    def __init__(self):
        self.calls: list[bytes] = []

    async def transcribe(self, wav: bytes) -> str:
        self.calls.append(wav)
        return "¿Cuál fue mi pregunta anterior?"

    async def health(self) -> bool:
        return True

    async def close(self) -> None:
        pass


class FakeHermes:
    def __init__(self):
        self.calls: list[tuple[str, str]] = []

    async def ask(self, transcript: str, session_id: str) -> str:
        self.calls.append((transcript, session_id))
        return "Esta es una respuesta breve de Hermes."

    async def health(self) -> bool:
        return True

    async def close(self) -> None:
        pass


def make_wav(seconds: float = 0.05, sample_rate: int = 16_000) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(b"\x00\x00" * int(seconds * sample_rate))
    return output.getvalue()


def settings(tmp_path: Path) -> Settings:
    return Settings(
        database_path=tmp_path / "bridge.sqlite3",
        audio_dir=tmp_path / "audio",
        device_tokens={"esp32-voice-01": "test-token"},
        stt_base_url="https://stt.invalid",
        stt_api_key="stt-secret",
        stt_model="whisper-1",
        stt_language="es",
        hermes_base_url="http://hermes.invalid:8642",
        hermes_api_key="hermes-secret",
        hermes_model="hermes-agent",
    )


def headers(request_id: str = "request-12345678") -> dict[str, str]:
    return {
        "Authorization": "Bearer test-token",
        "X-Device-Id": "esp32-voice-01",
        "X-Request-Id": request_id,
        "Content-Type": "audio/wav",
    }


def wait_for_completion(client: TestClient, request_id: str) -> dict:
    auth = {key: value for key, value in headers(request_id).items() if key != "Content-Type"}
    for _ in range(100):
        response = client.get(f"/v1/voice/jobs/{request_id}", headers=auth)
        assert response.status_code == 200
        body = response.json()
        if body["status"] in {"completed", "failed"}:
            return body
        time.sleep(0.01)
    raise AssertionError("job did not complete")


def test_voice_pipeline_and_audio_cleanup(tmp_path: Path) -> None:
    stt = FakeSTT()
    hermes = FakeHermes()
    app = create_app(settings(tmp_path), stt_client=stt, hermes_client=hermes)
    with TestClient(app) as client:
        response = client.post(
            "/v1/voice/jobs", headers=headers(), content=make_wav()
        )
        assert response.status_code == 202
        completed = wait_for_completion(client, "request-12345678")

        assert completed["status"] == "completed"
        assert completed["transcript"] == "¿Cuál fue mi pregunta anterior?"
        assert completed["answer"] == "Esta es una respuesta breve de Hermes."
        assert completed["session_id"].startswith("voice-")
        assert len(stt.calls) == 1
        assert hermes.calls == [
            ("¿Cuál fue mi pregunta anterior?", completed["session_id"])
        ]
        assert list((tmp_path / "audio").glob("*.wav")) == []


def test_request_id_is_idempotent_and_session_persists(tmp_path: Path) -> None:
    stt = FakeSTT()
    hermes = FakeHermes()
    app = create_app(settings(tmp_path), stt_client=stt, hermes_client=hermes)
    with TestClient(app) as client:
        first = client.post(
            "/v1/voice/jobs", headers=headers("first-request-01"), content=make_wav()
        )
        assert first.status_code == 202
        first_job = wait_for_completion(client, "first-request-01")

        duplicate = client.post(
            "/v1/voice/jobs", headers=headers("first-request-01"), content=make_wav()
        )
        assert duplicate.status_code == 200
        assert duplicate.json()["status"] == "completed"

        second = client.post(
            "/v1/voice/jobs", headers=headers("second-request-02"), content=make_wav()
        )
        assert second.status_code == 202
        second_job = wait_for_completion(client, "second-request-02")

        assert len(stt.calls) == 2
        assert first_job["session_id"] == second_job["session_id"]
        assert hermes.calls[0][1] == hermes.calls[1][1]


def test_rejects_bad_auth_and_invalid_audio(tmp_path: Path) -> None:
    app = create_app(settings(tmp_path), stt_client=FakeSTT(), hermes_client=FakeHermes())
    with TestClient(app) as client:
        unauthorized = client.post(
            "/v1/voice/jobs",
            headers={**headers(), "Authorization": "Bearer wrong"},
            content=make_wav(),
        )
        assert unauthorized.status_code == 401

        invalid = client.post(
            "/v1/voice/jobs", headers=headers("invalid-audio-01"), content=b"not a wav"
        )
        assert invalid.status_code == 422


def test_rejects_wrong_audio_shape_and_oversize(tmp_path: Path) -> None:
    app = create_app(settings(tmp_path), stt_client=FakeSTT(), hermes_client=FakeHermes())
    with TestClient(app) as client:
        wrong_rate = client.post(
            "/v1/voice/jobs",
            headers=headers("wrong-rate-0001"),
            content=make_wav(sample_rate=8_000),
        )
        assert wrong_rate.status_code == 422

    base_settings = settings(tmp_path / "oversize")
    configured = Settings(
        **{
            **{
                field: getattr(base_settings, field)
                for field in base_settings.__dataclass_fields__
            },
            "max_audio_bytes": 100,
        }
    )
    app = create_app(configured, stt_client=FakeSTT(), hermes_client=FakeHermes())
    with TestClient(app) as client:
        oversized = client.post(
            "/v1/voice/jobs",
            headers=headers("oversized-00001"),
            content=make_wav(seconds=1),
        )
        assert oversized.status_code == 413


def test_health_endpoints(tmp_path: Path) -> None:
    app = create_app(settings(tmp_path), stt_client=FakeSTT(), hermes_client=FakeHermes())
    with TestClient(app) as client:
        assert client.get("/health").json()["status"] == "ok"
        assert client.get("/health/deep").json() == {
            "status": "ok",
            "database": "ok",
            "stt": "ok",
            "hermes": "ok",
        }
