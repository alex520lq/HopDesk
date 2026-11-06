/* =====================================================================
 *  HopDesk - 公共头（平台无关，纯 C）
 *  协议类型、消息枚举、配置、连接状态。
 * ===================================================================== */
#ifndef HOP_H
#define HOP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HOP_VERSION
#define HOP_VERSION        "0.5.0"
#endif
#define HOP_DEFAULT_PORT   24817
#define HOP_DATA_PORT      24818         /* 文件数据通道，与控制通道分离 */
#define HOP_TOKEN_MAX      128
#define HOP_NAME_MAX       32
#define HOP_TEXT_MAX       (64 * 1024)   /* 文本上限 */
#define HOP_NAME_LEN_MAX   255           /* 文件名（UTF-8）上限 */
#define HOP_PATH_LEN_MAX   1024          /* 相对路径（UTF-8）上限 */
#define HOP_HEADER_SIZE    5

/* 分块大小。
 *
 * 默认 256 KB，不是 1 MB —— 这是实测出来的取舍：
 *   1 MB 块：本机回环 1245 MB/s，但传输期间控制通道 p99 从 ~180 µs
 *            涨到 400–1400 µs（尾延迟劣化约 5 倍）
 *   256 KB 块：1109 MB/s（慢 11%），控制通道 p99 与空载完全一致
 *
 * 11% 的吞吐在真实链路上看不见：百兆线速 11.5 MB/s，两者都比线路快 100 倍；
 * 而尾延迟劣化是用户能感觉到的"鼠标发涩"。所以默认选隔离性。
 * 链路升到 2.5G/10G 后可把 xfer_chunk_kb 调回 1024。 */
#define HOP_CHUNK_KB_DEFAULT 256
#define HOP_CHUNK_KB_MAX     4096
/* 单帧载荷上限：按最大分块算（分块头 id4+off8+len4，另留余量） */
#define HOP_MAX_PAYLOAD      ((uint32_t)HOP_CHUNK_KB_MAX * 1024u + 1024u)

