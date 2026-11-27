/* =====================================================================
 *  HopDesk - Headless CLI Engine Implementation (Pure C, Zero External Deps)
 * ===================================================================== */
#include "cli.h"
#include "config_schema.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

static void cli_write(const hop_cli_ops *ops, int err_stream, const char *s, size_t n) {
    if (!s || !n) return;
    if (err_stream) {
        if (ops && ops->write_err) ops->write_err(ops->io_ctx, s, n);
        else (void)fwrite(s, 1, n, stderr);
    } else {
        if (ops && ops->write_out) ops->write_out(ops->io_ctx, s, n);
        else (void)fwrite(s, 1, n, stdout);
    }
}

static void cli_puts(const hop_cli_ops *ops, int err_stream, const char *s) {
    if (s) cli_write(ops, err_stream, s, strlen(s));
}

static void cli_printf(const hop_cli_ops *ops, int err_stream, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t len = (size_t)n >= sizeof(buf) ? sizeof(buf) - 1 : (size_t)n;
    cli_write(ops, err_stream, buf, len);
}

static void json_str(const hop_cli_ops *ops, int err_stream, const char *s) {
    cli_puts(ops, err_stream, "\"");
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
            switch (*p) {
                case '\"': cli_puts(ops, err_stream, "\\\""); break;
                case '\\': cli_puts(ops, err_stream, "\\\\"); break;
                case '\b': cli_puts(ops, err_stream, "\\b"); break;
                case '\f': cli_puts(ops, err_stream, "\\f"); break;
                case '\n': cli_puts(ops, err_stream, "\\n"); break;
                case '\r': cli_puts(ops, err_stream, "\\r"); break;
                case '\t': cli_puts(ops, err_stream, "\\t"); break;
                default:
                    if (*p < 0x20) cli_printf(ops, err_stream, "\\u%04x", (unsigned)*p);
                    else cli_write(ops, err_stream, (const char *)p, 1);
                    break;
            }
        }
    }
    cli_puts(ops, err_stream, "\"");
}

static void json_enum_array(const hop_cli_ops *ops, const char *values) {
    cli_puts(ops, 0, "[");
    if (values) {
        const char *p = values;
        int first = 1;
        while (*p) {
            const char *bar = strchr(p, '|');
            size_t len = bar ? (size_t)(bar - p) : strlen(p);
            char item[64];
            if (len >= sizeof(item)) len = sizeof(item) - 1;
            memcpy(item, p, len);
            item[len] = '\0';
            if (!first) cli_puts(ops, 0, ",");
            json_str(ops, 0, item);
            first = 0;
            if (!bar) break;
            p = bar + 1;
        }
    }
    cli_puts(ops, 0, "]");
}

static int file_exists_utf8(const char *path) {
    if (!path || !path[0]) return 0;
#if defined(_WIN32)
    wchar_t wide[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1024) <= 0)
        return 0;
    DWORD attr = GetFileAttributesW(wide);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static void init_platform_defaults(hop_config *c, const hop_cli_ops *ops) {
    hop_config_defaults(c);
    if (ops && ops->default_device_name && ops->default_device_name[0])
        snprintf(c->device_name, sizeof(c->device_name), "%s", ops->default_device_name);
}

static void emit_error(const hop_cli_ops *ops, int json, const char *msg) {
    if (json) {
        cli_puts(ops, 0, "{\"ok\":false,\"error\":");
        json_str(ops, 0, msg ? msg : "unknown error");
        cli_puts(ops, 0, "}\n");
    } else {
        cli_printf(ops, 1, "error: %s\n", msg ? msg : "unknown error");
    }
}

int hop_cli_is_invocation(int argc, const char *const *argv) {
    if (argc < 2 || !argv || !argv[1]) return 0;
    const char *a = argv[1];
    return strcmp(a, "config") == 0 ||
           strcmp(a, "status") == 0 ||
           strcmp(a, "action") == 0 ||
           strcmp(a, "help") == 0 ||
           strcmp(a, "version") == 0 ||
           strcmp(a, "--help") == 0 ||
           strcmp(a, "-h") == 0 ||
           strcmp(a, "--version") == 0 ||
           strcmp(a, "-v") == 0;
}

const char *hop_cli_extract_config_flag(int argc, const char *const *argv) {
    if (!argv) return NULL;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            return argv[i + 1];
        if (strncmp(argv[i], "--config=", 9) == 0 && argv[i][9] != '\0')
            return argv[i] + 9;
    }
    return NULL;
}

