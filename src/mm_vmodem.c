/*
 * Virtual (SIP) modem, part of mmumblephone.
 *
 * A worker thread runs the SIP user agent and, once a call is answered,
 * a software Bell 212A modem over the call's audio, generating one RTP
 * packet every 20 ms.  The host side (mm_manager's serial layer) sees a
 * Hayes-style modem: AT commands are answered with OK, an incoming call
 * produces RING, auto-answer (S0) answers it, CONNECT is reported once
 * the modems have trained, and NO CARRIER when the call ends.  Dropping
 * DTR hangs up the call.
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
#include <ctype.h>

#include "mm_osal.h"
#include "mm_sip.h"
#include "mm_softmodem.h"
#include "mm_vmodem.h"

#define VM_FIFO_SIZE        (65536)
#define VM_PACKET_MS        (20)
#define VM_PACKET_SAMPLES   (SM_SAMPLE_RATE * VM_PACKET_MS / 1000)
#define VM_READ_TIMEOUT_MS  (1000)      /* Like VTIME=10 on a real serial port */
#define VM_RING_INTERVAL_MS (6000)
#define VM_DEFAULT_S0       (1)         /* Auto-answer on the first ring by default */
#define VM_RX_GAP_MS        (200)       /* No RTP for this long: feed silence to the modem */

/* Modem status bits, as returned by serial_get_modem_status() */
#define VM_MS_RING_ON       (0x0040)
#define VM_MS_RLSD_ON       (0x0080)

typedef struct vm_fifo {
    uint8_t data[VM_FIFO_SIZE];
    size_t  head;
    size_t  tail;
} vm_fifo_t;

struct mm_vmodem {
    sip_config_t   cfg;
    sip_ua_t      *ua;
    softmodem_t   *sm;
    sm_state_t     sm_state;

    osal_thread_t  thread;
    volatile int   running;
    osal_mutex_t   lock;
    osal_cond_t    cond;

    vm_fifo_t      rx_fifo;         /* Modem -> host (responses and received data) */
    vm_fifo_t      tx_fifo;         /* Host -> modem (data to transmit) */

    /* Command mode */
    char           cmd_buf[256];
    size_t         cmd_len;
    int            s0;              /* Rings before auto-answer, 0 = don't answer */

    /* Call state, protected by lock */
    int            ringing;
    int            ring_count;
    uint64_t       next_ring;
    int            answer_req;
    int            hangup_req;
    int            online;          /* Data mode */
    int            dcd;
    uint64_t       last_rx_audio;   /* Time RTP audio was last received */
};

static size_t fifo_count(const vm_fifo_t *f) {
    return (f->head - f->tail) & (VM_FIFO_SIZE - 1);
}

static int fifo_put(vm_fifo_t *f, uint8_t byte) {
    size_t next = (f->head + 1) & (VM_FIFO_SIZE - 1);

    if (next == f->tail) return -1;
    f->data[f->head] = byte;
    f->head = next;
    return 0;
}

static int fifo_get(vm_fifo_t *f) {
    int byte;

    if (f->head == f->tail) return -1;
    byte = f->data[f->tail];
    f->tail = (f->tail + 1) & (VM_FIFO_SIZE - 1);
    return byte;
}

static void fifo_clear(vm_fifo_t *f) {
    f->head = 0;
    f->tail = 0;
}

/* Queue a result code to the host, lock must be held. */
static void vm_respond(mm_vmodem_t *vm, const char *str) {
    const char *p;

    fifo_put(&vm->rx_fifo, '\r');
    fifo_put(&vm->rx_fifo, '\n');
    for (p = str; *p; p++) {
        fifo_put(&vm->rx_fifo, (uint8_t)*p);
    }
    fifo_put(&vm->rx_fifo, '\r');
    fifo_put(&vm->rx_fifo, '\n');
    osal_cond_broadcast(&vm->cond);
}

/* Tear down the software modem and return to command mode, lock must be held. */
static void vm_call_down(mm_vmodem_t *vm, int report) {
    if (report && (vm->ringing || vm->online || (vm->sm != NULL))) {
        vm_respond(vm, "NO CARRIER");
    }

    if (vm->sm != NULL) {
        softmodem_free(vm->sm);
        vm->sm = NULL;
    }

    vm->ringing = 0;
    vm->online = 0;
    vm->dcd = 0;
    fifo_clear(&vm->tx_fifo);
    osal_cond_broadcast(&vm->cond);
}

/*
 * Track software modem state changes, lock must be held.  The modem may
 * only be torn down when not called from within one of its own callbacks.
 */
