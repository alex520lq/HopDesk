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

#define HOP_VERSION        "0.1.0"
#define HOP_DEFAULT_PORT   24817
#define HOP_DATA_PORT      24818         /* 文件数据通道，与控制通道分离 */
#define HOP_TOKEN_MAX      128
#define HOP_NAME_MAX       32
#define HOP_TEXT_MAX       (64 * 1024)   /* 文本上限 */
#define HOP_NAME_LEN_MAX   255           /* 文件名（UTF-8）上限 */
#define HOP_PATH_LEN_MAX   1024          /* 相对路径（UTF-8）上限 */
#define HOP_HEADER_SIZE    5

/* 分块大小：1 MB。
 * 实测 1 MB 块 TCP 2548 MB/s，64 KB 块 1276 MB/s，1.4 KB 块 46 MB/s ——
 * 每块一次系统调用，块越大摊销越划算。1 MB 也是缓存行/页对齐的整数倍，
 * 磁盘顺序读在这个粒度上不会浪费预读。 */
#define HOP_CHUNK_SIZE     (1024u * 1024u)
/* 单帧载荷上限：一块 + 分块头（id4+off8+len4）+ 余量 */
#define HOP_MAX_PAYLOAD    (HOP_CHUNK_SIZE + 1024u)

/* ---- 消息类型 ---- */
typedef enum {
    HOP_MSG_HELLO        = 1,   /* C -> S: 握手 {token, name, w, h} */
    HOP_MSG_HELLO_ACK    = 2,   /* S -> C: 结果 {ok, name, w, h}   */
    HOP_MSG_PING         = 3,
    HOP_MSG_PONG         = 4,

    HOP_MSG_MOUSE_MOVE   = 10,  /* S -> C: dx,dy (int16 x2)        */
    HOP_MSG_MOUSE_BUTTON = 11,  /* S -> C: button(uint8),down(uint8) */
    HOP_MSG_MOUSE_WHEEL  = 12,  /* S -> C: dx,dy (int16 x2)        */
    HOP_MSG_EDGE_LEAVE   = 13,  /* C -> S: edge(uint8)             */
    HOP_MSG_MOUSE_ENTER  = 14,  /* S -> C: y(uint16, client 坐标)  */

    HOP_MSG_KEY_DOWN     = 20,  /* S -> C: keycode(uint16)         */
    HOP_MSG_KEY_UP       = 21,
    HOP_MSG_KEY_MODS     = 22,  /* S -> C: modmask(uint16)         */

    HOP_MSG_CLIP_TEXT    = 30,  /* 双向: origin(uint32) + UTF-8    */
    /* 31 曾是"整文件一帧传完"的 CLIP_FILE，已废弃：
     * 大文件走数据通道，单帧 8 MB 上限一并取消。 */

    HOP_MSG_DISP_WAKE    = 40,  /* S -> C: seconds(uint32)         */
    HOP_MSG_DISP_INPUT   = 41,  /* S -> C: vcp(uint16)             */
    HOP_MSG_DISP_SLEEP   = 42,

    /* ---- 数据通道（port+1，只跑文件/图片字节） ----
     * 与控制通道分开，是为了让大载荷不挤占鼠标/键盘帧：
     * 单条 TCP 流上的大块数据会造成队头阻塞，鼠标事件要等整块发完。
     * 图片也走这里 —— 一张 4K 截图裸 BGRA 就是 33 MB，塞不进控制通道。 */
    HOP_MSG_XFER_HELLO   = 50,  /* C -> S: ver(1)                  */
    HOP_MSG_FILE_BEGIN   = 51,  /* S -> C: id(4)+size(8)+mtime(8)+flags(1)
                                 *         +nlen(2)+name+plen(2)+relpath */
    HOP_MSG_FILE_CHUNK   = 52,  /* S -> C: id(4)+off(8)+len(4)+data */
    HOP_MSG_FILE_END     = 53,  /* S -> C: id(4)+status(1)          */
    HOP_MSG_XFER_DONE    = 54,  /* S -> C: count(4)+total(8)        */
    HOP_MSG_IMAGE_BEGIN  = 55   /* S -> C: id(4)+w(4)+h(4)+stride(4)+flags(1) */
} hop_msg_type;

/* ---- 鼠标按键 ---- */
#define HOP_BTN_LEFT     0
#define HOP_BTN_MIDDLE   1
#define HOP_BTN_RIGHT    2

/* ---- 文件条目标志 ---- */
#define HOP_FILE_FLAG_DIR    (1u << 0)   /* 空目录占位，无分块 */

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

/* ---- 配置 ---- */
typedef struct {
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
} hop_config;

void hop_config_defaults(hop_config *c);
int  hop_config_load(hop_config *c, const char *path);  /* 不存在返回 0 */

/* ---- Windows VK -> macOS 虚拟键码（平台无关的表，阶段 2） ---- */
#define HOP_KEY_NONE 0xFFFFu
uint16_t hop_vk_to_mac(int vk);                 /* 不支持返回 HOP_KEY_NONE */
int      hop_mac_key_is_mod(uint16_t mac_key);  /* 返回 HOP_MOD_* 位，非修饰键返回 0 */

/* ---- 日志 ---- */
typedef enum { HOP_LOG_INFO, HOP_LOG_WARN, HOP_LOG_ERROR } hop_log_level;
void hop_log_set(int enable);
void hop_log_open(const char *path);   /* NULL = 输出到 stderr */
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
void         hop_decoder_feed(hop_decoder *d, const uint8_t *buf, uint32_t n);
void         hop_decoder_reset(hop_decoder *d);
void         hop_decoder_free(hop_decoder *d);

/* ---- 小工具 ---- */
void hop_put_be16(uint8_t *p, uint16_t v);
void hop_put_be32(uint8_t *p, uint32_t v);
void hop_put_be64(uint8_t *p, uint64_t v);
uint16_t hop_get_be16(const uint8_t *p);
uint32_t hop_get_be32(const uint8_t *p);
uint64_t hop_get_be64(const uint8_t *p);

#ifdef __cplusplus
}
#endif
#endif /* HOP_H */