static void print_help(const hop_cli_ops *ops) {
    cli_puts(ops, 0,
        "HopDesk " HOP_VERSION " — Headless Configuration & Control CLI\n"
        "\n"
        "Usage:\n"
        "  hopdesk config path [--config <path>] [--json]\n"
        "  hopdesk config schema [--group <name>] [--key <key>] [--json]\n"
        "  hopdesk config get [key ...] [--config <path>] [--show-secret] [--no-live] [--json]\n"
        "  hopdesk config set key=value ... [--config <path>] [--no-live] [--strict] [--json]\n"
        "  hopdesk config reset [key ...] [--config <path>] [--no-live] [--json]\n"
        "  hopdesk config validate [--config <path>] [--strict] [--json]\n"
        "  hopdesk status [--json]\n"
        "  hopdesk action <reconnect|disconnect|display-mac|display-win|display-wake|display-sleep|reload> [--json]\n"
        "  hopdesk --agent [--config <path>]\n"
        "  hopdesk version [--json]\n");
}

static int cmd_config_path(const hop_cli_ops *ops, const char *cfg_path, int json) {
    int exists = file_exists_utf8(cfg_path);
    const char *log_path = (ops && ops->default_log_path) ? ops->default_log_path : "";
    if (json) {
        cli_puts(ops, 0, "{\"ok\":true,\"config_path\":");
        json_str(ops, 0, cfg_path ? cfg_path : "");
        cli_printf(ops, 0, ",\"exists\":%s,\"log_path\":", exists ? "true" : "false");
        json_str(ops, 0, log_path);
        cli_puts(ops, 0, "}\n");
    } else {
        cli_printf(ops, 0, "config_path=%s\nexists=%s\nlog_path=%s\n",
                   cfg_path ? cfg_path : "", exists ? "true" : "false", log_path);
    }
    return 0;
}

static int cmd_config_schema(const hop_cli_ops *ops, const char *group_filter,
                             const char *key_filter, int json) {
    size_t n = hop_config_spec_count();
    if (key_filter && !hop_config_spec_find(key_filter)) {
        char err[128];
        snprintf(err, sizeof(err), "unknown config key '%s'", key_filter);
        emit_error(ops, json, err);
        return 1;
    }
    if (json) {
        cli_puts(ops, 0, "{\"ok\":true,\"version\":");
        json_str(ops, 0, HOP_VERSION);
        cli_puts(ops, 0, ",\"parameters\":[");
        int first = 1;
        for (size_t i = 0; i < n; ++i) {
            const hop_config_spec *s = hop_config_spec_at(i);
            if (group_filter && strcmp(s->group, group_filter) != 0) continue;
            if (key_filter && strcmp(s->key, key_filter) != 0 &&
                (!s->alias || strcmp(s->alias, key_filter) != 0)) continue;
            if (!first) cli_puts(ops, 0, ",");
            first = 0;
            cli_puts(ops, 0, "{\"key\":");
            json_str(ops, 0, s->key);
            if (s->alias) {
                cli_puts(ops, 0, ",\"alias\":");
                json_str(ops, 0, s->alias);
            }
            cli_puts(ops, 0, ",\"group\":");
            json_str(ops, 0, s->group);
            cli_puts(ops, 0, ",\"type\":");
            json_str(ops, 0, hop_cfg_type_name(s->type));
            cli_puts(ops, 0, ",\"apply\":");
            json_str(ops, 0, hop_cfg_apply_name(s->apply));
            cli_printf(ops, 0, ",\"secret\":%s", s->secret ? "true" : "false");
            if (s->type == HOP_CFG_INT || s->type == HOP_CFG_U16 || s->type == HOP_CFG_HEX16) {
                cli_printf(ops, 0, ",\"min\":%d,\"max\":%d", s->min_val, s->max_val);
            } else if (s->type == HOP_CFG_STRING) {
                cli_printf(ops, 0, ",\"max_length\":%d", s->max_val);
            } else if (s->type == HOP_CFG_ENUM) {
                cli_puts(ops, 0, ",\"values\":");
                json_enum_array(ops, s->enum_values);
            }
            cli_puts(ops, 0, ",\"default\":");
            json_str(ops, 0, s->default_str);
            cli_puts(ops, 0, ",\"description\":");
            json_str(ops, 0, s->description);
            cli_puts(ops, 0, "}");
        }
        cli_puts(ops, 0, "]}\n");
    } else {
        for (size_t i = 0; i < n; ++i) {
            const hop_config_spec *s = hop_config_spec_at(i);
            if (group_filter && strcmp(s->group, group_filter) != 0) continue;
            if (key_filter && strcmp(s->key, key_filter) != 0 &&
                (!s->alias || strcmp(s->alias, key_filter) != 0)) continue;
            cli_printf(ops, 0, "%-28s %-12s %-7s %-9s default=%-24s %s\n",
                       s->key, s->group, hop_cfg_type_name(s->type),
                       hop_cfg_apply_name(s->apply), s->default_str, s->description);
        }
    }
    return 0;
}

