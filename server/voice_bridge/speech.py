from __future__ import annotations

import subprocess
import wave
from pathlib import Path


class InvalidSpeech(RuntimeError):
    pass


NORMALIZED_SAMPLE_RATE = 16_000
NORMALIZED_CHANNELS = 1
NORMALIZED_SAMPLE_WIDTH = 2
FRAME_CHUNK = 16_384


def normalize_speech(
    source: bytes,
    destination: Path,
    *,
    max_seconds: int,
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    source_path = destination.with_suffix(".source")
    temporary_path = destination.with_suffix(".tmp.wav")
    canonical_path = destination.with_suffix(".canonical.wav")
    promoted = False
    source_path.write_bytes(source)
    try:
        process = subprocess.run(
            [
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-i",
                str(source_path),
                "-ac",
                str(NORMALIZED_CHANNELS),
                "-ar",
                str(NORMALIZED_SAMPLE_RATE),
                "-c:a",
                "pcm_s16le",
                str(temporary_path),
            ],
            capture_output=True,
            check=False,
            timeout=60,
        )
        if process.returncode != 0:
            raise InvalidSpeech("could not normalize synthesized audio")

        with wave.open(str(temporary_path), "rb") as input_wav:
            if (
                input_wav.getnchannels() != NORMALIZED_CHANNELS
                or input_wav.getsampwidth() != NORMALIZED_SAMPLE_WIDTH
                or input_wav.getframerate() != NORMALIZED_SAMPLE_RATE
                or input_wav.getcomptype() != "NONE"
            ):
                raise InvalidSpeech("normalized speech has an unsupported format")
            frame_count = input_wav.getnframes()
            max_frames = max_seconds * NORMALIZED_SAMPLE_RATE
            if frame_count <= 0 or frame_count > max_frames:
                raise InvalidSpeech("normalized speech has an unsupported duration")

            with wave.open(str(canonical_path), "wb") as output_wav:
                output_wav.setnchannels(NORMALIZED_CHANNELS)
                output_wav.setsampwidth(NORMALIZED_SAMPLE_WIDTH)
                output_wav.setframerate(NORMALIZED_SAMPLE_RATE)
                remaining = frame_count
                while remaining:
                    chunk_frames = min(FRAME_CHUNK, remaining)
                    frames = input_wav.readframes(chunk_frames)
                    if len(frames) != chunk_frames * NORMALIZED_CHANNELS * NORMALIZED_SAMPLE_WIDTH:
                        raise InvalidSpeech("normalized speech ended before its declared frames")
                    output_wav.writeframesraw(frames)
                    remaining -= chunk_frames

        canonical_path.replace(destination)
        promoted = True
    except (OSError, subprocess.SubprocessError, wave.Error) as exc:
        raise InvalidSpeech("could not normalize synthesized audio") from exc
    finally:
        source_path.unlink(missing_ok=True)
        temporary_path.unlink(missing_ok=True)
        if not promoted:
            canonical_path.unlink(missing_ok=True)