static void vm_check_modem_state(mm_vmodem_t *vm, int allow_teardown) {
    sm_state_t state;

    if (vm->sm == NULL) return;

    state = softmodem_state(vm->sm);
    if (state == vm->sm_state) return;
    if (!allow_teardown && (state != SM_STATE_DATA)) return;

    vm->sm_state = state;
    printf("Virtual modem: %s (rx level %.1f dBm0)\n", softmodem_state_to_str(state), softmodem_rx_power_dbm0(vm->sm));

    switch (state) {
    case SM_STATE_DATA:
        vm->online = 1;
        vm->dcd = 1;
        fifo_clear(&vm->tx_fifo);
        vm_respond(vm, "CONNECT 1200");
        break;
    case SM_STATE_FAILED:
    case SM_STATE_CARRIER_LOST:
        vm_call_down(vm, 1);
        vm->hangup_req = 1;
        break;
    default:
        break;
    }
}

/* Software modem callbacks, called with lock held. */
static int vm_sm_get_byte(void *user) {
    mm_vmodem_t *vm = (mm_vmodem_t *)user;
    int byte;

    if (!vm->online) return -1;

    byte = fifo_get(&vm->tx_fifo);
    if (byte >= 0) osal_cond_broadcast(&vm->cond);
    return byte;
}

static void vm_sm_put_byte(void *user, uint8_t byte) {
    mm_vmodem_t *vm = (mm_vmodem_t *)user;

    /* Make sure CONNECT is reported before any data. */
    vm_check_modem_state(vm, 0);

    if (vm->online) {
        fifo_put(&vm->rx_fifo, byte);
        osal_cond_broadcast(&vm->cond);
    }
}

/* SIP callbacks, called from the worker thread without lock held. */
static void vm_incoming_call(void *user, const char *from) {
    mm_vmodem_t *vm = (mm_vmodem_t *)user;

    (void)from;
    osal_mutex_lock(&vm->lock);
    vm->ringing = 1;
    vm->ring_count = 0;
    vm->next_ring = osal_time_ms();
    osal_mutex_unlock(&vm->lock);
}

static void vm_call_ended(void *user, const char *reason) {
    mm_vmodem_t *vm = (mm_vmodem_t *)user;

    (void)reason;
    osal_mutex_lock(&vm->lock);
    vm_call_down(vm, 1);
    vm->answer_req = 0;
    osal_mutex_unlock(&vm->lock);
}

static void vm_rx_audio(void *user, const int16_t *amp, int len) {
    mm_vmodem_t *vm = (mm_vmodem_t *)user;

    osal_mutex_lock(&vm->lock);
    vm->last_rx_audio = osal_time_ms();
    if (vm->sm != NULL) {
        softmodem_rx(vm->sm, amp, len);
        vm_check_modem_state(vm, 1);
    }
    osal_mutex_unlock(&vm->lock);
}

static osal_thread_ret_t OSAL_THREAD_CALL vm_thread(void *arg) {
    mm_vmodem_t *vm = (mm_vmodem_t *)arg;
    uint64_t next_tick = osal_time_ms();

    while (vm->running) {
        uint64_t now = osal_time_ms();
        uint32_t wait = (next_tick > now) ? (uint32_t)(next_tick - now) : 0;
        int do_answer = 0;
        int do_hangup = 0;

        if (wait > VM_PACKET_MS) wait = VM_PACKET_MS;
        sip_ua_poll(vm->ua, wait);

        now = osal_time_ms();

        /* Ringing and auto-answer, hangup requests from the host. */
        osal_mutex_lock(&vm->lock);
        if (vm->ringing && (vm->sm == NULL) && (now >= vm->next_ring)) {
            vm_respond(vm, "RING");
            vm->ring_count++;
            vm->next_ring = now + VM_RING_INTERVAL_MS;
            if ((vm->s0 > 0) && (vm->ring_count >= vm->s0)) {
                vm->answer_req = 1;
            }
        }
        if (vm->answer_req) {
            vm->answer_req = 0;
            if (vm->ringing && (vm->sm == NULL)) {
                do_answer = 1;
            }
        }
        if (vm->hangup_req) {
            vm->hangup_req = 0;
            do_hangup = 1;
        }
        osal_mutex_unlock(&vm->lock);

        if (do_hangup) {
            sip_ua_hangup(vm->ua);
        } else if (do_answer) {
            if (sip_ua_answer(vm->ua) == 0) {
                osal_mutex_lock(&vm->lock);
                vm->ringing = 0;
                vm->sm = softmodem_create(1, vm->cfg.answer_tone, vm_sm_get_byte, vm_sm_put_byte, vm);
                vm->sm_state = softmodem_state(vm->sm);
                vm->last_rx_audio = now;
                printf("Virtual modem: answering, %s.\n", softmodem_state_to_str(vm->sm_state));
                osal_mutex_unlock(&vm->lock);
                next_tick = now;
            }
        }

        /* Transmit one packet of audio every 20 ms. */
        now = osal_time_ms();
        while (now >= next_tick) {
            int16_t amp[VM_PACKET_SAMPLES];
            int have_audio = 0;

            osal_mutex_lock(&vm->lock);
            if ((vm->sm != NULL) && sip_ua_media_active(vm->ua)) {
                softmodem_tx(vm->sm, amp, VM_PACKET_SAMPLES);
                vm_check_modem_state(vm, 1);
                have_audio = 1;
            }

            /*
             * If no audio is arriving (far end vanished without a BYE, or
             * one-way audio), feed the modem silence so that its handshake
             * timeout and carrier loss detection still work.
             */
            if ((vm->sm != NULL) && (now - vm->last_rx_audio > VM_RX_GAP_MS)) {
                int16_t silence[VM_PACKET_SAMPLES];

                memset(silence, 0, sizeof(silence));
                softmodem_rx(vm->sm, silence, VM_PACKET_SAMPLES);
                vm_check_modem_state(vm, 1);
            }
            osal_mutex_unlock(&vm->lock);

            if (have_audio) {
                sip_ua_send_audio(vm->ua, amp, VM_PACKET_SAMPLES);
            }

            next_tick += VM_PACKET_MS;
            if (now > next_tick + 200) {
                /* Fell too far behind (system was suspended?): resynchronize. */
                next_tick = now + VM_PACKET_MS;
            }
        }
    }

    return (osal_thread_ret_t)0;
}

