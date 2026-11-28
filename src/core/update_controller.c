#include "update_controller.h"

#include <stdio.h>
#include <string.h>

static int sanitize_channel(int ch) {
    return ch == HOP_UPDATE_CHANNEL_DEV ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;
}

static const char *channel_label_zh(int ch) {
    return ch == HOP_UPDATE_CHANNEL_DEV ? "开发版" : "稳定版";
}

void hop_update_snapshot_init(hop_update_snapshot *s, int target_channel) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->phase = HOP_UPDATE_IDLE;
    s->target_channel = sanitize_channel(target_channel);
}

void hop_update_snapshot_set_checking(hop_update_snapshot *s, int target_channel) {
    if (!s) return;
    int ch = sanitize_channel(target_channel);
    memset(s, 0, sizeof(*s));
    s->phase = HOP_UPDATE_CHECKING;
    s->target_channel = ch;
    snprintf(s->message, sizeof(s->message),
             "正在检查 %s 更新清单…", channel_label_zh(ch));
}

void hop_update_snapshot_set_manifest_result(hop_update_snapshot *s,
                                             const char *current_version,
                                             int current_channel,
                                             int current_ctrl_proto,
                                             int current_xfer_proto,
                                             int target_channel,
                                             const hop_update_manifest *manifest) {
    int ch = sanitize_channel(target_channel);
    int cur_ch = sanitize_channel(current_channel);
    int is_switch = 0;
    int valid = 0;
    int offered = 0;

    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->target_channel = ch;
    if (!manifest || !current_version) {
        s->phase = HOP_UPDATE_ERROR;
        snprintf(s->message, sizeof(s->message), "%s", "更新清单无效，请稍后重试。");
        return;
    }

    offered = hop_update_evaluate(current_version, cur_ch, ch, manifest, &is_switch, &valid);
    if (!valid) {
        s->phase = HOP_UPDATE_ERROR;
        snprintf(s->message, sizeof(s->message), "%s",
                 "当前版本号或通道清单无效，无法执行自动更新。");
        return;
    }

    s->is_channel_switch = is_switch;
    s->target_ctrl_proto = manifest->ctrl_proto;
    s->target_xfer_proto = manifest->xfer_proto;
    s->protocol_changed =
        (manifest->ctrl_proto > 0 && manifest->ctrl_proto != current_ctrl_proto) ||
        (manifest->xfer_proto > 0 && manifest->xfer_proto != current_xfer_proto);
    snprintf(s->target_version, sizeof(s->target_version), "%s", manifest->version);
    snprintf(s->target_tag, sizeof(s->target_tag), "%s", manifest->tag);
    snprintf(s->notes_url, sizeof(s->notes_url), "%s", manifest->notes_url);
    snprintf(s->summary, sizeof(s->summary), "%s", manifest->summary);

    if (!offered) {
        s->phase = HOP_UPDATE_UP_TO_DATE;
        snprintf(s->message, sizeof(s->message),
                 "当前已是 %s 最新版本（v%s）。", channel_label_zh(ch), current_version);
        return;
    }

    s->phase = HOP_UPDATE_AVAILABLE;
    if (is_switch) {
        snprintf(s->message, sizeof(s->message),
                 "可切换至 %s v%s（当前 %s v%s）%s",
                 channel_label_zh(ch), manifest->version,
                 channel_label_zh(cur_ch), current_version,
                 s->protocol_changed ? " · 协议版本变化，建议两端同步更新" : "");
    } else {
        snprintf(s->message, sizeof(s->message),
                 "发现 %s 新版本 v%s（当前 v%s）%s",
                 channel_label_zh(ch), manifest->version, current_version,
                 s->protocol_changed ? " · 协议版本变化，建议两端同步更新" : "");
    }
}

void hop_update_snapshot_set_downloading(hop_update_snapshot *s,
                                         uint64_t bytes_done,
                                         uint64_t bytes_total) {
    if (!s) return;
    s->phase = HOP_UPDATE_DOWNLOADING;
    s->bytes_done = bytes_done;
    s->bytes_total = bytes_total;
    if (bytes_total > 0) {
        unsigned pct = (unsigned)((bytes_done >= bytes_total)
            ? 100u : (100.0 * (double)bytes_done) / (double)bytes_total);
        snprintf(s->message, sizeof(s->message),
                 "正在下载 v%s（%u%%）…", s->target_version, pct);
    } else {
        snprintf(s->message, sizeof(s->message),
                 "正在下载 v%s…", s->target_version);
    }
}

void hop_update_snapshot_set_ready(hop_update_snapshot *s, uint64_t total_bytes) {
    if (!s) return;
    s->phase = HOP_UPDATE_READY;
    s->bytes_done = total_bytes;
    s->bytes_total = total_bytes;
    snprintf(s->message, sizeof(s->message),
             "v%s（%s）已下载并通过 SHA-256 校验，点击“重启并安装更新”即可切换。",
             s->target_version, channel_label_zh(s->target_channel));
}

void hop_update_snapshot_set_applying(hop_update_snapshot *s) {
    if (!s) return;
    s->phase = HOP_UPDATE_APPLYING;
    snprintf(s->message, sizeof(s->message),
             "正在准备安装 v%s 并重启 HopDesk…", s->target_version);
}

void hop_update_snapshot_set_error(hop_update_snapshot *s,
                                   int target_channel,
                                   const char *message) {
    if (!s) return;
    s->phase = HOP_UPDATE_ERROR;
    if (target_channel == HOP_UPDATE_CHANNEL_STABLE || target_channel == HOP_UPDATE_CHANNEL_DEV)
        s->target_channel = target_channel;
    snprintf(s->message, sizeof(s->message), "%s",
             (message && message[0]) ? message : "更新失败，当前版本未被修改。");
}
