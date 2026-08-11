from __future__ import annotations

import io
import unittest
import wave

from voice_bridge.speech import InvalidSpeech, normalize_speech


def wav_bytes(*, channels: int, sample_rate: int, sample_width: int = 2) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(sample_width)
        wav.setframerate(sample_rate)
        frame_bytes = channels * sample_width
        wav.writeframes(b"\x00" * (frame_bytes * sample_rate // 10))
    return output.getvalue()


def test_normalize_speech_outputs_pcm_mono_16k(tmp_path) -> None:
    destination = tmp_path / "speech.wav"
    normalize_speech(wav_bytes(channels=2, sample_rate=24000), destination, max_seconds=10)

    raw = destination.read_bytes()
    assert raw[:4] == b"RIFF"
    assert raw[8:12] == b"WAVE"
    assert raw[36:40] == b"data"
    assert raw[40:44] == (len(raw) - 44).to_bytes(4, "little")
    assert len(raw[44:]) > 0

    with wave.open(str(destination), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getsampwidth() == 2
        assert wav.getframerate() == 16_000
        assert wav.getnframes() == len(raw[44:]) // 2


def test_normalize_speech_rejects_audio_longer_than_limit(tmp_path) -> None:
    destination = tmp_path / "speech.wav"
    with unittest.TestCase().assertRaisesRegex(InvalidSpeech, "duration"):
        normalize_speech(wav_bytes(channels=1, sample_rate=16000), destination, max_seconds=0)
    assert not destination.exists()
