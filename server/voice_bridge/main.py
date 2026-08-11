from __future__ import annotations

import asyncio
import hashlib
import hmac
import re
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import Depends, FastAPI, Header, HTTPException, Request, Response, status

from .audio import InvalidAudio, inspect_wav
from .clients import HermesClient, STTClient
from .config import Settings
from .models import HealthView, JobAccepted, JobView
from .service import VoiceService
from .storage import Storage

REQUEST_ID_PATTERN = re.compile(r"^[A-Za-z0-9._-]{8,64}$")


def create_app(
    settings: Settings | None = None,
    *,
    stt_client: STTClient | None = None,
    hermes_client: HermesClient | None = None,
) -> FastAPI:
    resolved = settings or Settings.from_env()

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        resolved.audio_dir.mkdir(parents=True, exist_ok=True)
        storage = Storage(resolved.database_path)
        for stale_path in storage.recover_interrupted():
            Path(stale_path).unlink(missing_ok=True)
        stt = stt_client or STTClient(
            base_url=resolved.stt_base_url,
            api_key=resolved.stt_api_key,
            model=resolved.stt_model,
            language=resolved.stt_language,
            timeout=resolved.upstream_timeout_seconds,
        )
        hermes = hermes_client or HermesClient(
            base_url=resolved.hermes_base_url,
            api_key=resolved.hermes_api_key,
            model=resolved.hermes_model,
            timeout=resolved.upstream_timeout_seconds,
        )
        app.state.settings = resolved
        app.state.storage = storage
        app.state.service = VoiceService(storage=storage, stt=stt, hermes=hermes)
        try:
            yield
        finally:
            await app.state.service.shutdown()
            await stt.close()
            await hermes.close()
            storage.close()

    app = FastAPI(title="ESP32 Voice Bridge", version="0.1.0", lifespan=lifespan)

    async def authenticate(
        authorization: str = Header(default=""),
        x_device_id: str = Header(default=""),
    ) -> str:
        expected = resolved.device_tokens.get(x_device_id)
        prefix = "Bearer "
        supplied = authorization[len(prefix) :] if authorization.startswith(prefix) else ""
        if expected is None or not hmac.compare_digest(expected, supplied):
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="unauthorized")
        return x_device_id

    @app.post("/v1/voice/jobs", response_model=JobAccepted)
    async def create_voice_job(
        request: Request,
        response: Response,
        device_id: str = Depends(authenticate),
        x_request_id: str = Header(default=""),
    ) -> JobAccepted:
        if not REQUEST_ID_PATTERN.fullmatch(x_request_id):
            raise HTTPException(status_code=400, detail="invalid X-Request-Id")
        if request.headers.get("content-type", "").split(";", 1)[0] != "audio/wav":
            raise HTTPException(status_code=415, detail="Content-Type must be audio/wav")

        existing = app.state.storage.get_job(x_request_id)
        if existing is not None:
            if existing.device_id != device_id:
                raise HTTPException(status_code=409, detail="request id belongs to another device")
            response.status_code = status.HTTP_200_OK
            return JobAccepted(request_id=existing.request_id, status=existing.status)

        content_length = request.headers.get("content-length")
        if content_length:
            try:
                declared_length = int(content_length)
            except ValueError as exc:
                raise HTTPException(status_code=400, detail="invalid Content-Length") from exc
            if declared_length < 0:
                raise HTTPException(status_code=400, detail="invalid Content-Length")
            if declared_length > resolved.max_audio_bytes:
                raise HTTPException(status_code=413, detail="audio is too large")
        wav = await request.body()
        if len(wav) > resolved.max_audio_bytes:
            raise HTTPException(status_code=413, detail="audio is too large")
        try:
            inspect_wav(wav, max_seconds=resolved.max_audio_seconds)
        except InvalidAudio as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc

        device_key = hashlib.sha256(device_id.encode("utf-8")).hexdigest()[:16]
        audio_path = resolved.audio_dir / f"{device_key}-{x_request_id}.wav"
        await asyncio.to_thread(audio_path.write_bytes, wav)
        session_id = app.state.service.session_for(device_id)
        job, created = app.state.storage.create_job(
            request_id=x_request_id,
            device_id=device_id,
            audio_path=str(audio_path),
            hermes_session_id=session_id,
        )
        if not created:
            await asyncio.to_thread(audio_path.unlink, missing_ok=True)
            response.status_code = status.HTTP_200_OK
            return JobAccepted(request_id=job.request_id, status=job.status)
        app.state.service.start(job)
        response.status_code = status.HTTP_202_ACCEPTED
        return JobAccepted(request_id=job.request_id, status=job.status)

    @app.get("/v1/voice/jobs/{request_id}", response_model=JobView)
    async def get_voice_job(
        request_id: str, device_id: str = Depends(authenticate)
    ) -> JobView:
        job = app.state.storage.get_job(request_id)
        if job is None or job.device_id != device_id:
            raise HTTPException(status_code=404, detail="job not found")
        answer = job.answer
        truncated = bool(answer and len(answer) > resolved.display_answer_chars)
        if answer:
            answer = answer[: resolved.display_answer_chars]
        return JobView(
            request_id=job.request_id,
            status=job.status,
            transcript=job.transcript,
            answer=answer,
            session_id=job.hermes_session_id,
            truncated=truncated,
            error=job.error,
        )

    @app.get("/health", response_model=HealthView)
    async def health() -> HealthView:
        database_ok = app.state.storage.health()
        return HealthView(
            status="ok" if database_ok else "degraded",
            database="ok" if database_ok else "error",
            stt="configured",
            hermes="configured",
        )

    @app.get("/health/deep", response_model=HealthView)
    async def deep_health(response: Response) -> HealthView:
        database_ok, stt_ok, hermes_ok = await asyncio.gather(
            asyncio.to_thread(app.state.storage.health),
            app.state.service.stt.health(),
            app.state.service.hermes.health(),
        )
        all_ok = database_ok and stt_ok and hermes_ok
        if not all_ok:
            response.status_code = status.HTTP_503_SERVICE_UNAVAILABLE
        return HealthView(
            status="ok" if all_ok else "degraded",
            database="ok" if database_ok else "error",
            stt="ok" if stt_ok else "error",
            hermes="ok" if hermes_ok else "error",
        )

    return app
