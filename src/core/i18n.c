#define HOP_I18N_DEFINE_TABLES
#include "i18n.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static volatile int s_cfg_lang = HOP_LANG_AUTO;
static volatile int s_sys_idx = -1; /* -1 = not overridden by platform adapter */
static volatile int s_active_idx = 0; /* 0 = zh-CN, 1 = en */
static volatile int s_initialized = 0;

static int detect_system_lang_idx(void) {
    if (s_sys_idx == 0 || s_sys_idx == 1) return s_sys_idx;
#if defined(_WIN32)
    LANGID lid = GetUserDefaultUILanguage();
    return PRIMARYLANGID(lid) == LANG_CHINESE ? 0 : 1;
#else
    const char *env = getenv("LC_ALL");
    if (!env || !env[0]) env = getenv("LC_MESSAGES");
    if (!env || !env[0]) env = getenv("LANG");
    if (env && (strncmp(env, "zh", 2) == 0 || strncmp(env, "ZH", 2) == 0)) return 0;
    return 1;
#endif
}

void hop_i18n_set_system_lang(int active_lang) {
    s_sys_idx = (active_lang == HOP_LANG_EN) ? 1 : 0;
    if (s_cfg_lang == HOP_LANG_AUTO) {
        s_active_idx = s_sys_idx;
        s_initialized = 1;
    }
}

void hop_i18n_set_lang(int ui_lang) {
    if (ui_lang < HOP_LANG_AUTO || ui_lang > HOP_LANG_EN)
        ui_lang = HOP_LANG_AUTO;
    s_cfg_lang = ui_lang;
    if (ui_lang == HOP_LANG_ZH_CN) s_active_idx = 0;
    else if (ui_lang == HOP_LANG_EN) s_active_idx = 1;
    else s_active_idx = detect_system_lang_idx();
    s_initialized = 1;
}

int hop_i18n_get_config_lang(void) {
    return s_cfg_lang;
}

int hop_i18n_active_lang(void) {
    if (!s_initialized) hop_i18n_set_lang(HOP_LANG_AUTO);
    return s_active_idx == 0 ? HOP_LANG_ZH_CN : HOP_LANG_EN;
}

const char *hop_tr(hop_str_id id) {
    if (!s_initialized) hop_i18n_set_lang(HOP_LANG_AUTO);
    int idx = s_active_idx;
    if (idx < 0 || idx > 1) idx = 0;
    if ((unsigned)id >= (unsigned)HOP_STR_COUNT) return "";
    return k_hop_i18n[idx][id];
}
