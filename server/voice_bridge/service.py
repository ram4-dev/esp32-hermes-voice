from __future__ import annotations

import asyncio
import hashlib
import logging
import sqlite3
import time
from datetime import datetime, timedelta
from pathlib import Path

from .clients import HermesClient, STTClient, TTSClient, UpstreamError
from .models import JobRecord, JobStatus, TTSStatus
from .speech import InvalidSpeech, normalize_speech
from .storage import Storage

UTC = datetime.fromisoformat("1970-01-01T00:00:00+00:00").tzinfo
if UTC is None:  # pragma: no cover - datetime.fromisoformat always returns tzinfo here
    raise RuntimeError("UTC timezone is unavailable")

LOGGER = logging.getLogger(__name__)
_TTS_ERRORS = (InvalidSpeech, OSError, RuntimeError, UpstreamError, ValueError)
_JOB_ERRORS = _TTS_ERRORS + (sqlite3.Error,)


class VoiceService:
    def __init__(
        self,
        *,
        storage: Storage,
        stt: STTClient,
        hermes: HermesClient,
        tts: TTSClient,
        speech_dir: Path,
        max_tts_seconds: int,
        speech_retention_seconds: int,
    ):
        self.storage = storage
        self.stt = stt
        self.hermes = hermes
        self.tts = tts
        self.speech_dir = speech_dir
        self.max_tts_seconds = max_tts_seconds
        self.speech_retention_seconds = speech_retention_seconds
        self._tasks: set[asyncio.Task[None]] = set()
        self._cleanup_task: asyncio.Task[None] | None = None

    def session_for(self, device_id: str) -> str:
        digest = hashlib.sha256(device_id.encode("utf-8")).hexdigest()[:24]
        return self.storage.get_or_create_session(device_id, f"voice-{digest}")

    def start(self, job: JobRecord) -> None:
        task = asyncio.create_task(self._process(job), name=f"voice-job-{job.request_id}")
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    def start_cleanup(self) -> None:
        self.cleanup_expired_speech()
        self._cleanup_task = asyncio.create_task(self._cleanup_loop(), name="speech-cleanup")

    def cleanup_expired_speech(self) -> None:
        cutoff = datetime.fromtimestamp(time.time(), UTC) - timedelta(
            seconds=self.speech_retention_seconds
        )
        for path in self.storage.expire_speech_before(cutoff.isoformat()):
            try:
                Path(path).unlink(missing_ok=True)
            except OSError:
                LOGGER.exception("could not delete expired speech file %s", path)

    async def _cleanup_loop(self) -> None:
        try:
            while True:
                await asyncio.sleep(min(300, max(30, self.speech_retention_seconds // 2)))
                await asyncio.to_thread(self.cleanup_expired_speech)
        except asyncio.CancelledError as cancellation:
            del cancellation

    async def shutdown(self) -> None:
        if self._cleanup_task is not None:
            self._cleanup_task.cancel()
            await asyncio.gather(self._cleanup_task, return_exceptions=True)
        if self._tasks:
            for task in self._tasks:
                task.cancel()
            await asyncio.gather(*self._tasks, return_exceptions=True)

    async def _process(self, job: JobRecord) -> None:
        audio_path = Path(job.audio_path or "")
        try:
            self.storage.update_job(job.request_id, JobStatus.TRANSCRIBING)
            wav = await asyncio.to_thread(audio_path.read_bytes)
            transcript = await self.stt.transcribe(wav)
            self.storage.update_job(job.request_id, JobStatus.ASKING_HERMES, transcript=transcript)
            answer = await self.hermes.ask(transcript, job.hermes_session_id)
            self.storage.update_job(job.request_id, JobStatus.SYNTHESIZING, answer=answer)
            device_digest = hashlib.sha256(job.device_id.encode("utf-8")).hexdigest()[:16]
            speech_path = self.speech_dir / f"{device_digest}-{job.request_id}.wav"
            try:
                source_audio = await self.tts.synthesize(answer)
                await asyncio.to_thread(
                    normalize_speech,
                    source_audio,
                    speech_path,
                    max_seconds=self.max_tts_seconds,
                )
                self.storage.update_job(
                    job.request_id,
                    JobStatus.COMPLETED,
                    speech_path=str(speech_path),
                    tts_status=TTSStatus.READY,
                    clear_audio_path=True,
                )
            except _TTS_ERRORS as exc:
                LOGGER.exception("speech synthesis failed for %s", job.request_id)
                try:
                    speech_path.unlink(missing_ok=True)
                except OSError:
                    LOGGER.exception("could not delete failed speech file %s", speech_path)
                self.storage.update_job(
                    job.request_id,
                    JobStatus.COMPLETED,
                    tts_status=TTSStatus.FAILED,
                    tts_error=str(exc) or exc.__class__.__name__,
                    clear_audio_path=True,
                    clear_speech_path=True,
                )
        except _JOB_ERRORS as exc:
            LOGGER.exception("voice job %s failed", job.request_id)
            self.storage.update_job(
                job.request_id,
                JobStatus.FAILED,
                error=str(exc) or exc.__class__.__name__,
                tts_status=TTSStatus.FAILED,
                clear_audio_path=True,
            )
        finally:
            try:
                await asyncio.to_thread(audio_path.unlink, missing_ok=True)
            except OSError:
                LOGGER.exception("could not delete temporary audio for %s", job.request_id)