static void emit_json_typed_value(const hop_cli_ops *ops, const hop_config *cfg,
                                  const hop_config_spec *s, int show_secret) {
    char buf[512];
    if (!hop_config_format_value(cfg, s, buf, sizeof(buf))) {
        cli_puts(ops, 0, "null");
        return;
    }
    if (s->secret && !show_secret) {
        json_str(ops, 0, buf[0] ? "***" : "");
        return;
    }
    if (s->type == HOP_CFG_INT || s->type == HOP_CFG_U16) {
        cli_puts(ops, 0, buf);
    } else if (s->type == HOP_CFG_BOOL) {
        cli_puts(ops, 0, strcmp(buf, "1") == 0 ? "true" : "false");
    } else {
        json_str(ops, 0, buf);
    }
}

static int cmd_config_get(const hop_cli_ops *ops, const char *cfg_path, int custom_path,
                          int no_live, int show_secret, int json,
                          const char *const *keys, int key_count) {
    for (int i = 0; i < key_count; ++i) {
        if (!hop_config_spec_find(keys[i])) {
            char err[128];
            snprintf(err, sizeof(err), "unknown config key '%s'", keys[i]);
            emit_error(ops, json, err);
            return 1;
        }
    }

    hop_config cfg;
    init_platform_defaults(&cfg, ops);
    const char *source = "defaults";
    int agent_live = 0;
    if (!custom_path && !no_live && ops && ops->agent_get_config) {
        hop_config live;
        if (ops->agent_get_config(ops->io_ctx, &live) == 1) {
            cfg = live;
            source = "agent";
            agent_live = 1;
        }
    }
    if (!agent_live) {
        int missing = 0;
        char err[256] = {0};
        if (hop_config_load_ex(&cfg, cfg_path, &missing, err, sizeof(err))) {
            source = "file";
        } else if (!missing) {
            emit_error(ops, json, err);
            return 1;
        }
    }

    if (json) {
        cli_puts(ops, 0, "{\"ok\":true,\"source\":");
        json_str(ops, 0, source);
        cli_puts(ops, 0, ",\"config_path\":");
        json_str(ops, 0, cfg_path ? cfg_path : "");
        cli_printf(ops, 0, ",\"token_set\":%s,\"values\":{", cfg.token[0] ? "true" : "false");
        int first = 1;
        if (key_count > 0) {
            for (int i = 0; i < key_count; ++i) {
                const hop_config_spec *s = hop_config_spec_find(keys[i]);
                if (!first) cli_puts(ops, 0, ",");
                first = 0;
                json_str(ops, 0, s->key);
                cli_puts(ops, 0, ":");
                emit_json_typed_value(ops, &cfg, s, show_secret);
            }
        } else {
            size_t total = hop_config_spec_count();
            for (size_t i = 0; i < total; ++i) {
                const hop_config_spec *s = hop_config_spec_at(i);
                if (!first) cli_puts(ops, 0, ",");
                first = 0;
                json_str(ops, 0, s->key);
                cli_puts(ops, 0, ":");
                emit_json_typed_value(ops, &cfg, s, show_secret);
            }
        }
        cli_puts(ops, 0, "}}\n");
    } else {
        if (key_count > 0) {
            for (int i = 0; i < key_count; ++i) {
                const hop_config_spec *s = hop_config_spec_find(keys[i]);
                char buf[512];
                (void)hop_config_format_value(&cfg, s, buf, sizeof(buf));
                const char *shown = (s->secret && !show_secret) ? (buf[0] ? "***" : "") : buf;
                if (key_count == 1) cli_printf(ops, 0, "%s\n", shown);
                else cli_printf(ops, 0, "%s=%s\n", s->key, shown);
            }
        } else {
            size_t total = hop_config_spec_count();
            for (size_t i = 0; i < total; ++i) {
                const hop_config_spec *s = hop_config_spec_at(i);
                char buf[512];
                (void)hop_config_format_value(&cfg, s, buf, sizeof(buf));
                const char *shown = (s->secret && !show_secret) ? (buf[0] ? "***" : "") : buf;
                cli_printf(ops, 0, "%s=%s\n", s->key, shown);
            }
        }
    }
    return 0;
}

