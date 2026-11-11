static void dispatch_message(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    (void)user;
    if (s_rejected) return;
    if (!s_authed && g_mac.cfg.role==HOP_ROLE_SERVER) {
        hop_hello_msg hello;
        if(type!=HOP_MSG_HELLO || !hop_hello_decode(p,len,&hello)){
            hop_log(HOP_LOG_WARN,"net: control handshake rejected: invalid HELLO or incompatible protocol");s_rejected=1;return;
        }
        if(!hop_token_matches(g_mac.cfg.token,hello.token,hello.token_len)){
            hop_log(HOP_LOG_WARN,"net: control handshake rejected: token mismatch");s_rejected=1;return;
        }
        if(!hop_session_generate(&s_control_session)){
            hop_log(HOP_LOG_ERROR,"net: unable to create control session");s_rejected=1;return;
        }
        hop_session_publish(&s_control_session);s_authed=1;s_last_rx=hop_monotonic_ms();
        uint32_t pw=hello.width,ph=hello.height;
        dispatch_async(dispatch_get_main_queue(),^{mac_capture_peer_geometry(pw,ph);});
        uint8_t ack[2+HOP_NAME_MAX+9+HOP_SESSION_BYTES];size_t nl=strlen(g_mac.cfg.device_name);
        ack[0]=1;ack[1]=(uint8_t)nl;memcpy(ack+2,g_mac.cfg.device_name,nl);
        hop_put_be32(ack+2+nl,(uint32_t)g_mac.screen_w);hop_put_be32(ack+6+nl,(uint32_t)g_mac.screen_h);
        ack[10+nl]=HOP_CTRL_PROTO_VER;memcpy(ack+11+nl,s_control_session.bytes,HOP_SESSION_BYTES);
        net_send(HOP_MSG_HELLO_ACK,ack,(uint32_t)nl+11+HOP_SESSION_BYTES);set_connected(1);return;
    }
    if (!s_authed) {
        int ok;
        uint32_t w, h;
        hop_session_id session;
        if (type != HOP_MSG_HELLO_ACK ||
            !hop_hello_ack_decode(p, len, &ok, &w, &h, &session) || !ok) {
            hop_log(HOP_LOG_WARN,"net: control handshake rejected: invalid ACK or incompatible protocol");
            s_rejected = 1;
            return;
        }
        inject_set_peer_geometry(w, h);
        s_control_session = session; hop_session_publish(&session);
        s_authed = 1; s_last_rx = hop_monotonic_ms();
        set_connected(1);
        return;
    }
    s_last_rx = hop_monotonic_ms();
    if(g_mac.cfg.role==HOP_ROLE_SERVER && type==HOP_MSG_EDGE_LEAVE){
        dispatch_async(dispatch_get_main_queue(),^{mac_capture_return();});return;
    }
    if(g_mac.cfg.role==HOP_ROLE_SERVER && type>=HOP_MSG_MOUSE_MOVE && type<=HOP_MSG_KEY_MODS)return;
    switch (type) {
        case HOP_MSG_HELLO:
        case HOP_MSG_HELLO_ACK:
            s_rejected = 1;
            break;
        case HOP_MSG_PING:
            net_send(HOP_MSG_PONG, NULL, 0);
            break;
        case HOP_MSG_EDGE_LEAVE:
            if (g_mac.cfg.role == HOP_ROLE_CLIENT && len == 0) {
                inject_reset();
                statusbar_update(1, 0);
            }
            break;
        case HOP_MSG_MOUSE_ENTER:
            if (len >= 2) inject_enter((int)hop_get_be16(p));
            break;
        case HOP_MSG_MOUSE_MOVE:
            if (len >= 4)
                inject_move((int16_t)hop_get_be16(p),
                           (int16_t)hop_get_be16(p + 2));
            break;
        case HOP_MSG_MOUSE_BUTTON:
            if (len >= 2) inject_button(p[0], p[1]);
            break;
        case HOP_MSG_MOUSE_WHEEL:
            if (len >= 4)
                inject_wheel((int16_t)hop_get_be16(p),
                            (int16_t)hop_get_be16(p + 2));
            break;
        case HOP_MSG_KEY_DOWN:
            if (len >= 2) inject_key(hop_get_be16(p), 1);
            break;
        case HOP_MSG_KEY_UP:
            if (len >= 2) inject_key(hop_get_be16(p), 0);
            break;
        case HOP_MSG_CLIP_FILE_OFFER:
            clipboard_on_remote_file_offer(p, len);
            break;
        case HOP_MSG_CLIP_FILE_OFFER_CLEAR:
            clipboard_on_remote_file_offer_clear(p, len);
            break;
        case HOP_MSG_CLIP_PASTE_REQUEST:
            clipboard_on_paste_request(p, len);
            break;
        case HOP_MSG_CLIP_PASTE_FAILED:
            clipboard_on_paste_failed(p, len);
            break;
        case HOP_MSG_CLIP_TEXT:
            /* origin(4) + UTF-8；自己发出去的不写回 */
            if (len > 4 && hop_clip_should_apply_remote(hop_get_be32(p), (g_mac.cfg.role==HOP_ROLE_SERVER?HOP_DEV_SERVER:HOP_DEV_CLIENT))) {
                char *u = (char *)malloc((size_t)(len - 3));
                if (u) {
                    memcpy(u, p + 4, len - 4);
                    u[len - 4] = '\0';
                    clipboard_apply(u);
                    free(u);
                }
            }
            break;
        case HOP_MSG_DISP_WAKE:
            if (len >= 4) disp_wake((int)hop_get_be32(p));
            break;
        case HOP_MSG_DISP_INPUT:
            if (len >= 2) disp_set_input(hop_get_be16(p));
            break;
        case HOP_MSG_DISP_SLEEP:
            disp_sleep();
            break;
        default:
            break;
    }
}
