static void on_message(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    connection *c = (connection *)user;
    net_server *s = c->server;
    if (c->rejected) return;
    if (!c->authed && s->cfg.role==HOP_ROLE_CLIENT) {
        int ok;uint32_t w,h;hop_session_id session;
        if(type!=HOP_MSG_HELLO_ACK || !hop_hello_ack_decode(p,len,&ok,&w,&h,&session)||!ok){
            hop_log(HOP_LOG_WARN,"net: control handshake rejected: invalid ACK or incompatible protocol");
            c->rejected=1;shutdown(c->sock,SD_BOTH);return;
        }
        EnterCriticalSection(&s->lock);
        queue_clear(s);s->client=c->sock;c->epoch=++s->epoch;c->authed=1;
        c->session=session;hop_session_publish(&session);
        c->last_rx=s->last_ping=hop_monotonic_ms();
        PostMessageW(s->hwnd,WM_APP_LINK,1,(LPARAM)c->epoch);
        if(!post_message(c,type,p,len))c->rejected=1;
        LeaveCriticalSection(&s->lock);return;
    }
    if (!c->authed) {
        hop_hello_msg hello;
        if (type != HOP_MSG_HELLO || !hop_hello_decode(p, len, &hello)) {
            hop_log(HOP_LOG_WARN,"net: control handshake rejected: invalid HELLO or incompatible protocol");
            c->rejected = 1; shutdown(c->sock, SD_BOTH); return;
        }
        if (!hop_token_matches(s->cfg.token, hello.token, hello.token_len)) {
            hop_log(HOP_LOG_WARN,"net: control handshake rejected: token mismatch");
            c->rejected = 1; shutdown(c->sock, SD_BOTH); return;
        }
        if (!hop_session_generate(&c->session)) { c->rejected = 1; shutdown(c->sock, SD_BOTH); return; }
        EnterCriticalSection(&s->lock);
        if (WaitForSingleObject(s->stop, 0) == WAIT_OBJECT_0) {
            c->rejected = 1;
        } else {
            int replaced = s->client != INVALID_SOCKET;
            if (replaced) shutdown(s->client, SD_BOTH);
            queue_clear(s);
            s->client = c->sock; c->epoch = ++s->epoch; c->authed = 1;
            hop_session_publish(&c->session);
            c->last_rx = s->last_ping = hop_monotonic_ms();
            uint8_t ack[2 + HOP_NAME_MAX + 9 + HOP_SESSION_BYTES];
            const char *name = s->cfg.device_name[0] ? s->cfg.device_name : "HopDesk";
            size_t nl = strlen(name);
            ack[0] = 1; ack[1] = (uint8_t)nl;
            memcpy(ack + 2, name, nl);
            hop_put_be32(ack + 2 + nl, (uint32_t)s->cfg.screen_w);
            hop_put_be32(ack + 6 + nl, (uint32_t)s->cfg.screen_h);
            ack[nl + 10] = HOP_CTRL_PROTO_VER;
            memcpy(ack + nl + 11, c->session.bytes, HOP_SESSION_BYTES);
            queue_push(s, HOP_MSG_HELLO_ACK, ack, (uint32_t)nl + 11 + HOP_SESSION_BYTES);
            PostMessageW(s->hwnd, WM_APP_LINK, replaced ? 2 : 1, (LPARAM)c->epoch);
            if (!post_message(c, type, p, len)) c->rejected = 1;
            hop_log(HOP_LOG_INFO, "net: authenticated control session %lu%s",
                    c->epoch, replaced ? " (replaced old peer)" : "");
        }
        LeaveCriticalSection(&s->lock);
        return;
    }
    EnterCriticalSection(&s->lock);
    if (s->client != c->sock || s->epoch != c->epoch) c->rejected = 1;
    else {
        c->last_rx = hop_monotonic_ms();
        if (type == HOP_MSG_HELLO || type == HOP_MSG_HELLO_ACK) c->rejected = 1;
        else if (type == HOP_MSG_PING) queue_push(s, HOP_MSG_PONG, NULL, 0);
        else if (type != HOP_MSG_PONG && !post_message(c, type, p, len)) c->rejected = 1;
    }
    LeaveCriticalSection(&s->lock);
}
