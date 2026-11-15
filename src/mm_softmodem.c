/*
 * Software Bell 212A (1200 bps DPSK) modem.
 *
 * Bell 212A is a full-duplex, frequency-division modem:
 *   - Originating modem transmits on a 1200 Hz carrier, answering modem
 *     transmits on a 2400 Hz carrier.
 *   - 600 baud, differential PSK, two bits (one dibit) per symbol.
 *   - Data is scrambled with the 1 + x^-14 + x^-17 self-synchronizing
 *     scrambler (the same one used by V.22.)
 *   - Asynchronous characters (start bit, 8 data bits, stop bit) are sent
 *     directly through the scrambler; idle line is continuous marks.
 *
 * Like the software fax modems used on VoIP gateways, the modem works on
 * 8 kHz PCM samples and needs no real-time clock of its own: the receiver
 * recovers symbol timing from the signal, and differential detection makes
 * carrier phase recovery unnecessary.
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

#include "mm_softmodem.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif /* M_PI */

#define SM_BAUD             (600)
#define SM_SPS              ((float)SM_SAMPLE_RATE / (float)SM_BAUD)   /* 13.333 samples per symbol */

/* Transmit pulse shaping is done at 24 kHz, where a symbol is exactly 40 samples. */
#define TX_UPSAMPLE         (3)
#define TX_SPS24            (40)
#define TX_SPAN_SYMBOLS     (8)
#define TX_TAPS24           (TX_SPS24 * TX_SPAN_SYMBOLS)

/* Receive matched filter */
#define RX_SPAN_SYMBOLS     (8)
#define RX_TAPS             (107)   /* ~8 symbols at 8 kHz, odd */

#define RRC_ALPHA           (0.75f)

/* Levels: a 0 dBm0 sine wave has a peak amplitude of ~22656 in 16-bit linear PCM. */
#define DBM0_REF_AMPLITUDE  (22656.0f)
#define TX_LEVEL_DBM0       (-10.0f)
#define TONE_LEVEL_DBM0     (-12.0f)
#define CD_ON_DBM0          (-43.0f)
#define CD_OFF_DBM0         (-48.0f)

/* Handshake timing, in milliseconds */
#define ANSWER_SILENCE_MS   (2000)  /* Billing delay before answer tone */
#define ANSWER_TONE_MAX_MS  (3300)  /* Give up waiting for the originator, start carrier anyway */
#define ANSWER_TONE_DET_MS  (150)   /* Far-end carrier needed to end answer tone */
#define TONE_DETECT_MS      (150)   /* Originate: answer tone needed before carrier */
#define TRAINING_MIN_MS     (765)   /* Originate: minimum scrambled marks before data mode */
#define ANS_TRAINING_MIN_MS (1000)  /* Answer: allow the originator its 765 ms of marks first */
#define TRAINING_MAX_MS     (8000)  /* Handshake timeout */
#define TRAINING_ONES_BITS  (96)    /* Answer: consecutive descrambled marks needed to declare connect */
#define ORIG_ONES_BITS      (918)   /* Originate: 765 ms of descrambled marks before data mode */
#define CD_ON_MS            (20)
#define CD_OFF_MS           (40)
#define CARRIER_LOSS_MS     (1000)  /* Carrier must be gone this long in data mode to hang up */

#define MS_TO_SAMPLES(ms)   ((uint32_t)(ms) * (SM_SAMPLE_RATE / 1000))

typedef struct sm_complex {
    float re;
    float im;
} sm_complex_t;

struct softmodem {
    int             answer_mode;
    sm_state_t      state;
    uint32_t        state_samples;      /* Samples spent in the current state */

    sm_get_byte_fn  get_byte;
    sm_put_byte_fn  put_byte;
    void           *user;

