/*
 * HopDesk portable core regression tests.
 * These tests intentionally exercise only platform-independent code.
 */
#include "hop.h"
#include "config_schema.h"
#include "display_switch.h"
#include "display_catalog.h"
#include "cli.h"
#include "update_manifest.h"
#include "update_controller.h"
#include "xfer_budget.h"
#include "xfer_task.h"
#include "xfer_task_history.h"
#include "xfer_tracking.h"
#include "xfer_rate.h"
#include "xfer_ack_window.h"
#include "xfer_buffer_pool.h"
#include "xfer_rx_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

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
    CHECK(!hop_decoder_feed(d, stream, HOP_HEADER_SIZE + valid));
    CHECK(hop_decoder_failed(d));
    CHECK(g_messages == 0);
    CHECK(!hop_decoder_feed(d, stream + HOP_HEADER_SIZE, valid));
    CHECK(g_messages == 0);
    hop_decoder_reset(d);
    CHECK(!hop_decoder_failed(d));
    CHECK(hop_decoder_feed(d, stream + HOP_HEADER_SIZE, valid));
    CHECK(g_messages == 1);
    CHECK(g_types[0] == 9);
    CHECK(g_lengths[0] == sizeof(payload));
    CHECK(memcmp(g_payloads[0], payload, sizeof(payload)) == 0);
    hop_decoder_free(d);
}

static void test_token_authentication(void) {
    static const uint8_t good[] = "shared-secret";
    static const uint8_t bad[] = "shared-secrex";
    CHECK(hop_token_matches("shared-secret", good, sizeof(good) - 1));
    CHECK(!hop_token_matches("shared-secret", bad, sizeof(bad) - 1));
    CHECK(!hop_token_matches("", good, sizeof(good) - 1));
    CHECK(!hop_token_matches("", (const uint8_t *)"", 0));
    CHECK(!hop_token_matches("shared-secret", NULL, 0));
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
    CHECK(c.ui_theme == HOP_THEME_SYSTEM);
    CHECK(c.xfer_chunk_kb == HOP_CHUNK_KB_DEFAULT);
    CHECK(c.xfer_rate_limit_mib == 128);
#if defined(_WIN32)
    CHECK(c.audio_mode == HOP_AUDIO_PLAY);
#else
    CHECK(c.audio_mode == HOP_AUDIO_SEND);
#endif
    unsigned default_audio_mode = c.audio_mode;
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
          "xfer_rate_limit_mib=256\n"
          "disp_win_input=0x0F\n"
          "ui_theme=dark\n"
          "switch_push_ms=175\n", f);
    fclose(f);

    hop_config_defaults(&c);
    CHECK(hop_config_load(&c, path) == 1);
    CHECK(c.port == 25001);
    CHECK(c.data_port == 25002);
    CHECK(strcmp(c.token, "test-secret") == 0);
    CHECK(strcmp(c.peer_host, "127.0.0.1") == 0);
    CHECK(c.xfer_chunk_kb == 512);
    CHECK(c.xfer_rate_limit_mib == 256);
    CHECK(c.audio_mode == default_audio_mode);
    CHECK(c.disp_win_input == 0x0F);
    CHECK(c.ui_theme == HOP_THEME_DARK);
    c.ui_theme = HOP_THEME_LIGHT;
    CHECK(hop_config_save_atomic(&c, path) == 1);
    hop_config_defaults(&c);
    CHECK(hop_config_load(&c, path) == 1);
    CHECK(c.ui_theme == HOP_THEME_LIGHT);
    CHECK(c.switch_delay_ms == 175);

    /* UI persistence writes a complete config atomically and remains readable. */
    snprintf(c.device_name, sizeof(c.device_name), "%s", "HopDesk-Test");
    snprintf(c.mac_input_cmd, sizeof(c.mac_input_cmd), "%s", "m1ddc display 1 set input");
    c.switch_cooldown_ms = 425;
    c.edge_margin_px = 48;
    c.audio_mode = HOP_AUDIO_MODES;
    CHECK(hop_config_save_atomic(&c, path) == 1);
    hop_config_defaults(&c);
    CHECK(hop_config_load(&c, path) == 1);
    CHECK(strcmp(c.device_name, "HopDesk-Test") == 0);
    CHECK(c.port == 25001 && c.data_port == 25002);
    CHECK(strcmp(c.token, "test-secret") == 0);
    CHECK(c.switch_delay_ms == 175 && c.switch_cooldown_ms == 425);
    CHECK(c.edge_margin_px == 48 && c.disp_win_input == 0x0F);
    CHECK(c.xfer_chunk_kb == 512);
    CHECK(c.xfer_rate_limit_mib == 256);
    CHECK(c.audio_mode == HOP_AUDIO_MODES);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) { fputs("xfer_rate_limit_mib=8\n", f); fclose(f); }
    hop_config before_invalid = c;
    CHECK(hop_config_load(&c, path) == 0);
    CHECK(c.xfer_rate_limit_mib == before_invalid.xfer_rate_limit_mib);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) { fputs("audio_mode=4\n", f); fclose(f); }
    before_invalid = c;
    CHECK(hop_config_load(&c, path) == 0);
    CHECK(c.audio_mode == before_invalid.audio_mode);
    remove(path);
}

