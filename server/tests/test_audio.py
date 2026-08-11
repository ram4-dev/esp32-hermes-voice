from __future__ import annotations

import pytest

from voice_bridge.audio import InvalidAudio, inspect_wav

from .test_app import make_wav


def test_inspect_wav_accepts_expected_format() -> None:
    info = inspect_wav(make_wav(seconds=0.25), max_seconds=30)
    assert info.sample_rate == 16_000
    assert info.channels == 1
    assert info.sample_width == 2
    assert info.duration_seconds == pytest.approx(0.25)


def test_inspect_wav_rejects_wrong_rate() -> None:
    with pytest.raises(InvalidAudio, match="16000"):
        inspect_wav(make_wav(sample_rate=8_000), max_seconds=30)
