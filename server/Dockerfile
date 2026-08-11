FROM python:3.12-slim AS runtime

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    VOICE_DATA_DIR=/data

RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*

RUN addgroup --system bridge && adduser --system --ingroup bridge bridge
WORKDIR /app

COPY pyproject.toml ./
COPY voice_bridge ./voice_bridge
RUN pip install --no-cache-dir --root-user-action=ignore .

RUN mkdir -p /data/audio /data/speech && chown -R bridge:bridge /data
USER bridge

EXPOSE 8787
CMD ["uvicorn", "voice_bridge.main:create_app", "--factory", "--host", "0.0.0.0", "--port", "8787", "--proxy-headers"]
