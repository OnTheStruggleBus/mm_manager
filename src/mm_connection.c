/*
 * This is a "Manager" for the Nortel Millennium payhone.
 *
 * It can provision a Nortel Millennium payphone with Rev 1.0 or 1.3
 * Control PCP.  CDRs, Alarms, and Maintenance Reports can also be
 * retieved.
 *
 * www.github.com/hharte/mm_manager
 *
 * Copyright (c) 2020-2023, Howard M. Harte
 */

#include <stdio.h>   /* Standard input/output definitions */
#include <stdint.h>
#include <inttypes.h>
#include <errno.h> /* Error number definitions */
#include <time.h>  /* time_t, struct tm, time, gmtime */

#include "mm_manager.h"
#include "mm_serial.h"
#include "mm_udp.h"
#include "mm_sip.h"
#include "mm_vmodem.h"

extern int manager_running;
extern const char* modem_responses[];
extern time_t mm_time(int test_mode, time_t* rawtime);

int mm_connection_open(mm_connection_t* connection, const char *modem_dev, int baudrate, int test_mode) {
    int   status;

    connection->test_mode = test_mode;
    if (connection->sip_config != NULL) {
        /* SIP virtual modem: a software modem on a SIP call takes the place of a physical modem. */
        struct mm_vmodem *vmodem;

        connection->bytestream = NULL;
        vmodem = vmodem_open(connection->sip_config);
        if (vmodem == NULL) {
            fprintf(stderr, "Unable to start SIP virtual modem.\n");
            mm_connection_close(connection);
            return(-ENODEV);
        }
        connection->proto.serial_context = open_virtual_serial(vmodem, connection->logstream);
    }
    else if (test_mode) {
        if (modem_dev == NULL) {
            (void)fprintf(stderr, "mm_manager: -f <filename> must be specified.\n");
            mm_connection_close(connection);
            return(-EINVAL);
        }

        if (!(connection->bytestream = fopen(modem_dev, "r"))) {
            fprintf(stderr, "Error opening input stream: %s\n", modem_dev);
            mm_connection_close(connection);
            return(-EPERM);
        }
    }
    else {
        connection->bytestream = NULL;
        if (modem_dev == NULL) {
            (void)fprintf(stderr, "mm_manager: -f <modem_dev> must be specified.\n");
            mm_connection_close(connection);
            return(-EINVAL);
        }
    }

    if (connection->sip_config == NULL) {
        connection->proto.serial_context = open_serial(modem_dev, connection->logstream, connection->bytestream);
    }

    if (connection->proto.serial_context == NULL) {
        fprintf(stderr, "Unable to open modem: %s.", modem_dev);
        mm_connection_close(connection);
        return(-ENODEV);
    }

    init_serial(connection->proto.serial_context, baudrate);
    status = init_modem(connection->proto.serial_context, connection->modem_reset_string, connection->modem_init_string);

    if (status == 0) {
        printf("Modem initialized.\n");
    }
    else {
        fprintf(stderr, "Error initializing modem.\n");
        mm_connection_close(connection);
        return(-EIO);
    }
    return (0);
}

int mm_connection_wait(mm_connection_t* connection)
{
    int   modem_response = 0;

    time_t rawtime;
    struct tm ptm = { 0 };

    while (manager_running) {
        modem_response = wait_for_modem_response(connection->proto.serial_context, 1);

        mm_time(connection->test_mode, &rawtime);
        localtime_r(&rawtime, &ptm);

        switch (modem_response) {
        case MODEM_RSP_OK:
            break;
        case MODEM_RSP_RING:
            printf("%04d-%02d-%02d %2d:%02d:%02d: Ringing...\n\n",
                ptm.tm_year + 1900, ptm.tm_mon + 1, ptm.tm_mday, ptm.tm_hour, ptm.tm_min, ptm.tm_sec);
            continue;
        case MODEM_RSP_CONNECT:
            printf("%04d-%02d-%02d %2d:%02d:%02d: Connected!\n\n",
                ptm.tm_year + 1900, ptm.tm_mon + 1, ptm.tm_mday, ptm.tm_hour, ptm.tm_min, ptm.tm_sec);

            proto_connect(&connection->proto);
            break;
        case MODEM_RSP_NO_CARRIER:
            proto_disconnect(&connection->proto);
            printf("%04d-%02d-%02d %2d:%02d:%02d: Carrier lost.\n\n",
                ptm.tm_year + 1900, ptm.tm_mon + 1, ptm.tm_mday, ptm.tm_hour, ptm.tm_min, ptm.tm_sec);

            continue;
        case MODEM_RSP_NULL:
            break;
        case MODEM_RSP_READ_ERROR:
            manager_running = 0;
            printf("%04d-%02d-%02d %2d:%02d:%02d: Error communicating with modem, shutting down.\n\n",
                ptm.tm_year + 1900, ptm.tm_mon + 1, ptm.tm_mday, ptm.tm_hour, ptm.tm_min, ptm.tm_sec);
                break;
        default:
            printf("%04d-%02d-%02d %2d:%02d:%02d: Unhandled modem response = %d (%s)\n\n",
                ptm.tm_year + 1900, ptm.tm_mon + 1, ptm.tm_mday, ptm.tm_hour, ptm.tm_min, ptm.tm_sec,
                modem_response, modem_response <= MODEM_RSP_NULL ? modem_responses[modem_response] : "Unknown");
            continue;
        }
        break;
    }

    return (connection->proto.connected);
}

int mm_connection_close(mm_connection_t* connection) {
    close_serial(connection->proto.serial_context);
    connection->proto.serial_context = NULL;

    if (connection->bytestream) {
        fclose(connection->bytestream);
        connection->bytestream = NULL;
    }

    if (connection->logstream) {
        fclose(connection->logstream);
        connection->logstream = NULL;
    }

    if (connection->proto.pcapstream) {
        mm_close_pcap(connection->proto.pcapstream);
        connection->proto.pcapstream = NULL;
    }

    if (connection->proto.send_udp) {
        mm_close_udp();
        connection->proto.send_udp = 0;
    }

    return (0);
}
