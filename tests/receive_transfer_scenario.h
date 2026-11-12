#ifndef HOP_TEST_RECEIVE_TRANSFER_SCENARIO_H
#define HOP_TEST_RECEIVE_TRANSFER_SCENARIO_H

/* Shared wire scenario for the native Windows and macOS transfer executables.
 * The harness supplies socket I/O, clipboard observation and staging checks. */
static const uint8_t receive_first[] = {0x21, 0x42, 0x63};
static const uint8_t receive_second[] = {0x80, 0x00, 0x91, 0xff, 0x05, 0x32};

static void receive_expect_file(FILE *file, const uint8_t *bytes, size_t length) {
    uint8_t data[sizeof(receive_second)];
    assert(file && length <= sizeof(data));
    assert(fread(data, 1, length, file) == length);
    assert(memcmp(data, bytes, length) == 0);
    assert(fgetc(file) == EOF && !ferror(file));
    assert(fclose(file) == 0);
}

static void receive_send(peer_socket peer, const hop_session_id *session,
                         uint32_t batch, uint8_t type, const uint8_t *body, uint32_t length) {
    uint8_t payload[192];
    assert(length <= sizeof(payload) - HOP_DATA_PREFIX);
    hop_data_prefix(payload, session, batch);
    memcpy(payload + HOP_DATA_PREFIX, body, length);
    write_frame(peer, type, payload, length + HOP_DATA_PREFIX);
}

static const uint8_t *receive_reply(peer_socket peer, const hop_session_id *session,
                                   uint32_t batch, uint8_t type, uint32_t length) {
    const uint8_t *body;
    uint32_t reply_batch, body_length;
    assert(read_frame(peer) == type);
    assert(hop_data_unwrap(type, s_last_body, s_last_len, session, &reply_batch,
                           &body, &body_length));
    assert(reply_batch == batch && body_length == length);
    return body;
}

static void receive_file_begin(peer_socket peer, const hop_session_id *session,
                               uint32_t batch, uint32_t id, const char *name, uint32_t size) {
    uint8_t body[96] = {0};
    uint16_t length = (uint16_t)strlen(name);
    assert(29u + 2u * length <= sizeof(body));
    hop_put_be32(body, batch);
    hop_put_be32(body + 4, id);
    hop_put_be64(body + 8, size);
    hop_put_be16(body + 25, length);
    memcpy(body + 27, name, length);
    hop_put_be16(body + 27 + length, length);
    memcpy(body + 29 + length, name, length);
    receive_send(peer, session, batch, HOP_MSG_FILE_BEGIN, body, 29u + 2u * length);
}

static uint32_t receive_file_chunk(peer_socket peer, const hop_session_id *session,
                                   uint32_t batch, uint32_t id, const uint8_t *bytes,
                                   uint32_t size) {
    uint8_t body[20 + sizeof(receive_second)] = {0};
    assert(size <= sizeof(body) - 20);
    uint32_t crc = hop_crc32(0, bytes, size);
    hop_put_be32(body, id);
    hop_put_be32(body + 12, size);
    hop_put_be32(body + 16, crc);
    memcpy(body + 20, bytes, size);
    receive_send(peer, session, batch, HOP_MSG_FILE_CHUNK, body, 20 + size);
    return hop_xfer_file_crc_step(0, 0, crc);
}

static void receive_file_complete(peer_socket peer, const hop_session_id *session,
                                  uint32_t batch, uint32_t id, const uint8_t *bytes,
                                  uint32_t size) {
    uint8_t end[9];
    uint32_t crc = receive_file_chunk(peer, session, batch, id, bytes, size);
    hop_put_be32(end, id);
    end[4] = HOP_FILE_OK;
    hop_put_be32(end + 5, crc);
    receive_send(peer, session, batch, HOP_MSG_FILE_END, end, sizeof(end));
    const uint8_t *ack = receive_reply(peer, session, batch, HOP_MSG_FILE_ACK, 9);
    assert(hop_get_be32(ack + 4) == id && ack[8] == HOP_FILE_ACK_OK);
}

