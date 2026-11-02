/*
 * HopDesk portable core regression tests.
 * These tests intentionally exercise only platform-independent code.
 */
#include "hop.h"
#include "update_manifest.h"

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
    uint8_t end[9] = {0}, ack[9] = {0}, abortp[5] = {0}, donep[20] = {0};
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

    hop_put_be32(abortp, 21); abortp[4]=HOP_ABORT_CANCEL;
    hop_xfer_abort_msg ab;
    CHECK(hop_xfer_decode_abort(abortp,sizeof(abortp),&ab));
    CHECK(ab.batch==21 && ab.reason==HOP_ABORT_CANCEL);
    CHECK(!hop_xfer_decode_abort(abortp,6,&ab));

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

int main(void) {
    hop_crc32_init();

    test_endian_and_frame_encoding();
    test_decoder_fragmentation_and_coalescing();
    test_decoder_rejects_oversized_frame();
    test_crc32();
    test_token_authentication();
    test_config_defaults_and_loading();
    test_dual_roles();
    test_keymap();
    test_keymap_ranges();
    test_decoder_reset_mid_frame();
    test_decoder_all_split_points();
    test_xfer_hello_validation();
    test_xfer_relative_path_validation();
    test_xfer_image_bounds();
    test_xfer_chunk_bounds();
    test_xfer_retry_policy();
    test_clipboard_origin_routing();
    test_file_crc_fold();
    test_relative_path_wire_bytes();
    test_file_begin_decode();
    test_image_begin_decode();
    test_file_end_ack_abort_done_decoders();
    test_update_manifest_validation();

    if (g_failures) {
        fprintf(stderr, "HopDesk core tests: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("HopDesk core tests: all tests passed");
    return 0;
}
