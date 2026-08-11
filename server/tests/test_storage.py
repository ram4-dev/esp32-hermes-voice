from __future__ import annotations

import sqlite3
from typing import cast

from voice_bridge.models import JobStatus, TTSStatus
from voice_bridge.storage import Storage


def test_storage_migrates_v010_jobs(tmp_path) -> None:
    database = tmp_path / "legacy.sqlite3"
    connection = sqlite3.connect(database)
    connection.executescript(
        """
        CREATE TABLE jobs (
            request_id TEXT PRIMARY KEY,
            device_id TEXT NOT NULL,
            status TEXT NOT NULL,
            audio_path TEXT,
            transcript TEXT,
            answer TEXT,
            error TEXT,
            hermes_session_id TEXT NOT NULL,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );
        INSERT INTO jobs VALUES (
            'legacy-0001', 'device', 'completed', NULL, 'hola', 'respuesta', NULL,
            'voice-session', '2025-01-01T00:00:00+00:00', '2025-01-01T00:00:00+00:00'
        );
        """
    )
    connection.commit()
    connection.close()

    storage = Storage(database)
    try:
        job = storage.get_job("legacy-0001")
        assert job is not None
        assert job.tts_status == TTSStatus.PENDING
        assert job.speech_path is None
    finally:
        storage.close()


def test_expire_speech_updates_state_and_returns_file(tmp_path) -> None:
    storage = Storage(tmp_path / "bridge.sqlite3")
    speech_path = tmp_path / "speech.wav"
    speech_path.write_bytes(b"RIFF")
    try:
        job, created = storage.create_job(
            request_id="expire-0001",
            device_id="device",
            audio_path=str(tmp_path / "input.wav"),
            hermes_session_id="voice-session",
        )
        assert created and job.status == JobStatus.QUEUED

        completed_status = cast(JobStatus, JobStatus.COMPLETED)
        ready_status = cast(TTSStatus, TTSStatus.READY)
        storage.update_job(
            job.request_id,
            completed_status,
            answer="answer",
            speech_path=str(speech_path),
            tts_status=ready_status,
        )
        with storage._connection:
            storage._connection.execute(
                "UPDATE jobs SET updated_at = ? WHERE request_id = ?",
                ("2020-01-01T00:00:00+00:00", job.request_id),
            )

        paths = storage.expire_speech_before("2021-01-01T00:00:00+00:00")
        assert paths == [str(speech_path)]
        expired = storage.get_job(job.request_id)
        assert expired is not None
        assert expired.tts_status == TTSStatus.EXPIRED
        assert expired.speech_path is None
    finally:
        storage.close()