    /* Transmitter */
    float           tx_pulse[TX_TAPS24];
    sm_complex_t    tx_sym[TX_SPAN_SYMBOLS];   /* tx_sym[0] is the newest symbol */
    int             tx_frac24;              /* Position of current sample past newest symbol, 24 kHz units */
    int             tx_quadrant;            /* Current absolute phase, in units of 90 degrees */
    double          tx_carrier_phase;
    double          tx_carrier_step;
    double          tone_phase;
    double          tone_step;
    float           tx_gain;
    float           tone_gain;
    uint32_t        tx_scramble;            /* Scrambler shift register */
    uint16_t        tx_shift;               /* Async character being shifted out */
    int             tx_bits_left;

    /* Receiver */
    float           rx_filter[RX_TAPS];
    sm_complex_t    rx_hist[RX_TAPS * 2];   /* Doubled so the convolution never wraps */
    int             rx_hist_idx;
    double          rx_carrier_phase;
    double          rx_carrier_step;
    float           rx_power;
    float           cd_on_level;
    float           cd_off_level;
    int             carrier_on;
    uint32_t        cd_count;
    uint32_t        carrier_off_samples;

    /* Answer tone detector (originate mode) */
    double          det_tone_phase;
    double          det_tone_step;
    sm_complex_t    det_tone_lp;
    float           det_tone_power;
    uint32_t        tone_count;
    int             tone_seen;

    /* Symbol timing recovery */
    sm_complex_t    z_prev;
    float           strobe_count;
    int             strobe_is_mid;
    sm_complex_t    sym_prev;
    sm_complex_t    sym_mid;
    int             have_sym_prev;
    uint32_t        symbols_since_cd;
    float           rotation;

    /* Descrambler and async framing */
    uint32_t        rx_scramble;
    uint32_t        rx_ones_run;
    int             training_ones_seen;
    int             rx_in_char;
    int             rx_bit_count;
    uint16_t        rx_char;
};

static const char *sm_state_names[] = {
    "Answer Silence",
    "Answer Tone",
    "Wait Answer Tone",
    "Training",
    "Data",
    "Failed",
    "Carrier Lost"
};

/* Root-raised-cosine impulse response, t in symbol periods. */
static float rrc(float t, float alpha) {
    double x = (double)t;
    double a = (double)alpha;

    if (fabs(x) < 1e-6) {
        return (float)(1.0 - a + 4.0 * a / M_PI);
    }

    if (fabs(fabs(4.0 * a * x) - 1.0) < 1e-6) {
        return (float)((a / sqrt(2.0)) * ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * a)) +
                                          (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * a))));
    }

    return (float)((sin(M_PI * x * (1.0 - a)) + 4.0 * a * x * cos(M_PI * x * (1.0 + a))) /
                   (M_PI * x * (1.0 - (4.0 * a * x) * (4.0 * a * x))));
}

static float dbm0_to_amplitude(float dbm0) {
    return DBM0_REF_AMPLITUDE * powf(10.0f, dbm0 / 20.0f);
}

static void set_state(softmodem_t *sm, sm_state_t state) {
    sm->state = state;
    sm->state_samples = 0;
}

