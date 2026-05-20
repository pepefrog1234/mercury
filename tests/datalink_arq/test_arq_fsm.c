/*
 * ARQ FSM Unit Tests
 *
 * Tests for datalink_arq/arq_fsm.c — state transitions, callback
 * invocations and timeout handling.
 *
 * All 9 arq_fsm_callbacks_t function pointers are faked via FFF.
 * arq_protocol_build_* and arq_timing_* are mocked to isolate FSM logic.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include <stdint.h>

#include "unity.h"
#include "fff.h"

DEFINE_FFF_GLOBALS;

#include "freedv/freedv_api.h"
#include "arq_fsm.h"
#include "arq_protocol.h"

/* Provided by arq_test_stubs.c */
extern void mock_set_uptime_ms(uint64_t ms);

/* ---- FFF Fakes for arq_fsm_callbacks_t ---- */

FAKE_VOID_FUNC(fake_send_tx_frame, int, int, size_t, const uint8_t *);
FAKE_VOID_FUNC(fake_notify_connected, const char *);
FAKE_VOID_FUNC(fake_notify_pending, const char *);
FAKE_VOID_FUNC(fake_notify_cancelpending);
FAKE_VOID_FUNC(fake_notify_disconnected, bool);
FAKE_VOID_FUNC(fake_deliver_rx_data, const uint8_t *, size_t);
FAKE_VALUE_FUNC(int, fake_tx_backlog);
FAKE_VALUE_FUNC(int, fake_tx_read, uint8_t *, size_t);
FAKE_VOID_FUNC(fake_send_buffer_status, int);

static int fake_tx_backlog_value(void)
{
    return 10;
}

static int fake_tx_backlog_large_value(void)
{
    return 395;
}

static arq_fsm_callbacks_t test_callbacks = {
    .send_tx_frame       = fake_send_tx_frame,
    .notify_connected    = fake_notify_connected,
    .notify_pending      = fake_notify_pending,
    .notify_cancelpending = fake_notify_cancelpending,
    .notify_disconnected = fake_notify_disconnected,
    .deliver_rx_data     = fake_deliver_rx_data,
    .tx_backlog          = fake_tx_backlog,
    .tx_read             = fake_tx_read,
    .send_buffer_status  = fake_send_buffer_status,
};

static arq_session_t sess;
static arq_timing_ctx_t timing;

/* ---- Helper: create a minimal event ---- */
static arq_event_t make_event(arq_event_id_t id)
{
    arq_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.id = id;
    return ev;
}

/* ---- setUp / tearDown ---- */

void setUp(void)
{
    /* Reset all FFF fakes */
    RESET_FAKE(fake_send_tx_frame);
    RESET_FAKE(fake_notify_connected);
    RESET_FAKE(fake_notify_pending);
    RESET_FAKE(fake_notify_cancelpending);
    RESET_FAKE(fake_notify_disconnected);
    RESET_FAKE(fake_deliver_rx_data);
    RESET_FAKE(fake_tx_backlog);
    RESET_FAKE(fake_tx_read);
    RESET_FAKE(fake_send_buffer_status);
    FFF_RESET_HISTORY();

    /* Init session and register callbacks */
    mock_set_uptime_ms(1000);
    arq_timing_init(&timing);
    arq_fsm_set_timing(&timing);
    arq_fsm_set_callbacks(&test_callbacks);
    arq_fsm_init(&sess);
}

void tearDown(void) { }

/* ---- Connection lifecycle tests ---- */

/* Initial state shall be DISCONNECTED */
void test_init_state_disconnected(void)
{
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
}

/* APP_LISTEN transitions to LISTENING */
void test_listen_transitions_to_listening(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);
}

/* APP_CONNECT transitions to CALLING */
void test_connect_transitions_to_calling(void)
{
    /* First go to LISTENING */
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    /* Then CONNECT */
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "TEST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
    /* Remote callsign should be stored */
    TEST_ASSERT_EQUAL_STRING("TEST1", sess.remote_call);
}