static void test_dual_roles(void) {
    const char *path="hopdesk-role-test.conf";
    hop_config c,loaded;
    for(int role=HOP_ROLE_SERVER;role<=HOP_ROLE_CLIENT;++role){
        hop_config_defaults(&c);c.role=(hop_role)role;
        snprintf(c.peer_host,sizeof(c.peer_host),"127.0.0.1");
        CHECK(hop_config_save_atomic(&c,path));
        hop_config_defaults(&loaded);CHECK(hop_config_load(&loaded,path));
        CHECK(loaded.role==c.role);CHECK(!strcmp(loaded.peer_host,c.peer_host));
    }
    FILE *f=fopen(path,"w");CHECK(f!=NULL);
    if(f){fputs("role=invalid\nport=9999\n",f);fclose(f);
        loaded=c;CHECK(!hop_config_load(&loaded,path));CHECK(loaded.port==c.port);}
    /* Independent input_mode persistence and legacy fallback when input_mode is absent. */
    f=fopen(path,"w");CHECK(f!=NULL);
    if(f){fputs("role=client\nhost=10.0.0.2\n",f);fclose(f);
        hop_config_defaults(&loaded);CHECK(hop_config_load(&loaded,path));
        CHECK(loaded.role==HOP_ROLE_CLIENT && loaded.input_mode==0);}
    hop_config_defaults(&c);c.role=HOP_ROLE_CLIENT;c.input_mode=HOP_INPUT_MOUSE;
    snprintf(c.peer_host,sizeof(c.peer_host),"10.0.0.2");
    CHECK(hop_config_save_atomic(&c,path));
    hop_config_defaults(&loaded);CHECK(hop_config_load(&loaded,path));
    CHECK(loaded.role==HOP_ROLE_CLIENT && loaded.input_mode==HOP_INPUT_MOUSE);
    c.input_mode=HOP_INPUT_MODES+1u;CHECK(!hop_config_save_atomic(&c,path));
    remove(path);

    /* Peer capability wire codec and takeover reconciliation. */
    {
        uint8_t wire[HOP_CAPABILITIES_BYTES];
        hop_capabilities_msg decoded;
        CHECK(hop_capabilities_encode(wire, HOP_INPUT_MOUSE, HOP_INPUT_MOUSE, HOP_AUDIO_SEND) == HOP_CAPABILITIES_BYTES);
        CHECK(hop_capabilities_decode(wire, sizeof(wire), &decoded));
        CHECK(decoded.input_mode == HOP_INPUT_MOUSE && decoded.takeover_mask == HOP_INPUT_MOUSE &&
              decoded.audio_mode == HOP_AUDIO_SEND);
        CHECK(!hop_capabilities_encode(wire, HOP_INPUT_MODES + 1u, 0, 0));
        CHECK(!hop_capabilities_decode(wire, sizeof(wire) - 1, &decoded));
        /* Explicit peer takeover clears claimed bits even on the listening end. */
        CHECK(hop_input_reconcile(HOP_INPUT_MODES, HOP_INPUT_MOUSE, HOP_INPUT_MOUSE, HOP_ROLE_SERVER) == HOP_INPUT_KEYBOARD);
        CHECK(hop_input_reconcile(HOP_INPUT_MODES, HOP_INPUT_MODES, HOP_INPUT_MODES, HOP_ROLE_SERVER) == 0);
        /* Initial overlap without takeover yields deterministically on the dialing side. */
        CHECK(hop_input_reconcile(HOP_INPUT_MODES, HOP_INPUT_MODES, 0, HOP_ROLE_CLIENT) == 0);
        CHECK(hop_input_reconcile(HOP_INPUT_MODES, HOP_INPUT_MODES, 0, HOP_ROLE_SERVER) == HOP_INPUT_MODES);
        /* Non-overlapping capabilities stay intact on both sides. */
        CHECK(hop_input_reconcile(HOP_INPUT_MOUSE, HOP_INPUT_KEYBOARD, 0, HOP_ROLE_CLIENT) == HOP_INPUT_MOUSE);
    }
    CHECK(hop_mac_to_vk(0x00)=='A');
    CHECK(hop_mac_to_vk(0x37)==0xA2); /* Cmd -> Windows Ctrl */
    CHECK(hop_mac_to_vk(0x3B)==0x5B); /* Control -> Windows key */
    CHECK(hop_mac_to_vk(0xFFFF)==HOP_KEY_NONE);
    const unsigned keys[]={'A','Z','1',0x70,0x25,0xA0,0xA1,0xA2,0xA3,0x5B,0x5C};
    for(unsigned n=0;n<sizeof(keys)/sizeof(keys[0]);++n)
        CHECK(hop_mac_to_vk(hop_vk_to_mac((uint16_t)keys[n]))==keys[n]);
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


static void test_xfer_hello_validation(void) {
    uint8_t hello[2 + HOP_TOKEN_MAX + HOP_SESSION_BYTES];
    hop_session_id session = {{1}};
    uint32_t n = hop_xfer_hello_encode(hello, "test", &session);
    CHECK(n == 6 + HOP_SESSION_BYTES);
    CHECK(hop_xfer_hello_valid(hello, n, "test"));
    CHECK(!hop_xfer_hello_valid(hello, n, "nope"));
    CHECK(!hop_xfer_hello_valid(hello, n - 1, "test"));
    CHECK(!hop_xfer_hello_valid(hello, n + 1, "test"));
    hello[0] = HOP_XFER_PROTO_VER - 1;
    CHECK(!hop_xfer_hello_valid(hello, n, "test"));
    CHECK(!hop_xfer_hello_valid(NULL, n, "test"));
    CHECK(!hop_xfer_hello_valid(hello, n, NULL));
    uint32_t valid_n = n;
    n = hop_xfer_hello_encode(hello, "", &session);
    CHECK(n == 0);
    uint8_t empty_hello[2 + HOP_SESSION_BYTES] = {HOP_XFER_PROTO_VER, 0};
    memcpy(empty_hello + 2, session.bytes, HOP_SESSION_BYTES);
    CHECK(!hop_xfer_hello_valid(empty_hello, sizeof(empty_hello), ""));
    CHECK(!hop_xfer_hello_valid(empty_hello, sizeof(empty_hello), "x"));
    CHECK(!hop_xfer_hello_valid(hello, 2, "")); /* v5 is not a session handshake */
    char too_long[HOP_TOKEN_MAX + 2];
    memset(too_long, 'x', sizeof(too_long)); too_long[sizeof(too_long)-1] = 0;
    CHECK(!hop_xfer_hello_valid(hello, valid_n, too_long));
}

static void test_xfer_relative_path_validation(void) {
    CHECK(hop_xfer_relpath_safe("file.txt"));
    CHECK(hop_xfer_relpath_safe("folder/sub/file.bin"));
    CHECK(hop_xfer_relpath_safe("a..b"));
    CHECK(hop_xfer_relpath_safe("你好/文件.txt"));

    CHECK(!hop_xfer_relpath_safe(NULL));
    CHECK(!hop_xfer_relpath_safe(""));
    CHECK(!hop_xfer_relpath_safe("../escape"));
    CHECK(!hop_xfer_relpath_safe("folder/../escape"));
    CHECK(!hop_xfer_relpath_safe("..\\escape"));
    CHECK(!hop_xfer_relpath_safe("folder\\..\\escape"));
    CHECK(!hop_xfer_relpath_safe("/etc/passwd"));
    CHECK(!hop_xfer_relpath_safe("\\\\server\\share"));
    CHECK(!hop_xfer_relpath_safe("C:\\Windows\\file"));
    CHECK(!hop_xfer_relpath_safe("folder//file"));
    CHECK(!hop_xfer_relpath_safe("folder/"));
    CHECK(!hop_xfer_relpath_safe("./file"));
    CHECK(!hop_xfer_relpath_safe("folder/./file"));
    CHECK(!hop_xfer_relpath_safe("file:stream"));
    CHECK(!hop_xfer_relpath_safe("bad\nname"));
    CHECK(!hop_xfer_relpath_safe("bad?name"));
    CHECK(!hop_xfer_relpath_safe("bad*name"));
    CHECK(!hop_xfer_relpath_safe("bad<name>"));
    CHECK(!hop_xfer_relpath_safe("trailing "));
    CHECK(!hop_xfer_relpath_safe("trailing."));
    CHECK(!hop_xfer_relpath_safe("folder/trailing /a.txt"));
    CHECK(!hop_xfer_relpath_safe("CON"));
    CHECK(!hop_xfer_relpath_safe("folder/nul.txt"));
    CHECK(!hop_xfer_relpath_safe("com1"));
    CHECK(!hop_xfer_relpath_safe("LPT9.log"));
    CHECK(hop_xfer_relpath_safe("console.txt"));
    CHECK(hop_xfer_relpath_safe("COM0.txt"));
}

static void test_xfer_image_bounds(void) {
    CHECK(hop_xfer_image_valid(1, 1, 4));
    CHECK(hop_xfer_image_valid(1920, 1080, 1920 * 4));
    CHECK(hop_xfer_image_valid(32768, 2048, 32768 * 4));
    CHECK(!hop_xfer_image_valid(0, 1, 4));
    CHECK(!hop_xfer_image_valid(1, 0, 4));
    CHECK(!hop_xfer_image_valid(-1, 1, 4));
    CHECK(!hop_xfer_image_valid(32769, 1, 32769 * 4));
    CHECK(!hop_xfer_image_valid(10, 10, 39));
    CHECK(!hop_xfer_image_valid(32768, 2049, 32768 * 4));
    CHECK(!hop_xfer_image_valid(1, 1, -1));
}

static void make_chunk(uint8_t *p, uint64_t off, uint32_t n) {
    memset(p, 0, 20 + n);
    hop_put_be64(p + 4, off);
    hop_put_be32(p + 12, n);
}

static void test_xfer_chunk_bounds(void) {
    uint8_t p[24];
    make_chunk(p, 0, 4);
    CHECK(hop_xfer_chunk_valid(p, sizeof(p), 4, 0));
    CHECK(!hop_xfer_chunk_valid(p, sizeof(p), 3, 0));
    CHECK(hop_xfer_chunk_valid(p, sizeof(p), 4, 1));
    CHECK(!hop_xfer_chunk_valid(p, sizeof(p), 3, 1));
    CHECK(!hop_xfer_chunk_valid(p, 19, 4, 0));
    CHECK(!hop_xfer_chunk_valid(p, 23, 4, 0));
    CHECK(!hop_xfer_chunk_valid(p, 25, 4, 0));

    make_chunk(p, UINT64_MAX - 2u, 4);
    CHECK(!hop_xfer_chunk_valid(p, sizeof(p), UINT64_MAX, 0));
    make_chunk(p, HOP_IMG_MAX_BYTES - 2u, 4);
    CHECK(!hop_xfer_chunk_valid(p, sizeof(p), 0, 1));
    make_chunk(p, 0, 0);
    CHECK(!hop_xfer_chunk_valid(p, 20, 0, 0));
}

static void test_xfer_retry_policy(void) {
    CHECK(hop_xfer_file_retry_delay_ms(1) == 200);
    CHECK(hop_xfer_file_retry_delay_ms(2) == 400);
    CHECK(hop_xfer_file_retry_delay_ms(3) == 800);
    CHECK(hop_xfer_job_retry_delay_ms(1) == 500);
    CHECK(hop_xfer_job_retry_delay_ms(2) == 1000);
    CHECK(hop_xfer_job_retry_delay_ms(3) == 2000);
    CHECK(hop_xfer_job_retry_delay_ms(4) == 4000);
    CHECK(hop_xfer_job_retry_delay_ms(5) == 8000);
    CHECK(hop_xfer_job_retry_delay_ms(20) == 8000);
    CHECK(HOP_XFER_FILE_RETRY_MAX == 3);
    CHECK(HOP_XFER_JOB_RETRY_MAX == 5);
    CHECK(HOP_XFER_RECONNECT_BUDGET_MS == 30000);
}

static void test_keymap_ranges(void) {
    for (int vk = 0x70; vk <= 0x83; ++vk)
        CHECK(hop_vk_to_mac(vk) != HOP_KEY_NONE);
    for (int vk = 0x60; vk <= 0x69; ++vk)
        CHECK(hop_vk_to_mac(vk) != HOP_KEY_NONE);
    for (int vk = 0; vk <= 0xFF; ++vk) {
        uint16_t kc = hop_vk_to_mac(vk);
        CHECK(kc == HOP_KEY_NONE || kc <= 0x7E);
    }
    CHECK(hop_mac_key_is_mod(0x3B) == HOP_MOD_CONTROL);
    CHECK(hop_mac_key_is_mod(0x3A) == HOP_MOD_ALT);
    CHECK(hop_mac_key_is_mod(0x39) == HOP_MOD_CAPS);
}

static void test_decoder_reset_mid_frame(void) {
    reset_messages();
    hop_decoder *d = hop_decoder_new(NULL, on_message);
    CHECK(d != NULL);
    if (!d) return;
    uint8_t a[32], b[32];
    const uint8_t pa[] = {1, 2, 3, 4};
    const uint8_t pb[] = {9, 8};
    hop_encode_frame(31, pa, sizeof(pa), a);
    uint32_t nb = hop_encode_frame(32, pb, sizeof(pb), b);
    hop_decoder_feed(d, a, HOP_HEADER_SIZE + 2);
    CHECK(g_messages == 0);
    hop_decoder_reset(d);
    hop_decoder_feed(d, b, nb);
    CHECK(g_messages == 1);
    CHECK(g_types[0] == 32);
    CHECK(g_lengths[0] == sizeof(pb));
    CHECK(memcmp(g_payloads[0], pb, sizeof(pb)) == 0);
    hop_decoder_free(d);
}

static void test_decoder_all_split_points(void) {
    const uint8_t payload[] = {0, 1, 2, 3, 4, 5, 6};
    uint8_t frame[32];
    uint32_t n = hop_encode_frame(77, payload, sizeof(payload), frame);
    for (uint32_t split = 1; split < n; ++split) {
        reset_messages();
        hop_decoder *d = hop_decoder_new(NULL, on_message);
        CHECK(d != NULL);
        if (!d) return;
        hop_decoder_feed(d, frame, split);
        hop_decoder_feed(d, frame + split, n - split);
        CHECK(g_messages == 1);
        CHECK(g_types[0] == 77);
        CHECK(g_lengths[0] == sizeof(payload));
        CHECK(memcmp(g_payloads[0], payload, sizeof(payload)) == 0);
        hop_decoder_free(d);
    }
}



static void test_lazy_clipboard_protocol(void) {
    const char *names[] = {"report.txt", "资料", "empty-folder"};
    uint8_t offer[HOP_CLIP_OFFER_MAX_PAYLOAD];
    uint32_t n = hop_clip_file_offer_encode(offer, sizeof(offer),
                                            HOP_DEV_SERVER, 42, names, 3);
    CHECK(n > HOP_CLIP_OFFER_HEADER_BYTES);
    hop_clip_file_offer_msg decoded;
    CHECK(hop_clip_file_offer_decode(offer, n, &decoded));
    CHECK(decoded.origin == HOP_DEV_SERVER && decoded.generation == 42 && decoded.count == 3);
    CHECK(decoded.name_lens[0] == strlen(names[0]));
    CHECK(memcmp(decoded.names[0], names[0], decoded.name_lens[0]) == 0);
    CHECK(decoded.name_lens[1] == strlen(names[1]));
    CHECK(memcmp(decoded.names[1], names[1], decoded.name_lens[1]) == 0);
    CHECK(!hop_clip_file_offer_decode(offer, n - 1, &decoded));
    offer[n] = 0xEE;
    CHECK(!hop_clip_file_offer_decode(offer, n + 1, &decoded));

    const char *bad_names[] = {"../escape"};
    CHECK(hop_clip_file_offer_encode(offer, sizeof(offer), HOP_DEV_SERVER, 43,
                                     bad_names, 1) == 0);
    CHECK(hop_clip_file_offer_encode(offer, sizeof(offer), HOP_DEV_SERVER, 0,
                                     names, 3) == 0);

    uint8_t clear[HOP_CLIP_OFFER_CLEAR_BYTES];
    hop_put_be32(clear, HOP_DEV_CLIENT);
    hop_put_be64(clear + 4, 77);
    uint32_t origin = 0; uint64_t generation = 0;
    CHECK(hop_clip_file_offer_clear_decode(clear, sizeof(clear), &origin, &generation));
    CHECK(origin == HOP_DEV_CLIENT && generation == 77);
    CHECK(!hop_clip_file_offer_clear_decode(clear, sizeof(clear)-1, &origin, &generation));

    uint8_t request[HOP_CLIP_PASTE_REQUEST_BYTES];
    uint32_t request_id = HOP_XFER_PASTE_BATCH_FLAG | 9u;
    CHECK(hop_clip_paste_request_encode(request, request_id, HOP_DEV_SERVER, 42, 1234) ==
          HOP_CLIP_PASTE_REQUEST_BYTES);
    hop_clip_paste_request_msg req;
    CHECK(hop_clip_paste_request_decode(request, sizeof(request), &req));
    CHECK(req.request_id == request_id && req.origin == HOP_DEV_SERVER &&
          req.generation == 42 && req.clipboard_version == 1234);
    CHECK(!hop_clip_paste_request_decode(request, sizeof(request)-1, &req));
    hop_put_be32(request, 9);
    CHECK(!hop_clip_paste_request_decode(request, sizeof(request), &req));

    uint8_t failed[HOP_CLIP_PASTE_FAILED_BYTES];
    CHECK(hop_clip_paste_failed_encode(failed, request_id, HOP_CLIP_PASTE_FAIL_STALE) ==
          HOP_CLIP_PASTE_FAILED_BYTES);
    uint32_t failed_id = 0; uint8_t reason = 0;
    CHECK(hop_clip_paste_failed_decode(failed, sizeof(failed), &failed_id, &reason));
    CHECK(failed_id == request_id && reason == HOP_CLIP_PASTE_FAIL_STALE);
    CHECK(!hop_clip_paste_failed_decode(failed, sizeof(failed)-1, &failed_id, &reason));
}

static void test_clipboard_origin_routing(void) {
    CHECK(hop_clip_should_apply_remote(HOP_DEV_CLIENT, HOP_DEV_SERVER));
    CHECK(hop_clip_should_apply_remote(HOP_DEV_SERVER, HOP_DEV_CLIENT));
    CHECK(!hop_clip_should_apply_remote(HOP_DEV_SERVER, HOP_DEV_SERVER));
    CHECK(!hop_clip_should_apply_remote(HOP_DEV_CLIENT, HOP_DEV_CLIENT));
    CHECK(hop_clip_should_apply_remote(0, HOP_DEV_SERVER));
}

static void test_file_crc_fold(void) {
    const char a[] = "chunk-A";
    const char b[] = "chunk-B";
    uint32_t ca = hop_crc32(0, a, sizeof(a)-1);
    uint32_t cb = hop_crc32(0, b, sizeof(b)-1);
    uint32_t ab = hop_xfer_file_crc_step(hop_xfer_file_crc_step(0, 0, ca), 7, cb);
    uint32_t ba = hop_xfer_file_crc_step(hop_xfer_file_crc_step(0, 7, cb), 0, ca);
    uint32_t moved = hop_xfer_file_crc_step(0, 1, ca);
    CHECK(ab != ba);
    CHECK(hop_xfer_file_crc_step(0, 0, ca) != moved);
    CHECK(hop_xfer_file_crc_step(0, 0, ca) != hop_xfer_file_crc_step(0, 0, cb));
}

static void test_relative_path_wire_bytes(void) {
    const uint8_t good[] = "dir/file.txt";
    const uint8_t nul_inside[] = {'d','i','r',0,'/','x'};
    CHECK(hop_xfer_relpath_bytes_safe(good, sizeof(good)-1));
    CHECK(!hop_xfer_relpath_bytes_safe(good, HOP_PATH_LEN_MAX+1u));
    CHECK(!hop_xfer_relpath_bytes_safe(nul_inside, sizeof(nul_inside)));
    CHECK(!hop_xfer_relpath_bytes_safe((const uint8_t *)"a\\\\b", 3));
}

static void test_file_begin_decode(void) {
    const uint8_t name[] = "report.txt";
    const uint8_t path[] = "folder/report.txt";
    uint8_t p[29 + sizeof(name)-1 + sizeof(path)-1];
    memset(p, 0, sizeof(p));
    hop_put_be32(p, 0x11223344u);
    hop_put_be32(p+4, 7);
    hop_put_be64(p+8, 123456789u);
    hop_put_be64(p+16, 987654u);
    p[24] = 0;
    hop_put_be16(p+25, (uint16_t)(sizeof(name)-1));
    memcpy(p+27, name, sizeof(name)-1);
    hop_put_be16(p+27+sizeof(name)-1, (uint16_t)(sizeof(path)-1));
    memcpy(p+29+sizeof(name)-1, path, sizeof(path)-1);

    hop_xfer_file_begin_msg m;
    CHECK(hop_xfer_decode_file_begin(p, sizeof(p), &m));
    CHECK(m.batch == 0x11223344u && m.id == 7);
    CHECK(m.size == 123456789u && m.mtime == 987654u);
    CHECK(m.name_len == sizeof(name)-1);
    CHECK(memcmp(m.name, name, sizeof(name)-1) == 0);
    CHECK(m.relpath_len == sizeof(path)-1);
    CHECK(memcmp(m.relpath, path, sizeof(path)-1) == 0);
    CHECK(!hop_xfer_decode_file_begin(p, sizeof(p)-1, &m));

    uint8_t trailing[sizeof(p)+1];
    memcpy(trailing,p,sizeof(p)); trailing[sizeof(p)] = 0xEE;
    CHECK(!hop_xfer_decode_file_begin(trailing,sizeof(trailing),&m));
    hop_put_be16(p+25,0);
    CHECK(!hop_xfer_decode_file_begin(p,sizeof(p),&m));
}

static void test_image_begin_decode(void) {
    uint8_t p[20] = {0};
    hop_put_be32(p, 9);
    hop_put_be32(p+4, 2);
    hop_put_be32(p+8, 1920);
    hop_put_be32(p+12, 1080);
    hop_put_be32(p+16, 7680);
    hop_xfer_image_begin_msg m;
    CHECK(hop_xfer_decode_image_begin(p,sizeof(p),&m));
    CHECK(m.batch==9 && m.id==2 && m.width==1920 && m.height==1080 && m.stride==7680);
    CHECK(!hop_xfer_decode_image_begin(p,sizeof(p)-1,&m));
    CHECK(!hop_xfer_decode_image_begin(p,sizeof(p)+1,&m));
}

static void test_file_end_ack_abort_done_decoders(void) {
    uint8_t end[9] = {0}, ack[9] = {0}, abortp[6] = {0}, donep[20] = {0};
    hop_put_be32(end, 17); end[4] = HOP_FILE_OK; hop_put_be32(end+5, 0xAABBCCDDu);
    hop_xfer_file_end_msg e;
    CHECK(hop_xfer_decode_file_end(end,sizeof(end),&e));
    CHECK(e.id==17 && e.status==HOP_FILE_OK && e.file_crc==0xAABBCCDDu);
    CHECK(!hop_xfer_decode_file_end(end,8,&e));
    CHECK(!hop_xfer_decode_file_end(end,10,&e));

    hop_put_be32(ack, 21); hop_put_be32(ack+4, 17); ack[8]=HOP_FILE_ACK_CRC;
    hop_xfer_file_ack_msg a;
    CHECK(hop_xfer_decode_file_ack(ack,sizeof(ack),&a));
    CHECK(a.batch==21 && a.id==17 && a.status==HOP_FILE_ACK_CRC);
    CHECK(!hop_xfer_decode_file_ack(ack,8,&a));

    hop_put_be32(abortp, 21); abortp[4]=HOP_ABORT_CANCEL; abortp[5]=HOP_ABORT_TO_RECEIVER;
    hop_xfer_abort_msg ab;
    CHECK(hop_xfer_decode_abort(abortp,sizeof(abortp),&ab));
    CHECK(ab.batch==21 && ab.reason==HOP_ABORT_CANCEL && ab.target==HOP_ABORT_TO_RECEIVER);
    CHECK(!hop_xfer_decode_abort(abortp,5,&ab));
    abortp[5]=HOP_ABORT_TO_SENDER; CHECK(hop_xfer_decode_abort(abortp,sizeof(abortp),&ab) && ab.target==HOP_ABORT_TO_SENDER);
    abortp[5]=0; CHECK(!hop_xfer_decode_abort(abortp,sizeof(abortp),&ab));
    abortp[5]=3; CHECK(!hop_xfer_decode_abort(abortp,sizeof(abortp),&ab));

    hop_put_be32(donep,21); hop_put_be32(donep+4,2); hop_put_be32(donep+8,3);
    hop_put_be64(donep+12,123456);
    hop_xfer_done_msg d;
    CHECK(hop_xfer_decode_done(donep,sizeof(donep),&d));
    CHECK(d.batch==21 && d.ok==2 && d.total==3 && d.bytes==123456);
    CHECK(!hop_xfer_decode_done(donep,19,&d));
    CHECK(!hop_xfer_decode_done(donep,21,&d));
}

static void test_update_manifest_validation(void) {
    const char manifest[] =
        "format=1\n"
        "version=0.5.1\n"
        "tag=v0.5.1\n"
        "windows_url=https://github.com/alex520lq/HopDesk/releases/download/v0.5.1/HopDesk-Windows.zip\n"
        "windows_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "macos_url=https://github.com/alex520lq/HopDesk/releases/download/v0.5.1/HopDesk-macOS.zip\n"
        "macos_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "notes_url=https://github.com/alex520lq/HopDesk/releases/tag/v0.5.1\n";
    hop_update_manifest parsed;
    int valid = 0;
    CHECK(hop_update_manifest_parse(manifest, sizeof(manifest) - 1, &parsed));
    CHECK(strcmp(parsed.version, "0.5.1") == 0);
    CHECK(strcmp(parsed.tag, "v0.5.1") == 0);
    CHECK(strcmp(parsed.windows_sha256, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);
    CHECK(hop_version_compare("0.5.0", "0.5.1", &valid) < 0 && valid);
    CHECK(hop_version_compare("1.0.0", "0.9.99", &valid) > 0 && valid);
    CHECK(hop_version_compare("0.5.1", "0.5.1", &valid) == 0 && valid);
    CHECK(hop_version_compare("0.5.1-dev", "0.5.1", &valid) == 0 && !valid);

    char tampered[sizeof(manifest) + 1];
    memcpy(tampered, manifest, sizeof(manifest));
    char *url = strstr(tampered, "https://github.com/alex520lq/HopDesk");
    CHECK(url != NULL);
    if (url) url[8] = 'x';
    CHECK(!hop_update_manifest_parse(tampered, sizeof(manifest) - 1, &parsed));

    const char duplicate[] =
        "format=1\nversion=0.5.1\nversion=0.5.2\n"
        "tag=v0.5.1\nwindows_url=https://github.com/alex520lq/HopDesk/releases/download/v0.5.1/HopDesk-Windows.zip\n"
        "windows_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "macos_url=https://github.com/alex520lq/HopDesk/releases/download/v0.5.1/HopDesk-macOS.zip\n"
        "macos_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "notes_url=https://github.com/alex520lq/HopDesk/releases/tag/v0.5.1\n";
    CHECK(!hop_update_manifest_parse(duplicate, sizeof(duplicate) - 1, &parsed));

    /* SemVer -dev.N ordering and strict validation */
    CHECK(hop_version_compare("0.1.1-dev.1", "0.1.1-dev.2", &valid) < 0 && valid);
    CHECK(hop_version_compare("0.1.1-dev.2", "0.1.1", &valid) < 0 && valid);
    CHECK(hop_version_compare("0.1.1", "0.1.1-dev.1", &valid) > 0 && valid);
    CHECK(hop_version_compare("1.4.1-dev.1", "1.4.0", &valid) > 0 && valid);
    CHECK(hop_version_compare("0.1.1-dev.0", "0.1.1", &valid) == 0 && !valid);
    CHECK(hop_version_compare("0.1.1-dev.01", "0.1.1", &valid) == 0 && !valid);

    /* Dual-channel format=2 stable & dev manifests */
    const char stable_v2[] =
        "format=2\n"
        "channel=stable\n"
        "version=1.4.0\n"
        "tag=v1.4.0\n"
        "ctrl_proto=8\n"
        "xfer_proto=8\n"
        "windows_url=https://github.com/alex520lq/HopDesk/releases/download/v1.4.0/HopDesk-Windows.zip\n"
        "windows_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "macos_url=https://github.com/alex520lq/HopDesk/releases/download/v1.4.0/HopDesk-macOS.zip\n"
        "macos_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "notes_url=https://github.com/alex520lq/HopDesk/releases/tag/v1.4.0\n"
        "summary=Stable 1.4.0 release\n";
    const char dev_v2[] =
        "format=2\n"
        "channel=dev\n"
        "version=1.4.1-dev.1\n"
        "tag=v1.4.1-dev.1\n"
        "ctrl_proto=9\n"
        "xfer_proto=8\n"
        "windows_url=https://github.com/alex520lq/HopDesk/releases/download/v1.4.1-dev.1/HopDesk-Windows.zip\n"
        "windows_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "macos_url=https://github.com/alex520lq/HopDesk/releases/download/v1.4.1-dev.1/HopDesk-macOS.zip\n"
        "macos_sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\n"
        "notes_url=https://github.com/alex520lq/HopDesk/releases/tag/v1.4.1-dev.1\n"
        "summary=Dev preview build\n";
    hop_update_manifest stable_m, dev_m;
    CHECK(hop_update_manifest_parse(stable_v2, sizeof(stable_v2) - 1, &stable_m));
    CHECK(stable_m.format == 2 && stable_m.channel == HOP_UPDATE_CHANNEL_STABLE);
    CHECK(stable_m.ctrl_proto == 8 && stable_m.xfer_proto == 8);
    CHECK(strcmp(stable_m.summary, "Stable 1.4.0 release") == 0);

    CHECK(hop_update_manifest_parse(dev_v2, sizeof(dev_v2) - 1, &dev_m));
    CHECK(dev_m.format == 2 && dev_m.channel == HOP_UPDATE_CHANNEL_DEV);
    CHECK(dev_m.ctrl_proto == 9 && dev_m.xfer_proto == 8);

    /* Reject channel/version spoofing */
    const char spoofed[] =
        "format=2\nchannel=stable\nversion=1.4.1-dev.1\ntag=v1.4.1-dev.1\nctrl_proto=8\nxfer_proto=8\n"
        "windows_url=https://github.com/alex520lq/HopDesk/releases/download/v1.4.1-dev.1/HopDesk-Windows.zip\n"
        "windows_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "macos_url=https://github.com/alex520lq/HopDesk/releases/download/v1.4.1-dev.1/HopDesk-macOS.zip\n"
        "macos_sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\n"
        "notes_url=https://github.com/alex520lq/HopDesk/releases/tag/v1.4.1-dev.1\n";
    CHECK(!hop_update_manifest_parse(spoofed, sizeof(spoofed) - 1, &parsed));

    /* Evaluation rules: same-channel anti-rollback + cross-channel bidirectional switch */
    int is_switch = 0;
    CHECK(hop_update_evaluate("1.3.9", HOP_UPDATE_CHANNEL_STABLE, HOP_UPDATE_CHANNEL_STABLE,
                              &stable_m, &is_switch, &valid) == 1 && valid && !is_switch);
    CHECK(hop_update_evaluate("1.4.0", HOP_UPDATE_CHANNEL_STABLE, HOP_UPDATE_CHANNEL_STABLE,
                              &stable_m, &is_switch, &valid) == 0 && valid && !is_switch);
    CHECK(hop_update_evaluate("1.5.0", HOP_UPDATE_CHANNEL_STABLE, HOP_UPDATE_CHANNEL_STABLE,
                              &stable_m, &is_switch, &valid) == 0 && valid && !is_switch);
    /* Dev 1.4.1-dev.1 -> Stable 1.4.0 cross-channel switch MUST be allowed */
    CHECK(hop_update_evaluate("1.4.1-dev.1", HOP_UPDATE_CHANNEL_DEV, HOP_UPDATE_CHANNEL_STABLE,
                              &stable_m, &is_switch, &valid) == 1 && valid && is_switch);
    /* Stable 1.4.0 -> Dev 1.4.1-dev.1 cross-channel switch MUST be allowed */
    CHECK(hop_update_evaluate("1.4.0", HOP_UPDATE_CHANNEL_STABLE, HOP_UPDATE_CHANNEL_DEV,
                              &dev_m, &is_switch, &valid) == 1 && valid && is_switch);
    /* Manifest channel mismatch rejected */
    CHECK(hop_update_evaluate("1.4.0", HOP_UPDATE_CHANNEL_STABLE, HOP_UPDATE_CHANNEL_DEV,
                              &stable_m, &is_switch, &valid) == 0 && !valid);

    /* Update controller state transitions & protocol diff detection */
    hop_update_snapshot snap;
    hop_update_snapshot_init(&snap, HOP_UPDATE_CHANNEL_STABLE);
    CHECK(snap.phase == HOP_UPDATE_IDLE && snap.target_channel == HOP_UPDATE_CHANNEL_STABLE);
    hop_update_snapshot_set_checking(&snap, HOP_UPDATE_CHANNEL_DEV);
    CHECK(snap.phase == HOP_UPDATE_CHECKING && snap.target_channel == HOP_UPDATE_CHANNEL_DEV);
    hop_update_snapshot_set_manifest_result(&snap, "1.4.0", HOP_UPDATE_CHANNEL_STABLE,
                                            8, 8, HOP_UPDATE_CHANNEL_DEV, &dev_m);
    CHECK(snap.phase == HOP_UPDATE_AVAILABLE && snap.is_channel_switch == 1 && snap.protocol_changed == 1);
    CHECK(snap.target_ctrl_proto == 9 && snap.target_xfer_proto == 8);
    hop_update_snapshot_set_downloading(&snap, 512, 1024);
    CHECK(snap.phase == HOP_UPDATE_DOWNLOADING && snap.bytes_done == 512 && snap.bytes_total == 1024);
    hop_update_snapshot_set_ready(&snap, 1024);
    CHECK(snap.phase == HOP_UPDATE_READY && snap.bytes_done == 1024);
    hop_update_snapshot_set_applying(&snap);
    CHECK(snap.phase == HOP_UPDATE_APPLYING);
    hop_update_snapshot_set_error(&snap, HOP_UPDATE_CHANNEL_STABLE, "network error");
    CHECK(snap.phase == HOP_UPDATE_ERROR && strcmp(snap.message, "network error") == 0);

    /* update_channel config schema validation */
    hop_config cfg;
    char err[128] = {0};
    hop_config_defaults(&cfg);
    CHECK(hop_config_set_kv(&cfg, "update_channel", "dev", err, sizeof(err)) == 1);
    CHECK(cfg.update_channel == HOP_UPDATE_CHANNEL_DEV);
    CHECK(hop_config_set_kv(&cfg, "update_channel", "stable", err, sizeof(err)) == 1);
    CHECK(cfg.update_channel == HOP_UPDATE_CHANNEL_STABLE);
    CHECK(hop_config_set_kv(&cfg, "update_channel", "beta", err, sizeof(err)) == -1);
}

static void test_display_catalog_helpers(void) {
    char buf[160];
    CHECK(hop_display_input_preset_count() >= 5);
    CHECK(hop_display_input_find_preset(0x0Fu) == 0);
    CHECK(hop_display_input_find_preset(0x11u) == 2);
    CHECK(hop_display_input_find_preset(0x1Bu) == 4);
    CHECK(hop_display_input_find_preset(0xD0u) == -1);

    hop_display_input_format_label(0x0Fu, buf, sizeof(buf));
    CHECK(strstr(buf, "DisplayPort 1") != NULL && strstr(buf, "0x0F") != NULL);
    hop_display_input_format_label(0x11u, buf, sizeof(buf));
    CHECK(strstr(buf, "HDMI 1") != NULL && strstr(buf, "0x11") != NULL);
    hop_display_input_format_label(0xD0u, buf, sizeof(buf));
    CHECK(strstr(buf, "0xD0") != NULL);

    hop_display_info info;
    memset(&info, 0, sizeof(info));
    info.index = 2;
    info.is_primary = 0;
    info.attached = 1;
    info.width = 3840;
    info.height = 2160;
    info.pos_x = 1920;
    info.current_input = 0x0F;
    snprintf(info.device_id, sizeof(info.device_id), "%s", "\\\\.\\DISPLAY2");
    snprintf(info.friendly_name, sizeof(info.friendly_name), "%s", "BenQ PD2700U");
    hop_display_format_monitor_label(&info, buf, sizeof(buf));
    CHECK(strstr(buf, "BenQ PD2700U") != NULL);
    CHECK(strstr(buf, "3840") != NULL && strstr(buf, "2160") != NULL);
    CHECK(strstr(buf, "DisplayPort 1") != NULL);

    hop_display_mac_build_input_cmd(2, buf, sizeof(buf));
    CHECK(strcmp(buf, "m1ddc display 2 set input") == 0);
    CHECK(hop_display_mac_parse_input_cmd_index(buf) == 2);
    CHECK(hop_display_mac_parse_input_cmd_index("m1ddc display 1 set input") == 1);
    CHECK(hop_display_mac_parse_input_cmd_index("/usr/local/bin/custom-ddc") == 0);
    CHECK(hop_display_mac_parse_input_cmd_index("m1ddc display 0 set input") == 0);
}



typedef struct { hop_xfer_task task; uint32_t transfer_id; int acquired; } budget_thread_context;
#ifdef _WIN32
static DWORD WINAPI budget_reserve_worker(LPVOID value) {
    budget_thread_context *ctx=(budget_thread_context*)value;
    ctx->acquired=hop_xfer_task_init(&ctx->task,0,0,ctx->transfer_id) && hop_xfer_task_reserve_job(&ctx->task);
    return 0;
}
#else
static void *budget_reserve_worker(void *value) {
    budget_thread_context *ctx=(budget_thread_context*)value;
    ctx->acquired=hop_xfer_task_init(&ctx->task,0,0,ctx->transfer_id) && hop_xfer_task_reserve_job(&ctx->task);
    return NULL;
}
#endif

static void test_xfer_global_budget(void) {
    hop_xfer_budget_limits limits={2u,3u,1000u,600u,500u,2u,16u,32u};
    hop_xfer_budget_snapshot snap;
    hop_xfer_reservation_handle a,b,c;
    memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));memset(&c,0,sizeof(c));
    CHECK(hop_xfer_budget_reservation_init(&a,1001));
    CHECK(hop_xfer_budget_reservation_init(&b,1002));
    CHECK(hop_xfer_budget_reservation_init(&c,1003));
    CHECK(hop_xfer_budget_set_limits_for_test(&limits));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.limits.max_jobs==2&&snap.limits.max_entries==3);
    CHECK(snap.limits.max_pending_bytes==1000&&snap.limits.max_image_bytes==600);
    CHECK(snap.limits.max_rx_queue_slots==2&&snap.limits.max_rx_queue_bytes==16);
    CHECK(snap.limits.max_image_temp_bytes==32);
    CHECK(hop_xfer_budget_reserve_job(&a));
    CHECK(hop_xfer_budget_reserve_entry(&a,400));
    CHECK(hop_xfer_budget_reserve_entry(&a,300));
    CHECK(!hop_xfer_budget_reserve_entry(&a,301));
    CHECK(hop_xfer_budget_reserve_job(&b));
    CHECK(!hop_xfer_budget_reserve_job(&c));
    CHECK(hop_xfer_budget_reserve_entry(&b,1));
    CHECK(!hop_xfer_budget_reserve_entry(&b,0));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==2&&snap.entries==3&&snap.pending_bytes==701);
    CHECK(snap.jobs_high_water==2&&snap.entries_high_water==3);
    uint64_t released_before=snap.released_reservations;
    CHECK(hop_xfer_budget_release(&a));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.released_reservations==released_before+1);
    CHECK(hop_xfer_budget_release(&a)); /* A repeated return is an empty no-op. */
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.released_reservations==released_before+1);
    CHECK(snap.jobs==1&&snap.entries==1&&snap.pending_bytes==1);
    CHECK(hop_xfer_budget_release(&b));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==0&&snap.entries==0&&snap.pending_bytes==0);
    CHECK(snap.released_reservations==released_before+2);
    CHECK(hop_xfer_budget_reserve_job(&a));
    CHECK(hop_xfer_budget_reserve_image(&a,500));
    CHECK(!hop_xfer_budget_reserve_image(&a,501));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==1&&snap.entries==1&&snap.pending_bytes==500&&snap.image_bytes==500);
    CHECK(hop_xfer_budget_reserve_rx_buffer(&a,8));
    CHECK(hop_xfer_budget_reserve_rx_buffer(&a,8));
    CHECK(!hop_xfer_budget_reserve_rx_buffer(&a,1));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.rx_queue_slots==2&&snap.rx_queue_bytes==16);
    CHECK(snap.rx_queue_slots_high_water==2&&snap.rx_queue_bytes_high_water==16);
    CHECK(snap.rejected_rx_buffers==1);
    CHECK(hop_xfer_budget_release_rx_buffer(&a,8));
    CHECK(hop_xfer_budget_release_rx_buffer(&a,8));
    CHECK(!hop_xfer_budget_release_rx_buffer(&a,8));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.rx_queue_slots==0&&snap.rx_queue_bytes==0&&snap.released_rx_buffers==2);
    CHECK(snap.accounting_errors==1);
    CHECK(hop_xfer_budget_set_rx_pool_bytes(16));
    CHECK(!hop_xfer_budget_set_rx_pool_bytes(17));
    CHECK(hop_xfer_budget_set_rx_pool_bytes(0));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.rx_pool_bytes==0&&snap.rx_pool_bytes_high_water==16);
    CHECK(snap.rejected_rx_buffers==2);
    CHECK(hop_xfer_budget_reserve_image_temp(&a,16));
    CHECK(hop_xfer_budget_reserve_image_temp(&a,16));
    CHECK(!hop_xfer_budget_reserve_image_temp(&a,1));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.image_temp_bytes==32&&snap.image_temp_bytes_high_water==32);
    CHECK(snap.image_bytes==532); /* 500-byte payload reservation + 32 temp bytes */
    CHECK(snap.rejected_image_temp_bytes==1);
    CHECK(hop_xfer_budget_release_image_temp(&a,16));
    CHECK(hop_xfer_budget_release_image_temp(&a,16));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.image_temp_bytes==0&&snap.released_image_temp_bytes==32);
    CHECK(snap.image_bytes==500);
    hop_xfer_budget_release(&a);
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==0&&snap.entries==0&&snap.pending_bytes==0&&snap.image_bytes==0);

    /* Promote an already-reserved conversion buffer without double-counting
     * its physical allocation against the combined image-memory ceiling. */
    CHECK(hop_xfer_budget_reserve_job(&a));
    CHECK(hop_xfer_budget_reserve_job(&b));
    CHECK(hop_xfer_budget_reserve_image_temp(&a,16));
    CHECK(hop_xfer_budget_transfer_image_temp(&a,&b,8));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.image_bytes==16&&snap.image_temp_bytes==8);
    CHECK(snap.entries==1&&snap.pending_bytes==8);
    CHECK(snap.transferred_image_temp_bytes==8);
    CHECK(hop_xfer_budget_release_image_temp(&a,8));
    hop_xfer_budget_release(&a);
    hop_xfer_budget_release(&b);
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==0&&snap.entries==0&&snap.pending_bytes==0);
    CHECK(snap.image_bytes==0&&snap.image_temp_bytes==0);

    hop_xfer_budget_limits concurrent_limits={4u,64u,1024u,512u,512u,2u,1024u,1024u};
    CHECK(hop_xfer_budget_set_limits_for_test(&concurrent_limits));
    budget_thread_context contexts[32];
    memset(contexts,0,sizeof(contexts));
    for(int i=0;i<32;i++)contexts[i].transfer_id=(uint32_t)i+1;