softmodem_t *softmodem_create(int answer_mode, int answer_tone_hz,
                              sm_get_byte_fn get_byte, sm_put_byte_fn put_byte, void *user) {
    softmodem_t *sm;
    float sum;
    int i;
    double tx_carrier;
    double rx_carrier;

    sm = (softmodem_t *)calloc(1, sizeof(softmodem_t));
    if (sm == NULL) {
        return NULL;
    }

    sm->answer_mode = answer_mode;
    sm->get_byte = get_byte;
    sm->put_byte = put_byte;
    sm->user = user;

    if (answer_tone_hz <= 0) {
        answer_tone_hz = SM_ANSWER_TONE_BELL;
    }

    tx_carrier = answer_mode ? 2400.0 : 1200.0;
    rx_carrier = answer_mode ? 1200.0 : 2400.0;
    sm->tx_carrier_step = 2.0 * M_PI * tx_carrier / SM_SAMPLE_RATE;
    sm->rx_carrier_step = 2.0 * M_PI * rx_carrier / SM_SAMPLE_RATE;
    sm->tone_step = 2.0 * M_PI * answer_tone_hz / SM_SAMPLE_RATE;
    sm->det_tone_step = sm->tone_step;

    /* Transmit pulse at 24 kHz, normalized for unity gain per symbol. */
    sum = 0.0f;
    for (i = 0; i < TX_TAPS24; i++) {
        float t = ((float)i - (float)(TX_TAPS24 - 1) / 2.0f) / (float)TX_SPS24;
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i / (float)(TX_TAPS24 - 1));
        sm->tx_pulse[i] = rrc(t, RRC_ALPHA) * w;
        sum += sm->tx_pulse[i];
    }
    for (i = 0; i < TX_TAPS24; i++) {
        sm->tx_pulse[i] *= (float)TX_SPS24 / sum;
    }

    /*
     * Receive matched filter.  Normalized for a DC gain of 2 so that a
     * sine wave of amplitude A at the carrier frequency produces a
     * baseband magnitude of A.
     */
    sum = 0.0f;
    for (i = 0; i < RX_TAPS; i++) {
        float t = ((float)i - (float)(RX_TAPS - 1) / 2.0f) / SM_SPS;
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i / (float)(RX_TAPS - 1));
        sm->rx_filter[i] = rrc(t, RRC_ALPHA) * w;
        sum += sm->rx_filter[i];
    }
    for (i = 0; i < RX_TAPS; i++) {
        sm->rx_filter[i] *= 2.0f / sum;
    }

    sm->tx_gain = dbm0_to_amplitude(TX_LEVEL_DBM0);
    sm->tone_gain = dbm0_to_amplitude(TONE_LEVEL_DBM0);
    sm->cd_on_level = dbm0_to_amplitude(CD_ON_DBM0) * dbm0_to_amplitude(CD_ON_DBM0);
    sm->cd_off_level = dbm0_to_amplitude(CD_OFF_DBM0) * dbm0_to_amplitude(CD_OFF_DBM0);

    for (i = 0; i < TX_SPAN_SYMBOLS; i++) {
        sm->tx_sym[i].re = 0.0f;
        sm->tx_sym[i].im = 0.0f;
    }

    sm->strobe_count = SM_SPS / 2.0f;
    set_state(sm, answer_mode ? SM_STATE_ANSWER_SILENCE : SM_STATE_WAIT_ANSWER_TONE);

    return sm;
}

void softmodem_free(softmodem_t *sm) {
    free(sm);
}

sm_state_t softmodem_state(const softmodem_t *sm) {
    return sm->state;
}

const char *softmodem_state_to_str(sm_state_t state) {
    if ((unsigned)state < sizeof(sm_state_names) / sizeof(sm_state_names[0])) {
        return sm_state_names[state];
    }
    return "Unknown";
}

int softmodem_tx_busy(const softmodem_t *sm) {
    return sm->tx_bits_left > 0;
}

int softmodem_carrier_detect(const softmodem_t *sm) {
    return sm->carrier_on;
}

float softmodem_rx_power_dbm0(const softmodem_t *sm) {
    float p = sm->rx_power;

    if (p < 1.0f) p = 1.0f;
    return 10.0f * log10f(p / (DBM0_REF_AMPLITUDE * DBM0_REF_AMPLITUDE));
}

/*
 * Transmitter
 */

/* Next bit of async data: start bit, 8 data bits LSB first, stop bit. Idle is mark. */
static int tx_next_data_bit(softmodem_t *sm) {
    int bit;

    if (sm->tx_bits_left == 0) {
        if ((sm->state == SM_STATE_DATA) && (sm->get_byte != NULL)) {
            int byte = sm->get_byte(sm->user);

            if (byte >= 0) {
                sm->tx_shift = (uint16_t)((1u << 9) | ((unsigned)(byte & 0xFF) << 1));
                sm->tx_bits_left = 10;
            }
        }

        if (sm->tx_bits_left == 0) {
            return 1;
        }
    }

    bit = sm->tx_shift & 1;
    sm->tx_shift >>= 1;
    sm->tx_bits_left--;
    return bit;
}

/* Scrambler: 1 + x^-14 + x^-17 */
static int tx_scramble_bit(softmodem_t *sm, int in_bit) {
    int out_bit = (in_bit ^ (int)(sm->tx_scramble >> 13) ^ (int)(sm->tx_scramble >> 16)) & 1;

    sm->tx_scramble = (sm->tx_scramble << 1) | (uint32_t)out_bit;
    return out_bit;
}

