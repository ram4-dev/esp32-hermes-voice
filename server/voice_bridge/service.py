from __future__ import annotations

import asyncio
import hashlib
import logging
from pathlib import Path

from .clients import HermesClient, STTClient
from .models import JobRecord, JobStatus
from .storage import Storage

LOGGER = logging.getLogger(__name__)


class VoiceService:
    def __init__(
        self,
        *,
        storage: Storage,
        stt: STTClient,
        hermes: HermesClient,
    ):
        self.storage = storage
        self.stt = stt
        self.hermes = hermes
        self._tasks: set[asyncio.Task[None]] = set()

    def session_for(self, device_id: str) -> str:
        digest = hashlib.sha256(device_id.encode("utf-8")).hexdigest()[:24]
        return self.storage.get_or_create_session(device_id, f"voice-{digest}")

    def start(self, job: JobRecord) -> None:
        task = asyncio.create_task(self._process(job), name=f"voice-job-{job.request_id}")
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def shutdown(self) -> None:
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
            self.storage.update_job(
                job.request_id, JobStatus.ASKING_HERMES, transcript=transcript
            )
            answer = await self.hermes.ask(transcript, job.hermes_session_id)
            self.storage.update_job(
                job.request_id,
                JobStatus.COMPLETED,
                transcript=transcript,
                answer=answer,
                clear_audio_path=True,
            )
        except Exception as exc:  # boundary: persist a safe failure for the device
            LOGGER.exception("voice job %s failed", job.request_id)
            self.storage.update_job(
                job.request_id,
                JobStatus.FAILED,
                error=str(exc) or exc.__class__.__name__,
                clear_audio_path=True,
            )
        finally:
            try:
                await asyncio.to_thread(audio_path.unlink, missing_ok=True)
            except OSError:
                LOGGER.exception("could not delete temporary audio for %s", job.request_id)
