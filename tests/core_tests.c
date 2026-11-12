/*
 * HopDesk portable core regression tests.
 * These tests intentionally exercise only platform-independent code.
 */
#include "hop.h"
#include "update_manifest.h"
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
    CHECK(c.xfer_chunk_kb == HOP_CHUNK_KB_DEFAULT);
    CHECK(c.xfer_rate_limit_mib == 128);
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
    CHECK(c.disp_win_input == 0x0F);
    CHECK(c.switch_delay_ms == 175);

    /* UI persistence writes a complete config atomically and remains readable. */
    snprintf(c.device_name, sizeof(c.device_name), "%s", "HopDesk-Test");
    snprintf(c.mac_input_cmd, sizeof(c.mac_input_cmd), "%s", "m1ddc display 1 set input");
    c.switch_cooldown_ms = 425;
    c.edge_margin_px = 48;
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
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) { fputs("xfer_rate_limit_mib=8\n", f); fclose(f); }
    hop_config before_invalid = c;
    CHECK(hop_config_load(&c, path) == 0);
    CHECK(c.xfer_rate_limit_mib == before_invalid.xfer_rate_limit_mib);
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
    remove(path);
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
        "windows_url=https://github.com/alex520lq/HopDesk-Releases/releases/download/v0.5.1/HopDesk-Windows.zip\n"
        "windows_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "macos_url=https://github.com/alex520lq/HopDesk-Releases/releases/download/v0.5.1/HopDesk-macOS.zip\n"
        "macos_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "notes_url=https://github.com/alex520lq/HopDesk-Releases/releases/tag/v0.5.1\n";
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
    char *url = strstr(tampered, "https://github.com/alex520lq/HopDesk-Releases");
    CHECK(url != NULL);
    if (url) url[8] = 'x';
    CHECK(!hop_update_manifest_parse(tampered, sizeof(manifest) - 1, &parsed));

    const char duplicate[] =
        "format=1\nversion=0.5.1\nversion=0.5.2\n"
        "tag=v0.5.1\nwindows_url=https://github.com/alex520lq/HopDesk-Releases/releases/download/v0.5.1/HopDesk-Windows.zip\n"
        "windows_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "macos_url=https://github.com/alex520lq/HopDesk-Releases/releases/download/v0.5.1/HopDesk-macOS.zip\n"
        "macos_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "notes_url=https://github.com/alex520lq/HopDesk-Releases/releases/tag/v0.5.1\n";
    CHECK(!hop_update_manifest_parse(duplicate, sizeof(duplicate) - 1, &parsed));
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
    hop_xfer_buffer_pool_dispose(&pool);
    CHECK(pool.capacity == 0);

    /* Growth replaces retained idle storage and remains inside the pool cap. */
    hop_xfer_buffer_pool_init(&pool);
    slot = -1; data = NULL;
    CHECK(hop_xfer_buffer_pool_acquire_locked(&pool, 128, &slot, &data) == 1);
    CHECK(slot >= 0 && data != NULL && pool.capacity == 128);
    hop_xfer_buffer_pool_release_locked(&pool, slot);
    hop_xfer_buffer_pool_dispose(&pool);
    hop_xfer_budget_snapshot snapshot;
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
}

static void test_xfer_tracking_duplex_snapshot(void) {
    /* Each tracker includes 8192 file records; never place them on the stack. */
    static hop_xfer_tracking tx, rx;
    hop_xfer_status_pair pair;
    memset(&tx,0,sizeof(tx)); memset(&rx,0,sizeof(rx));
    hop_xfer_tracking_snapshot_pair(&tx,&rx,0,&pair);
    CHECK(!pair.send.active && !pair.receive.active);
    hop_xfer_tracking_begin(&tx,77,HOP_XFER_DIR_SEND,1,0,"send",HOP_XFER_SENDING);
    hop_xfer_tracking_file(&tx,77,1,"send",100,0,HOP_XFER_FILE_SENDING);
    hop_xfer_tracking_chunk(&tx,77,1,0,30,1000);
    /* Batch IDs can collide across directions without aliasing their state. */
    hop_xfer_tracking_begin(&rx,77,HOP_XFER_DIR_RECEIVE,1,0,"receive",HOP_XFER_RECEIVING);
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
    hop_xfer_task first={0}, tmp={0}, last={0};
    hop_xfer_task_snapshot snap;
    CHECK(hop_xfer_task_init(&first,0,0,1));
    const uint64_t first_id=first.id;
    /* The manager index has 8192 buckets. Keep the first record alive while
     * cycling enough IDs to force a real collision, then remove both orders. */
    for (unsigned i=0; i<8191; ++i) {
        CHECK(hop_xfer_task_init(&tmp,0,0,2));
        hop_xfer_task_dispose(&tmp);
    }
    CHECK(hop_xfer_task_init(&last,0,0,3));
    const uint64_t last_id=last.id;
    CHECK(last_id-first_id==8192);
    CHECK(hop_xfer_task_get_snapshot(first_id,&snap) && snap.transfer_id==1);
    CHECK(hop_xfer_task_get_snapshot(last_id,&snap) && snap.transfer_id==3);
    hop_xfer_task_dispose(&first);
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

int main(void) {
    hop_crc32_init();

    RUN_TEST(test_endian_and_frame_encoding);
    RUN_TEST(test_decoder_fragmentation_and_coalescing);
    RUN_TEST(test_decoder_rejects_oversized_frame);
    RUN_TEST(test_crc32);
    RUN_TEST(test_token_authentication);
    RUN_TEST(test_config_defaults_and_loading);
    RUN_TEST(test_dual_roles);
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
    RUN_TEST(test_xfer_tracking_duplex_snapshot);
    RUN_TEST(test_xfer_tracking_speed_and_eta);
    RUN_TEST(test_xfer_task_identity_and_lifecycle);
    RUN_TEST(test_xfer_task_index_collision);
    RUN_TEST(test_xfer_task_cancel_isolation_and_completion);
    RUN_TEST(test_xfer_task_cancel_timeout_race);
    RUN_TEST(test_xfer_task_concurrent_ledger_stress);
    RUN_TEST(test_xfer_task_image_buffer_transfer);
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
