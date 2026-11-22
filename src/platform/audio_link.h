#ifndef HOP_AUDIO_LINK_H
#define HOP_AUDIO_LINK_H

#include "audio_protocol.h"

typedef struct hop_native_audio hop_native_audio;

/* Platform audio is owned by the audio worker for one authenticated connection. */
hop_native_audio *hop_native_audio_open(unsigned *directions);
int hop_native_audio_capture(hop_native_audio *audio, int16_t *pcm, uint32_t *rate);
int hop_native_audio_play(hop_native_audio *audio, const int16_t *pcm,
                          unsigned frames, uint32_t rate);
void hop_native_audio_close(hop_native_audio *audio);

typedef struct {
    unsigned requested;
    unsigned active;
    unsigned unavailable;
} hop_audio_status;

/* Set from the Windows tray or owning macOS agent. No UI-process audio state. */
int hop_audio_set_mode(const hop_config *cfg, unsigned mode);
hop_audio_status hop_audio_get_status(void);
void hop_audio_shutdown(void);

#endif
