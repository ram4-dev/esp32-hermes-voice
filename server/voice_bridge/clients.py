from __future__ import annotations

import httpx


class UpstreamError(RuntimeError):
    pass


class STTClient:
    def __init__(
        self,
        *,
        base_url: str,
        api_key: str,
        model: str,
        language: str,
        timeout: float,
        transport: httpx.AsyncBaseTransport | None = None,
    ):
        self._base_url = base_url
        self._model = model
        self._language = language
        self._client = httpx.AsyncClient(
            headers={"Authorization": f"Bearer {api_key}"},
            timeout=timeout,
            transport=transport,
        )

    async def close(self) -> None:
        await self._client.aclose()

    async def transcribe(self, wav: bytes) -> str:
        data = {"model": self._model}
        if self._language:
            data["language"] = self._language
        try:
            response = await self._client.post(
                f"{self._base_url}/v1/audio/transcriptions",
                data=data,
                files={"file": ("recording.wav", wav, "audio/wav")},
            )
            response.raise_for_status()
            text = response.json().get("text", "").strip()
        except (httpx.HTTPError, ValueError, AttributeError) as exc:
            raise UpstreamError("speech-to-text request failed") from exc
        if not text:
            raise UpstreamError("speech-to-text returned an empty transcript")
        return text

    async def health(self) -> bool:
        try:
            response = await self._client.get(f"{self._base_url}/v1/models", timeout=10)
            return response.status_code < 500
        except httpx.HTTPError:
            return False


class HermesClient:
    SYSTEM_PROMPT = (
        "The user is speaking from a small ESP32 device and your answer will be read aloud. "
        "Answer in the same language as the user. Return a brief, self-contained spoken "
        "summary with the useful result first. Use plain text, short sentences, and no "
        "Markdown, bullet lists, or unnecessary URLs."
    )

    def __init__(
        self,
        *,
        base_url: str,
        api_key: str,
        model: str,
        timeout: float,
        transport: httpx.AsyncBaseTransport | None = None,
    ):
        self._base_url = base_url
        self._model = model
        self._client = httpx.AsyncClient(
            headers={"Authorization": f"Bearer {api_key}"},
            timeout=timeout,
            transport=transport,
        )

    async def close(self) -> None:
        await self._client.aclose()

    async def ask(self, transcript: str, session_id: str) -> str:
        payload = {
            "model": self._model,
            "stream": False,
            "messages": [
                {"role": "system", "content": self.SYSTEM_PROMPT},
                {"role": "user", "content": transcript},
            ],
        }
        try:
            response = await self._client.post(
                f"{self._base_url}/v1/chat/completions",
                headers={"X-Hermes-Session-Id": session_id},
                json=payload,
            )
            response.raise_for_status()
            answer = response.json()["choices"][0]["message"]["content"].strip()
        except (httpx.HTTPError, ValueError, KeyError, IndexError, TypeError) as exc:
            raise UpstreamError("Hermes request failed") from exc
        if not answer:
            raise UpstreamError("Hermes returned an empty answer")
        return answer

    async def health(self) -> bool:
        try:
            response = await self._client.get(f"{self._base_url}/health", timeout=10)
            return response.status_code == 200
        except httpx.HTTPError:
            return False


class TTSClient:
    def __init__(
        self,
        *,
        base_url: str,
        api_key: str,
        model: str,
        voice: str,
        speed: float,
        timeout: float,
        max_source_bytes: int,
        transport: httpx.AsyncBaseTransport | None = None,
    ):
        self._base_url = base_url
        self._model = model
        self._voice = voice
        self._speed = speed
        self._max_source_bytes = max_source_bytes
        self._client = httpx.AsyncClient(
            headers={"Authorization": f"Bearer {api_key}"},
            timeout=timeout,
            transport=transport,
        )

    async def close(self) -> None:
        await self._client.aclose()

    async def synthesize(self, text: str) -> bytes:
        payload = {
            "model": self._model,
            "voice": self._voice,
            "input": text,
            "speed": self._speed,
            "response_format": "wav",
        }
        try:
            response = await self._client.post(
                f"{self._base_url}/v1/audio/speech", json=payload
            )
            response.raise_for_status()
            audio = response.content
        except httpx.HTTPError as exc:
            raise UpstreamError("text-to-speech request failed") from exc
        if not audio:
            raise UpstreamError("text-to-speech returned empty audio")
        if len(audio) > self._max_source_bytes:
            raise UpstreamError("text-to-speech response is too large")
        return audio

    async def health(self) -> bool:
        try:
            response = await self._client.get(f"{self._base_url}/v1/models", timeout=10)
            return response.status_code < 500
        except httpx.HTTPError:
            return False
