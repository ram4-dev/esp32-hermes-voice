#ifndef VOICE_CLIENT_H
#define VOICE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VOICE_REMOTE_QUEUED,
    VOICE_REMOTE_TRANSCRIBING,
    VOICE_REMOTE_ASKING_HERMES,
    VOICE_REMOTE_SYNTHESIZING,
    VOICE_REMOTE_PLAYING,
} voice_remote_status_t;

typedef void (*voice_status_cb_t)(voice_remote_status_t status, void *context);
typedef bool (*voice_cancel_cb_t)(void *context);

typedef struct {
    char transcript[2048];
    char answer[4096];
    char error[256];
    char audio_error[256];
    bool truncated;
} voice_client_result_t;

int voice_client_run(
    const uint8_t *wav,
    size_t wav_bytes,
    const char *request_id,
    voice_status_cb_t status_callback,
    void *status_context,
    voice_cancel_cb_t cancel_callback,
    void *cancel_context,
    voice_client_result_t *result);

int voice_client_play_speech(
    const char *request_id,
    voice_status_cb_t status_callback,
    void *status_context,
    voice_cancel_cb_t cancel_callback,
    void *cancel_context,
    char *error,
    size_t error_capacity);

#endif /* VOICE_CLIENT_H */