static void tx_new_symbol(softmodem_t *sm) {
    /* Phase change for each dibit (first bit, second bit): 00:+90, 01:0, 11:+270, 10:+180 */
    static const int dibit_to_quadrant[4] = { 1, 0, 2, 3 };
    int b1 = tx_scramble_bit(sm, tx_next_data_bit(sm));
    int b2 = tx_scramble_bit(sm, tx_next_data_bit(sm));
    int i;

    sm->tx_quadrant = (sm->tx_quadrant + dibit_to_quadrant[(b1 << 1) | b2]) & 3;

    for (i = TX_SPAN_SYMBOLS - 1; i > 0; i--) {
        sm->tx_sym[i] = sm->tx_sym[i - 1];
    }

    sm->tx_sym[0].re = (sm->tx_quadrant == 0) ? 1.0f : (sm->tx_quadrant == 2) ? -1.0f : 0.0f;
    sm->tx_sym[0].im = (sm->tx_quadrant == 1) ? 1.0f : (sm->tx_quadrant == 3) ? -1.0f : 0.0f;
}

static float tx_dpsk_sample(softmodem_t *sm) {
    sm_complex_t b = { 0.0f, 0.0f };
    float out;
    int i;

    sm->tx_frac24 += TX_UPSAMPLE;
    if (sm->tx_frac24 >= TX_SPS24) {
        sm->tx_frac24 -= TX_SPS24;
        tx_new_symbol(sm);
    }

    for (i = 0; i < TX_SPAN_SYMBOLS; i++) {
        float h = sm->tx_pulse[sm->tx_frac24 + i * TX_SPS24];
        b.re += sm->tx_sym[i].re * h;
        b.im += sm->tx_sym[i].im * h;
    }

    out = (b.re * (float)cos(sm->tx_carrier_phase) - b.im * (float)sin(sm->tx_carrier_phase)) * sm->tx_gain;
    sm->tx_carrier_phase += sm->tx_carrier_step;
    if (sm->tx_carrier_phase > 2.0 * M_PI) sm->tx_carrier_phase -= 2.0 * M_PI;

    return out;
}

static int16_t clamp16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

void softmodem_tx(softmodem_t *sm, int16_t *amp, int len) {
    int i;

    for (i = 0; i < len; i++) {
        float out = 0.0f;

        switch (sm->state) {
        case SM_STATE_ANSWER_TONE:
            out = (float)sin(sm->tone_phase) * sm->tone_gain;
            sm->tone_phase += sm->tone_step;
            if (sm->tone_phase > 2.0 * M_PI) sm->tone_phase -= 2.0 * M_PI;
            break;
        case SM_STATE_TRAINING:
        case SM_STATE_DATA:
            out = tx_dpsk_sample(sm);
            break;
        default:
            break;
        }

        amp[i] = clamp16(out);
    }
}

/*
 * Receiver
 */

static void rx_reset_timing(softmodem_t *sm) {
    sm->strobe_count = SM_SPS / 2.0f;
    sm->strobe_is_mid = 0;
    sm->have_sym_prev = 0;
    sm->symbols_since_cd = 0;
    sm->rotation = 0.0f;
    sm->rx_ones_run = 0;
    sm->rx_in_char = 0;
}

static void rx_put_bit(softmodem_t *sm, int scrambled_bit) {
    int bit = (scrambled_bit ^ (int)(sm->rx_scramble >> 13) ^ (int)(sm->rx_scramble >> 16)) & 1;

    sm->rx_scramble = (sm->rx_scramble << 1) | (uint32_t)scrambled_bit;

    if (bit) {
        sm->rx_ones_run++;
    } else {
        sm->rx_ones_run = 0;
    }

    if (sm->state != SM_STATE_DATA) {
        return;
    }

    if (!sm->rx_in_char) {
        if (bit == 0) {
            sm->rx_in_char = 1;
            sm->rx_bit_count = 0;
            sm->rx_char = 0;
        }
    } else if (sm->rx_bit_count < 8) {
        sm->rx_char |= (uint16_t)(bit << sm->rx_bit_count);
        sm->rx_bit_count++;
    } else {
        /* Stop bit position. */
        if (sm->put_byte != NULL) {
            sm->put_byte(sm->user, (uint8_t)sm->rx_char);
        }

        if (bit == 0) {
            /* Deleted stop bit (rate adaptation): this is the next start bit. */
            sm->rx_bit_count = 0;
            sm->rx_char = 0;
        } else {
            sm->rx_in_char = 0;
        }
    }
}