/* Incoming CALL from LISTENING transitions to ACCEPTING */
void test_incoming_call_transitions_to_accepting(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);
    TEST_ASSERT_EQUAL_UINT8(0x42, sess.session_id);
    /* notify_pending should have been called */
    TEST_ASSERT_GREATER_THAN(0, fake_notify_pending_fake.call_count);
}

/* RX_ACCEPT from CALLING transitions to CONNECTED */
void test_accept_transitions_to_connected(void)
{
    /* LISTEN + CONNECT */
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    /* Simulate ACCEPT received */
    ev = make_event(ARQ_EV_RX_ACCEPT);
    ev.session_id = sess.session_id;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_GREATER_THAN(0, fake_notify_connected_fake.call_count);
}

/* APP_DISCONNECT from CONNECTED */
void test_disconnect_from_connected(void)
{
    /* Get to CONNECTED state */
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_ACCEPT);
    ev.session_id = sess.session_id;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);

    /* Reset call counts to track disconnect-specific calls */
    RESET_FAKE(fake_send_tx_frame);

    /* Disconnect */
    ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);

    /* Should either go to DISCONNECTING or DISCONNECTED */
    TEST_ASSERT_TRUE(
        sess.conn_state == ARQ_CONN_DISCONNECTING ||
        sess.conn_state == ARQ_CONN_DISCONNECTED
    );
}

/* RX_DISCONNECT transitions to DISCONNECTED */
void test_rx_disconnect_from_connected(void)
{
    /* Get to CONNECTED state */
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_ACCEPT);
    ev.session_id = sess.session_id;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    /* RX disconnect */
    ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
}

/* ---- Timeout tests ---- */

/* CALL timeout transitions to DISCONNECTED */
void test_call_timeout(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);

    /* Exhaust retries */
    for (int i = 0; i < ARQ_CALL_RETRY_SLOTS_DEFAULT + 2; i++) {
        ev = make_event(ARQ_EV_TIMER_RETRY);
        mock_set_uptime_ms(1000 + (uint64_t)(i + 1) * 10000);
        arq_fsm_dispatch(&sess, &ev);
        if (sess.conn_state == ARQ_CONN_DISCONNECTED)
            break;
    }

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
}

/* STOP_LISTEN returns to DISCONNECTED */
void test_stop_listen(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);

    ev = make_event(ARQ_EV_APP_STOP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
}

/* FSM timeout_ms returns INT_MAX when idle */
void test_timeout_ms_idle(void)
{
    int ms = arq_fsm_timeout_ms(&sess, 1000);
    /* When DISCONNECTED with no deadline, should return INT_MAX or large value */
    TEST_ASSERT_GREATER_THAN(60000, ms);
}

void test_keepalive_wait_accepts_peer_data(void)
{
    sess.conn_state = ARQ_CONN_CONNECTED;
    sess.dflow_state = ARQ_DFLOW_KEEPALIVE_WAIT;
    sess.role = ARQ_ROLE_CALLER;
    sess.session_id = 0x42;
    sess.keepalive_miss_count = 3;
    sess.rx_expected = 0;

    arq_event_t ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id = sess.session_id;
    ev.seq = 0;
    ev.mode = FREEDV_MODE_DATAC4;
    ev.data_bytes = 2;
    ev.payload_len = 2;
    ev.payload[0] = 'o';
    ev.payload[1] = 'k';
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_RX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(0, sess.keepalive_miss_count);
    TEST_ASSERT_EQUAL_UINT8(1, sess.rx_expected);
    TEST_ASSERT_EQUAL_INT(1, fake_deliver_rx_data_fake.call_count);
}

void test_keepalive_wait_accepts_turn_request(void)
{
    sess.conn_state = ARQ_CONN_CONNECTED;
    sess.dflow_state = ARQ_DFLOW_KEEPALIVE_WAIT;
    sess.role = ARQ_ROLE_CALLER;
    sess.session_id = 0x42;
    sess.keepalive_miss_count = 3;

    arq_event_t ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_ACK_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(0, sess.keepalive_miss_count);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_ACK, sess.deadline_event);
}

