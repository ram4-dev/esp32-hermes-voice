from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).parents[1]
RECORDER = (ROOT / "main" / "audio_recorder.c").read_text()
PLAYER = (ROOT / "main" / "audio_player.c").read_text()


def _function(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def test_microphone_opens_only_from_recorder_start() -> None:
    init = _function(
        RECORDER,
        "esp_err_t audio_recorder_init(void)",
        "esp_err_t audio_recorder_start",
    )
    start = _function(
        RECORDER, "esp_err_t audio_recorder_start", "esp_err_t audio_recorder_pause"
    )

    assert "open_microphone_locked()" not in init
    assert "s_codec_open = false" in init
    assert "open_microphone_locked()" in start


def test_recorder_closes_before_each_completion_callback() -> None:
    task = _function(
        RECORDER, "static void recorder_task", "esp_err_t audio_recorder_init"
    )

    assert task.count("prepare_completion()") == 2
    assert task.count("s_callbacks.on_complete") == 2
    assert task.index("prepare_completion()") < task.index("s_callbacks.on_complete")
    assert task.rfind("prepare_completion()") < task.rfind("s_callbacks.on_complete")


def test_playback_never_reopens_the_microphone() -> None:
    assert "audio_recorder_resume" not in RECORDER
    assert "audio_recorder_resume" not in PLAYER
    finish = _function(
        PLAYER, "esp_err_t audio_player_stream_finish", "void audio_player_stop"
    )

    assert "audio_recorder_pause()" in finish
    assert "s_playing = false" in finish