static void emit_key_list_json(const hop_cli_ops *ops, const hop_config *before,
                               const hop_config *after, int apply_filter) {
    size_t total = hop_config_spec_count();
    cli_puts(ops, 0, "[");
    int first = 1;
    for (size_t i = 0; i < total; ++i) {
        const hop_config_spec *s = hop_config_spec_at(i);
        if (hop_config_values_equal(before, after, s)) continue;
        if (apply_filter && (int)s->apply != apply_filter) continue;
        if (!first) cli_puts(ops, 0, ",");
        first = 0;
        json_str(ops, 0, s->key);
    }
    cli_puts(ops, 0, "]");
}

static int persist_and_report(const hop_cli_ops *ops, const char *cfg_path,
                              int custom_path, int no_live, int strict, int json,
                              const hop_config *before, const hop_config *after) {
    char err[256] = {0};
    if (!hop_config_validate(after, strict, err, sizeof(err))) {
        emit_error(ops, json, err);
        return 1;
    }

    int agent_notified = 0;
    if (!custom_path && !no_live && ops && ops->agent_save_config) {
        int rc = ops->agent_save_config(ops->io_ctx, after);
        if (rc == 1) {
            agent_notified = 1;
        } else if (rc < 0) {
            emit_error(ops, json, "running agent rejected configuration update");
            return 1;
        }
    }
    if (!agent_notified) {
        if (!hop_config_save_atomic(after, cfg_path)) {
            emit_error(ops, json, "failed to save configuration atomically");
            return 1;
        }
    }

    /* Collect non-fatal readiness warnings for AI callers. */
    const char *warnings[3];
    int warn_count = 0;
    if (!after->device_name[0]) warnings[warn_count++] = "name is empty";
    if (!after->token[0]) warnings[warn_count++] = "token is empty; authentication will fail until set";
    if (after->role == HOP_ROLE_CLIENT && !after->peer_host[0])
        warnings[warn_count++] = "host is empty while role=client";

    if (json) {
        cli_puts(ops, 0, "{\"ok\":true,\"config_path\":");
        json_str(ops, 0, cfg_path ? cfg_path : "");
        cli_printf(ops, 0, ",\"agent_notified\":%s,\"changed\":",
                   agent_notified ? "true" : "false");
        emit_key_list_json(ops, before, after, 0);
        cli_puts(ops, 0, ",\"applied_hot\":");
        if (agent_notified) emit_key_list_json(ops, before, after, HOP_CFG_APPLY_HOT);
        else cli_puts(ops, 0, "[]");
        cli_puts(ops, 0, ",\"requires_reconnect\":");
        emit_key_list_json(ops, before, after, HOP_CFG_APPLY_RECONNECT);
        cli_puts(ops, 0, ",\"requires_restart\":");
        emit_key_list_json(ops, before, after, HOP_CFG_APPLY_RESTART);
        cli_puts(ops, 0, ",\"warnings\":[");
        for (int i = 0; i < warn_count; ++i) {
            if (i) cli_puts(ops, 0, ",");
            json_str(ops, 0, warnings[i]);
        }
        cli_puts(ops, 0, "]}\n");
    } else {
        cli_printf(ops, 0, "saved=%s agent_notified=%s\n",
                   cfg_path ? cfg_path : "", agent_notified ? "true" : "false");
        for (int i = 0; i < warn_count; ++i)
            cli_printf(ops, 0, "warning: %s\n", warnings[i]);
    }
    return 0;
}

