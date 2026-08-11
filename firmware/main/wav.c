#include "wav.h"

#include <string.h>

#include "esp_heap_caps.h"

static void write_le16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value & 0xff);
    target[1] = (uint8_t)((value >> 8) & 0xff);
}

static void write_le32(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value & 0xff);
    target[1] = (uint8_t)((value >> 8) & 0xff);
    target[2] = (uint8_t)((value >> 16) & 0xff);
    target[3] = (uint8_t)((value >> 24) & 0xff);
}

esp_err_t wav_create_pcm16_mono(
    const uint8_t *pcm,
    size_t pcm_bytes,
    uint32_t sample_rate,
    uint8_t **wav_out,
    size_t *wav_bytes_out)
{
    if (pcm == NULL || pcm_bytes == 0 || wav_out == NULL || wav_bytes_out == NULL ||
        pcm_bytes > UINT32_MAX - WAV_HEADER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t total = WAV_HEADER_SIZE + pcm_bytes;
    uint8_t *wav = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (wav == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(wav, "RIFF", 4);
    write_le32(wav + 4, (uint32_t)(total - 8));
    memcpy(wav + 8, "WAVEfmt ", 8);
    write_le32(wav + 16, 16);
    write_le16(wav + 20, 1);
    write_le16(wav + 22, 1);
    write_le32(wav + 24, sample_rate);
    write_le32(wav + 28, sample_rate * 2);
    write_le16(wav + 32, 2);
    write_le16(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    write_le32(wav + 40, (uint32_t)pcm_bytes);
    memcpy(wav + WAV_HEADER_SIZE, pcm, pcm_bytes);

    *wav_out = wav;
    *wav_bytes_out = total;
    return ESP_OK;
}
