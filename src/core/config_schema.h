/* =====================================================================
 *  HopDesk - Unified Configuration Schema & Validation (Core, Pure C)
 *  Single source of truth for hopdesk.conf, GUI validation, IPC, and CLI.
 * ===================================================================== */
#ifndef HOP_CONFIG_SCHEMA_H
#define HOP_CONFIG_SCHEMA_H

#include "hop.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOP_CFG_INT = 0,
    HOP_CFG_U16,
    HOP_CFG_HEX16,
    HOP_CFG_BOOL,
    HOP_CFG_ENUM,
    HOP_CFG_STRING
} hop_cfg_type;

typedef enum {
    HOP_CFG_APPLY_HOT       = 1,
    HOP_CFG_APPLY_RECONNECT = 2,
    HOP_CFG_APPLY_RESTART   = 4
} hop_cfg_apply;

typedef struct {
    const char    *key;
    const char    *alias;        /* optional legacy key name, or NULL */
    const char    *group;        /* domain group name */
    hop_cfg_type   type;
    hop_cfg_apply  apply;
    int            secret;       /* 1 if value should be masked unless explicitly shown */
    int            min_val;
    int            max_val;
    const char    *enum_values;  /* pipe-separated names for ENUM, or NULL */
    const char    *default_str;  /* human/machine default representation */
    const char    *description;
    size_t         offset;
    size_t         size;
} hop_config_spec;

size_t hop_config_spec_count(void);
const hop_config_spec *hop_config_spec_at(size_t index);
const hop_config_spec *hop_config_spec_find(const char *key);

/* Parse and assign one key=value pair into c (accepts canonical key or alias).
 * Returns 1 on success, 0 if unknown key, -1 if invalid value (err populated). */
int hop_config_set_kv(hop_config *c, const char *key, const char *value,
                      char *err, size_t err_cap);

/* Reset one key to its platform default. Returns 1 on success, 0 if unknown. */
int hop_config_reset_key(hop_config *c, const char *key);

/* Format a config field value into canonical string representation. */
int hop_config_format_value(const hop_config *c, const hop_config_spec *spec,
                            char *out, size_t cap);

/* Compare one field between two hop_config structs. Returns 1 if equal. */
int hop_config_values_equal(const hop_config *a, const hop_config *b,
                            const hop_config_spec *spec);

/* Validate all field ranges and cross-field invariants.
 * When strict_runtime != 0, also requires non-empty device_name, token,
 * and peer_host (for client role). Returns 1 if valid, 0 otherwise. */
int hop_config_validate(const hop_config *c, int strict_runtime,
                        char *err, size_t err_cap);

/* Extended loader that reports detailed line/key errors into err.
 * Returns 1 on valid file loaded, 0 if file missing or invalid.
 * Sets *file_missing = 1 when the file could not be opened. */
int hop_config_load_ex(hop_config *c, const char *path,
                       int *file_missing, char *err, size_t err_cap);

const char *hop_cfg_type_name(hop_cfg_type type);
const char *hop_cfg_apply_name(hop_cfg_apply apply);

#ifdef __cplusplus
}
#endif
#endif /* HOP_CONFIG_SCHEMA_H */
