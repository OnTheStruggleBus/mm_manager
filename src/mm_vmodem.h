/*
 * Virtual (SIP) modem, part of mmumblephone.
 *
 * Presents the same interface as a Hayes-compatible serial modem
 * (AT commands, RING / CONNECT / NO CARRIER responses, DCD and DTR) to
 * the rest of mm_manager, but answers SIP calls and runs a software
 * Bell 212A modem over the call's G.711 audio instead.  This is the same
 * approach used by iaxmodem and t38modem for fax software.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#ifndef MM_VMODEM_H_
#define MM_VMODEM_H_

#include <stddef.h>
#include <stdint.h>

#include "mm_sip.h"

typedef struct mm_vmodem mm_vmodem_t;

mm_vmodem_t *vmodem_open(const sip_config_t *cfg);
void         vmodem_close(mm_vmodem_t *vm);

/* Serial-port style interface */
long         vmodem_read(mm_vmodem_t *vm, void *buf, size_t count);
long         vmodem_write(mm_vmodem_t *vm, const void *buf, size_t count);
int          vmodem_drain(mm_vmodem_t *vm);
int          vmodem_flush(mm_vmodem_t *vm);
int          vmodem_set_dtr(mm_vmodem_t *vm, int set);
int          vmodem_get_modem_status(mm_vmodem_t *vm);

#endif /* MM_VMODEM_H_ */