static void rx_symbol(softmodem_t *sm, sm_complex_t sym) {
    /* Quadrant of phase change to dibit (first bit << 1 | second bit): 0:01, 90:00, 180:10, 270:11 */
    static const int quadrant_to_dibit[4] = { 1, 0, 2, 3 };
    sm_complex_t d;
    float angle;
    float err;
    int quadrant;
    float c;
    float s;

    if (!sm->have_sym_prev) {
        sm->sym_prev = sym;
        sm->have_sym_prev = 1;
        return;
    }

    /* Differential detection: d = sym * conj(prev) */
    d.re = sym.re * sm->sym_prev.re + sym.im * sm->sym_prev.im;
    d.im = sym.im * sm->sym_prev.re - sym.re * sm->sym_prev.im;

    /* Remove any constant rotation due to carrier frequency offset. */
    c = cosf(sm->rotation);
    s = sinf(sm->rotation);
    angle = atan2f(d.im * c - d.re * s, d.re * c + d.im * s);

    quadrant = (int)lrintf(angle / ((float)M_PI / 2.0f));
    err = angle - (float)quadrant * ((float)M_PI / 2.0f);
    quadrant &= 3;

    if (sm->symbols_since_cd > 30) {
        sm->rotation += 0.02f * err;
    }

    sm->symbols_since_cd++;

    /* Let the timing loop settle before trusting the data. */
    if (sm->symbols_since_cd > 8) {
        int dibit = quadrant_to_dibit[quadrant];
        rx_put_bit(sm, (dibit >> 1) & 1);
        rx_put_bit(sm, dibit & 1);
    }
}

/* Gardner timing recovery, strobing twice per symbol. */
static void rx_timing(softmodem_t *sm, sm_complex_t z) {
    sm->strobe_count -= 1.0f;

    if (sm->strobe_count <= 0.0f) {
        /* The strobe instant lies between the previous and current sample. */
        float mu = -sm->strobe_count;   /* 0..1, weight of previous sample */
        sm_complex_t y;

        y.re = sm->z_prev.re * mu + z.re * (1.0f - mu);
        y.im = sm->z_prev.im * mu + z.im * (1.0f - mu);

        if (sm->strobe_is_mid) {
            sm->sym_mid = y;
            sm->strobe_count += SM_SPS / 2.0f;
        } else {
            float adjust = 0.0f;

            if (sm->have_sym_prev) {
                float e = (sm->sym_prev.re - y.re) * sm->sym_mid.re +
                          (sm->sym_prev.im - y.im) * sm->sym_mid.im;
                float gain = (sm->symbols_since_cd < 100) ? 1.0f : 0.25f;

                e /= (sm->rx_power + 1.0f);
                if (e > 1.0f) e = 1.0f;
                if (e < -1.0f) e = -1.0f;
                adjust = gain * e;
            }

            rx_symbol(sm, y);
            sm->sym_prev = y;
            sm->strobe_count += SM_SPS / 2.0f + adjust;
        }

        sm->strobe_is_mid = !sm->strobe_is_mid;
    }

    sm->z_prev = z;
}

