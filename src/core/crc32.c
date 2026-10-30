/* =====================================================================
 *  HopDesk - CRC32（IEEE 802.3，与 zlib / PNG 同多项式）
 *
 *  增量式：hop_crc32(0, buf, n) 得整段校验值；
 *          hop_crc32(prev, buf, n) 可继续累加下一段。
 *
 *  为什么用 CRC32 而不是 SHA-256：
 *    这里要防的是传输/内存层面的位翻转，不是恶意篡改。CRC32 对突发错误
 *    的漏检率约 2^-32，而速度是 SHA-256 的十几倍（表驱动约 1–2 GB/s，
 *    相比网络 100 MB/s 完全不是瓶颈）。GB 级传输若用 SHA-256，
 *    光哈希就能吃掉可观 CPU。
 * ===================================================================== */
#include "hop.h"

static uint32_t s_tab[256];
static volatile int s_ready;

static void crc_build(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_tab[i] = c;
    }
    s_ready = 1;
}

/* 启动时单线程调用一次，避免首次并发进 crc_build */
void hop_crc32_init(void) {
    if (!s_ready) crc_build();
}

uint32_t hop_crc32(uint32_t seed, const void *data, size_t len) {
    if (!s_ready) crc_build();

    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = seed ^ 0xFFFFFFFFu;
    while (len--) c = s_tab[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
