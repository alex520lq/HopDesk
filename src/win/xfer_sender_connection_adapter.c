#include <winsock2.h>
#include <windows.h>
#include "xfer_sender_internal.h"

/* Reconnect-kick is a platform transport operation: invalidate readiness and
 * interrupt the current socket while holding the same lock used by snapshots. */
void sender_context_request_reconnect(void *user) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    if (!ctx || !ctx->state_lock || !ctx->client || !ctx->peer_ready) return;
    EnterCriticalSection(ctx->state_lock);
    *ctx->peer_ready = 0;
    if (*ctx->client != INVALID_SOCKET) shutdown(*ctx->client, SD_BOTH);
    LeaveCriticalSection(ctx->state_lock);
}