static int load_base_config(const hop_cli_ops *ops, const char *cfg_path,
                            int custom_path, int no_live, hop_config *out,
                            char *err, size_t err_cap) {
    init_platform_defaults(out, ops);
    if (!custom_path && !no_live && ops && ops->agent_get_config) {
        hop_config live;
        if (ops->agent_get_config(ops->io_ctx, &live) == 1) {
            *out = live;
            return 1;
        }
    }
    int missing = 0;
    if (hop_config_load_ex(out, cfg_path, &missing, err, err_cap))
        return 1;
    if (missing) return 1;
    return 0;
}

static int cmd_config_set(const hop_cli_ops *ops, const char *cfg_path, int custom_path,
                          int no_live, int strict, int json,
                          const char *const *pairs, int pair_count) {
    if (pair_count <= 0) {
        emit_error(ops, json, "expected at least one key=value assignment");
        return 2;
    }
    hop_config before, after;
    char err[256] = {0};
    if (!load_base_config(ops, cfg_path, custom_path, no_live, &before, err, sizeof(err))) {
        emit_error(ops, json, err);
        return 1;
    }
    after = before;

    for (int i = 0; i < pair_count; ++i) {
        const char *eq = strchr(pairs[i], '=');
        if (!eq || eq == pairs[i]) {
            snprintf(err, sizeof(err), "invalid assignment '%s' (expected key=value)", pairs[i]);
            emit_error(ops, json, err);
            return 2;
        }
        char key[128];
        size_t klen = (size_t)(eq - pairs[i]);
        if (klen >= sizeof(key)) {
            emit_error(ops, json, "config key name too long");
            return 2;
        }
        memcpy(key, pairs[i], klen);
        key[klen] = '\0';
        int rc = hop_config_set_kv(&after, key, eq + 1, err, sizeof(err));
        if (rc <= 0) {
            emit_error(ops, json, err);
            return 1;
        }
    }
    return persist_and_report(ops, cfg_path, custom_path, no_live, strict, json, &before, &after);
}

static int cmd_config_reset(const hop_cli_ops *ops, const char *cfg_path, int custom_path,
                            int no_live, int json,
                            const char *const *keys, int key_count) {
    hop_config before, after;
    char err[256] = {0};
    if (!load_base_config(ops, cfg_path, custom_path, no_live, &before, err, sizeof(err))) {
        emit_error(ops, json, err);
        return 1;
    }
    after = before;
    if (key_count == 0) {
        init_platform_defaults(&after, ops);
    } else {
        for (int i = 0; i < key_count; ++i) {
            if (!hop_config_reset_key(&after, keys[i])) {
                snprintf(err, sizeof(err), "unknown config key '%s'", keys[i]);
                emit_error(ops, json, err);
                return 1;
            }
            if (strcmp(keys[i], "name") == 0 && ops && ops->default_device_name && ops->default_device_name[0])
                snprintf(after.device_name, sizeof(after.device_name), "%s", ops->default_device_name);
        }
    }
    return persist_and_report(ops, cfg_path, custom_path, no_live, 0, json, &before, &after);
}

static int cmd_config_validate(const hop_cli_ops *ops, const char *cfg_path,
                               int strict, int json) {
    hop_config cfg;
    init_platform_defaults(&cfg, ops);
    int missing = 0;
    char err[256] = {0};
    int loaded = hop_config_load_ex(&cfg, cfg_path, &missing, err, sizeof(err));
    if (!loaded) {
        emit_error(ops, json, missing ? "config file does not exist" : err);
        return 1;
    }
    if (!hop_config_validate(&cfg, strict, err, sizeof(err))) {
        emit_error(ops, json, err);
        return 1;
    }
    if (json) {
        cli_puts(ops, 0, "{\"ok\":true,\"valid\":true,\"strict\":");
        cli_puts(ops, 0, strict ? "true" : "false");
        cli_puts(ops, 0, ",\"config_path\":");
        json_str(ops, 0, cfg_path ? cfg_path : "");
        cli_puts(ops, 0, "}\n");
    } else {
        cli_printf(ops, 0, "valid: %s\n", cfg_path ? cfg_path : "");
    }
    return 0;
}