#ifdef _WIN32
    HANDLE threads[32]={0};
    int created=0;
    for(int i=0;i<32;i++){threads[i]=CreateThread(NULL,0,budget_reserve_worker,&contexts[i],0,NULL);if(threads[i])created++;else CHECK(0);}
    for(int i=0;i<32;i++)if(threads[i]){CHECK(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0);CloseHandle(threads[i]);}
#else
    pthread_t threads[32];
    int created=0,started[32]={0};
    for(int i=0;i<32;i++){if(pthread_create(&threads[i],NULL,budget_reserve_worker,&contexts[i])==0){created++;started[i]=1;}else CHECK(0);}
    for(int i=0;i<32;i++)if(started[i])CHECK(pthread_join(threads[i],NULL)==0);
#endif
    int acquired=0;
    for(int i=0;i<32;i++)if(contexts[i].acquired)acquired++;
    CHECK(created==32&&acquired==4);
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==4&&snap.jobs_high_water==4);
    hop_xfer_task_snapshot records[32]; int total=0;
    CHECK(hop_xfer_task_list(records,32,&total)==32 && total==32);
    int reserved=0; for(int i=0;i<32;i++) reserved+=(int)records[i].jobs;
    CHECK(reserved==4);
    CHECK(hop_xfer_task_list(records,2,&total)==2 && total==32);
    for(int i=0;i<32;i++)hop_xfer_task_dispose(&contexts[i].task);
    CHECK(hop_xfer_task_list(NULL,0,&total)==0 && total==0);
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==0);
    hop_xfer_budget_limits defaults={16u,32768u,8ull*1024ull*1024ull*1024ull,256ull*1024ull*1024ull,256ull*1024ull*1024ull,8u,8ull*1024ull*1024ull,512ull*1024ull*1024ull};
    CHECK(hop_xfer_budget_set_limits_for_test(&defaults));
}


