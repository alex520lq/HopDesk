#ifndef HOP_I18N_H
#define HOP_I18N_H

#include "i18n_table.h"

typedef enum {
    HOP_LANG_AUTO = 0,
    HOP_LANG_ZH_CN = 1,
    HOP_LANG_EN = 2
} hop_ui_lang;

void hop_i18n_set_system_lang(int active_lang); /* optional OS-detected fallback for AUTO */
void hop_i18n_set_lang(int ui_lang);
int  hop_i18n_get_config_lang(void);
int  hop_i18n_active_lang(void); /* HOP_LANG_ZH_CN or HOP_LANG_EN */
const char *hop_tr(hop_str_id id);

#endif /* HOP_I18N_H */