void test_idle_irs_local_data_shortens_turn_request_timer(void)
{
    sess.conn_state = ARQ_CONN_CONNECTED;
    sess.dflow_state = ARQ_DFLOW_IDLE_IRS;
    sess.role = ARQ_ROLE_CALLEE;
    sess.session_id = 0x42;
    sess.dflow_enter_ms = 1000;
    sess.deadline_ms = 16000;
    sess.deadline_event = ARQ_EV_TIMER_PEER_BACKLOG;
    fake_tx_backlog_fake.custom_fake = fake_tx_backlog_value;

    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    TEST_ASSERT_EQUAL_UINT64(4500, sess.deadline_ms);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_PEER_BACKLOG, sess.deadline_event);
    TEST_ASSERT_EQUAL_INT(0, fake_send_tx_frame_fake.call_count);
}

void test_idle_irs_local_data_requests_turn_after_peer_silence(void)
{
    mock_set_uptime_ms(6000);
    sess.conn_state = ARQ_CONN_CONNECTED;
    sess.dflow_state = ARQ_DFLOW_IDLE_IRS;
    sess.role = ARQ_ROLE_CALLEE;
    sess.session_id = 0x42;
    sess.dflow_enter_ms = 1000;
    sess.deadline_ms = 16000;
    sess.deadline_event = ARQ_EV_TIMER_PEER_BACKLOG;
    fake_tx_backlog_fake.custom_fake = fake_tx_backlog_value;

    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(ARQ_TURN_REQ_RETRIES, sess.tx_retries_left);
    TEST_ASSERT_EQUAL_INT(1, fake_send_tx_frame_fake.call_count);
}

void test_idle_irs_timer_requests_turn_for_queued_data(void)
{
    sess.conn_state = ARQ_CONN_CONNECTED;
    sess.dflow_state = ARQ_DFLOW_IDLE_IRS;
    sess.role = ARQ_ROLE_CALLEE;
    sess.session_id = 0x42;
    sess.deadline_event = ARQ_EV_TIMER_PEER_BACKLOG;
    fake_tx_backlog_fake.custom_fake = fake_tx_backlog_value;

    arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(ARQ_TURN_REQ_RETRIES, sess.tx_retries_left);
    TEST_ASSERT_EQUAL_INT(1, fake_send_tx_frame_fake.call_count);
}

void test_idle_iss_short_backlog_direct_switches_to_datac3_on_wide_link(void)
{
    mock_set_uptime_ms(20000);
    sess.conn_state = ARQ_CONN_CONNECTED;
    sess.dflow_state = ARQ_DFLOW_IDLE_ISS;
    sess.role = ARQ_ROLE_CALLER;
    sess.session_id = 0x42;
    sess.payload_mode = FREEDV_MODE_DATAC4;
    sess.peer_tx_mode = FREEDV_MODE_DATAC4;
    sess.peer_snr_x10 = 180;
    sess.startup_deadline_ms = 0;
    fake_tx_backlog_fake.custom_fake = fake_tx_backlog_value;

    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC3, sess.payload_mode);
    TEST_ASSERT_EQUAL_INT(0, sess.pending_tx_mode);
    TEST_ASSERT_EQUAL_INT(0, fake_send_tx_frame_fake.call_count);
}

void test_idle_iss_large_backlog_uses_datac3_until_link_is_proven(void)
{
    mock_set_uptime_ms(20000);
    sess.conn_state = ARQ_CONN_CONNECTED;
    sess.dflow_state = ARQ_DFLOW_IDLE_ISS;
    sess.role = ARQ_ROLE_CALLER;
    sess.session_id = 0x42;
    sess.payload_mode = FREEDV_MODE_DATAC4;
    sess.peer_tx_mode = FREEDV_MODE_DATAC4;
    sess.peer_snr_x10 = 180;
    sess.startup_deadline_ms = 0;
    fake_tx_backlog_fake.custom_fake = fake_tx_backlog_large_value;

    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC3, sess.payload_mode);
    TEST_ASSERT_EQUAL_INT(0, sess.pending_tx_mode);
}