static void test_xfer_ack_window(void) {
    hop_xfer_ack_window window;
    int status=0;
    uint64_t rtt_ns=0;
    hop_xfer_ack_window_reset(&window);
    for(uint32_t id=1;id<=HOP_XFER_ACK_WINDOW_CAPACITY;id++)
        CHECK(hop_xfer_ack_window_register(&window,10,id));
    CHECK(hop_xfer_ack_window_count(&window)==HOP_XFER_ACK_WINDOW_CAPACITY);
    CHECK(!hop_xfer_ack_window_register(&window,10,5));
    for(uint32_t id=1;id<=HOP_XFER_ACK_WINDOW_CAPACITY;id++)
        CHECK(hop_xfer_ack_window_mark_sent(&window,10,id,1000000000ull+id*1000000ull));

    /* ACKs can arrive out of order and must not overwrite each other. */
    CHECK(hop_xfer_ack_window_deliver(&window,10,3,2,1500000000ull));
    CHECK(!hop_xfer_ack_window_deliver(&window,10,3,0,1501000000ull));
    CHECK(!hop_xfer_ack_window_take(&window,10,1,&status,&rtt_ns));
    CHECK(hop_xfer_ack_window_take(&window,10,3,&status,&rtt_ns));
    CHECK(status==2&&rtt_ns==497000000ull);
    CHECK(hop_xfer_ack_window_count(&window)==3);
    CHECK(!hop_xfer_ack_window_deliver(&window,10,3,0,1600000000ull));

    CHECK(hop_xfer_ack_window_deliver(&window,10,1,0,1600000000ull));
    CHECK(hop_xfer_ack_window_take(&window,10,1,&status,&rtt_ns));
    CHECK(status==0&&rtt_ns==599000000ull);
    CHECK(hop_xfer_ack_window_register(&window,10,5));
    CHECK(hop_xfer_ack_window_count(&window)==3);
    hop_xfer_ack_window_unregister(&window,10,2);
    CHECK(hop_xfer_ack_window_count(&window)==2);
    hop_xfer_ack_window_unregister_batch(&window,10);
    CHECK(hop_xfer_ack_window_count(&window)==0);

    CHECK(hop_xfer_ack_window_register(&window,11,1));
    CHECK(hop_xfer_ack_window_mark_sent(&window,11,1,2000000000ull));
    CHECK(hop_xfer_ack_window_deliver(&window,11,1,1,2100000000ull));
    CHECK(hop_xfer_ack_window_take(&window,11,1,&status,&rtt_ns));
    CHECK(status==1&&rtt_ns==100000000ull);
    CHECK(hop_xfer_ack_window_count(&window)==0);

    /* Deadline starts after FILE_END is fully written, not when the ACK slot
     * is armed. This also covers an ACK racing ahead of deadline publication. */
    CHECK(hop_xfer_ack_window_register(&window,12,1));
    CHECK(!hop_xfer_ack_window_set_deadline(&window,12,1,5000));
    CHECK(hop_xfer_ack_window_mark_sent(&window,12,1,1000));
    CHECK(hop_xfer_ack_window_deliver(&window,12,1,0,4500));
    CHECK(hop_xfer_ack_window_set_deadline(&window,12,1,5000));
    int expired=1;
    CHECK(hop_xfer_ack_window_take_ex(&window,12,1,&status,&rtt_ns,&expired));
    CHECK(status==0&&!expired);

    CHECK(hop_xfer_ack_window_register(&window,12,2));
    CHECK(hop_xfer_ack_window_mark_sent(&window,12,2,6000));
    CHECK(hop_xfer_ack_window_set_deadline(&window,12,2,7000));
    CHECK(hop_xfer_ack_window_deliver(&window,12,2,0,7000));
    expired=0;
    CHECK(hop_xfer_ack_window_take_ex(&window,12,2,&status,&rtt_ns,&expired));
    CHECK(expired); /* deadline equality is expired, matching ack_wait's >= */
    CHECK(hop_xfer_ack_window_count(&window)==0);

    /* Reconnect checkpoint advances only through a contiguous resolved prefix. */
    int checkpoint = 0;
    CHECK(!hop_xfer_ack_window_advance_checkpoint(&checkpoint,0,0));
    CHECK(checkpoint==0);
    CHECK(!hop_xfer_ack_window_advance_checkpoint(&checkpoint,1,1));
    CHECK(checkpoint==0);
    CHECK(hop_xfer_ack_window_advance_checkpoint(&checkpoint,0,1));
    CHECK(checkpoint==1);
    CHECK(!hop_xfer_ack_window_advance_checkpoint(&checkpoint,3,1));
    CHECK(checkpoint==1);
    CHECK(hop_xfer_ack_window_advance_checkpoint(&checkpoint,1,1));
    CHECK(checkpoint==2);
    CHECK(!hop_xfer_ack_window_advance_checkpoint(&checkpoint,-1,1));

    /* Simulate a disconnect while filling a four-file window: only ACKed
     * contiguous entries may move the reconnect checkpoint. */
    hop_xfer_ack_window_reset(&window);
    for(uint32_t id=1;id<=HOP_XFER_ACK_WINDOW_CAPACITY;id++) {
        CHECK(hop_xfer_ack_window_register(&window,13,id));
        CHECK(hop_xfer_ack_window_mark_sent(&window,13,id,10000u+id));
        CHECK(hop_xfer_ack_window_set_deadline(&window,13,id,20000u+id));
    }
    CHECK(hop_xfer_ack_window_deliver(&window,13,1,HOP_FILE_ACK_OK,11000));
    CHECK(hop_xfer_ack_window_deliver(&window,13,2,HOP_FILE_ACK_OK,12000));
    checkpoint=0;
    CHECK(hop_xfer_ack_window_take(&window,13,1,&status,&rtt_ns));
    CHECK(status==HOP_FILE_ACK_OK);
    CHECK(hop_xfer_ack_window_advance_checkpoint(&checkpoint,0,1));
    CHECK(hop_xfer_ack_window_take(&window,13,2,&status,&rtt_ns));
    CHECK(status==HOP_FILE_ACK_OK);
    CHECK(hop_xfer_ack_window_advance_checkpoint(&checkpoint,1,1));
    CHECK(!hop_xfer_ack_window_take(&window,13,3,&status,&rtt_ns));
    CHECK(checkpoint==2);
    hop_xfer_ack_window_unregister_batch(&window,13);
    CHECK(hop_xfer_ack_window_count(&window)==0);
}

static void test_xfer_buffer_pool(void) {
    hop_xfer_buffer_pool pool;
    hop_xfer_buffer_pool_init(&pool);

    /* A later larger idle slot must win over an earlier undersized slot. */
    pool.slots[0].data = (uint8_t *)malloc(8);
    pool.slots[0].capacity = 8;
    pool.slots[1].data = (uint8_t *)malloc(64);
    pool.slots[1].capacity = 64;
    pool.capacity = 72;
    CHECK(pool.slots[0].data != NULL && pool.slots[1].data != NULL);
    int slot = -1;
    uint8_t *data = NULL;
    CHECK(hop_xfer_buffer_pool_acquire_locked(&pool, 32, &slot, &data) == 1);
    CHECK(slot == 1 && data == pool.slots[1].data && pool.capacity == 72);
    hop_xfer_buffer_pool_release_locked(&pool, slot);
    CHECK(!pool.slots[1].in_use);
    /* After a drain, retain only the largest idle buffer within the cache cap. */
    CHECK(hop_xfer_buffer_pool_trim_idle_locked(&pool, 32));
    CHECK(pool.capacity == 8 && pool.slots[0].data != NULL && pool.slots[1].data == NULL);
    hop_xfer_budget_snapshot snapshot;
    hop_xfer_budget_get_snapshot(&snapshot);
    CHECK(snapshot.rx_pool_bytes == 8);
    CHECK(hop_xfer_buffer_pool_trim_idle_locked(&pool, 0));
    CHECK(pool.capacity == 0 && pool.slots[0].data == NULL);
    hop_xfer_buffer_pool_dispose(&pool);

    /* An in-use slot prevents trimming; after release an oversized idle slot
     * is dropped rather than retained beyond the configured idle budget. */
    hop_xfer_buffer_pool_init(&pool);
    slot = -1; data = NULL;
    CHECK(hop_xfer_buffer_pool_acquire_locked(&pool, 128, &slot, &data) == 1);
    CHECK(slot >= 0 && data != NULL && pool.capacity == 128);
    CHECK(!hop_xfer_buffer_pool_trim_idle_locked(&pool, 64));
    CHECK(pool.capacity == 128 && pool.slots[slot].in_use);
    hop_xfer_buffer_pool_release_locked(&pool, slot);
    CHECK(hop_xfer_buffer_pool_trim_idle_locked(&pool, 64));
    CHECK(pool.capacity == 0 && pool.slots[slot].data == NULL);
    hop_xfer_buffer_pool_dispose(&pool);
    hop_xfer_budget_get_snapshot(&snapshot);
    CHECK(snapshot.rx_pool_bytes == 0);
}

static void test_xfer_rx_queue(void) {
    hop_xfer_rx_queue q;
    hop_xfer_rx_queue_init(&q, 2, 100);
    CHECK(hop_xfer_rx_queue_can_push(&q, 60));
    CHECK(hop_xfer_rx_queue_reserve(&q, 60));
    CHECK(!hop_xfer_rx_queue_can_push(&q, 50));
    CHECK(hop_xfer_rx_queue_commit_reserved(&q));
    CHECK(q.queued_slots == 1 && q.reserved_slots == 0 && q.queued_bytes == 60);
    CHECK(hop_xfer_rx_queue_dequeue(&q));
    CHECK(!hop_xfer_rx_queue_dequeue(&q)); /* single writer; no second in-flight task */
    CHECK(q.queued_slots == 0 && q.busy_slots == 1);
    CHECK(hop_xfer_rx_queue_can_push(&q, 40)); /* slot available, bytes still account for active write */
    CHECK(hop_xfer_rx_queue_push(&q, 40));
    CHECK(!hop_xfer_rx_queue_can_push(&q, 1)); /* slot cap reached by busy + queued */
    hop_xfer_rx_queue_complete(&q, 60);
    CHECK(q.busy_slots == 0 && q.queued_bytes == 40);
    CHECK(hop_xfer_rx_queue_dequeue(&q));
    hop_xfer_rx_queue_complete(&q, 40);
    CHECK(hop_xfer_rx_queue_drained(&q));
    CHECK(q.queued_bytes == 0);

    CHECK(hop_xfer_rx_queue_reserve(&q, 75));
    hop_xfer_rx_queue_rollback(&q, 75);
    CHECK(hop_xfer_rx_queue_drained(&q));
    CHECK(q.queued_bytes == 0);
}

static void test_xfer_rate_limiter(void) {
    hop_xfer_rate_limiter rate;
    hop_xfer_rate_init(&rate,1000,100,2000,100,0);
    CHECK(rate.current_rate_bytes_per_sec==1000&&rate.tokens_bytes==100);
    CHECK(hop_xfer_rate_wait_ns(&rate,50,0)==0);
    CHECK(rate.tokens_bytes==50);
    CHECK(hop_xfer_rate_wait_ns(&rate,100,0)==50000000ull);
    CHECK(rate.tokens_bytes==50);
    CHECK(hop_xfer_rate_wait_ns(&rate,100,25000000ull)==25000000ull);
    CHECK(rate.tokens_bytes==75);
    CHECK(hop_xfer_rate_wait_ns(&rate,100,50000000ull)==0);
    CHECK(rate.tokens_bytes==0);
    CHECK(hop_xfer_rate_wait_ns(&rate,50,100000000ull)==0);
    CHECK(hop_xfer_rate_wait_ns(&rate,101,100000000ull)==UINT64_MAX);

    hop_xfer_rate_init_from_config(&rate,8,1024,0);
    CHECK(rate.min_rate_bytes_per_sec==4ull*HOP_XFER_RATE_MIB_BYTES);
    CHECK(rate.max_rate_bytes_per_sec==16ull*HOP_XFER_RATE_MIB_BYTES);
    CHECK(rate.current_rate_bytes_per_sec==16ull*HOP_XFER_RATE_MIB_BYTES);
    hop_xfer_rate_init_from_config(&rate,9000,1024,0);
    CHECK(rate.max_rate_bytes_per_sec==4096ull*HOP_XFER_RATE_MIB_BYTES);
    CHECK(rate.current_rate_bytes_per_sec==64ull*HOP_XFER_RATE_MIB_BYTES);
    CHECK(hop_xfer_rate_queue_pressure_permille(7ull*1024ull*1024ull,0)==875);
    CHECK(hop_xfer_rate_queue_pressure_permille(0,6)==750);
    CHECK(hop_xfer_rate_queue_pressure_permille(UINT64_MAX,UINT32_MAX)==1000);

    hop_xfer_rate_init(&rate,1000,100,2000,100,0);
    hop_xfer_rate_note_send_time(&rate,100,1000);
    hop_xfer_rate_feedback(&rate,50000,0);
    CHECK(rate.current_rate_bytes_per_sec==1020&&rate.rate_increases==1);
    uint64_t healthy_rate=rate.current_rate_bytes_per_sec;
    hop_xfer_rate_feedback(&rate,1000000,900);
    CHECK(rate.current_rate_bytes_per_sec<healthy_rate&&rate.rate_decreases==1);

    hop_xfer_rate_init(&rate,1000,100,2000,100,0);
    for(int i=0;i<8;i++)hop_xfer_rate_note_send_time(&rate,100,250000);
    CHECK(rate.send_congested==1);
    CHECK(rate.current_rate_bytes_per_sec==1000&&rate.rate_decreases==0);
    hop_xfer_rate_feedback(&rate,0,0);
    CHECK(rate.current_rate_bytes_per_sec==800&&rate.rate_decreases==1);

    /* A 4 MiB chunk naturally takes ~32 ms at 128 MiB/s. That is not
     * congestion when the observed socket write is close to the allowance. */
    hop_xfer_rate_init(&rate,128ull*1024ull*1024ull,64ull*1024ull*1024ull,
                       512ull*1024ull*1024ull,8ull*1024ull*1024ull,0);
    for(int i=0;i<8;i++)hop_xfer_rate_note_send_time(&rate,4ull*1024ull*1024ull,35000);
    CHECK(rate.send_congested==0);
    CHECK(rate.current_rate_bytes_per_sec==128ull*1024ull*1024ull);
    hop_xfer_rate_feedback(&rate,50000,0);
    CHECK(rate.current_rate_bytes_per_sec==128ull*1024ull*1024ull+
          (512ull*1024ull*1024ull)/100ull);

    /* Healthy chunk streaming decays a stale high ACK RTT from an earlier file. */
    hop_xfer_rate_init(&rate,1000,100,2000,100,0);
    hop_xfer_rate_feedback(&rate,1000000,0);
    CHECK(rate.ack_rtt_ewma_us==1000000&&rate.rate_decreases==1);
    for(int i=0;i<32;i++)hop_xfer_rate_feedback(&rate,0,0);
    CHECK(rate.ack_rtt_ewma_us<100000&&rate.rate_increases>0);

    /* Per-channel decoder limit rejects control frames above HOP_CTRL_MAX_PAYLOAD. */
    {
        uint8_t hdr[HOP_HEADER_SIZE];
        hop_encode_header(hdr, HOP_MSG_CLIP_TEXT, HOP_CTRL_MAX_PAYLOAD + 1u);
        hop_decoder *ctrl = hop_decoder_new_with_limit(NULL, NULL, HOP_CTRL_MAX_PAYLOAD);
        CHECK(ctrl != NULL);
        CHECK(!hop_decoder_feed(ctrl, hdr, sizeof(hdr)));
        CHECK(hop_decoder_failed(ctrl));
        hop_decoder_free(ctrl);
    }
}

static void test_xfer_task_image_buffer_transfer(void) {
    hop_xfer_budget_limits limits={4u,8u,4096u,2048u,2048u,2u,1024u,1024u};
    hop_xfer_budget_snapshot snap;
    hop_xfer_task source,target;
    memset(&source,0,sizeof(source));
    memset(&target,0,sizeof(target));
    CHECK(hop_xfer_budget_set_limits_for_test(&limits));
    CHECK(hop_xfer_task_init(&source,0,0,201));
    CHECK(hop_xfer_task_init(&target,0,0,202));
    CHECK(hop_xfer_task_reserve_job(&source));
    CHECK(hop_xfer_task_reserve_job(&target));
    CHECK(hop_xfer_task_set_state(&source,HOP_XFER_TASK_ACTIVE));
    CHECK(hop_xfer_task_reserve_image_temp(&source,512));
    CHECK(hop_xfer_task_transfer_image_temp(&source,&target,256));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==2&&snap.entries==1&&snap.pending_bytes==256);
    CHECK(snap.image_bytes==512&&snap.image_temp_bytes==256);
    CHECK(snap.transferred_image_temp_bytes==256);
    CHECK(hop_xfer_task_release_image_temp(&source,256));
    CHECK(hop_xfer_task_finish(&source,HOP_XFER_TASK_COMPLETED));
    CHECK(hop_xfer_task_set_state(&target,HOP_XFER_TASK_QUEUED));
    CHECK(hop_xfer_task_finish(&target,HOP_XFER_TASK_COMPLETED));
    hop_xfer_task_dispose(&source);
    hop_xfer_task_dispose(&target);
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==0&&snap.entries==0&&snap.pending_bytes==0);
    CHECK(snap.image_bytes==0&&snap.image_temp_bytes==0);
    hop_xfer_budget_limits defaults={16u,32768u,8ull*1024ull*1024ull*1024ull,256ull*1024ull*1024ull,256ull*1024ull*1024ull,8u,8ull*1024ull*1024ull,512ull*1024ull*1024ull};
    CHECK(hop_xfer_budget_set_limits_for_test(&defaults));
}

static void test_xfer_tracking_stage_timing(void) {
    static hop_xfer_tracking t;
    memset(&t,0,sizeof(t));
    hop_xfer_tracking_begin(&t,77,HOP_XFER_DIR_SEND,1,100,"timing",HOP_XFER_SENDING);
    hop_xfer_tracking_add_time(&t,77,HOP_XFER_TIME_SCAN,1500000ull);
    hop_xfer_tracking_add_time(&t,77,HOP_XFER_TIME_SCAN,2500000ull);
    hop_xfer_tracking_add_time(&t,77,HOP_XFER_TIME_WRITE,7000000ull);
    CHECK(t.stage_ns[HOP_XFER_TIME_SCAN]==4000000ull);
    CHECK(t.stage_ns[HOP_XFER_TIME_WRITE]==7000000ull);
    hop_xfer_tracking_add_time(&t,77,-1,999);
    hop_xfer_tracking_add_time(&t,77,HOP_XFER_TIME_COUNT,999);
    hop_xfer_tracking_add_time(&t,77,HOP_XFER_TIME_SCAN,UINT64_MAX);
    CHECK(t.stage_ns[HOP_XFER_TIME_SCAN]==UINT64_MAX);
    CHECK(hop_xfer_tracking_end(&t,77,HOP_XFER_COMPLETED,"done"));
    hop_xfer_tracking_add_time(&t,77,HOP_XFER_TIME_SEND,1000);
    CHECK(t.stage_ns[HOP_XFER_TIME_SEND]==0); /* terminal tasks cannot accrue time */
    hop_xfer_tracking_dispose(&t);
}