static void receive_wait_status(uint32_t batch, int phase, uint64_t bytes) {
    uint64_t deadline = hop_monotonic_ms() + 4000;
    do {
        hop_xfer_status_pair pair;
        xfer_get_status_pair(&pair);
        if (pair.receive.batch == batch && pair.receive.phase == phase &&
            pair.receive.bytes_done >= bytes) return;
        test_pause();
    } while (hop_monotonic_ms() < deadline);
    assert(!"receive status did not reach the expected state");
}

static void run_receive_transfer(const hop_session_id *session, const char *token, uint16_t port) {
    const uint32_t resume_batch = 701, cancel_batch = 702;
    peer_socket peer = connect_data_peer(port, token, session);
    receive_file_begin(peer, session, resume_batch, 1, "first.bin", sizeof(receive_first));
    receive_file_complete(peer, session, resume_batch, 1, receive_first, sizeof(receive_first));
    receive_file_begin(peer, session, resume_batch, 2, "second.bin", sizeof(receive_second));
    receive_file_chunk(peer, session, resume_batch, 2, receive_second, 2);
    receive_wait_status(resume_batch, HOP_XFER_RECEIVING, sizeof(receive_first) + 2);
    close_peer(peer);

    /* Resume the unacknowledged file from offset zero, leaving file 1 intact.
     * A late chunk from another batch must not advance the new file. */
    peer = connect_data_peer(port, token, session);
    receive_file_begin(peer, session, resume_batch, 2, "second.bin", sizeof(receive_second));
    receive_file_chunk(peer, session, resume_batch - 1, 2, receive_second, 1);
    receive_file_complete(peer, session, resume_batch, 2, receive_second, sizeof(receive_second));

    uint8_t done[20];
    hop_put_be32(done, resume_batch);
    hop_put_be32(done + 4, 2);
    hop_put_be32(done + 8, 2);
    hop_put_be64(done + 12, sizeof(receive_first) + sizeof(receive_second));
    for (int attempt = 0; attempt < 2; ++attempt) {
        receive_send(peer, session, resume_batch, HOP_MSG_XFER_DONE, done, sizeof(done));
        const uint8_t *ack = receive_reply(peer, session, resume_batch, HOP_MSG_XFER_DONE_ACK, 5);
        assert(ack[4] == HOP_DONE_ACK_COMPLETE);
        assert(receive_publications() == 1);
    }

    receive_file_begin(peer, session, cancel_batch, 1, "partial.bin", sizeof(receive_second));
    receive_file_chunk(peer, session, cancel_batch, 1, receive_second, 2);
    receive_wait_status(cancel_batch, HOP_XFER_RECEIVING, 2);
    assert(receive_staging_exists(session, cancel_batch));
    hop_xfer_task_snapshot task;
    int total;
    assert(hop_xfer_task_list(&task, 1, &total) == 1 && total == 1);
    assert(task.transfer_id == cancel_batch && task.direction == HOP_XFER_DIR_RECEIVE);
    assert(xfer_cancel_task(task.id));
    const uint8_t *abort_body = receive_reply(peer, session, cancel_batch, HOP_MSG_XFER_ABORT, 6);
    assert(abort_body[4] == HOP_ABORT_CANCEL && abort_body[5] == HOP_ABORT_TO_SENDER);
    receive_wait_status(cancel_batch, HOP_XFER_CANCELLED, 0);
    assert(!receive_staging_exists(session, cancel_batch));
    receive_file_begin(peer, session, cancel_batch, 1, "partial.bin", sizeof(receive_second));
    abort_body = receive_reply(peer, session, cancel_batch, HOP_MSG_XFER_ABORT, 6);
    assert(abort_body[4] == HOP_ABORT_CANCEL && !receive_staging_exists(session, cancel_batch));
    assert(receive_publications() == 1);

    hop_xfer_budget_snapshot budget;
    xfer_get_budget_snapshot(&budget);
    assert(!budget.jobs && !budget.entries && !budget.pending_bytes &&
           !budget.rx_queue_slots && !budget.rx_queue_bytes && !budget.accounting_errors);
    close_peer(peer);
    puts("PASS: native Receiver reconnect, exact staged bytes, DONE replay, cancellation and budget cleanup (TCP peer double)");
}

#endif
