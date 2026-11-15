/*
 * Software Bell 212A (1200 bps DPSK) modem, part of mm_manager.
 *
 * This is a pure DSP implementation of a Bell 212A modem operating on
 * 8 kHz linear PCM samples.  It allows mm_manager to talk to a Millennium
 * terminal over a SIP / RTP (G.711) call without a physical modem, in
 * the same way that software fax modems (spandsp, iaxmodem, t38modem)
 * terminate fax calls on VoIP gateways.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#ifndef MM_SOFTMODEM_H_
#define MM_SOFTMODEM_H_

#include <stdint.h>

#define SM_SAMPLE_RATE      (8000)

/* Answer tone selection */
#define SM_ANSWER_TONE_BELL (2225)  /* Bell 212A answer tone */
#define SM_ANSWER_TONE_V25  (2100)  /* ITU-T V.25 answer tone */

typedef enum sm_state {
    SM_STATE_ANSWER_SILENCE = 0,    /* Answer: billing delay silence */
    SM_STATE_ANSWER_TONE,           /* Answer: sending answer tone */
    SM_STATE_WAIT_ANSWER_TONE,      /* Originate: waiting for answer tone */
    SM_STATE_TRAINING,              /* Sending scrambled marks, waiting for far-end carrier */
    SM_STATE_DATA,                  /* Connected, data mode */
    SM_STATE_FAILED,                /* Handshake failed */
    SM_STATE_CARRIER_LOST           /* Carrier lost while in data mode */
} sm_state_t;

/* Returns the next byte to transmit, or -1 if none is available. */
typedef int  (*sm_get_byte_fn)(void *user);
/* Called for each byte received in data mode. */
typedef void (*sm_put_byte_fn)(void *user, uint8_t byte);

typedef struct softmodem softmodem_t;

softmodem_t *softmodem_create(int answer_mode, int answer_tone_hz,
                              sm_get_byte_fn get_byte, sm_put_byte_fn put_byte, void *user);
void         softmodem_free(softmodem_t *sm);

/* Process received audio samples. */
void         softmodem_rx(softmodem_t *sm, const int16_t *amp, int len);

/* Generate audio samples to transmit. */
void         softmodem_tx(softmodem_t *sm, int16_t *amp, int len);

sm_state_t   softmodem_state(const softmodem_t *sm);
const char  *softmodem_state_to_str(sm_state_t state);

/* Returns non-zero while a character is still being shifted out. */
int          softmodem_tx_busy(const softmodem_t *sm);

/* Returns non-zero if the far-end carrier is present. */
int          softmodem_carrier_detect(const softmodem_t *sm);

/* Received signal power, in dBm0 (approximately.) */
float        softmodem_rx_power_dbm0(const softmodem_t *sm);

/* G.711 codecs */
uint8_t      sm_linear_to_ulaw(int16_t pcm);
int16_t      sm_ulaw_to_linear(uint8_t ulaw);
uint8_t      sm_linear_to_alaw(int16_t pcm);
int16_t      sm_alaw_to_linear(uint8_t alaw);

#endif /* MM_SOFTMODEM_H_ */
