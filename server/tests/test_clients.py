from __future__ import annotations

import json

import httpx
import pytest

from voice_bridge.clients import TTSClient, UpstreamError


@pytest.mark.asyncio
async def test_nan_kokoro_request_contract() -> None:
    captured: dict[str, object] = {}

    def handler(request: httpx.Request) -> httpx.Response:
        captured["url"] = str(request.url)
        captured["authorization"] = request.headers.get("authorization")
        captured["payload"] = json.loads(request.content)
        return httpx.Response(200, content=b"RIFF-audio")

    client = TTSClient(
        base_url="https://api.nan.builders",
        api_key="test-key",
        model="kokoro",
        voice="ef_dora",
        speed=1.0,
        timeout=10,
        max_source_bytes=1024,
        transport=httpx.MockTransport(handler),
    )
    try:
        assert await client.synthesize("Hola") == b"RIFF-audio"
    finally:
        await client.close()

    assert captured == {
        "url": "https://api.nan.builders/v1/audio/speech",
        "authorization": "Bearer test-key",
        "payload": {
            "model": "kokoro",
            "voice": "ef_dora",
            "input": "Hola",
            "speed": 1.0,
            "response_format": "wav",
        },
    }


@pytest.mark.asyncio
async def test_tts_rejects_oversized_response() -> None:
    client = TTSClient(
        base_url="https://api.nan.builders",
        api_key="test-key",
        model="kokoro",
        voice="ef_dora",
        speed=1.0,
        timeout=10,
        max_source_bytes=4,
        transport=httpx.MockTransport(
            lambda request: httpx.Response(200, content=b"too large")
        ),
    )
    try:
        with pytest.raises(UpstreamError, match="too large"):
            await client.synthesize("Hola")
    finally:
        await client.close()
