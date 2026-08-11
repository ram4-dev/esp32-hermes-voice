# ESP32 Voice Agent

Push-to-talk voice interface for the Waveshare
`ESP32-S3-Touch-AMOLED-2.06`. The device records a WAV file, sends it to a
local service in Proxmox, and displays the answer returned by Hermes Agent.

```text
ESP32 touch + microphones
        │  HTTPS, audio/wav
        ▼
Voice bridge on Proxmox
        ├── OpenAI-compatible speech-to-text API
        ├── Hermes Agent API with a persistent device session
        └── NaN Builders Kokoro TTS API (optional audio response)
```

The ESP32 never receives the STT, Hermes, or Kokoro credentials. Input audio is
deleted from the bridge after processing. Normalized speech is kept for 15 minutes
and then removed, while job metadata, transcripts, answers, and the Hermes session
ID remain in SQLite.

## Repository layout

- `firmware/`: ESP-IDF 5.5 firmware using Waveshare's official board support
  package, LVGL, ES7210 microphone input, PSRAM recording, ES8311 playback,
  streaming WAV decoding, WAV encoding, and the bridge client.
- `server/`: FastAPI bridge, SQLite state, STT/Hermes clients, Docker Compose,
  Caddy TLS, and tests.

## 1. Prepare Hermes Agent

Enable the OpenAI-compatible API server in Hermes' environment:

```dotenv
API_SERVER_ENABLED=true
API_SERVER_HOST=0.0.0.0
API_SERVER_PORT=8642
API_SERVER_KEY=replace-with-a-long-random-value
```

Start Hermes with `hermes gateway` and verify `GET /health`. Restrict port 8642
to the bridge host or container network; it should not be exposed publicly.

## 2. Deploy the bridge on Proxmox

Run this inside a Proxmox VM or unprivileged LXC that can reach Hermes and the
STT endpoint. Docker inside an LXC requires nesting support; a small Debian VM
is the simplest default.

```bash
cd server
cp .env.example .env
openssl rand -hex 32
```

Put the generated value in both:

- `.env` as the token in `VOICE_DEVICE_TOKENS`.
- Firmware menuconfig as `Device bearer token`.

Set the real STT URL, STT model, STT key, Hermes URL, Hermes key, and NaN
Builders TTS settings in `.env`, then start the services:

```bash
docker compose up -d --build
docker compose ps
curl --resolve voice.local:443:PROXMOX_VM_IP https://voice.local/health
```

Create a local DNS entry so `voice.local` resolves to the VM/LXC address from
the Wi-Fi network used by the ESP32. A static DHCP lease is recommended.

### Trust Caddy's local CA

Caddy creates a private CA for `voice.local`. Export its public root
certificate after the first startup:

```bash
docker compose cp \
  caddy:/data/caddy/pki/authorities/local/root.crt \
  ../firmware/main/certs/server_root_ca.pem
```

This certificate is public material, not a private key. Never copy
`root.key` into the firmware or repository.

## 3. Build and flash the firmware

Install ESP-IDF 5.5.x and activate its shell, then:

```bash
cd firmware
idf.py set-target esp32s3
idf.py menuconfig
```

Under **ESP32 Voice Agent**, configure:

- Wi-Fi SSID and password.
- `https://voice.local` as the bridge base URL.
- The device ID from `VOICE_DEVICE_TOKENS`.
- The matching device token.
- **Embed a private/local root CA**.
- Speaker volume (ES8311): `65` by default.

`sdkconfig` and the real CA certificate are intentionally ignored by Git.

Build, flash, and inspect the serial log:

```bash
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

The board may need a long press on `PWR` to start. If flashing does not begin,
power-cycle it while holding `BOOT`.

## Device flow

1. Hold the circular control to record.
2. Release it to create and upload a 16 kHz, mono, 16-bit PCM WAV.
3. The screen advances through upload, transcription, Hermes, and
   **Sintetizando** states.
4. The transcript and Hermes answer appear in a scrollable view while the
   normalized WAV is downloaded in streaming chunks to the ES8311 speaker.
5. **Reproduciendo** keeps the text visible. Pressing the record control stops
   playback (barge-in) and starts a new recording.
6. If Kokoro or audio download fails, the text answer remains available and
   **REINTENTAR** retries only the authenticated speech download. If upload
   fails, the original WAV remains in PSRAM and the same idempotency key is
   retried.

Recordings stop automatically after 30 seconds. Only one pending recording is
kept; starting a new recording replaces a failed pending one. A reboot clears a
pending recording because audio is never written to flash.

## Bridge API

### `POST /v1/voice/jobs`

Headers:

```text
Authorization: Bearer <device-token>
X-Device-Id: esp32-voice-01
X-Request-Id: <8-64 safe characters>
Content-Type: audio/wav
```

The body is the WAV. A new request returns `202`; an existing idempotency key
returns its current state with `200`.

### `GET /v1/voice/jobs/{request_id}`

Returns one of `queued`, `transcribing`, `asking_hermes`, `synthesizing`,
`completed`, or `failed`. Completed jobs include `transcript`, `answer`,
`session_id`, `truncated`, and `tts_status` (`ready`, `failed`, or `expired`).
A ready job includes `speech_url`.

### `GET /v1/voice/jobs/{request_id}/speech`

The same device bearer token authenticates this endpoint. It streams the
normalized mono 16 kHz, 16-bit PCM WAV. Audio expires after 15 minutes and
returns `410`; a job without usable speech returns `409` while its text answer
remains valid.

### Health

- `GET /health`: process and SQLite readiness.
- `GET /health/deep`: also probes STT, Hermes, and Kokoro.

TTS is deliberately a best-effort stage: a Kokoro failure never discards the
Hermes text answer.

## Development and tests

```bash
cd server
uv sync --extra test
uv run ruff check voice_bridge tests
uv run pytest -q
docker build -t esp32-voice-bridge:local .
```

Firmware CI uses the reproducible ESP-IDF 5.5.4 container:

```bash
docker run --rm -e HOME=/tmp/idf-home \
  -v "$PWD/../firmware:/project" -w /project \
  espressif/idf:v5.5.4 bash -lc 'idf.py reconfigure && idf.py build'
```

The tests cover WAV validation and normalization, Kokoro's request contract,
authentication, the complete STT-to-Hermes-to-TTS pipeline, text fallback,
speech retention and expiration, SQLite migration, audio deletion, idempotent
retries, persistent sessions, and health checks.

## Security notes

- Do not commit `.env`, `sdkconfig`, the local CA copy, or any API key.
- Use separate device and Hermes tokens.
- Keep the bridge and Hermes ports private to the local network.
- Back up SQLite only if retaining transcripts and answers is desired.
- Embeddings are intentionally not part of the device protocol. They can be
  added later as a server-side stage when a concrete semantic store is chosen.