/* ---- 消息类型 ---- */
typedef enum {
    HOP_MSG_HELLO        = 1,   /* C -> S: 握手 {token, name, w, h, ver} */
    HOP_MSG_HELLO_ACK    = 2,   /* S -> C: 结果 {ok, name, w, h, ver, session}   */
    HOP_MSG_PING         = 3,
    HOP_MSG_PONG         = 4,

    HOP_MSG_MOUSE_MOVE   = 10,  /* S -> C: dx,dy (int16 x2)        */
    HOP_MSG_MOUSE_BUTTON = 11,  /* S -> C: button(uint8),down(uint8) */
    HOP_MSG_MOUSE_WHEEL  = 12,  /* S -> C: dx,dy (int16 x2)        */
    HOP_MSG_EDGE_LEAVE   = 13,  /* C -> S: edge(uint8); S -> C: empty = sharing ended */
    HOP_MSG_MOUSE_ENTER  = 14,  /* S -> C: y(uint16, client 坐标)  */

    HOP_MSG_KEY_DOWN     = 20,  /* S -> C: keycode(uint16)         */
    HOP_MSG_KEY_UP       = 21,
    HOP_MSG_KEY_MODS     = 22,  /* S -> C: modmask(uint16)         */

    HOP_MSG_CLIP_TEXT    = 30,  /* 双向: origin(uint32) + UTF-8    */
    /* 31 曾是"整文件一帧传完"的 CLIP_FILE，已废弃。 */
    HOP_MSG_CLIP_FILE_OFFER       = 32, /* 双向: 轻量文件 Offer，不含路径/文件数据 */
    HOP_MSG_CLIP_FILE_OFFER_CLEAR = 33, /* 双向: 清除/替换旧 Offer */
    HOP_MSG_CLIP_PASTE_REQUEST    = 34, /* 双向: 真正跨端粘贴时才请求数据 */
    HOP_MSG_CLIP_PASTE_FAILED     = 35, /* 双向: Offer 过期/源不可用/队列拒绝 */

    HOP_MSG_DISP_WAKE    = 40,  /* S -> C: seconds(uint32)         */
    HOP_MSG_DISP_INPUT   = 41,  /* S -> C: vcp(uint16)             */
    HOP_MSG_DISP_SLEEP   = 42,

    /* ---- 数据通道（port+1，只跑文件/图片字节） ----
     * 与控制通道分开，是为了让大载荷不挤占鼠标/键盘帧：
     * 单条 TCP 流上的大块数据会造成队头阻塞，鼠标事件要等整块发完。
     * 图片也走这里 —— 一张 4K 截图裸 BGRA 就是 33 MB，塞不进控制通道。 */
    HOP_MSG_XFER_HELLO   = 50,  /* 双向: ver(1)+token_len(1)+token+session(16) */
    HOP_MSG_FILE_BEGIN   = 51,  /* S -> C: id(4)+size(8)+mtime(8)+flags(1)
                                 *         +nlen(2)+name+plen(2)+relpath */
    HOP_MSG_FILE_CHUNK   = 52,  /* 双向: id(4)+off(8)+len(4)+crc(4)+data */
    HOP_MSG_FILE_END     = 53,  /* 双向: id(4)+status(1)+file_crc(4) */
    HOP_MSG_XFER_DONE    = 54,  /* S -> C: batch(4)+ok(4)+total(4)+bytes(8)
                                 * ok < total 就是部分成功，接收端据此报错 */
    HOP_MSG_IMAGE_BEGIN  = 55,  /* S -> C: id(4)+w(4)+h(4)+stride(4)+flags(1) */
    HOP_MSG_XFER_ABORT   = 56,  /* 双向: batch(4)+reason(1)，收到即中止本批 */
    HOP_MSG_FILE_ACK     = 57   /* C -> S: batch(4)+id(4)+status(1)
                                 * 单文件结论。没有 ACK 就没有"完成" ——
                                 * 这是发送端判断要不要重发这一个文件的唯一依据。 */
} hop_msg_type;

/* ---- 鼠标按键 ---- */
#define HOP_BTN_LEFT     0
#define HOP_BTN_MIDDLE   1
#define HOP_BTN_RIGHT    2

/* ---- 文件条目标志 ---- */
#define HOP_FILE_FLAG_DIR    (1u << 0)   /* 空目录占位，无分块 */

/* ---- FILE_END 的 status（发送端报告"我这边怎么了"） ---- */
#define HOP_FILE_OK          0
#define HOP_FILE_IO_ERROR    1
#define HOP_FILE_CRC_ERROR   2

/* ---- Lazy file clipboard ----
 * File Offer IDs are monotonically increasing generations scoped to the
 * authenticated control session and origin device. Paste request IDs reserve
 * the high bit of the data-channel batch namespace. */
#define HOP_CLIP_OFFER_MAX_ITEMS 256u
#define HOP_CLIP_OFFER_HEADER_BYTES 14u
#define HOP_CLIP_OFFER_MAX_PAYLOAD (HOP_CLIP_OFFER_HEADER_BYTES + HOP_CLIP_OFFER_MAX_ITEMS * (2u + HOP_NAME_LEN_MAX))
#define HOP_CLIP_OFFER_CLEAR_BYTES 12u
#define HOP_CLIP_PASTE_REQUEST_BYTES 24u
#define HOP_CLIP_PASTE_FAILED_BYTES 5u
#define HOP_XFER_PASTE_BATCH_FLAG 0x80000000u
#define HOP_CLIP_PASTE_FAIL_STALE 1u
#define HOP_CLIP_PASTE_FAIL_BUSY 2u
#define HOP_CLIP_PASTE_FAIL_SOURCE_IO 3u
#define HOP_CLIP_PASTE_FAIL_TRANSFER 4u

typedef struct {
    uint32_t origin;
    uint64_t generation;
    uint16_t count;
    const uint8_t *names[HOP_CLIP_OFFER_MAX_ITEMS];
    uint16_t name_lens[HOP_CLIP_OFFER_MAX_ITEMS];
} hop_clip_file_offer_msg;