static void test_xfer_tracking_reset_and_sparse_ids(void) {
    static hop_xfer_tracking t;
    CHECK(sizeof(hop_xfer_tracking) < 2048); /* records and paths live outside the tracker */
    CHECK(sizeof(hop_xfer_file_record) < 64);
    hop_xfer_tracking_begin(&t, 1, HOP_XFER_DIR_RECEIVE, 4, 0, "old", HOP_XFER_RECEIVING);
    CHECK(t.capacity == 4 && t.files != NULL);
    hop_xfer_tracking_file(&t, 1, 1, "old-first", 100, 0, HOP_XFER_FILE_RECEIVING);
    hop_xfer_tracking_chunk(&t, 1, 1, 0, 100, 1000);
    hop_xfer_tracking_phase(&t, 1, 0, HOP_XFER_FILE_COMPLETED, 1);
    hop_xfer_tracking_file(&t, 1, 4, "old-tail", 1, 0, HOP_XFER_FILE_RECEIVING);
    hop_xfer_tracking_add_time(&t, 1, HOP_XFER_TIME_RECEIVE, 100);
    t.revision = 2;
    uint64_t revision_before_reset = t.revision;

    hop_xfer_tracking_reset(&t);
    CHECK(t.count == 0 && !t.status.active && t.status.batch == 0 && t.revision == revision_before_reset);
    CHECK(t.sample_ms == 0 && t.sample_bytes == 0 && t.stage_ns[HOP_XFER_TIME_RECEIVE] == 0);
    CHECK(t.capacity == 4 && t.files != NULL);
    CHECK(t.path_storage == NULL && t.path_used == 0 && t.path_capacity == 0);

    hop_xfer_tracking_begin(&t, 2, HOP_XFER_DIR_RECEIVE, 3, 0, "new", HOP_XFER_RECEIVING);
    CHECK(t.revision > revision_before_reset);
    CHECK(t.count == 0 && t.capacity == 3); /* the old four-entry table shrinks */

    hop_xfer_tracking_file(&t, 2, 3, "new-third", 30, 0, HOP_XFER_FILE_RECEIVING);
    CHECK(t.count == 3 && t.status.bytes_total == 30 && t.status.files_done == 0);
    CHECK(t.path_used == strlen("new-third") + 1);
    hop_xfer_file_status snapshot[4];
    CHECK(hop_xfer_tracking_files_page(&t, 2, 0, snapshot, 3, NULL, NULL) == 3);
    CHECK(snapshot[0].relative_path[0] == '\0' && snapshot[0].bytes_total == 0 &&
          snapshot[0].bytes_done == 0 && snapshot[0].phase == HOP_XFER_FILE_QUEUED);
    CHECK(snapshot[1].relative_path[0] == '\0' && snapshot[1].bytes_total == 0);
    CHECK(strcmp(snapshot[2].relative_path, "new-third") == 0);

    hop_xfer_tracking_file(&t, 2, 1, "new-first", 10, 0, HOP_XFER_FILE_RECEIVING);
    hop_xfer_tracking_chunk(&t, 1, 1, 0, 10, 1100);
    CHECK(t.status.bytes_total == 40 && t.status.bytes_done == 0);
    CHECK(t.path_used == strlen("new-third") + strlen("new-first") + 2);
    CHECK(t.path_capacity <= (size_t)HOP_XFER_FILE_LIMIT * sizeof(snapshot[0].relative_path));
    CHECK(hop_xfer_tracking_files_page(&t, 2, 0, snapshot, 3, NULL, NULL) == 3);
    CHECK(strcmp(snapshot[0].relative_path, "new-first") == 0);
    CHECK(strcmp(snapshot[2].relative_path, "new-third") == 0);
    size_t path_used_before_retry = t.path_used;
    hop_xfer_tracking_file(&t, 2, 1, "new-first", 10, 0, HOP_XFER_FILE_RECEIVING);
    CHECK(t.path_used == path_used_before_retry);
    CHECK(!hop_xfer_tracking_reserve(&t, HOP_XFER_FILE_LIMIT + 1u));
    CHECK(t.capacity == 3);

    /* An empty next batch releases the previous batch's retained file table;
     * a later sparse file ID grows only as far as that ID. */
    hop_xfer_tracking_begin(&t, 3, HOP_XFER_DIR_RECEIVE, 0, 0, "empty", HOP_XFER_RECEIVING);
    CHECK(t.files == NULL && t.capacity == 0 && t.count == 0);
    CHECK(t.path_storage == NULL && t.path_used == 0 && t.path_capacity == 0);
    hop_xfer_tracking_file(&t, 3, 2, "late-entry", 1, 0, HOP_XFER_FILE_RECEIVING);
    CHECK(t.capacity == 2 && t.count == 2);
    CHECK(t.path_used == strlen("late-entry") + 1);
    hop_xfer_tracking_dispose(&t);
    CHECK(t.files == NULL && t.capacity == 0 && t.count == 0);
    CHECK(t.path_storage == NULL && t.path_used == 0 && t.path_capacity == 0);
}

static void test_xfer_tracking_file_pages(void) {
    static hop_xfer_tracking t;
    memset(&t, 0, sizeof(t));
    hop_xfer_tracking_begin(&t, 44, HOP_XFER_DIR_SEND, 3, 0, "page-test", HOP_XFER_SENDING);
    CHECK(t.capacity == 3);
    hop_xfer_tracking_file(&t, 44, 1, "one.txt", 10, 0, HOP_XFER_FILE_SENDING);
    hop_xfer_tracking_file(&t, 44, 2, "two.txt", 20, 0, HOP_XFER_FILE_QUEUED);
    hop_xfer_tracking_file(&t, 44, 3, "three.txt", 30, 0, HOP_XFER_FILE_QUEUED);

    int total = -1;
    uint64_t revision = 0;
    CHECK(hop_xfer_tracking_files_page(&t, 44, 0, NULL, 0, &total, &revision) == 0);
    CHECK(total == 3 && revision == t.revision);
    hop_xfer_file_status page[2];
    memset(page, 0, sizeof(page));
    int n = hop_xfer_tracking_files_page(&t, 44, 1, page, 2, &total, &revision);
    CHECK(n == 2 && total == 3);
    CHECK(strcmp(page[0].relative_path, "two.txt") == 0);
    CHECK(strcmp(page[1].relative_path, "three.txt") == 0);

    uint64_t before = revision;
    hop_xfer_tracking_chunk(&t, 44, 1, 0, 5, 1000);
    CHECK(t.revision > before);
    CHECK(hop_xfer_tracking_files_page(&t, 44, 0, page, 1, &total, &revision) == 1);
    CHECK(revision == t.revision && page[0].bytes_done == 5);
    CHECK(hop_xfer_tracking_files_page(&t, 45, 0, page, 1, &total, &revision) == 0);
    CHECK(total == 0 && revision == 0);
    CHECK(hop_xfer_tracking_files_page(&t, 44, 0, page, -1, &total, &revision) == -1);
    hop_xfer_tracking_dispose(&t);
}

static void test_xfer_tracking_duplex_snapshot(void) {
    /* File tables are heap-backed and grow only to the current batch size. */
    static hop_xfer_tracking tx, rx;
    hop_xfer_status_pair pair;
    memset(&tx,0,sizeof(tx)); memset(&rx,0,sizeof(rx));
    hop_xfer_tracking_snapshot_pair(&tx,&rx,0,&pair);
    CHECK(!pair.send.active && !pair.receive.active);
    hop_xfer_tracking_begin(&tx,77,HOP_XFER_DIR_SEND,1,0,"send",HOP_XFER_SENDING);
    CHECK(tx.capacity == 1);
    hop_xfer_tracking_file(&tx,77,1,"send",100,0,HOP_XFER_FILE_SENDING);
    hop_xfer_tracking_chunk(&tx,77,1,0,30,1000);
    /* Batch IDs can collide across directions without aliasing their state. */
    hop_xfer_tracking_begin(&rx,77,HOP_XFER_DIR_RECEIVE,1,0,"receive",HOP_XFER_RECEIVING);
    CHECK(rx.capacity == 1);
    hop_xfer_tracking_file(&rx,77,1,"receive",200,0,HOP_XFER_FILE_RECEIVING);
    hop_xfer_tracking_chunk(&rx,77,1,0,40,1000);
    tx.revision=1; rx.revision=2;
    hop_xfer_tracking_snapshot_pair(&tx,&rx,3,&pair);
    CHECK(pair.send.active && pair.receive.active && pair.primary_direction==HOP_XFER_DIR_RECEIVE);
    CHECK(pair.send.batch==77 && pair.receive.batch==77);
    CHECK(pair.send.bytes_done==30 && pair.send.bytes_total==100);
    CHECK(pair.receive.bytes_done==40 && pair.receive.bytes_total==200);
    CHECK(pair.send.queued_count==3 && pair.receive.queued_count==3);
    CHECK(hop_xfer_tracking_end(&rx,77,HOP_XFER_COMPLETED,"ok"));
    hop_xfer_tracking_snapshot_pair(&tx,&rx,0,&pair);
    CHECK(pair.primary_direction==HOP_XFER_DIR_SEND && pair.send.active && !pair.receive.active);
    CHECK(pair.receive.phase==HOP_XFER_COMPLETED && pair.send.bytes_done==30);
    /* Copies must not change retroactively when trackers advance. */
    hop_xfer_tracking_chunk(&tx,77,1,30,20,1200);
    CHECK(pair.send.bytes_done==30 && tx.status.bytes_done==50);
    hop_xfer_tracking_dispose(&tx);
    hop_xfer_tracking_dispose(&rx);
}

static void test_xfer_tracking_speed_and_eta(void) {
    static hop_xfer_tracking tracking;
    memset(&tracking,0,sizeof(tracking));
    hop_xfer_tracking_begin(&tracking,7,HOP_XFER_DIR_SEND,1,0,"large.bin",HOP_XFER_SENDING);
    hop_xfer_tracking_file(&tracking,7,1,"large.bin",1000000,0,HOP_XFER_FILE_SENDING);
    hop_xfer_tracking_chunk(&tracking,7,1,0,100000,1000);
    CHECK(tracking.status.bytes_done==100000);
    CHECK(tracking.status.speed_bytes_per_sec==0);
    CHECK(tracking.status.last_progress_ms==1000);
    hop_xfer_tracking_chunk(&tracking,7,1,100000,100000,2000);
    CHECK(tracking.status.bytes_done==200000);
    CHECK(tracking.status.speed_bytes_per_sec==100000);
    CHECK(tracking.status.eta_seconds==8);
    CHECK(tracking.status.last_progress_ms==2000);

    /* A long pause must not be averaged into the next active throughput sample. */
    hop_xfer_tracking_chunk(&tracking,7,1,200000,100000,5000);
    CHECK(tracking.status.speed_bytes_per_sec==0&&tracking.status.eta_seconds==0);
    hop_xfer_tracking_chunk(&tracking,7,1,300000,100000,6000);
    CHECK(tracking.status.speed_bytes_per_sec==100000);
    CHECK(tracking.status.eta_seconds==6);

    /* Re-registering a retry rolls progress and its rate sample back. */
    hop_xfer_tracking_file(&tracking,7,1,"large.bin",1000000,0,HOP_XFER_FILE_SENDING);
    CHECK(tracking.status.bytes_done==0);
    CHECK(tracking.status.speed_bytes_per_sec==0);
    CHECK(tracking.status.last_progress_ms==0);
    CHECK(hop_xfer_tracking_end(&tracking,7,HOP_XFER_COMPLETED,"done"));
    CHECK(tracking.status.speed_bytes_per_sec==0&&tracking.status.eta_seconds==0);
    hop_xfer_tracking_dispose(&tracking);
}

static void test_xfer_task_identity_and_lifecycle(void) {
    hop_xfer_budget_limits limits={4u,8u,4096u,2048u,2048u,2u,1024u,1024u};
    hop_xfer_budget_snapshot snap;
    hop_xfer_task a,b,moved;
    memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));memset(&moved,0,sizeof(moved));
    CHECK(hop_xfer_budget_set_limits_for_test(&limits));
    CHECK(hop_xfer_task_init_classified(&a,UINT64_C(0xABCDEF),UINT32_C(0x80000001),123,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_UNSPECIFIED));
    CHECK(hop_xfer_task_init(&b,0,0,124));
    CHECK(a.id!=0&&b.id!=0&&a.id!=b.id);
    hop_xfer_task_snapshot as,bs;
    CHECK(hop_xfer_task_get_snapshot(a.id,&as) && hop_xfer_task_get_snapshot(b.id,&bs));
    CHECK(as.offer_id==UINT64_C(0xABCDEF)&&as.request_id==UINT32_C(0x80000001)&&as.transfer_id==123);
    CHECK(bs.offer_id==0&&bs.request_id==0&&bs.transfer_id==124);
    CHECK(as.reservation_handle_id && bs.reservation_handle_id && as.reservation_handle_id!=bs.reservation_handle_id);
    CHECK(as.state==HOP_XFER_TASK_CREATED && as.direction==HOP_XFER_DIR_RECEIVE && as.kind==HOP_TASK_KIND_UNSPECIFIED);
    uint8_t session[HOP_TASK_SESSION_BYTES]={1}, zero_session[HOP_TASK_SESSION_BYTES]={0};
    CHECK(!hop_xfer_task_bind_session(&a,zero_session));
    CHECK(hop_xfer_task_bind_session(&a,session));
    CHECK(hop_xfer_task_set_kind(&a,HOP_TASK_KIND_FILES));
    CHECK(hop_xfer_task_set_kind(&a,HOP_TASK_KIND_IMAGE));
    CHECK(hop_xfer_task_set_kind(&a,HOP_TASK_KIND_FILES));
    CHECK(!hop_xfer_task_set_kind(&a,HOP_TASK_KIND_IMAGE_WORK));
    CHECK(hop_xfer_task_get_snapshot(a.id,&as) && as.kind==HOP_TASK_KIND_MIXED && !memcmp(as.session,session,sizeof(session)));
    session[0]=2; CHECK(hop_xfer_task_bind_session(&a,session));
    CHECK(hop_xfer_task_get_snapshot(a.id,&as) && !memcmp(as.session,session,sizeof(session)));
    int total=0; hop_xfer_task_snapshot list[2];
    CHECK(hop_xfer_task_list(list,2,&total)==2 && total==2);
    hop_xfer_task copied=a; /* A copied handle must never mutate or release the owner. */
    CHECK(!hop_xfer_task_reserve_job(&copied));
    hop_xfer_task_dispose(&copied);
    CHECK(hop_xfer_task_get_snapshot(a.id,&as));
    CHECK(!hop_xfer_task_init(&a,0,0,456)); /* cannot replace an existing owner */
    CHECK(hop_xfer_task_reserve_job(&a));
    CHECK(hop_xfer_task_reserve_entry(&a,100));
    CHECK(hop_xfer_task_reserve_image(&a,400));
    CHECK(hop_xfer_task_reserve_rx_buffer(&a,128));
    CHECK(hop_xfer_task_reserve_image_temp(&a,256));
    CHECK(hop_xfer_task_set_state(&a,HOP_XFER_TASK_SCANNING));
    CHECK(hop_xfer_task_set_state(&a,HOP_XFER_TASK_QUEUED));
    CHECK(hop_xfer_task_set_state(&a,HOP_XFER_TASK_ACTIVE));
    CHECK(hop_xfer_task_set_state(&a,HOP_XFER_TASK_QUEUED)); /* retry */
    CHECK(hop_xfer_task_set_state(&a,HOP_XFER_TASK_ACTIVE));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==1&&snap.entries==2&&snap.pending_bytes==500&&snap.image_bytes==656);
    CHECK(snap.rx_queue_slots==1&&snap.rx_queue_bytes==128);
    CHECK(snap.image_temp_bytes==256);
    hop_xfer_task_audit audit;
    CHECK(hop_xfer_task_reconcile(&audit) && !audit.mismatches && audit.task_count==2);
    CHECK(audit.jobs==1 && audit.entries==2 && audit.pending_bytes==500 && audit.image_bytes==656);
    CHECK(audit.rx_queue_slots==1 && audit.rx_queue_bytes==128 && audit.image_temp_bytes==256);
    uint64_t task_releases_before=snap.released_reservations;
    CHECK(!hop_xfer_task_finish(&a,HOP_XFER_TASK_COMPLETED));
    CHECK(hop_xfer_task_release_rx_buffer(&a,128));
    CHECK(!hop_xfer_task_finish(&a,HOP_XFER_TASK_COMPLETED));
    CHECK(hop_xfer_task_release_image_temp(&a,256));
    CHECK(hop_xfer_task_finish(&a,HOP_XFER_TASK_COMPLETED));
    CHECK(!hop_xfer_task_finish(&a,HOP_XFER_TASK_CANCELLED));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.released_reservations==task_releases_before+1);
    CHECK(!hop_xfer_task_set_state(&a,HOP_XFER_TASK_ACTIVE));
    CHECK(!hop_xfer_task_bind_session(&a,session));
    CHECK(!hop_xfer_task_set_kind(&a,HOP_TASK_KIND_IMAGE));
    CHECK(!hop_xfer_task_reserve_entry(&a,1));
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==0&&snap.entries==0&&snap.pending_bytes==0&&snap.image_bytes==0);
    uint64_t b_id=b.id; hop_xfer_task stale=b;
    CHECK(hop_xfer_task_move(&moved,&b));
    CHECK(!b.initialized&&moved.initialized&&moved.id==b_id);
    CHECK(!hop_xfer_task_finish(&stale,HOP_XFER_TASK_CANCELLED));
    CHECK(hop_xfer_task_get_snapshot(b_id,&bs) && bs.state==HOP_XFER_TASK_CREATED);
    CHECK(hop_xfer_task_reserve_job(&moved));
    CHECK(hop_xfer_task_set_state(&moved,HOP_XFER_TASK_ACTIVE));
    CHECK(hop_xfer_task_finish(&moved,HOP_XFER_TASK_CANCELLED));
    hop_xfer_task_dispose(&a);
    hop_xfer_task_dispose(&a); /* Disposing the emptied handle is a no-op. */
    hop_xfer_task_dispose(&b);
    hop_xfer_task_dispose(&moved);
    CHECK(!hop_xfer_task_get_snapshot(b_id,&bs));
    CHECK(hop_xfer_task_list(NULL,0,&total)==0 && total==0);
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.jobs==0&&snap.entries==0&&snap.pending_bytes==0&&snap.image_bytes==0);
    CHECK(snap.released_reservations==task_releases_before+2);
    CHECK(hop_xfer_task_reconcile(&audit) && !audit.mismatches && audit.task_count==0);
    /* A rogue reservation is visible as a discrepancy, never silently repaired. */
    hop_xfer_reservation_handle rogue={0};
    CHECK(hop_xfer_budget_reservation_init(&rogue,999)); CHECK(hop_xfer_budget_reserve_job(&rogue));
    CHECK(hop_xfer_task_reconcile(&audit) && (audit.mismatches&HOP_TASK_AUDIT_USAGE));
    hop_xfer_budget_release(&rogue);
    CHECK(hop_xfer_task_reconcile(&audit) && !audit.mismatches);
    /* A corrupted active handle must remain owned when its counters cannot
     * be subtracted from the global ledger. Never zero it and lose the claim. */
    hop_xfer_reservation_handle inconsistent;
    memset(&inconsistent,0,sizeof(inconsistent));
    CHECK(hop_xfer_budget_reservation_init(&inconsistent,1004));
    CHECK(hop_xfer_budget_reserve_job(&inconsistent));
    hop_xfer_budget_get_snapshot(&snap);
    uint64_t errors_before=snap.accounting_errors;
    uint64_t releases_before=snap.released_reservations;
    inconsistent.jobs=2; /* Simulate a damaged local claim against one global Job. */
    CHECK(!hop_xfer_budget_release(&inconsistent));
    CHECK(inconsistent.active && inconsistent.jobs==2);
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.accounting_errors==errors_before+1 && snap.jobs==1);
    /* Failed return must preserve the exact claim so its owner can repair and retry. */
    inconsistent.jobs=1;
    CHECK(hop_xfer_budget_release(&inconsistent));
    CHECK(!inconsistent.active && !inconsistent.jobs);
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(!snap.jobs && snap.released_reservations==releases_before+1);
    CHECK(hop_xfer_budget_release(&inconsistent)); /* Idempotent after the one successful return. */
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.released_reservations==releases_before+1);

    hop_xfer_budget_limits defaults={16u,32768u,8ull*1024ull*1024ull*1024ull,256ull*1024ull*1024ull,256ull*1024ull*1024ull,8u,8ull*1024ull*1024ull,512ull*1024ull*1024ull};
    CHECK(hop_xfer_budget_set_limits_for_test(&defaults));
}

