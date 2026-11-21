#ifndef HOP_AUDIO_PROTOCOL_H
#define HOP_AUDIO_PROTOCOL_H

#include "hop.h"

#define HOP_AUDIO_PORT 24819
#define HOP_AUDIO_SEND 1u
#define HOP_AUDIO_PLAY 2u
#define HOP_AUDIO_MODES (HOP_AUDIO_SEND | HOP_AUDIO_PLAY)
#define HOP_AUDIO_HELLO_SIZE (6 + HOP_SESSION_BYTES + HOP_TOKEN_MAX)
#define HOP_AUDIO_HEADER_SIZE 8u
#define HOP_AUDIO_FRAMES 480u
#define HOP_AUDIO_FRAME_BYTES 4u /* stereo, signed 16-bit little-endian PCM */

void hop_audio_hello(uint8_t out[HOP_AUDIO_HELLO_SIZE], unsigned mode,
                     const char *token, const hop_session_id *session);
unsigned hop_audio_peer_mode(const uint8_t hello[HOP_AUDIO_HELLO_SIZE],
                             const char *token, const hop_session_id *session);
unsigned hop_audio_directions(unsigned local, unsigned peer);
void hop_audio_header(uint8_t out[HOP_AUDIO_HEADER_SIZE], uint32_t rate, unsigned frames);
int hop_audio_read_header(const uint8_t in[HOP_AUDIO_HEADER_SIZE],
                          uint32_t *rate, unsigned *frames);

#endif