void test_idle_iss_large_backlog_uses_datac1_after_one_clean_ack_on_high_snr(void)
{
    mock_set_uptime_ms(20000);
    sess.conn_state = ARQ_CONN_CONNECTED;
    sess.dflow_state = ARQ_DFLOW_IDLE_ISS;
    sess.role = ARQ_ROLE_CALLER;
    sess.session_id = 0x42;
    sess.payload_mode = FREEDV_MODE_DATAC3;
    sess.peer_tx_mode = FREEDV_MODE_DATAC3;
    sess.peer_snr_x10 = 180;
    sess.tx_success_count = ARQ_DATAC1_FAST_CLEAN_ACKS;
    sess.startup_deadline_ms = 0;
    fake_tx_backlog_fake.custom_fake = fake_tx_backlog_large_value;

    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC1, sess.payload_mode);
    TEST_ASSERT_EQUAL_INT(0, sess.pending_tx_mode);
}

void test_idle_iss_large_backlog_blocks_datac1_fast_path_after_retry(void)
{
    mock_set_uptime_ms(20000);
    sess.conn_state = ARQ_CONN_CONNECTED;
    sess.dflow_state = ARQ_DFLOW_IDLE_ISS;
    sess.role = ARQ_ROLE_CALLER;
    sess.session_id = 0x42;
    sess.payload_mode = FREEDV_MODE_DATAC3;
    sess.peer_tx_mode = FREEDV_MODE_DATAC3;
    sess.peer_snr_x10 = 180;
    sess.tx_success_count = ARQ_DATAC1_FAST_CLEAN_ACKS;
    sess.consecutive_retries = 1;
    sess.startup_deadline_ms = 0;
    fake_tx_backlog_fake.custom_fake = fake_tx_backlog_large_value;

    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC3, sess.payload_mode);
    TEST_ASSERT_EQUAL_INT(0, sess.pending_tx_mode);
}

void test_idle_iss_large_backlog_direct_switches_to_datac1_after_clean_acks(void)
{
    mock_set_uptime_ms(20000);
    sess.conn_state = ARQ_CONN_CONNECTED;
    sess.dflow_state = ARQ_DFLOW_IDLE_ISS;
    sess.role = ARQ_ROLE_CALLER;
    sess.session_id = 0x42;
    sess.payload_mode = FREEDV_MODE_DATAC3;
    sess.peer_tx_mode = FREEDV_MODE_DATAC3;
    sess.peer_snr_x10 = 180;
    sess.speed_level = ARQ_DATAC1_MIN_STABILITY_LEVEL;
    sess.startup_deadline_ms = 0;
    fake_tx_backlog_fake.custom_fake = fake_tx_backlog_large_value;

    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC1, sess.payload_mode);
    TEST_ASSERT_EQUAL_INT(0, sess.pending_tx_mode);
}

int main(void)
{
    UNITY_BEGIN();
    /* Connection lifecycle tests */
    RUN_TEST(test_init_state_disconnected);
    RUN_TEST(test_listen_transitions_to_listening);
    RUN_TEST(test_connect_transitions_to_calling);
    RUN_TEST(test_incoming_call_transitions_to_accepting);
    RUN_TEST(test_accept_transitions_to_connected);
    RUN_TEST(test_disconnect_from_connected);
    RUN_TEST(test_rx_disconnect_from_connected);
    /* Timeout tests */
    RUN_TEST(test_call_timeout);
    RUN_TEST(test_stop_listen);
    RUN_TEST(test_timeout_ms_idle);
    RUN_TEST(test_keepalive_wait_accepts_peer_data);
    RUN_TEST(test_keepalive_wait_accepts_turn_request);
    RUN_TEST(test_idle_irs_local_data_shortens_turn_request_timer);
    RUN_TEST(test_idle_irs_local_data_requests_turn_after_peer_silence);
    RUN_TEST(test_idle_irs_timer_requests_turn_for_queued_data);
    RUN_TEST(test_idle_iss_short_backlog_direct_switches_to_datac3_on_wide_link);
    RUN_TEST(test_idle_iss_large_backlog_uses_datac3_until_link_is_proven);
    RUN_TEST(test_idle_iss_large_backlog_uses_datac1_after_one_clean_ack_on_high_snr);
    RUN_TEST(test_idle_iss_large_backlog_blocks_datac1_fast_path_after_retry);
    RUN_TEST(test_idle_iss_large_backlog_direct_switches_to_datac1_after_clean_acks);
    return UNITY_END();
}
