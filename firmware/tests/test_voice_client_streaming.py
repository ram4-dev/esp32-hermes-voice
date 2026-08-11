from __future__ import annotations

import struct

HEADER_SIZE = 44


def wav_header(data_size: int) -> bytes:
    return (
        b"RIFF"
        + struct.pack("<I", data_size + 36)
        + b"WAVEfmt "
        + struct.pack(
            "<IHHIIHH4sI",
            16,
            1,
            1,
            16000,
            32000,
            2,
            16,
            b"data",
            data_size,
        )
    )


def stream_wav(events: list[bytes]) -> tuple[bytes, bool]:
    header = bytearray()
    declared = None
    received = 0
    carry = bytearray()
    writes: list[bytes] = []
    excess = False

    for event in events:
        offset = 0
        if declared is None:
            needed = HEADER_SIZE - len(header)
            header.extend(event[:needed])
            offset = min(len(event), needed)
            if len(header) < HEADER_SIZE:
                continue
            declared = struct.unpack_from("<I", header, 40)[0]
            assert declared % 2 == 0

        payload = event[offset:]
        remaining = declared - received
        if len(payload) > remaining:
            payload = payload[:remaining]
            excess = True

        if carry:
            take = min(2 - len(carry), len(payload))
            carry.extend(payload[:take])
            payload = payload[take:]
            if len(carry) == 2:
                writes.append(bytes(carry))
                carry.clear()
        even = len(payload) & ~1
        if even:
            writes.append(payload[:even])
        if even < len(payload):
            carry.extend(payload[even:])
        received += len(event[offset:]) if not excess else len(payload)

    if declared is None or received != declared or carry:
        return b"".join(writes), True
    return b"".join(writes), excess


def test_odd_http_cuts_preserve_pcm_frames() -> None:
    payload = bytes(range(10))
    events = [
        wav_header(len(payload))[:7],
        wav_header(len(payload))[7:44] + payload[:1],
        payload[1:4],
        payload[4:7],
        payload[7:],
    ]

    output, rejected = stream_wav(events)

    assert not rejected
    assert output == payload


def test_trailing_bytes_are_rejected_without_playing_them() -> None:
    payload = b"\x10\x11\x12\x13"
    output, rejected = stream_wav([wav_header(len(payload)) + payload + b"TRAIL"])

    assert rejected
    assert output == payload


def test_truncated_payload_is_rejected() -> None:
    payload = b"\x20\x21\x22\x23"
    output, rejected = stream_wav([wav_header(len(payload)) + payload[:2]])

    assert rejected
    assert output == payload[:2]


def test_firmware_tracks_declared_bytes_and_mutable_codec_buffers() -> None:
    from pathlib import Path

    root = Path(__file__).parents[1]
    client = (root / "main" / "voice_client.c").read_text()
    player_header = (root / "main" / "audio_player.h").read_text()

    assert "declared_data_bytes" in client
    assert "data_received != download.declared_data_bytes" in client
    assert "write_aligned_pcm" in client
    assert "uint8_t *data" in player_header