static void test_xfer_task_index_collision(void) {
    hop_xfer_task first={0}, moved={0}, tmp={0}, last={0};
    hop_xfer_task_snapshot snap;
    CHECK(hop_xfer_task_init(&first,0,0,1));
    CHECK(first.transfer_id==1);
    CHECK(hop_xfer_task_move(&moved,&first));
    CHECK(moved.transfer_id==1 && first.transfer_id==0);
    const uint64_t first_id=moved.id;
    /* Keep the first record alive while cycling HOP_TASK_INDEX_BUCKETS IDs
     * to force a real bucket collision, then remove both orders. */
    for (unsigned i=0; i<HOP_TASK_INDEX_BUCKETS-1u; ++i) {
        CHECK(hop_xfer_task_init(&tmp,0,0,2));
        hop_xfer_task_dispose(&tmp);
    }
    CHECK(hop_xfer_task_init(&last,0,0,3));
    const uint64_t last_id=last.id;
    CHECK(last_id-first_id==HOP_TASK_INDEX_BUCKETS);
    CHECK(hop_xfer_task_get_snapshot(first_id,&snap) && snap.transfer_id==1);
    CHECK(hop_xfer_task_get_snapshot(last_id,&snap) && snap.transfer_id==3);
    hop_xfer_task_dispose(&moved);
    CHECK(moved.transfer_id==0);
    CHECK(!hop_xfer_task_get_snapshot(first_id,&snap));
    CHECK(hop_xfer_task_get_snapshot(last_id,&snap) && snap.transfer_id==3);
    hop_xfer_task_dispose(&last);
    CHECK(!hop_xfer_task_get_snapshot(last_id,&snap));
}

