#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Return values use ESP-IDF's esp_err_t-compatible integer type. */

/* The bridge always serves normalized PCM WAV audio. */
#define AUDIO_PLAYER_SAMPLE_RATE 16000
#define AUDIO_PLAYER_CHANNELS 1
#define AUDIO_PLAYER_BITS_PER_SAMPLE 16
#ifdef CONFIG_VOICE_AUDIO_VOLUME
#define AUDIO_PLAYER_VOLUME CONFIG_VOICE_AUDIO_VOLUME
#else
#define AUDIO_PLAYER_VOLUME 65
#endif

int audio_player_init(void);
int audio_player_stream_begin(void);
int audio_player_stream_write(const uint8_t *data, size_t length);
int audio_player_stream_finish(void);
void audio_player_stop(void);
bool audio_player_is_playing(void);

#endif /* AUDIO_PLAYER_H */