mm_vmodem_t *vmodem_open(const sip_config_t *cfg) {
    mm_vmodem_t *vm;
    sip_callbacks_t cb;

    vm = (mm_vmodem_t *)calloc(1, sizeof(mm_vmodem_t));
    if (vm == NULL) return NULL;

    vm->cfg = *cfg;
    vm->s0 = VM_DEFAULT_S0;
    osal_mutex_init(&vm->lock);
    osal_cond_init(&vm->cond);

    cb.incoming_call = vm_incoming_call;
    cb.call_ended = vm_call_ended;
    cb.rx_audio = vm_rx_audio;

    vm->ua = sip_ua_create(cfg, &cb, vm);
    if (vm->ua == NULL) {
        osal_cond_destroy(&vm->cond);
        osal_mutex_destroy(&vm->lock);
        free(vm);
        return NULL;
    }

    vm->running = 1;
    if (osal_thread_create(&vm->thread, vm_thread, vm) != 0) {
        fprintf(stderr, "Virtual modem: unable to create thread.\n");
        sip_ua_destroy(vm->ua);
        osal_cond_destroy(&vm->cond);
        osal_mutex_destroy(&vm->lock);
        free(vm);
        return NULL;
    }

    return vm;
}

void vmodem_close(mm_vmodem_t *vm) {
    if (vm == NULL) return;

    vm->running = 0;
    osal_thread_join(vm->thread);

    sip_ua_destroy(vm->ua);
    if (vm->sm != NULL) softmodem_free(vm->sm);
    osal_cond_destroy(&vm->cond);
    osal_mutex_destroy(&vm->lock);
    free(vm);
}

/* Execute an AT command line, lock must be held. */
static void vm_at_command(mm_vmodem_t *vm, const char *line) {
    const char *p;
    const char *result = "OK";

    while (isspace((unsigned char)*line)) line++;

    if ((toupper((unsigned char)line[0]) != 'A') || (toupper((unsigned char)line[1]) != 'T')) {
        return;     /* Not a command, ignore. */
    }

    p = line + 2;
    while (*p != '\0') {
        char c = (char)toupper((unsigned char)*p++);

        switch (c) {
        case ' ':
            continue;
        case 'S': {
            int reg = 0;

            while (isdigit((unsigned char)*p)) reg = reg * 10 + (*p++ - '0');
            if (*p == '=') {
                int value = 0;

                p++;
                while (isdigit((unsigned char)*p)) value = value * 10 + (*p++ - '0');
                if (reg == 0) vm->s0 = value;
            } else if (*p == '?') {
                char buf[16];

                p++;
                snprintf(buf, sizeof(buf), "%03d", (reg == 0) ? vm->s0 : 0);
                vm_respond(vm, buf);
            }
            continue;
        }
        case '&':
        case '%':
        case '\\':
            if (*p) p++;
            break;
        case '+':
            /* Extended commands (e.g. +MS=B212) take the rest of the line. */
            p += strlen(p);
            continue;
        case 'Z':
            vm->s0 = VM_DEFAULT_S0;
            break;
        case 'A':
            if (vm->ringing) {
                vm->answer_req = 1;
                return;     /* Result code will be CONNECT or NO CARRIER */
            }
            result = "NO CARRIER";
            break;
        case 'H':
            if (vm->ringing || (vm->sm != NULL)) {
                vm_call_down(vm, 0);
                vm->hangup_req = 1;
            }
            break;
        case 'D':
            /* Dialing out is not supported: the terminal always calls the Manager. */
            vm_respond(vm, "NO DIALTONE");
            return;
        case 'O':
            if (vm->sm_state == SM_STATE_DATA && (vm->sm != NULL)) {
                vm->online = 1;
                vm_respond(vm, "CONNECT 1200");
                return;
            }
            result = "NO CARRIER";
            break;
        case 'I':
            vm_respond(vm, "mmumblephone SIP virtual modem (Bell 212A), a fork of mm_manager");
            break;
        default:
            break;
        }

        /* Skip any parameters: digits, '=' and '?' */
        while ((*p == '=') || (*p == '?') || isdigit((unsigned char)*p)) p++;
    }

    vm_respond(vm, result);
}