typedef struct { hop_xfer_task_id id; int result; } cancel_race_context;
#ifdef _WIN32
static DWORD WINAPI cancel_race_worker(LPVOID p) {
    cancel_race_context *c=(cancel_race_context *)p; c->result=hop_xfer_task_request_cancel(c->id); return 0;
}
#else
static void *cancel_race_worker(void *p) {
    cancel_race_context *c=(cancel_race_context *)p; c->result=hop_xfer_task_request_cancel(c->id); return NULL;
}
#endif
static void test_xfer_task_cancel_isolation_and_completion(void) {
    hop_xfer_task send={0},receive={0},internal={0}; hop_xfer_task_snapshot snap;
    CHECK(hop_xfer_task_init_classified(&send,0,0,100,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
    CHECK(hop_xfer_task_init_classified(&receive,0,0,100,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_IMAGE));
    CHECK(hop_xfer_task_init_classified(&internal,0,0,100,HOP_XFER_DIR_NONE,HOP_TASK_KIND_IMAGE_WORK));
    CHECK(hop_xfer_task_reserve_job(&send)); CHECK(hop_xfer_task_reserve_job(&receive));
    CHECK(!hop_xfer_task_request_cancel(internal.id));
    CHECK(hop_xfer_task_request_cancel(send.id)); CHECK(hop_xfer_task_request_cancel(send.id));
    CHECK(hop_xfer_task_cancel_requested(send.id) && !hop_xfer_task_cancel_requested(receive.id));
    /* Cancellation is also an admission barrier for every new resource class. */
    CHECK(!hop_xfer_task_reserve_job(&send));
    CHECK(!hop_xfer_task_reserve_entry(&send, 16));
    CHECK(!hop_xfer_task_reserve_image(&send, 16));
    CHECK(!hop_xfer_task_reserve_rx_buffer(&send, 16));
    CHECK(!hop_xfer_task_begin_completion(&send)); CHECK(hop_xfer_task_begin_completion(&receive));
    CHECK(!hop_xfer_task_request_cancel(receive.id));
    CHECK(hop_xfer_task_get_snapshot(receive.id,&snap) && snap.completing && !snap.cancel_requested);
    CHECK(hop_xfer_task_finish(&send,HOP_XFER_TASK_CANCELLED));
    CHECK(hop_xfer_task_finish(&receive,HOP_XFER_TASK_COMPLETED));
    CHECK(!hop_xfer_task_request_cancel(send.id));
    hop_xfer_task_dispose(&send);hop_xfer_task_dispose(&receive);hop_xfer_task_dispose(&internal);
    hop_xfer_budget_snapshot budget;hop_xfer_budget_get_snapshot(&budget);
    CHECK(budget.jobs==0 && budget.entries==0 && budget.pending_bytes==0);
    for (int i=0;i<128;i++) {
        hop_xfer_task task={0}; cancel_race_context ctx={0};
        CHECK(hop_xfer_task_init_classified(&task,0,0,101,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
        ctx.id=task.id;
#ifdef _WIN32
        HANDLE th=CreateThread(NULL,0,cancel_race_worker,&ctx,0,NULL);
        CHECK(th!=NULL); if(!th){hop_xfer_task_dispose(&task);continue;}
        int completed=hop_xfer_task_begin_completion(&task);
        CHECK(WaitForSingleObject(th,INFINITE)==WAIT_OBJECT_0); CloseHandle(th);
#else
        pthread_t th; int created=pthread_create(&th,NULL,cancel_race_worker,&ctx)==0;
        CHECK(created); if(!created){hop_xfer_task_dispose(&task);continue;}
        int completed=hop_xfer_task_begin_completion(&task);
        CHECK(pthread_join(th,NULL)==0);
#endif
        CHECK((completed!=0) != (ctx.result!=0));
        CHECK(hop_xfer_task_finish(&task,completed?HOP_XFER_TASK_COMPLETED:HOP_XFER_TASK_CANCELLED));
        hop_xfer_task_dispose(&task);
    }
}


/* Timeout and cancel must have exactly one winner under the manager lock.
 * Equal transfer IDs on an unrelated sender must remain untouched. */
static void test_xfer_task_cancel_timeout_race(void) {
    for (int i=0;i<128;i++) {
        hop_xfer_task rx={0},tx={0}; cancel_race_context ctx={0}; hop_xfer_task_audit audit;
        CHECK(hop_xfer_task_init_classified(&rx,0,0,313,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_FILES));
        CHECK(hop_xfer_task_init_classified(&tx,0,0,313,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
        CHECK(hop_xfer_task_reserve_job(&rx)); CHECK(hop_xfer_task_reserve_job(&tx));
        CHECK(hop_xfer_task_set_state(&rx,HOP_XFER_TASK_ACTIVE));
        CHECK(!hop_xfer_task_rx_disconnect_expired(&rx,0,1,100));
        ctx.id=rx.id;
#ifdef _WIN32
        HANDLE th=CreateThread(NULL,0,cancel_race_worker,&ctx,0,NULL);
        CHECK(th!=NULL); if(!th){hop_xfer_task_dispose(&rx);hop_xfer_task_dispose(&tx);continue;}
        int expired=hop_xfer_task_rx_disconnect_expired(&rx,0,101,100);
        CHECK(WaitForSingleObject(th,INFINITE)==WAIT_OBJECT_0); CloseHandle(th);
#else
        pthread_t th; int created=pthread_create(&th,NULL,cancel_race_worker,&ctx)==0;
        CHECK(created); if(!created){hop_xfer_task_dispose(&rx);hop_xfer_task_dispose(&tx);continue;}
        int expired=hop_xfer_task_rx_disconnect_expired(&rx,0,101,100);
        CHECK(pthread_join(th,NULL)==0);
#endif
        CHECK((expired!=0)!=(ctx.result!=0));
        CHECK(!hop_xfer_task_cancel_requested(tx.id));
        CHECK(hop_xfer_task_finish(&rx,expired?HOP_XFER_TASK_FAILED:HOP_XFER_TASK_CANCELLED));
        CHECK(hop_xfer_task_reconcile(&audit) && !audit.mismatches && audit.jobs==1);
        CHECK(hop_xfer_task_finish(&tx,HOP_XFER_TASK_COMPLETED));
        hop_xfer_task_dispose(&rx);hop_xfer_task_dispose(&tx);
        CHECK(hop_xfer_task_reconcile(&audit) && !audit.mismatches && audit.jobs==0);
    }
}


/* Exercises shared manager and ledger concurrently, not real network I/O. Each
 * worker owns its handle; equal transfer IDs on opposite directions must not
 * alias. Audit while other threads reserve/release, not just after joining. */
typedef struct { int worker, errors; } task_stress_context;
static void task_stress_run(task_stress_context *c) {
    for (int i=0; i<1000; ++i) {
        hop_xfer_task task={0}; hop_xfer_task_snapshot snapshot;
        hop_xfer_task_audit audit; int has_rx=0, has_temp=0;
        int direction=(c->worker & 1)?HOP_XFER_DIR_RECEIVE:HOP_XFER_DIR_SEND;
        if (!hop_xfer_task_init_classified(&task,0,0,(uint32_t)(i+1),direction,HOP_TASK_KIND_FILES)) {
            ++c->errors; continue;
        }
        if (!hop_xfer_task_reserve_job(&task) || !hop_xfer_task_reserve_entry(&task,128) ||
            !hop_xfer_task_set_state(&task,HOP_XFER_TASK_ACTIVE)) { ++c->errors; goto cleanup; }
        if (direction==HOP_XFER_DIR_RECEIVE) {
            if (!hop_xfer_task_reserve_rx_buffer(&task,64)) { ++c->errors; goto cleanup; }
            has_rx=1;
        } else {
            if (!hop_xfer_task_reserve_image_temp(&task,64)) { ++c->errors; goto cleanup; }
            has_temp=1;
        }
        if (!hop_xfer_task_get_snapshot(task.id,&snapshot) || snapshot.direction!=direction ||
            snapshot.transfer_id!=(uint32_t)(i+1) || !hop_xfer_task_reconcile(&audit) || audit.mismatches)
            ++c->errors;
        if (has_rx) { if (!hop_xfer_task_release_rx_buffer(&task,64)) ++c->errors; else has_rx=0; }
        if (has_temp) { if (!hop_xfer_task_release_image_temp(&task,64)) ++c->errors; else has_temp=0; }
        if (i%3==0) {
            if (!hop_xfer_task_request_cancel(task.id) || hop_xfer_task_begin_completion(&task) ||
                !hop_xfer_task_finish(&task,HOP_XFER_TASK_CANCELLED)) ++c->errors;
        } else if (i%3==1) {
            if (!hop_xfer_task_begin_completion(&task) || hop_xfer_task_request_cancel(task.id) ||
                !hop_xfer_task_finish(&task,HOP_XFER_TASK_COMPLETED)) ++c->errors;
        } else if (!hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED)) ++c->errors;
cleanup:
        if (has_rx && !hop_xfer_task_release_rx_buffer(&task,64)) ++c->errors;
        if (has_temp && !hop_xfer_task_release_image_temp(&task,64)) ++c->errors;
        hop_xfer_task_dispose(&task);
        if (task.initialized) ++c->errors;
    }
}
#ifdef _WIN32
static DWORD WINAPI task_stress_worker(LPVOID p) { task_stress_run((task_stress_context *)p); return 0; }
#else
static void *task_stress_worker(void *p) { task_stress_run((task_stress_context *)p); return NULL; }
#endif
static void test_xfer_task_concurrent_ledger_stress(void) {
    enum { WORKERS=6 }; task_stress_context contexts[WORKERS]={0}; int created[WORKERS]={0};
#ifdef _WIN32
    HANDLE threads[WORKERS]={0};
#else
    pthread_t threads[WORKERS];
#endif
    for (int i=0; i<WORKERS; ++i) {
        contexts[i].worker=i;
#ifdef _WIN32
        threads[i]=CreateThread(NULL,0,task_stress_worker,&contexts[i],0,NULL);
        created[i]=threads[i]!=NULL;
#else
        created[i]=pthread_create(&threads[i],NULL,task_stress_worker,&contexts[i])==0;
#endif
        CHECK(created[i]);
    }
    for (int i=0; i<WORKERS; ++i) if (created[i]) {
#ifdef _WIN32
        CHECK(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0); CloseHandle(threads[i]);
#else
        CHECK(pthread_join(threads[i],NULL)==0);
#endif
        CHECK(contexts[i].errors==0);
    }
    hop_xfer_task_audit audit;
    CHECK(hop_xfer_task_reconcile(&audit) && !audit.mismatches && !audit.task_count);
    CHECK(!audit.jobs && !audit.entries && !audit.pending_bytes && !audit.image_temp_bytes &&
          !audit.rx_queue_slots && !audit.rx_queue_bytes);
}

#define RUN_TEST(fn) do { \
    fprintf(stderr, "RUN %s\\n", #fn); \
    fflush(stderr); \
    fn(); \
} while (0)


static int g_sink_calls;
static hop_xfer_task_snapshot g_sink_last;
static void counting_sink(const hop_xfer_task_snapshot *snap) {
    ++g_sink_calls; g_sink_last=*snap;
}

static void test_xfer_task_revisions(void) {
    hop_xfer_task_set_terminal_sink(NULL);
    uint64_t tasks_before = 0, history_before = 0;
    hop_xfer_task_get_revisions(&tasks_before, &history_before);
    hop_xfer_task task;
    memset(&task, 0, sizeof(task));
    CHECK(hop_xfer_task_init_classified(&task, 11, 12, 13,
        HOP_XFER_DIR_SEND, HOP_TASK_KIND_FILES));
    uint64_t tasks_created = 0, history_created = 0;
    hop_xfer_task_get_revisions(&tasks_created, &history_created);
    CHECK(tasks_created > tasks_before && history_created == history_before);
    int live_total = 0;
    uint64_t list_revision = 0;
    CHECK(hop_xfer_task_list_revisioned(NULL, 0, &live_total, &list_revision) == 0);
    CHECK(live_total >= 1 && list_revision == tasks_created);

    CHECK(hop_xfer_task_set_state(&task, HOP_XFER_TASK_SCANNING));
    CHECK(hop_xfer_task_set_summary(&task, 1, 2, 10, 20, "revision.bin", "scanning"));
    uint64_t tasks_updated = 0;
    hop_xfer_task_get_revisions(&tasks_updated, NULL);
    CHECK(tasks_updated > tasks_created);

    CHECK(hop_xfer_task_finish(&task, HOP_XFER_TASK_COMPLETED));
    uint64_t tasks_finished = 0, history_finished = 0;
    hop_xfer_task_get_revisions(&tasks_finished, &history_finished);
    CHECK(tasks_finished > tasks_updated && history_finished > history_before);

    hop_xfer_task_dispose(&task);
    uint64_t tasks_disposed = 0, history_disposed = 0;
    hop_xfer_task_get_revisions(&tasks_disposed, &history_disposed);
    CHECK(tasks_disposed > tasks_finished && history_disposed == history_finished);
}

static void test_xfer_task_summary_and_sink(void) {
    hop_xfer_task_set_terminal_sink(NULL);
    g_sink_calls=0;
    hop_xfer_task task;
    memset(&task,0,sizeof(task));
    CHECK(hop_xfer_task_init_classified(&task,1,2,3,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
    CHECK(hop_xfer_task_set_summary(&task,4,5,100,200,"report.zip","7z done"));
    CHECK(hop_xfer_task_set_terminal_sink(counting_sink));
    CHECK(hop_xfer_task_finish(&task,HOP_XFER_TASK_COMPLETED));
    CHECK(g_sink_calls==1);
    CHECK(g_sink_last.id==task.id && g_sink_last.state==HOP_XFER_TASK_COMPLETED);
    CHECK(g_sink_last.files_done==4 && g_sink_last.files_total==5);
    CHECK(g_sink_last.bytes_done==100 && g_sink_last.bytes_total==200);
    CHECK(strcmp(g_sink_last.name,"report.zip")==0);
    CHECK(strcmp(g_sink_last.detail,"7z done")==0);
    CHECK(g_sink_last.finished_at>0);
    /* summary is display-only: rejected once terminal */
    CHECK(!hop_xfer_task_set_summary(&task,9,9,9,9,"x","y"));
    /* a second finish never happens: the sink fires exactly once */
    CHECK(!hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED));
    CHECK(g_sink_calls==1);
    hop_xfer_task_snapshot hist[HOP_TASK_HISTORY_CAPACITY];
    int total=0;
    int n=hop_xfer_task_history_list(hist,HOP_TASK_HISTORY_CAPACITY,
                                     HOP_XFER_DIR_SEND,HOP_XFER_TASK_COMPLETED,1,&total);
    CHECK(n>=1 && total>=1);
    int found=0;
    for(int i=0;i<n;++i)
        if(hist[i].id==task.id) {
            found=1;
            CHECK(hist[i].files_done==4 && hist[i].files_total==5);
            CHECK(strcmp(hist[i].name,"report.zip")==0);
            CHECK(strcmp(hist[i].detail,"7z done")==0);
            CHECK(hist[i].finished_at>0);
        }
    CHECK(found);
    /* dispose without finish auto-interrupts and fires the sink once */
    hop_xfer_task t2;
    memset(&t2,0,sizeof(t2));
    CHECK(hop_xfer_task_init_classified(&t2,1,2,4,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_FILES));
    hop_xfer_task_dispose(&t2);
    CHECK(g_sink_calls==2 && g_sink_last.state==HOP_XFER_TASK_INTERRUPTED);
    /* clearing the sink stops notifications */
    CHECK(hop_xfer_task_set_terminal_sink(NULL));
    hop_xfer_task t3;
    memset(&t3,0,sizeof(t3));
    CHECK(hop_xfer_task_init_classified(&t3,1,2,5,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
    CHECK(hop_xfer_task_finish(&t3,HOP_XFER_TASK_CANCELLED));
    CHECK(g_sink_calls==2);
    hop_xfer_task_dispose(&t3);
    hop_xfer_task_dispose(&task);
}

static void test_xfer_task_history_store_persistence(void) {
    hop_xfer_task_history_store_close();
    remove("hop-core-history-store.bin");
    remove("hop-core-history-store.bin.tmp");
    CHECK(!hop_xfer_task_history_store_is_open());
    CHECK(hop_xfer_task_history_store_open("hop-core-history-store.bin",NULL));
    CHECK(hop_xfer_task_history_store_is_open());
    hop_xfer_task a;
    memset(&a,0,sizeof(a));
    CHECK(hop_xfer_task_init_classified(&a,1,7,70,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
    CHECK(hop_xfer_task_set_summary(&a,1,2,10,20,"a.txt","ok"));
    CHECK(hop_xfer_task_finish(&a,HOP_XFER_TASK_COMPLETED));
    hop_xfer_task b;
    memset(&b,0,sizeof(b));
    CHECK(hop_xfer_task_init_classified(&b,1,8,80,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_FILES));
    CHECK(hop_xfer_task_set_summary(&b,0,1,0,5,"b.png","missing"));
    CHECK(hop_xfer_task_finish(&b,HOP_XFER_TASK_FAILED));
    hop_xfer_task_history_record recs[8];
    int total=-1;
    int n=hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,1,&total);
    CHECK(n==2 && total==2);
    CHECK(recs[0].record_id==2);
    CHECK(recs[0].state==HOP_XFER_TASK_FAILED && recs[0].direction==HOP_XFER_DIR_RECEIVE);
    CHECK(recs[0].transfer_id==80 && strcmp(recs[0].name,"b.png")==0);
    CHECK(recs[1].record_id==1 && recs[1].files_total==2 && strcmp(recs[1].name,"a.txt")==0);
    CHECK(recs[1].task_id==a.id);
    n=hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_SEND,HOP_XFER_TASK_EMPTY,0,&total);
    CHECK(n==1 && recs[0].direction==HOP_XFER_DIR_SEND);
    n=hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_NONE,HOP_XFER_TASK_FAILED,0,&total);
    CHECK(n==1 && recs[0].state==HOP_XFER_TASK_FAILED);
    n=hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_NONE,HOP_XFER_TASK_CANCELLED,0,&total);
    CHECK(n==0 && total==0);
    CHECK(hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,2,&total)==-1);
    CHECK(hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_NONE,HOP_XFER_TASK_ACTIVE,0,&total)==-1);
    hop_xfer_task_dispose(&a);
    hop_xfer_task_dispose(&b);
    /* reopen simulates a restart: records survive, ids continue from the file */
    hop_xfer_task_history_store_close();
    CHECK(hop_xfer_task_history_store_open("hop-core-history-store.bin",NULL));
    n=hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,0,&total);
    CHECK(n==2 && total==2);
    CHECK(recs[0].record_id==1 && recs[1].record_id==2);
    hop_xfer_task c;
    memset(&c,0,sizeof(c));
    CHECK(hop_xfer_task_init_classified(&c,1,9,90,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
    CHECK(hop_xfer_task_set_summary(&c,3,3,30,30,"c.bin","done"));
    CHECK(hop_xfer_task_finish(&c,HOP_XFER_TASK_CANCELLED));
    n=hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,1,&total);
    CHECK(n==3 && recs[0].record_id==3 && recs[0].transfer_id==90);
    hop_xfer_task_dispose(&c);
    hop_xfer_task_history_store_close();
}

static void write_legacy_history_file(const char *path) {
    FILE *f=fopen(path,"wb");
    CHECK(f!=NULL);
    static const char magic[8]={'H','O','P','X','H','S','T','1'};
    uint32_t version=1,record_size=(uint32_t)sizeof(hop_xfer_history_entry),count=2;
    CHECK(fwrite(magic,8,1,f)==1);
    CHECK(fwrite(&version,sizeof(version),1,f)==1);
    CHECK(fwrite(&record_size,sizeof(record_size),1,f)==1);
    CHECK(fwrite(&count,sizeof(count),1,f)==1);
    hop_xfer_history_entry rows[2];
    memset(rows,0,sizeof(rows));
    rows[0].batch=11; rows[0].direction=HOP_XFER_DIR_SEND; rows[0].phase=HOP_XFER_COMPLETED;
    rows[0].files_done=2; rows[0].files_total=2; rows[0].bytes_done=20; rows[0].bytes_total=20;
    rows[0].finished_at=1700000000u;
    snprintf(rows[0].name,sizeof(rows[0].name),"%s","old-new.bin");
    rows[1].batch=10; rows[1].direction=HOP_XFER_DIR_RECEIVE; rows[1].phase=HOP_XFER_PARTIAL;
    rows[1].files_done=1; rows[1].files_total=3; rows[1].bytes_done=5; rows[1].bytes_total=15;
    rows[1].finished_at=1699999999u;
    snprintf(rows[1].name,sizeof(rows[1].name),"%s","old-recv");
    CHECK(fwrite(rows,sizeof(rows[0]),2,f)==2);
    CHECK(fclose(f)==0);
}

static void test_xfer_task_history_store_legacy_migration(void) {
    hop_xfer_task_history_store_close();
    remove("hop-core-history-store2.bin");
    remove("hop-core-history-store2.bin.tmp");
    remove("hop-core-history-legacy.bin");
    write_legacy_history_file("hop-core-history-legacy.bin");
    CHECK(hop_xfer_task_history_store_open("hop-core-history-store2.bin","hop-core-history-legacy.bin"));
    hop_xfer_task_history_record recs[8];
    int total=-1;
    int n=hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,0,&total);
    CHECK(n==2 && total==2);
    /* disk order is newest-first; ids ascend with finish time (oldest = id 1) */
    CHECK(recs[0].record_id==1 && recs[0].transfer_id==10);
    CHECK(recs[0].state==HOP_XFER_TASK_PARTIAL && recs[0].direction==HOP_XFER_DIR_RECEIVE);
    CHECK(recs[0].finished_at==1699999999ull*1000ull);
    CHECK(recs[0].task_id==0 && strcmp(recs[0].name,"old-recv")==0);
    CHECK(recs[1].record_id==2 && recs[1].transfer_id==11);
    CHECK(recs[1].state==HOP_XFER_TASK_COMPLETED && strcmp(recs[1].name,"old-new.bin")==0);
    /* migration persisted the v2 store: reopening keeps data without the legacy file */
    hop_xfer_task_history_store_close();
    remove("hop-core-history-legacy.bin");
    CHECK(hop_xfer_task_history_store_open("hop-core-history-store2.bin",NULL));
    n=hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,0,&total);
    CHECK(n==2 && total==2);
    CHECK(recs[1].record_id==2 && recs[1].transfer_id==11);
    hop_xfer_task_history_store_close();
    /* a corrupt v2 store is rejected without crashing; session starts empty */
    FILE *f=fopen("hop-core-history-store2.bin","wb");
    CHECK(f!=NULL);
    CHECK(fwrite("garbage-not-a-store",1,19,f)==19);
    CHECK(fclose(f)==0);
    CHECK(hop_xfer_task_history_store_open("hop-core-history-store2.bin",NULL));
    total=-1;
    n=hop_xfer_task_history_store_list(recs,8,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,0,&total);
    CHECK(n==0 && total==0);
    hop_xfer_task_history_store_close();
    remove("hop-core-history-store2.bin");
    remove("hop-core-history-store2.bin.tmp");
    remove("hop-core-history-store.bin");
    remove("hop-core-history-store.bin.tmp");
}

typedef struct {
    char out[32768];
    size_t out_len;
    char err[4096];
    size_t err_len;
    int agent_running;
    int save_calls;
    int action_calls;
    hop_cli_action last_action;
    hop_config live_cfg;
} test_cli_capture;

static void test_cli_write_out(void *ctx, const char *data, size_t len) {
    test_cli_capture *c = (test_cli_capture *)ctx;
    size_t avail = sizeof(c->out) - 1 - c->out_len;
    if (len > avail) len = avail;
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    c->out[c->out_len] = '\0';
}

static void test_cli_write_err(void *ctx, const char *data, size_t len) {
    test_cli_capture *c = (test_cli_capture *)ctx;
    size_t avail = sizeof(c->err) - 1 - c->err_len;
    if (len > avail) len = avail;
    memcpy(c->err + c->err_len, data, len);
    c->err_len += len;
    c->err[c->err_len] = '\0';
}

static void test_cli_reset_buf(test_cli_capture *c) {
    c->out_len = 0; c->out[0] = '\0';
    c->err_len = 0; c->err[0] = '\0';
}

static int test_cli_agent_status(void *ctx, hop_cli_status *out) {
    test_cli_capture *c = (test_cli_capture *)ctx;
    if (!c->agent_running) return 0;
    memset(out, 0, sizeof(*out));
    out->running = 1;
    out->agent_pid = 4242;
    out->connected = 1;
    out->role = c->live_cfg.role;
    out->input_mode = c->live_cfg.input_mode;
    out->audio_requested = c->live_cfg.audio_mode;
    snprintf(out->peer_label, sizeof(out->peer_label), "%s", "MacPeer");
    return 1;
}

static int test_cli_agent_get_cfg(void *ctx, hop_config *out) {
    test_cli_capture *c = (test_cli_capture *)ctx;
    if (!c->agent_running) return 0;
    *out = c->live_cfg;
    return 1;
}

static int test_cli_agent_save_cfg(void *ctx, const hop_config *cfg) {
    test_cli_capture *c = (test_cli_capture *)ctx;
    if (!c->agent_running) return 0;
    c->live_cfg = *cfg;
    c->save_calls++;
    return 1;
}

static int test_cli_agent_action(void *ctx, hop_cli_action action) {
    test_cli_capture *c = (test_cli_capture *)ctx;
    if (!c->agent_running) return 0;
    c->action_calls++;
    c->last_action = action;
    return 1;
}


typedef struct { char steps[32], fail[16]; unsigned n; int attach_fail_once; } display_case;
static int display_step(display_case *c, char step) {
    c->steps[c->n++] = step; c->steps[c->n] = 0;
    return strchr(c->fail, step) == NULL;
}
static int display_wake(void *p) { return display_step(p, 'W'); }
static void display_clear_wake(void *p) { (void)display_step(p, 'c'); }
static int display_attach(void *p, int attach) {
    display_case *c = (display_case *)p;
    char step = attach ? 'A' : 'D';
    c->steps[c->n++] = step; c->steps[c->n] = 0;
    if (attach && c->attach_fail_once) { c->attach_fail_once = 0; return 0; }
    return strchr(c->fail, step) == NULL;
}
static int display_local(void *p, int target) { return display_step(p, target ? 'M' : 'P'); }
static int display_peer(void *p, int target) { return display_step(p, target ? 'm' : 'p'); }
static int display_sleep(void *p) { return display_step(p, 's'); }
static void test_display_switch(void) {
    struct {int target; int use_sleep; int attach_once; const char *fail, *steps; int result;} cases[] = {
        {1,0,0,"","WAMD",HOP_DISPLAY_OK},
        {0,0,0,"","APc",HOP_DISPLAY_OK},
        {1,0,0,"W","W",HOP_DISPLAY_WAKE_FAILED},
        {1,0,0,"A","WA",HOP_DISPLAY_ATTACH_FAILED},
        {1,0,0,"M","WAMmD",HOP_DISPLAY_OK},
        {1,0,0,"Mm","WAMmAP",HOP_DISPLAY_INPUT_FAILED},
        {1,0,0,"D","WAMDAP",HOP_DISPLAY_DETACH_FAILED},
        {1,0,0,"DP","WAMDAP",HOP_DISPLAY_ROLLBACK_FAILED},
        {0,0,0,"P","APpc",HOP_DISPLAY_OK},
        {0,0,0,"Pp","APp",HOP_DISPLAY_INPUT_FAILED},
        {0,1,0,"Pp","APpsc",HOP_DISPLAY_OK},
        {0,1,0,"Pps","APps",HOP_DISPLAY_INPUT_FAILED},
        {0,0,1,"","ApAPc",HOP_DISPLAY_OK},
        {0,0,0,"A","ApA",HOP_DISPLAY_ATTACH_FAILED}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        display_case c = {{0},{0},0,cases[i].attach_once};
        snprintf(c.fail,sizeof(c.fail),"%s",cases[i].fail);
        hop_display_ops ops = {
            &c, display_wake, display_clear_wake, display_local, display_peer,
            cases[i].use_sleep ? display_sleep : NULL, display_attach
        };
        CHECK(hop_display_switch_run(cases[i].target,&ops) == cases[i].result);
        CHECK(!strcmp(c.steps,cases[i].steps));
    }
    /* Reality-based AUTO direction resolution */
    CHECK(hop_display_resolve_target(HOP_DISPLAY_MAC, 0, -1, 0x11) == HOP_DISPLAY_MAC);
    CHECK(hop_display_resolve_target(HOP_DISPLAY_WINDOWS, 1, 0x0F, 0x11) == HOP_DISPLAY_WINDOWS);
    CHECK(hop_display_resolve_target(HOP_DISPLAY_AUTO, 0, -1, 0x11) == HOP_DISPLAY_WINDOWS);
    CHECK(hop_display_resolve_target(HOP_DISPLAY_AUTO, 1, 0x11, 0x11) == HOP_DISPLAY_WINDOWS);
    CHECK(hop_display_resolve_target(HOP_DISPLAY_AUTO, 1, 0x0F11, 0x11) == HOP_DISPLAY_WINDOWS);
    CHECK(hop_display_resolve_target(HOP_DISPLAY_AUTO, 1, 0x0F, 0x11) == HOP_DISPLAY_MAC);
    CHECK(hop_display_resolve_target(HOP_DISPLAY_AUTO, 1, -1, 0x11) == HOP_DISPLAY_MAC);

    hop_display_msg messages[] = {
        {HOP_DISPLAY_REQUEST,0,0,HOP_DISPLAY_MAC},
        {HOP_DISPLAY_REQUEST,0,0,HOP_DISPLAY_AUTO},
        {HOP_DISPLAY_WAKE,0,123,1800},
        {HOP_DISPLAY_WAKE_CLEAR,0,0,0},
        {HOP_DISPLAY_INPUT,0,124,17},
        {HOP_DISPLAY_SLEEP,0,125,0},
        {HOP_DISPLAY_RESULT,1,124,0},
        {HOP_DISPLAY_COMPLETE,1,0,HOP_DISPLAY_AUTO}
    };
    for (size_t i=0;i<sizeof(messages)/sizeof(messages[0]);++i) {
        uint8_t wire[HOP_DISPLAY_WIRE_SIZE]; hop_display_msg decoded;
        CHECK(hop_display_encode(wire,&messages[i]));
        CHECK(hop_display_decode(&decoded,wire,sizeof(wire)));
        CHECK(decoded.op==messages[i].op && decoded.id==messages[i].id &&
            decoded.ok==messages[i].ok && decoded.value==messages[i].value);
        CHECK(!hop_display_decode(&decoded,wire,sizeof(wire)-1));
        wire[0]=255; CHECK(!hop_display_decode(&decoded,wire,sizeof(wire)));
    }
    uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
    hop_display_msg bad={HOP_DISPLAY_WAKE,0,0,60};
    CHECK(!hop_display_encode(wire,&bad));
    bad.id=1;bad.value=3601;CHECK(!hop_display_encode(wire,&bad));
    bad.op=HOP_DISPLAY_INPUT;bad.value=65536;CHECK(!hop_display_encode(wire,&bad));
    bad.op=HOP_DISPLAY_REQUEST;bad.id=0;bad.value=3;CHECK(!hop_display_encode(wire,&bad));
    bad.op=HOP_DISPLAY_RESULT;bad.id=1;bad.value=0;bad.ok=2;CHECK(!hop_display_encode(wire,&bad));
    bad.op=HOP_DISPLAY_WAKE_CLEAR;bad.id=1;bad.value=0;bad.ok=0;CHECK(!hop_display_encode(wire,&bad));
    bad.op=HOP_DISPLAY_SLEEP;bad.id=0;bad.value=0;bad.ok=0;CHECK(!hop_display_encode(wire,&bad));
    hop_config cfg;hop_config_defaults(&cfg);char err[128];
    CHECK(cfg.disp_shared==0 && cfg.disp_device[0]==0);
    CHECK(cfg.disp_ddc_retries==3 && cfg.disp_verify_timeout_ms==8000 &&
          cfg.disp_poll_ms==200 && cfg.disp_wake_settle_ms==300 &&
          cfg.disp_wake_timeout_ms==10000 && cfg.disp_sleep_fallback==0);
    CHECK(hop_config_set_kv(&cfg,"disp_shared","true",err,sizeof(err))==1);
    CHECK(hop_config_set_kv(&cfg,"disp_device","\\\\.\\DISPLAY2",err,sizeof(err))==1);
    CHECK(hop_config_set_kv(&cfg,"disp_ddc_retries","5",err,sizeof(err))==1);
    CHECK(hop_config_set_kv(&cfg,"disp_verify_timeout_ms","10000",err,sizeof(err))==1);
    CHECK(hop_config_set_kv(&cfg,"disp_poll_ms","250",err,sizeof(err))==1);
    CHECK(hop_config_set_kv(&cfg,"disp_wake_settle_ms","350",err,sizeof(err))==1);
    CHECK(hop_config_set_kv(&cfg,"disp_wake_timeout_ms","12000",err,sizeof(err))==1);
    CHECK(hop_config_set_kv(&cfg,"disp_sleep_fallback","true",err,sizeof(err))==1);
    CHECK(cfg.disp_shared==1 && !strcmp(cfg.disp_device,"\\\\.\\DISPLAY2") &&
          cfg.disp_ddc_retries==5 && cfg.disp_verify_timeout_ms==10000 &&
          cfg.disp_poll_ms==250 && cfg.disp_wake_settle_ms==350 &&
          cfg.disp_wake_timeout_ms==12000 && cfg.disp_sleep_fallback==1);
    CHECK(hop_config_validate(&cfg,0,err,sizeof(err)));
    CHECK(hop_config_set_kv(&cfg,"disp_ddc_retries","0",err,sizeof(err))==-1);
    CHECK(hop_config_set_kv(&cfg,"disp_poll_ms","10",err,sizeof(err))==-1);
    CHECK(hop_config_set_kv(&cfg,"disp_verify_timeout_ms","500",err,sizeof(err))==-1);
    CHECK(hop_config_set_kv(&cfg,"disp_wake_timeout_ms","500",err,sizeof(err))==-1);
    {
        test_cli_capture cap; memset(&cap, 0, sizeof(cap)); cap.agent_running = 1;
        hop_cli_ops cops; memset(&cops, 0, sizeof(cops));
        cops.io_ctx = &cap; cops.write_out = test_cli_write_out; cops.write_err = test_cli_write_err;
        cops.agent_action = test_cli_agent_action;
        const char *argv[] = {"hopdesk", "action", "display-toggle", "--json"};
        CHECK(hop_cli_run(4, argv, &cops) == 0);
        CHECK(cap.action_calls == 1 && cap.last_action == HOP_CLI_ACTION_DISPLAY_TOGGLE);
    }
    cfg.disp_win_input=cfg.disp_mac_input;CHECK(!hop_config_validate(&cfg,0,err,sizeof(err)));
    cfg.disp_win_input=15;
    CHECK(hop_config_set_kv(&cfg,"disp_device","DISPLAY2",err,sizeof(err))==1);
    CHECK(!hop_config_validate(&cfg,0,err,sizeof(err)));
    cfg.disp_device[0]=0;CHECK(!hop_config_validate(&cfg,0,err,sizeof(err)));
}

static void test_config_schema_and_runtime_apply(void) {
    CHECK(hop_config_spec_count() >= 50u);
    CHECK(hop_config_spec_find("port") != NULL);
    CHECK(hop_config_spec_find("audio_port") != NULL);
    CHECK(hop_config_spec_find("switch_push_ms") == hop_config_spec_find("switch_delay_ms"));
    CHECK(hop_config_spec_find("nonexistent_key") == NULL);

    hop_config c;
    hop_config_defaults(&c);
    char err[128] = {0};
    CHECK(hop_config_validate(&c, 0, err, sizeof(err)) == 1);
    CHECK(hop_config_validate(&c, 1, err, sizeof(err)) == 0); /* empty name/token */

    /* Named edge & bool & modifier enums */
    CHECK(hop_config_set_kv(&c, "server_edge", "bottom", err, sizeof(err)) == 1);
    CHECK(c.server_edge == HOP_EDGE_BOTTOM);
    CHECK(hop_config_set_kv(&c, "client_edge", "2", err, sizeof(err)) == 1);
    CHECK(c.client_edge == HOP_EDGE_TOP);
    CHECK(hop_config_set_kv(&c, "wheel_invert", "true", err, sizeof(err)) == 1);
    CHECK(c.wheel_invert == 1);
    CHECK(hop_config_set_kv(&c, "mod_ctrl", "control", err, sizeof(err)) == 1);
    CHECK(c.mod_ctrl == HOP_MOD_TARGET_CONTROL);
    CHECK(hop_config_set_kv(&c, "audio_port", "24829", err, sizeof(err)) == 1);
    CHECK(c.audio_port == 24829);

    /* Cross-field validation checks */
    c.audio_port = c.port;
    CHECK(hop_config_validate(&c, 0, err, sizeof(err)) == 0);
    c.audio_port = 24829;
    c.net_heartbeat_timeout_ms = c.net_heartbeat_interval_ms;
    CHECK(hop_config_validate(&c, 0, err, sizeof(err)) == 0);
    c.net_heartbeat_timeout_ms = 6000;
    c.net_ctrl_queue_hard = c.net_ctrl_queue_soft - 1;
    CHECK(hop_config_validate(&c, 0, err, sizeof(err)) == 0);
    c.net_ctrl_queue_hard = 4096;
    c.xfer_max_image_temp_mb = c.xfer_max_image_mb - 1;
    CHECK(hop_config_validate(&c, 0, err, sizeof(err)) == 0);
    c.xfer_max_image_temp_mb = 512;
    CHECK(hop_config_validate(&c, 0, err, sizeof(err)) == 1);

    /* Runtime propagation to keymap, protocol, budget */
    c.mod_ctrl = HOP_MOD_TARGET_CONTROL;
    c.mod_win = HOP_MOD_TARGET_COMMAND;
    c.net_heartbeat_interval_ms = 1500;
    c.net_heartbeat_timeout_ms = 4500;
    c.xfer_max_jobs = 24;
    hop_runtime_apply_config(&c);
    CHECK(hop_vk_to_mac(0xA2) == 0x3B); /* VK_LCONTROL -> Mac Control */
    CHECK(hop_vk_to_mac(0x5B) == 0x37); /* VK_LWIN -> Mac Command */
    CHECK(hop_protocol_heartbeat_interval_ms() == 1500u);
    CHECK(hop_protocol_heartbeat_timeout_ms() == 4500u);
    hop_xfer_budget_snapshot snap;
    hop_xfer_budget_get_snapshot(&snap);
    CHECK(snap.limits.max_jobs == 24u);

    /* Restore defaults so subsequent tests see standard values */
    hop_config def;
    hop_config_defaults(&def);
    hop_runtime_apply_config(&def);
    CHECK(hop_vk_to_mac(0xA2) == 0x37);
    CHECK(hop_protocol_heartbeat_interval_ms() == HOP_HEARTBEAT_INTERVAL_MS);
}

static void test_cli_engine(void) {
    const char *path = "hopdesk-cli-test.conf";
    remove(path);

    test_cli_capture cap;
    memset(&cap, 0, sizeof(cap));
    hop_config_defaults(&cap.live_cfg);
    snprintf(cap.live_cfg.device_name, sizeof(cap.live_cfg.device_name), "%s", "LiveBox");

    hop_cli_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.default_config_path = path;
    ops.default_log_path = "hopdesk-cli-test.log";
    ops.default_device_name = "TestDev";
    ops.write_out = test_cli_write_out;
    ops.write_err = test_cli_write_err;
    ops.io_ctx = &cap;
    ops.agent_get_status = test_cli_agent_status;
    ops.agent_get_config = test_cli_agent_get_cfg;
    ops.agent_save_config = test_cli_agent_save_cfg;
    ops.agent_action = test_cli_agent_action;

    const char *a_cli[] = {"hopdesk", "config", "schema", "--json"};
    CHECK(hop_cli_is_invocation(4, a_cli) == 1);
    const char *a_agent[] = {"hopdesk", "--agent", "--config", "x.conf"};
    CHECK(hop_cli_is_invocation(4, a_agent) == 0);
    CHECK(strcmp(hop_cli_extract_config_flag(4, a_agent), "x.conf") == 0);

    /* 1. Schema JSON export */
    test_cli_reset_buf(&cap);
    CHECK(hop_cli_run(4, a_cli, &ops) == 0);
    CHECK(strstr(cap.out, "\"ok\":true") != NULL);
    CHECK(strstr(cap.out, "\"audio_port\"") != NULL);
    CHECK(strstr(cap.out, "\"mouse_scale_pct\"") != NULL);

    /* 2. Offline config set + atomic save + warnings */
    test_cli_reset_buf(&cap);
    const char *a_set[] = {"hopdesk", "config", "set",
                           "host=127.0.0.1", "token=my-secret", "mouse_scale_pct=125",
                           "wheel_invert=true", "audio_port=24820", "--json"};
    CHECK(hop_cli_run(9, a_set, &ops) == 0);
    CHECK(strstr(cap.out, "\"ok\":true") != NULL);
    CHECK(strstr(cap.out, "\"agent_notified\":false") != NULL);
    CHECK(strstr(cap.out, "\"mouse_scale_pct\"") != NULL);
    CHECK(strstr(cap.out, "\"audio_port\"") != NULL);

    /* 3. Secret masking on get vs --show-secret */
    test_cli_reset_buf(&cap);
    const char *a_get[] = {"hopdesk", "config", "get", "token", "mouse_scale_pct", "--json"};
    CHECK(hop_cli_run(6, a_get, &ops) == 0);
    CHECK(strstr(cap.out, "\"token\":\"***\"") != NULL);
    CHECK(strstr(cap.out, "\"mouse_scale_pct\":125") != NULL);

    test_cli_reset_buf(&cap);
    const char *a_get_sec[] = {"hopdesk", "config", "get", "token", "--show-secret", "--json"};
    CHECK(hop_cli_run(6, a_get_sec, &ops) == 0);
    CHECK(strstr(cap.out, "\"token\":\"my-secret\"") != NULL);

    /* 4. Strict validation passes once token & name are set */
    test_cli_reset_buf(&cap);
    const char *a_val[] = {"hopdesk", "config", "validate", "--strict", "--json"};
    CHECK(hop_cli_run(5, a_val, &ops) == 0);
    CHECK(strstr(cap.out, "\"valid\":true") != NULL);

    /* 5. Invalid range rejected without corrupting file */
    test_cli_reset_buf(&cap);
    const char *a_bad[] = {"hopdesk", "config", "set", "mouse_scale_pct=999", "--json"};
    CHECK(hop_cli_run(5, a_bad, &ops) == 1);
    CHECK(strstr(cap.out, "\"ok\":false") != NULL);

    /* 6. Live agent IPC path: status, set, action */
    cap.agent_running = 1;
    test_cli_reset_buf(&cap);
    const char *a_st[] = {"hopdesk", "status", "--json"};
    CHECK(hop_cli_run(3, a_st, &ops) == 0);
    CHECK(strstr(cap.out, "\"running\":true") != NULL);
    CHECK(strstr(cap.out, "\"agent_pid\":4242") != NULL);

    test_cli_reset_buf(&cap);
    const char *a_live_set[] = {"hopdesk", "config", "set", "switch_delay_ms=180", "--json"};
    CHECK(hop_cli_run(5, a_live_set, &ops) == 0);
    CHECK(cap.save_calls == 1);
    CHECK(cap.live_cfg.switch_delay_ms == 180);
    CHECK(strstr(cap.out, "\"agent_notified\":true") != NULL);
    CHECK(strstr(cap.out, "\"applied_hot\":[\"switch_delay_ms\"]") != NULL);

    test_cli_reset_buf(&cap);
    const char *a_act[] = {"hopdesk", "action", "reload", "--json"};
    CHECK(hop_cli_run(4, a_act, &ops) == 0);
    CHECK(cap.action_calls == 1 && cap.last_action == HOP_CLI_ACTION_RELOAD);

    remove(path);
}

int main(void) {
    hop_crc32_init();

    RUN_TEST(test_endian_and_frame_encoding);
    RUN_TEST(test_decoder_fragmentation_and_coalescing);
    RUN_TEST(test_decoder_rejects_oversized_frame);
    RUN_TEST(test_crc32);
    RUN_TEST(test_token_authentication);
    RUN_TEST(test_config_defaults_and_loading);
    RUN_TEST(test_dual_roles);
    RUN_TEST(test_display_switch);
    RUN_TEST(test_display_catalog_helpers);
    RUN_TEST(test_config_schema_and_runtime_apply);
    RUN_TEST(test_cli_engine);
    RUN_TEST(test_keymap);
    RUN_TEST(test_keymap_ranges);
    RUN_TEST(test_decoder_reset_mid_frame);
    RUN_TEST(test_decoder_all_split_points);
    RUN_TEST(test_xfer_hello_validation);
    RUN_TEST(test_xfer_relative_path_validation);
    RUN_TEST(test_xfer_image_bounds);
    RUN_TEST(test_xfer_chunk_bounds);
    RUN_TEST(test_xfer_retry_policy);
    RUN_TEST(test_clipboard_origin_routing);
    RUN_TEST(test_lazy_clipboard_protocol);
    RUN_TEST(test_file_crc_fold);
    RUN_TEST(test_relative_path_wire_bytes);
    RUN_TEST(test_file_begin_decode);
    RUN_TEST(test_image_begin_decode);
    RUN_TEST(test_file_end_ack_abort_done_decoders);
    RUN_TEST(test_update_manifest_validation);
    RUN_TEST(test_xfer_global_budget);
    RUN_TEST(test_xfer_ack_window);
    RUN_TEST(test_xfer_buffer_pool);
    RUN_TEST(test_xfer_rx_queue);
    RUN_TEST(test_xfer_rate_limiter);
    RUN_TEST(test_xfer_tracking_stage_timing);
    RUN_TEST(test_xfer_tracking_reset_and_sparse_ids);
    RUN_TEST(test_xfer_tracking_file_pages);
    RUN_TEST(test_xfer_tracking_duplex_snapshot);
    RUN_TEST(test_xfer_tracking_speed_and_eta);
    RUN_TEST(test_xfer_task_identity_and_lifecycle);
    RUN_TEST(test_xfer_task_index_collision);
    RUN_TEST(test_xfer_task_cancel_isolation_and_completion);
    RUN_TEST(test_xfer_task_cancel_timeout_race);
    RUN_TEST(test_xfer_task_concurrent_ledger_stress);
    RUN_TEST(test_xfer_task_image_buffer_transfer);
    RUN_TEST(test_xfer_task_revisions);
    RUN_TEST(test_xfer_task_summary_and_sink);
    RUN_TEST(test_xfer_task_history_store_persistence);
    RUN_TEST(test_xfer_task_history_store_legacy_migration);

    if (g_failures) {
        fprintf(stderr, "HopDesk core tests: %d failure(s)\\n", g_failures);
        return 1;
    }
    puts("HopDesk core tests: all tests passed");
    return 0;
}
