#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WAV_HEADER_SIZE 44

esp_err_t wav_create_pcm16_mono(
    const uint8_t *pcm,
    size_t pcm_bytes,
    uint32_t sample_rate,
    uint8_t **wav_out,
    size_t *wav_bytes_out);
