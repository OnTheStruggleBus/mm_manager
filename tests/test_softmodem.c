/*
 * Loopback test for the software Bell 212A modem.
 *
 * Connects an originating and an answering software modem through a
 * simulated telephone channel (G.711 companding, delay, echo, noise and
 * sample clock offset) and verifies that data passes in both directions.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "src/mm_softmodem.h"

#define TEST_BYTES  (300)
#define BLOCK       (160)

typedef struct endpoint {
    const char *name;
    uint8_t tx_data[TEST_BYTES];
    int     tx_pos;
    uint8_t rx_data[TEST_BYTES * 2];
    int     rx_pos;
    int     enabled;
} endpoint_t;

static int get_byte(void *user) {
    endpoint_t *ep = (endpoint_t *)user;

    if (ep->enabled && (ep->tx_pos < TEST_BYTES)) {
        return ep->tx_data[ep->tx_pos++];
    }
    return -1;
}

static void put_byte(void *user, uint8_t byte) {
    endpoint_t *ep = (endpoint_t *)user;

    if (ep->rx_pos < (int)sizeof(ep->rx_data)) {
        ep->rx_data[ep->rx_pos++] = byte;
    }
}

/* Simple one-way channel: sample clock offset (linear interpolation), echo is added by caller. */
typedef struct channel {
    double ratio;       /* Output samples per input sample */
    double pos;
    int16_t last;
    uint32_t noise_seed;
    float noise_amp;
    int use_alaw;
} channel_t;

static float noise(channel_t *ch) {
    /* Approximately Gaussian noise via sum of uniforms. */
    float s = 0.0f;
    for (int i = 0; i < 4; i++) {
        ch->noise_seed = ch->noise_seed * 1103515245u + 12345u;
        s += (float)((ch->noise_seed >> 8) & 0xFFFF) / 65536.0f - 0.5f;
    }
    return s * ch->noise_amp;
}

static int16_t codec(channel_t *ch, float v) {
    int16_t s;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    s = (int16_t)lrintf(v);
    return ch->use_alaw ? sm_alaw_to_linear(sm_linear_to_alaw(s)) : sm_ulaw_to_linear(sm_linear_to_ulaw(s));
}

/* Resample in -> out, returns number of output samples. */
static int channel_run(channel_t *ch, const int16_t *in, int len, int16_t *out, int max_out) {
    int n = 0;
    for (int i = 0; i < len; i++) {
        while ((ch->pos < 1.0) && (n < max_out)) {
            double v = ch->last + (in[i] - ch->last) * ch->pos;
            out[n] = (int16_t)lrint(v);
            n++;
            ch->pos += 1.0 / ch->ratio;
        }
        ch->pos -= 1.0;
        ch->last = in[i];
    }
    return n;
}