typedef struct {
    uint32_t request_id;
    uint32_t origin;
    uint64_t generation;
    uint64_t clipboard_version;
} hop_clip_paste_request_msg;

/* ---- FILE_ACK 的 status（接收端报告"这个文件到底成没成"） ---- */
#define HOP_FILE_ACK_OK       0
#define HOP_FILE_ACK_CRC      1   /* 块或整文件 CRC 不符 → 值得重发 */
#define HOP_FILE_ACK_DISK     2   /* 接收端写盘失败 → 重发也未必有用，但仍重试 */
#define HOP_FILE_ACK_BADPATH  3   /* 相对路径被拒 → 重发无意义，直接放弃该文件 */

/* ---- 数据通道协议版本 ----
 * v1: FILE_CHUNK = id+off+len+data          FILE_END = id+status
 * v2: FILE_CHUNK 加 crc(4)，FILE_END 加 file_crc(4)，新增 XFER_ABORT
 * v3: 新增 FILE_ACK（逐文件确认），XFER_DONE 载荷改为 20 字节并带真实计数
 * v4: 数据通道 HELLO 增加 token 身份校验，握手未通过不得传输。
 * v5: 整文件 CRC fold 同时纳入 chunk offset 与 chunk CRC，校验块位置。
 * v6: 控制握手协商 session；数据 HELLO 绑定它，所有数据帧携带 session+batch。
 * 两端必须同版本：旧端发来的块在新端会按错位的 CRC 字段解析，
 * 结果是每块都校验失败。所以连上就先比版本，别让用户对着
 * "传输一直失败"猜原因。 */
#define HOP_XFER_PROTO_VER   6
#define HOP_CTRL_PROTO_VER   7

/* ---- XFER_ABORT 的 reason ---- */
#define HOP_ABORT_CRC        1
#define HOP_ABORT_DISK       2
#define HOP_ABORT_TOO_LARGE  3
#define HOP_ABORT_BAD_PATH   4
#define HOP_ABORT_CANCEL     5   /* 用户主动取消，两端都不再重试 */

/* ---- 传输重试策略 ----
 * 两层，各管各的，别混：
 *   文件级 —— 单个文件失败（CRC/写盘/超时），原地重发这一个文件
 *   作业级 —— 连接断了，整批退回队列，等重连后从断点条目继续
 * 两层都有限次：无限重试等于把队列焊死。 */
#define HOP_XFER_FILE_RETRY_MAX  3    /* 单文件最多尝试次数（含首次） */
#define HOP_XFER_JOB_RETRY_MAX   5    /* 连接级最多重试次数 */
#define HOP_XFER_ACK_TIMEOUT_MS  10000 /* 等单个文件 ACK 的上限 */
/* 断线后等对端回来的总预算。没有它，一个断线的批次会永远挂在队列里：
 * 菜单上的"取消传输"一直亮着，对端几小时后回来还会突然接着传。 */
#define HOP_XFER_RECONNECT_BUDGET_MS 30000

static inline unsigned hop_xfer_file_retry_delay_ms(int attempt) {
    return 200u << (attempt > 1 ? attempt - 1 : 0);      /* 200,400,800… */
}
static inline unsigned hop_xfer_job_retry_delay_ms(int attempt) {
    unsigned d = 500u << (attempt > 1 ? attempt - 1 : 0); /* 0.5,1,2,4,8s */
    return d > 8000u ? 8000u : d;
}

/* ---- 图片上限 ----
 * 接收端必须自己设限：对端声称 100000×100000 就能把内存吃光。
 * 33 MB 是 4K 截图的裸 BGRA，留到 256 MB 足够任何真实用途。 */
#define HOP_IMG_MAX_BYTES    (256ull * 1024ull * 1024ull)
#define HOP_IMG_MAX_SIDE     32768

