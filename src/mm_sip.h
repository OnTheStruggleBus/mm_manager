/*
 * Minimal SIP user agent with G.711 RTP, for the virtual modem.
 *
 * Supports registration (with digest authentication) to a SIP registrar
 * or PBX, and answering incoming calls, so that a Millennium terminal
 * connected to an ATA (or any phone line that reaches the PBX) can call
 * the SIP number that mmumblephone is registered on.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#ifndef MM_SIP_H_
#define MM_SIP_H_

#include <stdint.h>

#define SIP_DEFAULT_PORT        (5060)
#define SIP_DEFAULT_RTP_PORT    (10000)
#define SIP_DEFAULT_EXPIRES     (300)

#define RTP_PT_PCMU             (0)
#define RTP_PT_PCMA             (8)

typedef struct sip_config {
    char     user[64];          /* SIP user / extension */
    char     auth_user[64];     /* Authentication user name, defaults to user */
    char     password[128];
    char     registrar[256];    /* Registrar host, empty for no registration */
    uint16_t registrar_port;
    char     domain[256];       /* SIP domain, defaults to registrar */
    char     bind_ip[64];       /* Local IPv4 address the SIP and RTP sockets bind to (default 127.0.0.1) */
    char     allow_ip[64];      /* Only accept SIP and RTP from this IPv4 address (default: the registrar) */
    char     local_ip[64];      /* IP address to advertise, defaults to bind_ip */
    uint16_t local_port;        /* Local SIP UDP port */
    uint16_t rtp_port;          /* Local RTP UDP port */
    int      expires;           /* Registration expiry, in seconds */
    int      codec;             /* Preferred codec: RTP_PT_PCMU or RTP_PT_PCMA */
    int      answer_tone;       /* Answer tone frequency, Hz */
    int      debug;
} sip_config_t;

typedef struct sip_callbacks {
    /* A call is being offered (ringing.) */
    void (*incoming_call)(void *user, const char *from);
    /* The call was ended by the far end, or failed. */
    void (*call_ended)(void *user, const char *reason);
    /* Received audio, decoded to 16-bit linear PCM at 8 kHz. */
    void (*rx_audio)(void *user, const int16_t *amp, int len);
} sip_callbacks_t;

typedef struct sip_ua sip_ua_t;

/*
 * Parse a SIP configuration string:
 *   user[:password]@registrar[:port][;option=value]...
 *   listen[;option=value]...      (no registration, accept direct calls)
 * Options: bind, allow, port, rtp_port, expires, auth_user, domain, local_ip,
 *          codec (pcmu|pcma), answer_tone (2225|2100)
 *
 * The line carries card numbers and PINs, so the sockets never bind the
 * wildcard address: bind defaults to 127.0.0.1, and every SIP and RTP
 * packet from an address other than allow (or the registrar) is dropped.
 */
int        sip_config_parse(sip_config_t *cfg, const char *spec);

sip_ua_t  *sip_ua_create(const sip_config_t *cfg, const sip_callbacks_t *cb, void *user);
void       sip_ua_destroy(sip_ua_t *ua);

/* Process network traffic and timers, waiting at most timeout_ms. */
void       sip_ua_poll(sip_ua_t *ua, uint32_t timeout_ms);

/* Answer the incoming call. */
int        sip_ua_answer(sip_ua_t *ua);

/* Hang up (or reject) the current call. */
int        sip_ua_hangup(sip_ua_t *ua);

/* Returns non-zero when a call is answered and media is flowing. */
int        sip_ua_media_active(const sip_ua_t *ua);

/* Send one packet of audio (normally 160 samples / 20 ms.) */
int        sip_ua_send_audio(sip_ua_t *ua, const int16_t *amp, int len);

/* Returns non-zero if registered (or if registration is not configured.) */
int        sip_ua_registered(const sip_ua_t *ua);

/* MD5 digest of a string as 32 hex digits (used for digest authentication.) */
void       sip_md5_hex(const char *str, char out[33]);

#endif /* MM_SIP_H_ */
