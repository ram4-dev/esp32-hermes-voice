#include "audio_player.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"

static const char *TAG = "audio_player";
static esp_codec_dev_handle_t s_speaker;
static volatile bool s_playing;
static volatile bool s_abort;
static bool s_codec_open;

esp_err_t audio_player_init(void)
{
    s_speaker = bsp_audio_codec_speaker_init();
    if (s_speaker == NULL) {
        ESP_LOGE(TAG, "speaker codec initialization failed");
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(s_speaker, AUDIO_PLAYER_VOLUME) != ESP_OK) {
        ESP_LOGE(TAG, "could not set speaker volume");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t audio_player_stream_begin(void)
{
    if (s_speaker == NULL || s_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_abort) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_PLAYER_SAMPLE_RATE,
        .channel = AUDIO_PLAYER_CHANNELS,
        .bits_per_sample = AUDIO_PLAYER_BITS_PER_SAMPLE,
    };
    esp_err_t result = esp_codec_dev_open(s_speaker, &format);
    if (result != ESP_OK) {
        return result;
    }
    s_codec_open = true;
    s_playing = true;
    return ESP_OK;
}

esp_err_t audio_player_stream_write(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_abort) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_codec_open || !s_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = esp_codec_dev_write(s_speaker, (void *)data, (int)length);
    return result == ESP_OK ? ESP_OK : result;
}

esp_err_t audio_player_stream_finish(void)
{
    esp_err_t result = ESP_OK;
    if (s_codec_open) {
        result = esp_codec_dev_close(s_speaker);
        s_codec_open = false;
    }
    s_playing = false;
    s_abort = false;
    return result;
}

void audio_player_stop(void)
{
    /* The network callback observes this flag and closes the codec on its task. */
    s_abort = s_playing || s_codec_open;
}

bool audio_player_is_playing(void)
{
    return s_playing;
}