/* ---- 边缘 ---- */
typedef enum {
    HOP_EDGE_LEFT   = 0,
    HOP_EDGE_RIGHT  = 1,
    HOP_EDGE_TOP    = 2,
    HOP_EDGE_BOTTOM = 3
} hop_edge;

/* ---- 修饰键位（阶段 2） ---- */
#define HOP_MOD_SHIFT   (1u << 0)
#define HOP_MOD_CONTROL (1u << 1)
#define HOP_MOD_ALT     (1u << 2)
#define HOP_MOD_COMMAND (1u << 3)
#define HOP_MOD_CAPS    (1u << 4)   /* 大写锁定：切换语义，不是按住 */

/* ---- 剪贴板来源设备（回环抑制用） ---- */
#define HOP_DEV_SERVER  1u
#define HOP_DEV_CLIENT  2u

/* ---- 连接 / 键鼠归属 ---- */
typedef enum {
    HOP_LINK_DOWN      = 0,   /* 无连接          */
    HOP_LINK_LOCAL     = 1,   /* 已连接，键鼠在本端（服务端） */
    HOP_LINK_SHARING   = 2    /* 键鼠在对端      */
} hop_link_state;

typedef enum { HOP_ROLE_SERVER = 0, HOP_ROLE_CLIENT = 1 } hop_role;

/* ---- 配置 ---- */
typedef struct {
    hop_role role; /* independent of operating system; restart to change */
    uint16_t port;
    char     token[HOP_TOKEN_MAX];
    char     peer_host[64];        /* 客户端用：服务端地址 */
    char     device_name[HOP_NAME_MAX];
    int      server_edge;          /* 服务端移交边缘（hop_edge） */
    int      client_edge;          /* 客户端离开边缘 */
    int      screen_w;             /* 本机主屏宽（握手交换） */
    int      screen_h;

    /* ---- 边缘保护（对齐 Deskflow 的 switchDelay 语义） ---- */
    int      switch_delay_ms;      /* 贴边持续推多久才移交；0 = 立即移交 */
    int      switch_cooldown_ms;   /* 移交/收回后多久内不再触发，掐断乒乓 */
    int      edge_margin_px;       /* 内侧死区：进入内移 + 退出越界阈值 */

    /* ---- 阶段 4：显示切换 ---- */
    uint16_t disp_win_input;       /* DDC/CI 0x60 值：Windows 侧的输入源 */
    uint16_t disp_mac_input;       /* DDC/CI 0x60 值：Mac 侧的输入源 */
    int      disp_wake_sec;        /* DISP_WAKE 保持多久（秒） */
    char     mac_input_cmd[96];    /* Mac 端切源命令前缀，DDC 值追加在后 */
    char     mac_sleep_cmd[64];    /* Mac 端显示睡眠命令 */

    /* ---- 文件传输 ----
     * 只留一个连接。实测千兆线速 125 MB/s，单流 TCP 就能打满（BDP 只有几十 KB），
     * 多开连接在 1 Gbps 及以下收益为零；等链路升到 2.5G/10G 再加池化。 */
    uint16_t data_port;            /* 数据通道端口 */
    int      xfer_chunk_kb;        /* 分块大小（KB），64-4096 */
    int      xfer_rate_limit_mib;  /* 数据通道速率上限（MiB/s），16-4096 */
} hop_config;

void hop_config_defaults(hop_config *c);
int  hop_config_load(hop_config *c, const char *path);  /* 不存在返回 0 */
int  hop_config_save_atomic(const hop_config *c, const char *path); /* 同目录临时文件 + 原子替换 */

/* ---- Windows VK -> macOS 虚拟键码（平台无关的表，阶段 2） ---- */
#define HOP_KEY_NONE 0xFFFFu
uint16_t hop_mac_to_vk(uint16_t kc); /* inverse canonical wire key mapping */
uint16_t hop_vk_to_mac(int vk);                 /* 不支持返回 HOP_KEY_NONE */
int      hop_mac_key_is_mod(uint16_t mac_key);  /* 返回 HOP_MOD_* 位，非修饰键返回 0 */

