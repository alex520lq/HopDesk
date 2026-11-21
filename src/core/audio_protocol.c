#include "audio_protocol.h"
#include <string.h>

static const uint8_t magic[] = {'H', 'D', 'A', 1};

void hop_audio_hello(uint8_t out[HOP_AUDIO_HELLO_SIZE], unsigned mode,
                     const char *token, const hop_session_id *session) {
    size_t len = strlen(token);
    memset(out, 0, HOP_AUDIO_HELLO_SIZE);
    memcpy(out, magic, sizeof(magic));
    out[4] = (uint8_t)mode;
    if (len >= HOP_TOKEN_MAX) return;
    out[5] = (uint8_t)len;
    memcpy(out + 6, session->bytes, HOP_SESSION_BYTES);
    memcpy(out + 6 + HOP_SESSION_BYTES, token, len);
}

unsigned hop_audio_peer_mode(const uint8_t hello[HOP_AUDIO_HELLO_SIZE],
                             const char *token, const hop_session_id *session) {
    if (memcmp(hello, magic, sizeof(magic)) || (hello[4] & ~HOP_AUDIO_MODES) ||
        hello[5] == 0 || hello[5] >= HOP_TOKEN_MAX) return 0;
    hop_session_id remote;
    memcpy(remote.bytes, hello + 6, HOP_SESSION_BYTES);
    if (!hop_session_id_equal(&remote, session) ||
        !hop_token_matches(token, hello + 6 + HOP_SESSION_BYTES, hello[5])) return 0;
    return hello[4];
}

unsigned hop_audio_directions(unsigned local, unsigned peer) {
    unsigned directions = 0;
    if ((local & HOP_AUDIO_SEND) && (peer & HOP_AUDIO_PLAY)) directions |= HOP_AUDIO_SEND;
    if ((local & HOP_AUDIO_PLAY) && (peer & HOP_AUDIO_SEND)) directions |= HOP_AUDIO_PLAY;
    return directions;
}

void hop_audio_header(uint8_t out[HOP_AUDIO_HEADER_SIZE], uint32_t rate, unsigned frames) {
    hop_put_be32(out, rate);
    hop_put_be16(out + 4, (uint16_t)frames);
    hop_put_be16(out + 6, 0);
}

int hop_audio_read_header(const uint8_t in[HOP_AUDIO_HEADER_SIZE],
                          uint32_t *rate, unsigned *frames) {
    uint32_t r = hop_get_be32(in);
    unsigned n = hop_get_be16(in + 4);
    if (hop_get_be16(in + 6) ||
        (n ? (n > HOP_AUDIO_FRAMES || r < 8000 || r > 192000) : (r != 0))) return 0;
    *rate = r;
    *frames = n;
    return 1;
}