static int run_test(const char *desc, double clock_ratio, float noise_amp, float echo_gain, int answer_tone, int use_alaw) {
    endpoint_t orig_ep = { "orig", { 0 }, 0, { 0 }, 0, 0 };
    endpoint_t ans_ep = { "answer", { 0 }, 0, { 0 }, 0, 0 };
    softmodem_t *orig;
    softmodem_t *ans;
    channel_t ch_o2a = { clock_ratio, 0.0, 0, 1, noise_amp, use_alaw };
    channel_t ch_a2o = { 1.0 / clock_ratio, 0.0, 0, 2, noise_amp, use_alaw };
    int16_t o_tx[BLOCK], a_tx[BLOCK];
    int16_t o_rx[BLOCK * 2], a_rx[BLOCK * 2];
    int16_t a_echo[BLOCK * 2] = { 0 };
    int16_t o_echo[BLOCK * 2] = { 0 };
    int connect_block = -1;
    int errors = 0;
    int i;

    for (i = 0; i < TEST_BYTES; i++) {
        orig_ep.tx_data[i] = (uint8_t)(rand() & 0xFF);
        ans_ep.tx_data[i] = (uint8_t)(rand() & 0xFF);
    }

    orig = softmodem_create(0, answer_tone, get_byte, put_byte, &orig_ep);
    ans = softmodem_create(1, answer_tone, get_byte, put_byte, &ans_ep);

    for (int blk = 0; blk < 50 * 20; blk++) {
        int n_a, n_o;

        softmodem_tx(orig, o_tx, BLOCK);
        softmodem_tx(ans, a_tx, BLOCK);

        /* Each side hears the far end, plus an echo of its own signal. */
        for (i = 0; i < BLOCK; i++) {
            o_tx[i] = codec(&ch_o2a, (float)o_tx[i]);
            a_tx[i] = codec(&ch_a2o, (float)a_tx[i]);
        }

        n_a = channel_run(&ch_o2a, o_tx, BLOCK, a_rx, BLOCK * 2);
        n_o = channel_run(&ch_a2o, a_tx, BLOCK, o_rx, BLOCK * 2);

        for (i = 0; i < n_a; i++) {
            float v = (float)a_rx[i] + echo_gain * (float)a_echo[i % BLOCK] + noise(&ch_o2a);
            a_rx[i] = codec(&ch_o2a, v);
        }
        for (i = 0; i < n_o; i++) {
            float v = (float)o_rx[i] + echo_gain * (float)o_echo[i % BLOCK] + noise(&ch_a2o);
            o_rx[i] = codec(&ch_a2o, v);
        }
        memcpy(a_echo, a_tx, sizeof(a_tx));
        memcpy(o_echo, o_tx, sizeof(o_tx));

        softmodem_rx(ans, a_rx, n_a);
        softmodem_rx(orig, o_rx, n_o);

        if ((connect_block < 0) && (softmodem_state(ans) == SM_STATE_DATA) && (softmodem_state(orig) == SM_STATE_DATA)) {
            connect_block = blk;
        }

        /* Like the Millennium protocol, start sending shortly after both sides connect. */
        if ((connect_block >= 0) && (blk > connect_block + 10)) {
            orig_ep.enabled = 1;
            ans_ep.enabled = 1;
        }

        if ((softmodem_state(ans) >= SM_STATE_FAILED) || (softmodem_state(orig) >= SM_STATE_FAILED)) {
            break;
        }
        if ((orig_ep.tx_pos == TEST_BYTES) && (ans_ep.tx_pos == TEST_BYTES) &&
            (orig_ep.rx_pos >= TEST_BYTES) && (ans_ep.rx_pos >= TEST_BYTES)) {
            break;
        }
    }

    if (orig_ep.rx_pos != TEST_BYTES || ans_ep.rx_pos != TEST_BYTES) {
        errors++;
    }
    for (i = 0; i < TEST_BYTES; i++) {
        if (ans_ep.rx_data[i] != orig_ep.tx_data[i]) errors++;
        if (orig_ep.rx_data[i] != ans_ep.tx_data[i]) errors++;
    }

    printf("%-40s: connect at %5d ms, orig rx %3d/%d bytes, answer rx %3d/%d bytes, states %s/%s, errors %d: %s\n",
           desc, connect_block * 20, orig_ep.rx_pos, TEST_BYTES, ans_ep.rx_pos, TEST_BYTES,
           softmodem_state_to_str(softmodem_state(orig)), softmodem_state_to_str(softmodem_state(ans)),
           errors, errors ? "FAIL" : "PASS");

    softmodem_free(orig);
    softmodem_free(ans);
    return errors ? 1 : 0;
}

static int test_g711(void) {
    int max_err_u = 0, max_err_a = 0;
    for (int v = -32768; v <= 32767; v += 7) {
        int du = abs(sm_ulaw_to_linear(sm_linear_to_ulaw((int16_t)v)) - v);
        int da = abs(sm_alaw_to_linear(sm_linear_to_alaw((int16_t)v)) - v);
        int lim = abs(v) / 16 + 40;
        if (du > lim && abs(v) < 32000) max_err_u = du;
        if (da > lim && abs(v) < 32000) max_err_a = da;
    }
    printf("G.711 round trip: %s\n", (max_err_u || max_err_a) ? "FAIL" : "PASS");
    return (max_err_u || max_err_a) ? 1 : 0;
}

int main(void) {
    int failures = 0;

    srand(1234);
    failures += test_g711();
    failures += run_test("Clean channel", 1.0, 0.0f, 0.0f, SM_ANSWER_TONE_BELL, 0);
    failures += run_test("A-law channel", 1.0, 0.0f, 0.0f, SM_ANSWER_TONE_BELL, 1);
    failures += run_test("V.25 answer tone", 1.0, 0.0f, 0.0f, SM_ANSWER_TONE_V25, 0);
    failures += run_test("Clock offset +200ppm", 1.0002, 0.0f, 0.0f, SM_ANSWER_TONE_BELL, 0);
    failures += run_test("Clock offset -200ppm", 0.9998, 0.0f, 0.0f, SM_ANSWER_TONE_BELL, 0);
    failures += run_test("Noise (~25 dB SNR)", 1.0, 300.0f, 0.0f, SM_ANSWER_TONE_BELL, 0);
    failures += run_test("Echo -10 dB", 1.0, 0.0f, 0.316f, SM_ANSWER_TONE_BELL, 0);
    failures += run_test("Echo, noise and clock offset", 1.0001, 150.0f, 0.316f, SM_ANSWER_TONE_BELL, 0);

    printf("%s\n", failures ? "Some tests FAILED" : "All tests passed");
    return failures ? 1 : 0;
}
