/* HERMES Modem — ARQ Timing: instrumentation and telemetry
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_timing.h"

#include <string.h>

#include "../common/hermes_log.h"
#include "arq_protocol.h"

#define LOG_COMP "arq-timing"

/* FreeDV mode name helper (brief form) */
static const char *mode_name(int mode)
{
    switch (mode)
    {
    case 19: return "DATAC13";
    case 18: return "DATAC4";
    case 12: return "DATAC3";
    case 10: return "DATAC1";
    default: return "?";
    }
}

void arq_timing_init(arq_timing_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void arq_timing_record_tx_queue(arq_timing_ctx_t *ctx, int seq, int mode,
                                int backlog_bytes, int payload_bytes)
{
    ctx->tx_queue_ms = hermes_uptime_ms();
    if (payload_bytes > 0)
        ctx->tx_bytes += (uint64_t)payload_bytes;
    HLOGT(LOG_COMP, "tx_queue seq=%d mode=%s backlog=%d bytes=%d tx_total=%llu",
          seq, mode_name(mode), backlog_bytes, payload_bytes,
          (unsigned long long)ctx->tx_bytes);
}

void arq_timing_record_tx_start(arq_timing_ctx_t *ctx, int seq, int mode,
                                int backlog_bytes)
{
    ctx->tx_start_ms = hermes_uptime_ms();
    ctx->frames_tx++;
    HLOGT(LOG_COMP, "tx_start seq=%d mode=%s backlog=%d",
          seq, mode_name(mode), backlog_bytes);
}

void arq_timing_record_tx_end(arq_timing_ctx_t *ctx, int seq)
{
    ctx->tx_end_ms = hermes_uptime_ms();
    uint32_t dur = 0;
    if (ctx->tx_start_ms > 0 && ctx->tx_end_ms >= ctx->tx_start_ms)
        dur = (uint32_t)(ctx->tx_end_ms - ctx->tx_start_ms);
    ctx->last_tx_air_ms = dur;
    ctx->tx_air_ms += (uint64_t)dur;
    HLOGT(LOG_COMP, "tx_end seq=%d dur=%ums", seq, dur);
}

void arq_timing_record_rf_tx_start(arq_timing_ctx_t *ctx)
{
    ctx->rf_tx_start_ms = hermes_uptime_ms();
    ctx->rf_tx_active = true;
    HLOGT(LOG_COMP, "rf_tx_start");
}

void arq_timing_record_rf_tx_end(arq_timing_ctx_t *ctx)
{
    if (!ctx->rf_tx_active)
        return;

    uint64_t now = hermes_uptime_ms();
    uint32_t dur = 0;
    if (now >= ctx->rf_tx_start_ms)
        dur = (uint32_t)(now - ctx->rf_tx_start_ms);
    ctx->last_rf_air_ms = dur;
    ctx->rf_air_ms += (uint64_t)dur;
    ctx->rf_tx_active = false;
    HLOGT(LOG_COMP, "rf_tx_end dur=%ums", dur);
}

void arq_timing_record_ack_rx(arq_timing_ctx_t *ctx, int seq,
                               uint8_t ack_delay_raw, int peer_snr_x10)
{
    ctx->ack_rx_ms       = hermes_uptime_ms();
    ctx->last_snr_peer_x10 = peer_snr_x10;

    uint32_t peer_delay = arq_protocol_decode_ack_delay(ack_delay_raw);
    ctx->ack_delay_ms = peer_delay;
    ctx->ack_delay_total_ms += (uint64_t)peer_delay;
    ctx->ack_count++;

    if (ctx->tx_start_ms > 0)
    {
        uint32_t elapsed = (uint32_t)(ctx->ack_rx_ms - ctx->tx_start_ms);
        ctx->rtt_ms = (elapsed > peer_delay) ? (elapsed - peer_delay) : elapsed;
    }
    if (ctx->tx_end_ms > 0 && ctx->ack_rx_ms >= ctx->tx_end_ms)
        ctx->ack_wait_ms += (uint64_t)(ctx->ack_rx_ms - ctx->tx_end_ms);

    HLOGT(LOG_COMP, "ack_rx seq=%d rtt=%ums peer_delay=%ums snr_peer=%.1f",
          seq, ctx->rtt_ms, peer_delay, (float)peer_snr_x10 / 10.0f);
}

void arq_timing_record_data_rx(arq_timing_ctx_t *ctx, int seq,
                                int bytes, int snr_x10)
{
    ctx->data_rx_ms         = hermes_uptime_ms();
    ctx->last_snr_local_x10 = snr_x10;
    ctx->rx_bytes          += (uint64_t)bytes;
    ctx->frames_rx++;
    HLOGT(LOG_COMP, "data_rx seq=%d bytes=%d snr_local=%.1f rx_total=%llu",
          seq, bytes, (float)snr_x10 / 10.0f, (unsigned long long)ctx->rx_bytes);
}

void arq_timing_record_ack_tx(arq_timing_ctx_t *ctx, int seq)
{
    ctx->ack_tx_start_ms = hermes_uptime_ms();
    uint32_t delay = 0;
    if (ctx->data_rx_ms > 0)
        delay = (uint32_t)(ctx->ack_tx_start_ms - ctx->data_rx_ms);
    HLOGT(LOG_COMP, "ack_tx seq=%d delay_from_rx=%ums", seq, delay);
}

void arq_timing_record_retry(arq_timing_ctx_t *ctx, int seq,
                              int attempt, const char *reason)
{
    ctx->retry_count++;
    ctx->retries_total++;
    HLOGT(LOG_COMP, "retry seq=%d attempt=%d reason=%s", seq, attempt, reason);
}

void arq_timing_record_turn(arq_timing_ctx_t *ctx, bool to_iss,
                             const char *reason)
{
    (void)ctx;
    HLOGT(LOG_COMP, "turn dir=%s reason=%s", to_iss ? "→ISS" : "→IRS", reason);
}

void arq_timing_record_connect(arq_timing_ctx_t *ctx, int mode)
{
    arq_timing_init(ctx);  /* reset per-session counters */
    ctx->session_start_ms = hermes_uptime_ms();
    HLOGT(LOG_COMP, "connect mode=%s", mode_name(mode));
}

void arq_timing_record_disconnect(arq_timing_ctx_t *ctx, const char *reason)
{
    uint64_t now = hermes_uptime_ms();
    uint64_t session_ms = 0;
    if (now >= ctx->session_start_ms)
        session_ms = now - ctx->session_start_ms;

    double rf_duty_pct = 0.0;
    if (session_ms > 0)
        rf_duty_pct = ((double)ctx->rf_air_ms * 100.0) / (double)session_ms;

    double data_duty_pct = 0.0;
    if (session_ms > 0)
        data_duty_pct = ((double)ctx->tx_air_ms * 100.0) / (double)session_ms;

    double payload_bps = 0.0;
    if (ctx->tx_air_ms > 0)
        payload_bps = ((double)ctx->tx_bytes * 8.0 * 1000.0) / (double)ctx->tx_air_ms;

    uint64_t avg_ack_wait_ms = 0;
    uint64_t avg_ack_delay_ms = 0;
    if (ctx->ack_count > 0)
    {
        avg_ack_wait_ms = ctx->ack_wait_ms / ctx->ack_count;
        avg_ack_delay_ms = ctx->ack_delay_total_ms / ctx->ack_count;
    }

    HLOGT(LOG_COMP,
          "disconnect reason=%s tx_bytes=%llu rx_bytes=%llu "
          "frames_tx=%llu frames_rx=%llu retries=%llu "
          "rf_air=%llums tx_air=%llums session=%llums "
          "rf_duty=%.1f%% data_duty=%.1f%% "
          "payload_rate=%.1fbps avg_ack_wait=%llums avg_ack_delay=%llums",
          reason,
          (unsigned long long)ctx->tx_bytes,
          (unsigned long long)ctx->rx_bytes,
          (unsigned long long)ctx->frames_tx,
          (unsigned long long)ctx->frames_rx,
          (unsigned long long)ctx->retries_total,
          (unsigned long long)ctx->rf_air_ms,
          (unsigned long long)ctx->tx_air_ms,
          (unsigned long long)session_ms,
          rf_duty_pct,
          data_duty_pct,
          payload_bps,
          (unsigned long long)avg_ack_wait_ms,
          (unsigned long long)avg_ack_delay_ms);
}
