#ifndef HOPDESK_UI_H
#define HOPDESK_UI_H

#include <stddef.h>
#include <stdint.h>
#include <cui/cui.h>
#include "../core/hop.h"
#include "../core/update_controller.h"
#include "../core/xfer_status.h"
#include "../core/xfer_task.h"
#include "../core/xfer_task_history.h"

#define HOPDESK_UI_TASK_CAP 32
#define HOPDESK_UI_HISTORY_CAP 24
#define HOPDESK_UI_FILE_PAGE 24
#define HOPDESK_UI_LOG_CAP 12288

enum {
    HOPDESK_UI_OVERVIEW = 0,
    HOPDESK_UI_DEVICE,
    HOPDESK_UI_TRANSFER,
    HOPDESK_UI_SETTINGS,
    HOPDESK_UI_PAGE_COUNT
};

enum {
    HOPDESK_UI_DISPLAY_TO_MAC = 0,
    HOPDESK_UI_DISPLAY_TO_WINDOWS,
    HOPDESK_UI_DISPLAY_WAKE,
    HOPDESK_UI_DISPLAY_SLEEP,
    HOPDESK_UI_DISPLAY_TOGGLE
};

typedef struct hopdesk_ui_model {
    hop_config config;
    char local_addresses[512];
    char peer_label[128];
    char display_status[192];
    char display_devices[768];
    int connected;
    int sharing;
    int data_connected;
    int manually_disconnected;
    int permissions_known;
    int permission_post;
    int permission_listen;
    int permission_accessibility;
    unsigned audio_requested;
    unsigned audio_active;
    unsigned audio_unavailable;
    unsigned peer_input_mode;
    unsigned peer_audio_mode;
    int peer_capabilities_known;
    int protocol_mismatch;
    hop_update_snapshot update;

    hop_xfer_status_pair transfer;
    hop_xfer_task_snapshot tasks[HOPDESK_UI_TASK_CAP];
    int task_count;
    int task_total;
    uint64_t task_revision;
    int task_snapshot_valid;

    hop_xfer_task_history_record history[HOPDESK_UI_HISTORY_CAP];
    int history_count;
    int history_total;
    uint64_t history_revision;
    int history_snapshot_valid;

    hop_xfer_file_status files[HOPDESK_UI_FILE_PAGE];
    int file_count;
    int file_total;
    int file_direction;
    uint32_t file_batch;
    uint32_t file_offset;
    uint64_t file_revision;

    char logs[HOPDESK_UI_LOG_CAP];
    char log_note[128];
} hopdesk_ui_model;

typedef struct hopdesk_ui_ops {
    void (*snapshot)(hopdesk_ui_model *model, hop_xfer_task_id selected_task, uint32_t file_offset);
    void (*save_config_async)(const hop_config *config);
    int (*cancel_task)(hop_xfer_task_id id);
    int (*retry_file)(uint32_t batch, int file_index);
    void (*reconnect)(void);
    void (*disconnect)(void);
    void (*display_action)(int action);
    void (*update_action)(int action, int channel);
    void (*request_permissions)(void);
    int (*load_logs)(char *out, size_t capacity);
    int (*copy_text)(const char *utf8);
    void (*default_config)(hop_config *config);
    cui_font (*load_cjk_font)(cui_ctx *ui);
    void (*release_cjk_font)(void);
    void (*apply_theme)(int ui_theme, int persist);
    void (*set_input_mode)(unsigned input_mode);
} hopdesk_ui_ops;

void hopdesk_ui_set_ops(const hopdesk_ui_ops *ops);
void hopdesk_ui_init(cui_ctx *ui);
void hopdesk_ui_frame(cui_ctx *ui);
void hopdesk_ui_shutdown(void);
void hopdesk_ui_refresh(void);
void hopdesk_ui_show_page(int page);
void hopdesk_ui_save_complete(int ok, const char *message);
int hopdesk_ui_settings_dirty(void);

#endif