static int cmd_status(const hop_cli_ops *ops, int json) {
    hop_cli_status st;
    memset(&st, 0, sizeof(st));
    int ok = (ops && ops->agent_get_status) ? ops->agent_get_status(ops->io_ctx, &st) : 0;
    if (ok != 1) {
        if (json) {
            cli_puts(ops, 0, "{\"ok\":true,\"running\":false,\"config_path\":");
            json_str(ops, 0, (ops && ops->default_config_path) ? ops->default_config_path : "");
            cli_puts(ops, 0, "}\n");
        } else {
            cli_puts(ops, 0, "running=false\n");
        }
        return 0;
    }
    if (json) {
        cli_printf(ops, 0,
            "{\"ok\":true,\"running\":true,\"agent_pid\":%d,\"role\":%s,"
            "\"connected\":%s,\"sharing\":%s,\"data_connected\":%s,"
            "\"manually_disconnected\":%s,\"active_tasks\":%d,"
            "\"input_mode\":%u,\"peer_input_mode\":%u,"
            "\"audio_requested\":%u,\"audio_active\":%u,\"audio_unavailable\":%u,"
            "\"peer_audio_mode\":%u,\"peer_capabilities_known\":%s,\"peer_label\":",
            st.agent_pid,
            st.role == HOP_ROLE_SERVER ? "\"server\"" : "\"client\"",
            st.connected ? "true" : "false",
            st.sharing ? "true" : "false",
            st.data_connected ? "true" : "false",
            st.manually_disconnected ? "true" : "false",
            st.active_tasks,
            st.input_mode, st.peer_input_mode,
            st.audio_requested, st.audio_active, st.audio_unavailable,
            st.peer_audio_mode,
            st.peer_capabilities_known ? "true" : "false");
        json_str(ops, 0, st.peer_label);
        cli_puts(ops, 0, ",\"config_path\":");
        json_str(ops, 0, (ops && ops->default_config_path) ? ops->default_config_path : "");
        cli_puts(ops, 0, "}\n");
    } else {
        cli_printf(ops, 0,
            "running=true\nagent_pid=%d\nrole=%s\nconnected=%s\nsharing=%s\n"
            "data_connected=%s\nmanually_disconnected=%s\nactive_tasks=%d\n"
            "input_mode=%u\naudio_requested=%u\naudio_active=%u\npeer=%s\n",
            st.agent_pid, st.role == HOP_ROLE_SERVER ? "server" : "client",
            st.connected ? "true" : "false", st.sharing ? "true" : "false",
            st.data_connected ? "true" : "false", st.manually_disconnected ? "true" : "false",
            st.active_tasks, st.input_mode, st.audio_requested, st.audio_active, st.peer_label);
    }
    return 0;
}

static int cmd_action(const hop_cli_ops *ops, const char *name, int json) {
    hop_cli_action act;
    if (!name) {
        emit_error(ops, json, "missing action name");
        return 2;
    }
    if (strcmp(name, "reconnect") == 0) act = HOP_CLI_ACTION_RECONNECT;
    else if (strcmp(name, "disconnect") == 0) act = HOP_CLI_ACTION_DISCONNECT;
    else if (strcmp(name, "display-mac") == 0) act = HOP_CLI_ACTION_DISPLAY_MAC;
    else if (strcmp(name, "display-win") == 0) act = HOP_CLI_ACTION_DISPLAY_WIN;
    else if (strcmp(name, "display-wake") == 0) act = HOP_CLI_ACTION_DISPLAY_WAKE;
    else if (strcmp(name, "display-sleep") == 0) act = HOP_CLI_ACTION_DISPLAY_SLEEP;
    else if (strcmp(name, "reload") == 0) act = HOP_CLI_ACTION_RELOAD;
    else {
        char err[128];
        snprintf(err, sizeof(err), "unknown action '%s'", name);
        emit_error(ops, json, err);
        return 2;
    }

    int rc = (ops && ops->agent_action) ? ops->agent_action(ops->io_ctx, act) : 0;
    if (rc == 0) {
        emit_error(ops, json, "HopDesk background agent is not running");
        return 1;
    }
    if (rc < 0) {
        emit_error(ops, json, "agent rejected or failed to execute action");
        return 1;
    }
    if (json) {
        cli_puts(ops, 0, "{\"ok\":true,\"action\":");
        json_str(ops, 0, name);
        cli_puts(ops, 0, "}\n");
    } else {
        cli_printf(ops, 0, "ok: %s\n", name);
    }
    return 0;
}