/* ---- 日志 ---- */
typedef enum { HOP_LOG_INFO, HOP_LOG_WARN, HOP_LOG_ERROR } hop_log_level;
void hop_log_set(int enable);
/* NULL = 输出到 stderr。路径为 UTF-8。文件超过 HOP_LOG_ROTATE_BYTES 时
 * 轮转为 <path>.1（只留一份旧日志），常驻进程的日志不会无限增长。线程安全。 */
#define HOP_LOG_ROTATE_BYTES (5L * 1024L * 1024L)
void hop_log_open(const char *path);
void hop_log(hop_log_level lvl, const char *fmt, ...);

/* ---- 帧编解码 ---- */
/* 编码：out 至少 payload_len + HOP_HEADER_SIZE；返回总长度。*/
uint32_t hop_encode_frame(uint8_t type, const void *payload, uint32_t plen,
                          uint8_t *out);
/* 只编码 5 字节头部（type + length），用于分两次发送。*/
void hop_encode_header(uint8_t out[HOP_HEADER_SIZE], uint8_t type,
                       uint32_t plen);

/* 解码：流式喂字节，凑齐一帧回调。*/
typedef struct hop_decoder hop_decoder;
typedef void (*hop_on_message)(void *user, uint8_t type,
                               const uint8_t *payload, uint32_t plen);
hop_decoder *hop_decoder_new(void *user, hop_on_message cb);
/* 返回 1 = 正常；0 = 流已损坏（帧长超限 / 内存不足），解码器进入粘滞失败态，
 * 之后的 feed 一律返回 0、不再回调，直到 reset。
 *
 * 为什么不"丢掉这一帧继续解"：TCP 是字节流，长度字段一旦不可信，
 * 下一帧的起点就无从知道 —— 把垃圾当新帧头继续解只会制造更多错帧
 * （例如把文件数据当成 KEY_DOWN 注入）。唯一正确的恢复是断开连接、
 * 让两端在新连接上从帧边界重新开始。调用方看到 0 必须关闭连接。 */
int          hop_decoder_feed(hop_decoder *d, const uint8_t *buf, uint32_t n);
int          hop_decoder_failed(const hop_decoder *d);
void         hop_decoder_reset(hop_decoder *d);
void         hop_decoder_free(hop_decoder *d);

/* ---- 控制通道会话：握手、心跳、发送队列准入（平台无关，可单元测试） ----
 *
 * 连接的生死由应用层判定，不靠 TCP keepalive：keepalive 要 ~90 秒才报死，
 * 期间键鼠全部落进黑洞。两端各自每 HOP_HEARTBEAT_INTERVAL_MS 发一次 PING，
 * 收到任何帧都算对端活着；HOP_HEARTBEAT_TIMEOUT_MS 内一帧都没收到就断开重连。
 * v6 不兼容旧版握手；必须同时升级两端。 */
#define HOP_HANDSHAKE_TIMEOUT_MS   5000   /* 连上后多久必须完成 HELLO / HELLO_ACK */
#define HOP_HEARTBEAT_INTERVAL_MS  2000
#define HOP_HEARTBEAT_TIMEOUT_MS   6000
#define HOP_CONNECT_TIMEOUT_MS     3000   /* 客户端 connect 上限 */
#define HOP_CTRL_SEND_TIMEOUT_MS   3000   /* 控制通道单次 send 上限，超时即断开 */

/* 发送队列：鼠标移动/滚轮/心跳可以丢或合并；按键、松键、按钮、剪贴板、
 * 显示控制一个都不能丢 —— 丢一个 KEY_UP 就是对端卡键。
 * 软上限之后只接纳关键帧；硬上限 / 字节上限说明对端已经收不动了，
 * 继续攒只会让"松开"晚到几十秒，正确做法是断开、让对端 reset 输入状态。 */
