from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum

from pydantic import BaseModel


class JobStatus(StrEnum):
    QUEUED = "queued"
    TRANSCRIBING = "transcribing"
    ASKING_HERMES = "asking_hermes"
    COMPLETED = "completed"
    FAILED = "failed"


TERMINAL_STATUSES = {JobStatus.COMPLETED, JobStatus.FAILED}


@dataclass(frozen=True, slots=True)
class JobRecord:
    request_id: str
    device_id: str
    status: JobStatus
    audio_path: str | None
    transcript: str | None
    answer: str | None
    error: str | None
    hermes_session_id: str


class JobAccepted(BaseModel):
    request_id: str
    status: JobStatus


class JobView(BaseModel):
    request_id: str
    status: JobStatus
    transcript: str | None = None
    answer: str | None = None
    session_id: str | None = None
    truncated: bool = False
    error: str | None = None


class HealthView(BaseModel):
    status: str
    database: str
    stt: str
    hermes: str
