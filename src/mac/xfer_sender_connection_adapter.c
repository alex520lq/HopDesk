#include "xfer_sender_internal.h"
#include <sys/socket.h>

/* Reconnect-kick is a platform transport operation: invalidate readiness and
 * interrupt the current socket while holding the same lock used by snapshots. */
void sender_context_request_reconnect(void *user) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    if (!ctx || !ctx->session_lock || !ctx->sock || !ctx->peer_ready) return;
    pthread_mutex_lock(ctx->session_lock);
    *ctx->peer_ready = 0;
    if (*ctx->sock >= 0) shutdown(*ctx->sock, SHUT_RDWR);
    pthread_mutex_unlock(ctx->session_lock);
}
