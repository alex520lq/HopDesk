/*
 * HopDesk portable core regression tests.
 * These tests intentionally exercise only platform-independent code.
 */
#include "hop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_messages;
static uint8_t g_types[8];
static uint8_t g_payloads[8][32];
static uint32_t g_lengths[8];

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++g_failures; \
    } \
} while (0)

static void on_message(void *user, uint8_t type, const uint8_t *payload, uint32_t len) {
    (void)user;
    if (g_messages >= 8) {
        ++g_failures;
        return;
    }
    g_types[g_messages] = type;
    g_lengths[g_messages] = len;
    if (len > sizeof(g_payloads[0])) {
        ++g_failures;
        return;
    }
    if (len) memcpy(g_payloads[g_messages], payload, len);
    ++g_messages;
}

static void reset_messages(void) {
    g_messages = 0;
    memset(g_types, 0, sizeof(g_types));
    memset(g_lengths, 0, sizeof(g_lengths));
    memset(g_payloads, 0, sizeof(g_payloads));
}

static void test_endian_and_frame_encoding(void) {
    uint8_t bytes[16] = {0};
    hop_put_be16(bytes, 0xABCDu);
    hop_put_be32(bytes + 2, 0x12345678u);
    hop_put_be64(bytes + 6, UINT64_C(0x0123456789ABCDEF));

    CHECK(hop_get_be16(bytes) == 0xABCDu);
    CHECK(hop_get_be32(bytes + 2) == 0x12345678u);
    CHECK(hop_get_be64(bytes + 6) == UINT64_C(0x0123456789ABCDEF));

    uint8_t frame[32] = {0};
    const uint8_t payload[] = {0x10, 0x20, 0x30};
    uint32_t n = hop_encode_frame(42, payload, sizeof(payload), frame);
    CHECK(n == HOP_HEADER_SIZE + sizeof(payload));
    CHECK(frame[0] == 42);
    CHECK(hop_get_be32(frame + 1) == sizeof(payload));
    CHECK(memcmp(frame + HOP_HEADER_SIZE, payload, sizeof(payload)) == 0);
}

static void test_decoder_fragmentation_and_coalescing(void) {
    reset_messages();
    hop_decoder *d = hop_decoder_new(NULL, on_message);
    CHECK(d != NULL);
    if (!d) return;

    uint8_t a[32], b[32], both[64];
    const uint8_t pa[] = {'h','e','l','l','o'};
    uint32_t na = hop_encode_frame(7, pa, sizeof(pa), a);
    uint32_t nb = hop_encode_frame(8, NULL, 0, b);

    /* Feed one byte at a time to exercise every header/body boundary. */
    for (uint32_t i = 0; i < na; ++i) hop_decoder_feed(d, a + i, 1);
    CHECK(g_messages == 1);
    CHECK(g_types[0] == 7);
    CHECK(g_lengths[0] == sizeof(pa));
    CHECK(memcmp(g_payloads[0], pa, sizeof(pa)) == 0);

    /* Multiple frames in one TCP read, including a zero-length payload. */
    memcpy(both, a, na);
    memcpy(both + na, b, nb);
    reset_messages();
    hop_decoder_feed(d, both, na + nb);
    CHECK(g_messages == 2);
    CHECK(g_types[0] == 7 && g_lengths[0] == sizeof(pa));
    CHECK(g_types[1] == 8 && g_lengths[1] == 0);

    hop_decoder_free(d);
}

static void test_decoder_rejects_oversized_frame(void) {
    reset_messages();
    hop_decoder *d = hop_decoder_new(NULL, on_message);
    CHECK(d != NULL);
    if (!d) return;

    uint8_t stream[HOP_HEADER_SIZE + 16];
    hop_encode_header(stream, 99, HOP_MAX_PAYLOAD + 1u);
    const uint8_t payload[] = {0xAA, 0xBB};
    uint32_t valid = hop_encode_frame(9, payload, sizeof(payload),
                                     stream + HOP_HEADER_SIZE);
    hop_decoder_feed(d, stream, HOP_HEADER_SIZE + valid);

    CHECK(g_messages == 1);
    CHECK(g_types[0] == 9);
    CHECK(g_lengths[0] == sizeof(payload));
    CHECK(memcmp(g_payloads[0], payload, sizeof(payload)) == 0);
    hop_decoder_free(d);
}

static void test_crc32(void) {
    static const char input[] = "123456789";
    uint32_t whole = hop_crc32(0, input, 9);
    uint32_t first = hop_crc32(0, input, 4);
    uint32_t incremental = hop_crc32(first, input + 4, 5);

    CHECK(whole == 0xCBF43926u); /* IEEE CRC-32 check vector */
    CHECK(incremental == whole);
    CHECK(hop_crc32(0, "", 0) == 0u);
}

static void test_config_defaults_and_loading(void) {
    hop_config c;
    hop_config_defaults(&c);
    CHECK(c.port == HOP_DEFAULT_PORT);
    CHECK(c.data_port == HOP_DATA_PORT);
    CHECK(c.xfer_chunk_kb == HOP_CHUNK_KB_DEFAULT);
    CHECK(c.token[0] == '\0');

    const char *path = "hopdesk-core-test.conf";
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;

    fputs("# test config\n"
          "port=25001\n"
          "data_port=25002\n"
          "token=test-secret\n"
          "host=127.0.0.1\n"
          "xfer_chunk_kb=512\n"
          "disp_win_input=0x0F\n"
          "switch_push_ms=175\n", f);
    fclose(f);

    hop_config_defaults(&c);
    CHECK(hop_config_load(&c, path) == 1);
    CHECK(c.port == 25001);
    CHECK(c.data_port == 25002);
    CHECK(strcmp(c.token, "test-secret") == 0);
    CHECK(strcmp(c.peer_host, "127.0.0.1") == 0);
    CHECK(c.xfer_chunk_kb == 512);
    CHECK(c.disp_win_input == 0x0F);
    CHECK(c.switch_delay_ms == 175);
    remove(path);
}

static void test_keymap(void) {
    CHECK(hop_vk_to_mac('A') == 0x00);
    CHECK(hop_vk_to_mac('B') == 0x0B);
    CHECK(hop_vk_to_mac('0') == 0x1D);
    CHECK(hop_vk_to_mac(0x70) == 0x7A); /* VK_F1 -> kVK_F1 */
    CHECK(hop_vk_to_mac(0xA2) == 0x37); /* left Ctrl -> Command */
    CHECK(hop_vk_to_mac(0x5B) == 0x3B); /* left Win -> Control */
    CHECK(hop_vk_to_mac(0xFFFF) == HOP_KEY_NONE);
    CHECK(hop_mac_key_is_mod(0x38) == HOP_MOD_SHIFT);
    CHECK(hop_mac_key_is_mod(0x37) == HOP_MOD_COMMAND);
    CHECK(hop_mac_key_is_mod(0x00) == 0);
}

int main(void) {
    hop_crc32_init();

    test_endian_and_frame_encoding();
    test_decoder_fragmentation_and_coalescing();
    test_decoder_rejects_oversized_frame();
    test_crc32();
    test_config_defaults_and_loading();
    test_keymap();

    if (g_failures) {
        fprintf(stderr, "HopDesk core tests: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("HopDesk core tests: all tests passed");
    return 0;
}