#define HOP_CTRL_QUEUE_SOFT   2048
#define HOP_CTRL_QUEUE_HARD   4096
#define HOP_CTRL_QUEUE_BYTES  (16u * 1024u * 1024u)
typedef enum { HOP_Q_ADMIT = 0, HOP_Q_DROP = 1, HOP_Q_OVERFLOW = 2 } hop_queue_verdict;
int hop_msg_droppable(uint8_t type);
hop_queue_verdict hop_ctrl_queue_admit(uint8_t type, int count, size_t queued_bytes,
                                       uint32_t len);

/* v6 is deliberately incompatible with older peers: upgrade both endpoints. */
#define HOP_SESSION_BYTES 16u
#define HOP_DATA_PREFIX (HOP_SESSION_BYTES + 4u)
typedef struct { uint8_t bytes[HOP_SESSION_BYTES]; } hop_session_id;
int hop_session_id_valid(const hop_session_id *id);
void hop_session_hex(const hop_session_id *id, char out[33]);
int hop_session_id_equal(const hop_session_id *a, const hop_session_id *b);
int hop_session_generate(hop_session_id *out);
void hop_session_publish(const hop_session_id *id);
int hop_session_current(hop_session_id *out);
int hop_session_matches(const hop_session_id *id);
void hop_session_clear(const hop_session_id *expected);
uint64_t hop_session_generation(void);
/* Every non-HELLO data frame: session[16] + batch[4] + existing typed body.
 * CHUNK/END therefore carry explicit session AND batch without implicit state. */
void hop_data_prefix(uint8_t out[HOP_DATA_PREFIX], const hop_session_id *id, uint32_t batch);
int hop_data_unwrap(uint8_t type, const uint8_t *p, uint32_t len,
                    const hop_session_id *expected, uint32_t *batch,
                    const uint8_t **body, uint32_t *body_len);
uint32_t hop_xfer_hello_encode(uint8_t *out, const char *token, const hop_session_id *id);
int hop_xfer_hello_bound(const uint8_t *p, uint32_t len, const char *token,
                         const hop_session_id *expected);

/* HELLO: {tlen, token, nlen, name, w(4), h(4), ver(1)}；指针指向 payload 内部 */
typedef struct {
    const uint8_t *token;
    uint8_t        token_len;
    const uint8_t *name;
    uint8_t        name_len;
    uint32_t       width, height;
} hop_hello_msg;
int hop_hello_decode(const uint8_t *p, uint32_t len, hop_hello_msg *out);
/* expected 为空串 = 不校验（局域网自用，与旧行为一致）；否则长度与内容都必须相等。
 * 比较耗时不随第一个不同字节的位置变化。 */
int hop_token_matches(const char *expected, const uint8_t *got, size_t got_len);
/* HELLO_ACK: {ok, nlen, name, w(4), h(4), ver(1), session(16)} */
int hop_hello_ack_decode(const uint8_t *p, uint32_t len, int *ok,
                         uint32_t *width, uint32_t *height, hop_session_id *session);

/* 心跳判定。now/last 用同一个单调时钟（毫秒），回绕安全。 */
static inline int hop_heartbeat_due(uint64_t now, uint64_t last_sent) {
    return now - last_sent >= (uint64_t)HOP_HEARTBEAT_INTERVAL_MS;
}
static inline int hop_peer_silent(uint64_t now, uint64_t last_rx) {
    return now - last_rx > (uint64_t)HOP_HEARTBEAT_TIMEOUT_MS;
}
static inline int hop_handshake_expired(uint64_t now, uint64_t started) {
    return now - started > (uint64_t)HOP_HANDSHAKE_TIMEOUT_MS;
}

/* 单调时钟（毫秒）。clock.c，平台代码用；不受系统改时间影响。 */
uint64_t hop_monotonic_ms(void);
#define HOP_WATCHDOG_EXIT 70
#define HOP_IO_DEADLINE_MS 15000u
int hop_watchdog_start(void);
int hop_watchdog_begin(const char *operation, unsigned timeout_ms);
void hop_watchdog_ping(int ticket);
void hop_watchdog_end(int ticket);

