from __future__ import annotations

import io
import wave
from dataclasses import dataclass


class InvalidAudio(ValueError):
    pass


@dataclass(frozen=True, slots=True)
class WavInfo:
    sample_rate: int
    channels: int
    sample_width: int
    frames: int
    duration_seconds: float


def inspect_wav(data: bytes, *, max_seconds: int) -> WavInfo:
    try:
        with wave.open(io.BytesIO(data), "rb") as wav:
            info = WavInfo(
                sample_rate=wav.getframerate(),
                channels=wav.getnchannels(),
                sample_width=wav.getsampwidth(),
                frames=wav.getnframes(),
                duration_seconds=wav.getnframes() / max(wav.getframerate(), 1),
            )
            compression = wav.getcomptype()
    except (EOFError, wave.Error) as exc:
        raise InvalidAudio("invalid WAV container") from exc

    if compression != "NONE":
        raise InvalidAudio("WAV must contain uncompressed PCM")
    if info.sample_rate != 16_000:
        raise InvalidAudio("WAV sample rate must be 16000 Hz")
    if info.channels != 1:
        raise InvalidAudio("WAV must be mono")
    if info.sample_width != 2:
        raise InvalidAudio("WAV samples must be 16-bit")
    if info.frames == 0:
        raise InvalidAudio("WAV contains no audio frames")
    if info.duration_seconds > max_seconds + 0.1:
        raise InvalidAudio(f"WAV duration exceeds {max_seconds} seconds")
    return info