static void rx_tone_detect(softmodem_t *sm, float sample, float total_power) {
    sm_complex_t m;
    float tone_power;

    m.re = sample * (float)cos(sm->det_tone_phase);
    m.im = -sample * (float)sin(sm->det_tone_phase);
    sm->det_tone_phase += sm->det_tone_step;
    if (sm->det_tone_phase > 2.0 * M_PI) sm->det_tone_phase -= 2.0 * M_PI;

    /* Narrow (~25 Hz) low-pass around the tone frequency. */
    sm->det_tone_lp.re += 0.02f * (m.re - sm->det_tone_lp.re);
    sm->det_tone_lp.im += 0.02f * (m.im - sm->det_tone_lp.im);
    tone_power = 4.0f * (sm->det_tone_lp.re * sm->det_tone_lp.re + sm->det_tone_lp.im * sm->det_tone_lp.im);
    sm->det_tone_power = tone_power;

    if ((tone_power > sm->cd_on_level) && (tone_power > 0.5f * total_power)) {
        sm->tone_count++;
    } else {
        sm->tone_count = 0;
    }
}

void softmodem_rx(softmodem_t *sm, const int16_t *amp, int len) {
    int i;
    int k;

    for (i = 0; i < len; i++) {
        float x = (float)amp[i];
        sm_complex_t m;
        sm_complex_t z = { 0.0f, 0.0f };
        const sm_complex_t *h;
        float p;
        float total_power;
        int tone_present;

        /* Mix the receive carrier down to baseband and apply matched filter. */
        m.re = x * (float)cos(sm->rx_carrier_phase);
        m.im = -x * (float)sin(sm->rx_carrier_phase);
        sm->rx_carrier_phase += sm->rx_carrier_step;
        if (sm->rx_carrier_phase > 2.0 * M_PI) sm->rx_carrier_phase -= 2.0 * M_PI;

        sm->rx_hist[sm->rx_hist_idx] = m;
        sm->rx_hist[sm->rx_hist_idx + RX_TAPS] = m;
        sm->rx_hist_idx++;
        if (sm->rx_hist_idx >= RX_TAPS) sm->rx_hist_idx = 0;

        h = &sm->rx_hist[sm->rx_hist_idx];
        for (k = 0; k < RX_TAPS; k++) {
            z.re += h[k].re * sm->rx_filter[k];
            z.im += h[k].im * sm->rx_filter[k];
        }

        p = z.re * z.re + z.im * z.im;
        sm->rx_power += (p - sm->rx_power) * (1.0f / 64.0f);

        /* In originate mode, the answer tone falls within the receive band. */
        total_power = sm->rx_power;
        tone_present = 0;
        if (!sm->answer_mode) {
            rx_tone_detect(sm, x, total_power);
            tone_present = sm->tone_count > MS_TO_SAMPLES(10);
            if (sm->tone_count > MS_TO_SAMPLES(TONE_DETECT_MS)) {
                sm->tone_seen = 1;
            }
        }

        /* Carrier detect with hysteresis. */
        if (!sm->carrier_on) {
            if ((sm->rx_power > sm->cd_on_level) && !tone_present) {
                if (++sm->cd_count >= MS_TO_SAMPLES(CD_ON_MS)) {
                    sm->carrier_on = 1;
                    sm->cd_count = 0;
                    rx_reset_timing(sm);
                }
            } else {
                sm->cd_count = 0;
            }
        } else {
            if ((sm->rx_power < sm->cd_off_level) || tone_present) {
                if (++sm->cd_count >= MS_TO_SAMPLES(CD_OFF_MS)) {
                    sm->carrier_on = 0;
                    sm->cd_count = 0;
                }
            } else {
                sm->cd_count = 0;
            }
        }

        if (sm->carrier_on) {
            rx_timing(sm, z);
            sm->carrier_off_samples = 0;
        } else {
            sm->z_prev = z;
            sm->carrier_off_samples++;
            sm->rx_ones_run = 0;
        }

        /* Handshake state machine */
        sm->state_samples++;
        switch (sm->state) {
        case SM_STATE_ANSWER_SILENCE:
            if (sm->state_samples >= MS_TO_SAMPLES(ANSWER_SILENCE_MS)) {
                set_state(sm, SM_STATE_ANSWER_TONE);
            }
            break;
        case SM_STATE_ANSWER_TONE:
            /* Far-end carrier for long enough, or timeout: start our carrier. */
            if ((sm->carrier_on && (sm->symbols_since_cd > (uint32_t)(ANSWER_TONE_DET_MS * SM_BAUD / 1000))) ||
                (sm->state_samples >= MS_TO_SAMPLES(ANSWER_TONE_MAX_MS))) {
                set_state(sm, SM_STATE_TRAINING);
            }
            break;
        case SM_STATE_WAIT_ANSWER_TONE:
            if (sm->tone_seen) {
                set_state(sm, SM_STATE_TRAINING);
            } else if (sm->state_samples >= MS_TO_SAMPLES(TRAINING_MAX_MS)) {
                set_state(sm, SM_STATE_FAILED);
            }
            break;
        case SM_STATE_TRAINING:
            if (sm->carrier_on &&
                (sm->rx_ones_run >= (uint32_t)(sm->answer_mode ? TRAINING_ONES_BITS : ORIG_ONES_BITS))) {
                sm->training_ones_seen = 1;
            }
            if ((sm->state_samples >= MS_TO_SAMPLES(sm->answer_mode ? ANS_TRAINING_MIN_MS : TRAINING_MIN_MS)) &&
                sm->carrier_on && sm->training_ones_seen) {
                set_state(sm, SM_STATE_DATA);
                sm->rx_in_char = 0;
            } else if (sm->state_samples >= MS_TO_SAMPLES(TRAINING_MAX_MS)) {
                set_state(sm, SM_STATE_FAILED);
            }
            break;
        case SM_STATE_DATA:
            if (sm->carrier_off_samples >= MS_TO_SAMPLES(CARRIER_LOSS_MS)) {
                set_state(sm, SM_STATE_CARRIER_LOST);
            }
            break;
        default:
            break;
        }
    }
}