long vmodem_read(mm_vmodem_t *vm, void *buf, size_t count) {
    uint64_t deadline = osal_time_ms() + VM_READ_TIMEOUT_MS;
    size_t n = 0;

    osal_mutex_lock(&vm->lock);
    while (fifo_count(&vm->rx_fifo) == 0) {
        uint64_t now = osal_time_ms();

        if (now >= deadline) break;
        osal_cond_timedwait(&vm->cond, &vm->lock, (uint32_t)(deadline - now));
    }

    while (n < count) {
        int byte = fifo_get(&vm->rx_fifo);

        if (byte < 0) break;
        ((uint8_t *)buf)[n++] = (uint8_t)byte;
    }
    osal_mutex_unlock(&vm->lock);

    return (long)n;
}

long vmodem_write(mm_vmodem_t *vm, const void *buf, size_t count) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t i;

    osal_mutex_lock(&vm->lock);
    for (i = 0; i < count; i++) {
        if (vm->online) {
            while (fifo_put(&vm->tx_fifo, p[i]) != 0) {
                if (!vm->online) break;
                osal_cond_timedwait(&vm->cond, &vm->lock, 100);
            }
        } else if ((p[i] == '\r') || (p[i] == '\n')) {
            vm->cmd_buf[vm->cmd_len] = '\0';
            if (vm->cmd_len > 0) vm_at_command(vm, vm->cmd_buf);
            vm->cmd_len = 0;
        } else if (vm->cmd_len < sizeof(vm->cmd_buf) - 1) {
            vm->cmd_buf[vm->cmd_len++] = (char)p[i];
        }
    }
    osal_mutex_unlock(&vm->lock);

    return (long)count;
}

int vmodem_drain(mm_vmodem_t *vm) {
    /* Allow plenty of time: 1200 bps is ~120 characters per second. */
    uint64_t deadline;

    osal_mutex_lock(&vm->lock);
    deadline = osal_time_ms() + 2000 + fifo_count(&vm->tx_fifo) * 10;
    while (vm->online && ((fifo_count(&vm->tx_fifo) > 0) || ((vm->sm != NULL) && softmodem_tx_busy(vm->sm)))) {
        uint64_t now = osal_time_ms();

        if (now >= deadline) break;
        osal_cond_timedwait(&vm->cond, &vm->lock, 20);
    }
    osal_mutex_unlock(&vm->lock);
    return 0;
}

int vmodem_flush(mm_vmodem_t *vm) {
    osal_mutex_lock(&vm->lock);
    fifo_clear(&vm->rx_fifo);
    fifo_clear(&vm->tx_fifo);
    vm->cmd_len = 0;
    osal_mutex_unlock(&vm->lock);
    return 0;
}

int vmodem_set_dtr(mm_vmodem_t *vm, int set) {
    if (!set) {
        /* Like AT&D2: dropping DTR hangs up and returns to command mode. */
        osal_mutex_lock(&vm->lock);
        if (vm->ringing || (vm->sm != NULL) || vm->online) {
            vm_call_down(vm, 0);
            vm->hangup_req = 1;
        }
        fifo_clear(&vm->rx_fifo);
        osal_mutex_unlock(&vm->lock);
    }
    return 0;
}

int vmodem_get_modem_status(mm_vmodem_t *vm) {
    int status = 0;

    osal_mutex_lock(&vm->lock);
    if (vm->dcd) status |= VM_MS_RLSD_ON;
    if (vm->ringing) status |= VM_MS_RING_ON;
    osal_mutex_unlock(&vm->lock);

    return status;
}
