from __future__ import annotations

import sqlite3
import threading
from datetime import UTC, datetime
from pathlib import Path

from .models import JobRecord, JobStatus, TTSStatus


def _now() -> str:
    return datetime.now(UTC).isoformat()


class Storage:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self._connection = sqlite3.connect(path, check_same_thread=False)
        self._connection.row_factory = sqlite3.Row
        self._lock = threading.RLock()
        with self._lock, self._connection:
            self._connection.executescript(
                """
                PRAGMA journal_mode = WAL;
                CREATE TABLE IF NOT EXISTS sessions (
                    device_id TEXT PRIMARY KEY,
                    hermes_session_id TEXT NOT NULL,
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS jobs (
                    request_id TEXT PRIMARY KEY,
                    device_id TEXT NOT NULL,
                    status TEXT NOT NULL,
                    audio_path TEXT,
                    transcript TEXT,
                    answer TEXT,
                    speech_path TEXT,
                    tts_status TEXT NOT NULL DEFAULT 'pending',
                    tts_error TEXT,
                    error TEXT,
                    hermes_session_id TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS jobs_device_id_idx ON jobs(device_id);
                """
            )
            self._add_column_if_missing(
                "jobs", "speech_path", "ALTER TABLE jobs ADD COLUMN speech_path TEXT"
            )
            self._add_column_if_missing(
                "jobs",
                "tts_status",
                "ALTER TABLE jobs ADD COLUMN tts_status TEXT NOT NULL DEFAULT 'pending'",
            )
            self._add_column_if_missing(
                "jobs", "tts_error", "ALTER TABLE jobs ADD COLUMN tts_error TEXT"
            )

    def _add_column_if_missing(self, table: str, column: str, statement: str) -> None:
        columns = {
            str(row["name"])
            for row in self._connection.execute(f"PRAGMA table_info({table})").fetchall()
        }
        if column not in columns:
            self._connection.execute(statement)

    def close(self) -> None:
        with self._lock:
            self._connection.close()

    def health(self) -> bool:
        with self._lock:
            return self._connection.execute("SELECT 1").fetchone()[0] == 1

    def get_or_create_session(self, device_id: str, session_id: str) -> str:
        with self._lock, self._connection:
            self._connection.execute(
                "INSERT OR IGNORE INTO sessions(device_id, hermes_session_id, created_at) "
                "VALUES (?, ?, ?)",
                (device_id, session_id, _now()),
            )
            row = self._connection.execute(
                "SELECT hermes_session_id FROM sessions WHERE device_id = ?", (device_id,)
            ).fetchone()
        return str(row["hermes_session_id"])

    def create_job(
        self,
        *,
        request_id: str,
        device_id: str,
        audio_path: str,
        hermes_session_id: str,
    ) -> tuple[JobRecord, bool]:
        timestamp = _now()
        with self._lock, self._connection:
            cursor = self._connection.execute(
                """
                INSERT OR IGNORE INTO jobs(
                    request_id, device_id, status, audio_path, hermes_session_id,
                    created_at, updated_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    request_id,
                    device_id,
                    JobStatus.QUEUED.value,
                    audio_path,
                    hermes_session_id,
                    timestamp,
                    timestamp,
                ),
            )
            created = cursor.rowcount == 1
            row = self._connection.execute(
                "SELECT * FROM jobs WHERE request_id = ?", (request_id,)
            ).fetchone()
        return self._to_record(row), created

    def get_job(self, request_id: str) -> JobRecord | None:
        with self._lock:
            row = self._connection.execute(
                "SELECT * FROM jobs WHERE request_id = ?", (request_id,)
            ).fetchone()
        return self._to_record(row) if row is not None else None

    def update_job(
        self,
        request_id: str,
        status: JobStatus,
        *,
        transcript: str | None = None,
        answer: str | None = None,
        speech_path: str | None = None,
        tts_status: TTSStatus | None = None,
        tts_error: str | None = None,
        error: str | None = None,
        clear_audio_path: bool = False,
        clear_speech_path: bool = False,
    ) -> None:
        assignments = ["status = ?", "updated_at = ?"]
        values: list[str | None] = [status.value, _now()]
        if transcript is not None:
            assignments.append("transcript = ?")
            values.append(transcript)
        if answer is not None:
            assignments.append("answer = ?")
            values.append(answer)
        if speech_path is not None:
            assignments.append("speech_path = ?")
            values.append(speech_path)
        if tts_status is not None:
            assignments.append("tts_status = ?")
            values.append(tts_status.value)
        if tts_error is not None:
            assignments.append("tts_error = ?")
            values.append(tts_error)
        if error is not None:
            assignments.append("error = ?")
            values.append(error)
        if clear_audio_path:
            assignments.append("audio_path = NULL")
        if clear_speech_path:
            assignments.append("speech_path = NULL")
        values.append(request_id)
        with self._lock, self._connection:
            self._connection.execute(
                f"UPDATE jobs SET {', '.join(assignments)} WHERE request_id = ?", values
            )

    def recover_interrupted(self) -> list[str]:
        active = (
            JobStatus.QUEUED.value,
            JobStatus.TRANSCRIBING.value,
            JobStatus.ASKING_HERMES.value,
            JobStatus.SYNTHESIZING.value,
        )
        with self._lock, self._connection:
            rows = self._connection.execute(
                f"SELECT audio_path, speech_path FROM jobs "
                f"WHERE status IN ({','.join('?' for _ in active)})",
                active,
            ).fetchall()
            self._connection.execute(
                f"""
                UPDATE jobs
                SET status = ?, error = ?, audio_path = NULL, speech_path = NULL,
                    tts_status = ?, updated_at = ?
                WHERE status IN ({','.join('?' for _ in active)})
                """,
                (
                    JobStatus.FAILED.value,
                    "bridge restarted while the job was running",
                    TTSStatus.FAILED.value,
                    _now(),
                    *active,
                ),
            )
        paths: list[str] = []
        for row in rows:
            paths.extend(
                str(path) for path in (row["audio_path"], row["speech_path"]) if path
            )
        return paths

    def expire_speech_before(self, cutoff: str) -> list[str]:
        with self._lock, self._connection:
            rows = self._connection.execute(
                "SELECT speech_path FROM jobs "
                "WHERE tts_status = ? AND updated_at < ? AND speech_path IS NOT NULL",
                (TTSStatus.READY.value, cutoff),
            ).fetchall()
            self._connection.execute(
                "UPDATE jobs SET speech_path = NULL, tts_status = ? "
                "WHERE tts_status = ? AND updated_at < ?",
                (TTSStatus.EXPIRED.value, TTSStatus.READY.value, cutoff),
            )
        return [str(row["speech_path"]) for row in rows]

    @staticmethod
    def _to_record(row: sqlite3.Row) -> JobRecord:
        return JobRecord(
            request_id=str(row["request_id"]),
            device_id=str(row["device_id"]),
            status=JobStatus(row["status"]),
            audio_path=row["audio_path"],
            transcript=row["transcript"],
            answer=row["answer"],
            speech_path=row["speech_path"],
            tts_status=TTSStatus(row["tts_status"] or TTSStatus.PENDING.value),
            tts_error=row["tts_error"],
            error=row["error"],
            hermes_session_id=str(row["hermes_session_id"]),
        )
