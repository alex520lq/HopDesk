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
        if (s_manual_disconnect) { s_rejected = 1; return; }
        hop_session_publish(&s_control_session);s_authed=1;s_last_rx=hop_monotonic_ms();
        pthread_mutex_lock(&s_lock);
        if (hello.name && hello.name_len) {
            size_t copy = hello.name_len < sizeof(s_peer_name) - 1 ? hello.name_len : sizeof(s_peer_name) - 1;
            memcpy(s_peer_name, hello.name, copy);
            s_peer_name[copy] = '\0';
        } else s_peer_name[0] = '\0';
        pthread_mutex_unlock(&s_lock);
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
        if (s_manual_disconnect) { s_rejected = 1; return; }
        pthread_mutex_lock(&s_lock);
        if (len >= 2 && p[1] > 0 && 2u + p[1] <= len) {
            size_t copy = p[1] < sizeof(s_peer_name) - 1 ? p[1] : sizeof(s_peer_name) - 1;
            memcpy(s_peer_name, p + 2, copy);
            s_peer_name[copy] = '\0';
        } else s_peer_name[0] = '\0';
        pthread_mutex_unlock(&s_lock);
        inject_set_peer_geometry(w, h);
        {
            uint32_t pw = w, ph = h;
            dispatch_async(dispatch_get_main_queue(),^{mac_capture_peer_geometry(pw,ph);});
        }
        s_control_session = session; hop_session_publish(&session);
        s_authed = 1; s_last_rx = hop_monotonic_ms();
        set_connected(1);
        return;
    }
    s_last_rx = hop_monotonic_ms();
    unsigned local_input = atomic_load(&s_local_input_mode) & HOP_INPUT_MODES;
    switch (type) {
        case HOP_MSG_HELLO:
        case HOP_MSG_HELLO_ACK:
            s_rejected = 1;
            break;
        case HOP_MSG_PING:
            net_send(HOP_MSG_PONG, NULL, 0);
            break;
        case HOP_MSG_CAPABILITIES: {
            hop_capabilities_msg msg;
            if (hop_capabilities_decode(p, len, &msg)) {
                atomic_store(&s_peer_input_mode, msg.input_mode);
                atomic_store(&s_peer_audio_mode, msg.audio_mode);
                atomic_store(&s_peer_caps_known, 1);
                unsigned next = hop_input_reconcile(local_input, msg.input_mode,
                                                    msg.takeover_mask, g_mac.cfg.role);
                if (next != local_input) {
                    hop_log(HOP_LOG_INFO, "input: peer capability sync adjusted local input_mode %u -> %u",
                            local_input, next);
                    net_apply_input_mode(next, 0, 1);
                }
            }
            break;
        }
        case HOP_MSG_EDGE_LEAVE:
            if (len >= 1 && (local_input & HOP_INPUT_MOUSE)) {
                dispatch_async(dispatch_get_main_queue(),^{mac_capture_return();});
            } else if (len == 0 && !(local_input & HOP_INPUT_MOUSE)) {
                inject_reset();
                statusbar_update(1, 0);
            }
            break;
        case HOP_MSG_MOUSE_ENTER:
            if (!(local_input & HOP_INPUT_MOUSE) && len >= 2) inject_enter((int)hop_get_be16(p));
            break;
        case HOP_MSG_MOUSE_MOVE:
            if (!(local_input & HOP_INPUT_MOUSE) && len >= 4)
                inject_move((int16_t)hop_get_be16(p),
                           (int16_t)hop_get_be16(p + 2));
            break;
        case HOP_MSG_MOUSE_BUTTON:
            if (!(local_input & HOP_INPUT_MOUSE) && len >= 2) inject_button(p[0], p[1], len >= 3 ? p[2] : 1);
            break;
        case HOP_MSG_MOUSE_WHEEL:
            if (!(local_input & HOP_INPUT_MOUSE) && len >= 4)
                inject_wheel((int16_t)hop_get_be16(p),
                            (int16_t)hop_get_be16(p + 2));
            break;
        case HOP_MSG_KEY_DOWN:
            if (!(local_input & HOP_INPUT_KEYBOARD) && len >= 2) inject_key(hop_get_be16(p), 1);
            break;
        case HOP_MSG_KEY_UP:
            if (!(local_input & HOP_INPUT_KEYBOARD) && len >= 2) inject_key(hop_get_be16(p), 0);
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