/*
 * G.711 codecs
 */
#define ULAW_BIAS   (0x84)
#define ULAW_CLIP   (32635)

uint8_t sm_linear_to_ulaw(int16_t pcm) {
    int sign;
    int exponent;
    int mantissa;
    int sample = pcm;

    sign = (sample >> 8) & 0x80;
    if (sign) sample = -sample;
    if (sample > ULAW_CLIP) sample = ULAW_CLIP;
    sample += ULAW_BIAS;

    exponent = 7;
    for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }

    mantissa = (sample >> (exponent + 3)) & 0x0F;
    return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

int16_t sm_ulaw_to_linear(uint8_t ulaw) {
    int sign;
    int exponent;
    int mantissa;
    int sample;

    ulaw = (uint8_t)~ulaw;
    sign = ulaw & 0x80;
    exponent = (ulaw >> 4) & 0x07;
    mantissa = ulaw & 0x0F;
    sample = (((mantissa << 3) + ULAW_BIAS) << exponent) - ULAW_BIAS;

    return (int16_t)(sign ? -sample : sample);
}

uint8_t sm_linear_to_alaw(int16_t pcm) {
    int sign;
    int exponent;
    int mantissa;
    int sample = pcm;
    uint8_t alaw;

    sign = (sample >= 0) ? 0x80 : 0x00;
    if (!sign) sample = -sample - 1;
    if (sample > 32767) sample = 32767;

    if (sample >= 256) {
        exponent = 7;
        for (int mask = 0x4000; (sample & mask) == 0 && exponent > 1; mask >>= 1) {
            exponent--;
        }
        mantissa = (sample >> (exponent + 3)) & 0x0F;
        alaw = (uint8_t)((exponent << 4) | mantissa);
    } else {
        alaw = (uint8_t)(sample >> 4);
    }

    return (uint8_t)((alaw | sign) ^ 0x55);
}

int16_t sm_alaw_to_linear(uint8_t alaw) {
    int sign;
    int exponent;
    int mantissa;
    int sample;

    alaw ^= 0x55;
    sign = alaw & 0x80;
    exponent = (alaw >> 4) & 0x07;
    mantissa = alaw & 0x0F;

    if (exponent == 0) {
        sample = (mantissa << 4) + 8;
    } else {
        sample = ((mantissa << 4) + 0x108) << (exponent - 1);
    }

    return (int16_t)(sign ? sample : -sample);
}