/* ---- 小工具 ---- */
void hop_put_be16(uint8_t *p, uint16_t v);
void hop_put_be32(uint8_t *p, uint32_t v);
void hop_put_be64(uint8_t *p, uint64_t v);
uint16_t hop_get_be16(const uint8_t *p);
uint32_t hop_get_be32(const uint8_t *p);
uint64_t hop_get_be64(const uint8_t *p);

/* ---- 数据通道输入校验（平台无关，可单元测试） ---- */
typedef struct {
    uint32_t batch, id;
    uint64_t size, mtime;
    uint8_t flags;
    const uint8_t *name;
    uint16_t name_len;
    const uint8_t *relpath;
    uint16_t relpath_len;
} hop_xfer_file_begin_msg;

typedef struct {
    uint32_t batch, id, width, height, stride;
} hop_xfer_image_begin_msg;

typedef struct {
    uint32_t id;
    uint8_t status;
    uint32_t file_crc;
} hop_xfer_file_end_msg;

typedef struct {
    uint32_t batch, id;
    uint8_t status;
} hop_xfer_file_ack_msg;

typedef struct {
    uint32_t batch;
    uint8_t reason;
} hop_xfer_abort_msg;

typedef struct {
    uint32_t batch, ok, total;
    uint64_t bytes;
} hop_xfer_done_msg;

int hop_xfer_decode_file_begin(const uint8_t *payload, uint32_t len,
                               hop_xfer_file_begin_msg *out);
int hop_xfer_decode_image_begin(const uint8_t *payload, uint32_t len,
                                hop_xfer_image_begin_msg *out);
int hop_xfer_decode_file_end(const uint8_t *payload, uint32_t len,
                             hop_xfer_file_end_msg *out);
int hop_xfer_decode_file_ack(const uint8_t *payload, uint32_t len,
                             hop_xfer_file_ack_msg *out);
int hop_xfer_decode_abort(const uint8_t *payload, uint32_t len,
                          hop_xfer_abort_msg *out);
int hop_xfer_decode_done(const uint8_t *payload, uint32_t len,
                         hop_xfer_done_msg *out);
int hop_xfer_hello_valid(const uint8_t *payload, uint32_t len, const char *token);
int hop_xfer_relpath_bytes_safe(const uint8_t *rel, size_t len);
int hop_xfer_relpath_safe(const char *rel);
int hop_clip_should_apply_remote(uint32_t origin, uint32_t local_device);
uint32_t hop_clip_file_offer_encode(uint8_t *out, uint32_t capacity,
                                    uint32_t origin, uint64_t generation,
                                    const char *const *names, int count);
int hop_clip_file_offer_decode(const uint8_t *payload, uint32_t len,
                               hop_clip_file_offer_msg *out);
int hop_clip_file_offer_clear_decode(const uint8_t *payload, uint32_t len,
                                     uint32_t *origin, uint64_t *generation);
uint32_t hop_clip_paste_request_encode(uint8_t out[HOP_CLIP_PASTE_REQUEST_BYTES],
                                       uint32_t request_id, uint32_t origin,
                                       uint64_t generation, uint64_t clipboard_version);
int hop_clip_paste_request_decode(const uint8_t *payload, uint32_t len,
                                  hop_clip_paste_request_msg *out);
uint32_t hop_clip_paste_failed_encode(uint8_t out[HOP_CLIP_PASTE_FAILED_BYTES],
                                      uint32_t request_id, uint8_t reason);
int hop_clip_paste_failed_decode(const uint8_t *payload, uint32_t len,
                                 uint32_t *request_id, uint8_t *reason);
uint32_t hop_xfer_file_crc_step(uint32_t seed, uint64_t offset, uint32_t chunk_crc);
int hop_xfer_image_valid(int32_t width, int32_t height, int32_t stride);
int hop_xfer_chunk_valid(const uint8_t *payload, uint32_t len,
                         uint64_t declared_size, int is_image);

/* ---- CRC32（IEEE 802.3，增量式） ---- */
void     hop_crc32_init(void);
uint32_t hop_crc32(uint32_t seed, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* HOP_H */
