#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_PLAYER_SAMPLE_RATE 16000
#define AUDIO_PLAYER_CHANNELS 1
#define AUDIO_PLAYER_BITS_PER_SAMPLE 16

#ifdef CONFIG_VOICE_AUDIO_VOLUME
#define AUDIO_PLAYER_DEFAULT_VOLUME CONFIG_VOICE_AUDIO_VOLUME
#else
#define AUDIO_PLAYER_DEFAULT_VOLUME 65
#endif

esp_err_t audio_player_init(void);
esp_err_t audio_player_set_volume(uint8_t volume);
uint8_t audio_player_get_volume(void);
esp_err_t audio_player_set_audio_enabled(bool enabled);
bool audio_player_get_audio_enabled(void);
esp_err_t audio_player_stream_begin(void);
esp_err_t audio_player_stream_write(uint8_t *data, size_t length);
esp_err_t audio_player_stream_finish(void);
void audio_player_stop(void);
bool audio_player_is_playing(void);

#endif /* AUDIO_PLAYER_H */