int hop_cli_run(int argc, const char *const *argv, const hop_cli_ops *ops) {
    if (argc < 2 || !argv || !argv[1]) {
        print_help(ops);
        return 2;
    }
    const char *cmd = argv[1];
    int json = 0;
    for (int i = 1; i < argc; ++i)
        if (argv[i] && strcmp(argv[i], "--json") == 0) json = 1;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_help(ops);
        return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        if (json) {
            cli_puts(ops, 0, "{\"ok\":true,\"version\":");
            json_str(ops, 0, HOP_VERSION);
            cli_printf(ops, 0, ",\"ctrl_proto\":%d,\"xfer_proto\":%d}\n",
                       HOP_CTRL_PROTO_VER, HOP_XFER_PROTO_VER);
        } else {
            cli_printf(ops, 0, "HopDesk %s (ctrl v%d, xfer v%d)\n",
                       HOP_VERSION, HOP_CTRL_PROTO_VER, HOP_XFER_PROTO_VER);
        }
        return 0;
    }
    if (strcmp(cmd, "status") == 0) return cmd_status(ops, json);
    if (strcmp(cmd, "action") == 0) {
        const char *act_name = NULL;
        for (int i = 2; i < argc; ++i) {
            if (argv[i] && argv[i][0] != '-') { act_name = argv[i]; break; }
        }
        return cmd_action(ops, act_name, json);
    }
    if (strcmp(cmd, "config") != 0) {
        char err[128];
        snprintf(err, sizeof(err), "unknown command '%s'", cmd);
        emit_error(ops, json, err);
        return 2;
    }
    if (argc < 3 || !argv[2]) {
        emit_error(ops, json, "missing config subcommand (path|schema|get|set|reset|validate)");
        return 2;
    }

    const char *sub = argv[2];
    const char *cfg_override = NULL;
    const char *group_filter = NULL;
    const char *key_filter = NULL;
    int no_live = 0, show_secret = 0, strict = 0;
    const char *positionals[128];
    int pos_count = 0;

    for (int i = 3; i < argc; ++i) {
        const char *a = argv[i];
        if (!a) continue;
        if (strcmp(a, "--json") == 0) { json = 1; }
        else if (strcmp(a, "--no-live") == 0) { no_live = 1; }
        else if (strcmp(a, "--show-secret") == 0) { show_secret = 1; }
        else if (strcmp(a, "--strict") == 0) { strict = 1; }
        else if (strcmp(a, "--config") == 0 && i + 1 < argc) { cfg_override = argv[++i]; }
        else if (strncmp(a, "--config=", 9) == 0) { cfg_override = a + 9; }
        else if (strcmp(a, "--group") == 0 && i + 1 < argc) { group_filter = argv[++i]; }
        else if (strncmp(a, "--group=", 8) == 0) { group_filter = a + 8; }
        else if (strcmp(a, "--key") == 0 && i + 1 < argc) { key_filter = argv[++i]; }
        else if (strncmp(a, "--key=", 6) == 0) { key_filter = a + 6; }
        else if (a[0] == '-') {
            char err[128];
            snprintf(err, sizeof(err), "unknown option '%s'", a);
            emit_error(ops, json, err);
            return 2;
        } else if (pos_count < (int)(sizeof(positionals) / sizeof(positionals[0]))) {
            positionals[pos_count++] = a;
        }
    }

    const char *cfg_path = (cfg_override && cfg_override[0])
        ? cfg_override
        : ((ops && ops->default_config_path) ? ops->default_config_path : "hopdesk.conf");
    int custom_path = (cfg_override && cfg_override[0]) != 0;

    if (strcmp(sub, "path") == 0)
        return cmd_config_path(ops, cfg_path, json);
    if (strcmp(sub, "schema") == 0)
        return cmd_config_schema(ops, group_filter, key_filter, json);
    if (strcmp(sub, "get") == 0)
        return cmd_config_get(ops, cfg_path, custom_path, no_live, show_secret, json, positionals, pos_count);
    if (strcmp(sub, "set") == 0)
        return cmd_config_set(ops, cfg_path, custom_path, no_live, strict, json, positionals, pos_count);
    if (strcmp(sub, "reset") == 0)
        return cmd_config_reset(ops, cfg_path, custom_path, no_live, json, positionals, pos_count);
    if (strcmp(sub, "validate") == 0)
        return cmd_config_validate(ops, cfg_path, strict, json);

    char err[128];
    snprintf(err, sizeof(err), "unknown config subcommand '%s'", sub);
    emit_error(ops, json, err);
    return 2;
}
