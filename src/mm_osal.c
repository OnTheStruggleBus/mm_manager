/*
 * Minimal OS abstraction (sockets, threads, time) for the SIP virtual modem.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#include "mm_osal.h"

#ifdef _WIN32
# ifdef _MSC_VER
#  pragma comment(lib, "ws2_32.lib")
# endif
#else
# include <sys/time.h>
# include <fcntl.h>
#endif /* _WIN32 */

int osal_net_init(void) {
#ifdef _WIN32
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "%s: WSAStartup() failed: %d\n", __func__, WSAGetLastError());
        return -1;
    }
#endif /* _WIN32 */
    return 0;
}

int osal_thread_create(osal_thread_t *thread, osal_thread_fn fn, void *arg) {
#ifdef _WIN32
    *thread = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return (*thread == NULL) ? -1 : 0;
#else
    return pthread_create(thread, NULL, fn, arg);
#endif /* _WIN32 */
}

void osal_thread_join(osal_thread_t thread) {
#ifdef _WIN32
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif /* _WIN32 */
}

void osal_mutex_init(osal_mutex_t *m) {
#ifdef _WIN32
    InitializeCriticalSection(m);
#else
    pthread_mutex_init(m, NULL);
#endif /* _WIN32 */
}

void osal_mutex_destroy(osal_mutex_t *m) {
#ifdef _WIN32
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif /* _WIN32 */
}

void osal_mutex_lock(osal_mutex_t *m) {
#ifdef _WIN32
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif /* _WIN32 */
}

void osal_mutex_unlock(osal_mutex_t *m) {
#ifdef _WIN32
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif /* _WIN32 */
}

void osal_cond_init(osal_cond_t *c) {
#ifdef _WIN32
    InitializeConditionVariable(c);
#else
    pthread_cond_init(c, NULL);
#endif /* _WIN32 */
}

void osal_cond_destroy(osal_cond_t *c) {
#ifdef _WIN32
    (void)c;
#else
    pthread_cond_destroy(c);
#endif /* _WIN32 */
}

void osal_cond_broadcast(osal_cond_t *c) {
#ifdef _WIN32
    WakeAllConditionVariable(c);
#else
    pthread_cond_broadcast(c);
#endif /* _WIN32 */
}

int osal_cond_timedwait(osal_cond_t *c, osal_mutex_t *m, uint32_t timeout_ms) {
#ifdef _WIN32
    if (!SleepConditionVariableCS(c, m, timeout_ms)) {
        return 1;
    }
    return 0;
#else
    struct timeval now;
    struct timespec deadline;

    gettimeofday(&now, NULL);
    deadline.tv_sec = now.tv_sec + (time_t)(timeout_ms / 1000);
    deadline.tv_nsec = (long)now.tv_usec * 1000L + (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    return (pthread_cond_timedwait(c, m, &deadline) == ETIMEDOUT) ? 1 : 0;
#endif /* _WIN32 */
}

uint64_t osal_time_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
#endif /* _WIN32 */
}

void osal_sleep_ms(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif /* _WIN32 */
}

/* Random numbers for SIP tags, branches, Call-IDs and RTP SSRCs. */
uint32_t osal_random32(void) {
    static uint32_t state = 0;
    uint32_t x;

    if (state == 0) {
        uint32_t seed = (uint32_t)time(NULL) ^ (uint32_t)osal_time_ms() ^ (uint32_t)(uintptr_t)&x;
#ifndef _WIN32
        int fd = open("/dev/urandom", O_RDONLY);

        if (fd >= 0) {
            uint32_t r;

            if (read(fd, &r, sizeof(r)) == (ssize_t)sizeof(r)) {
                seed ^= r;
            }
            close(fd);
        }
#endif /* _WIN32 */
        state = seed ? seed : 0x12345678u;
    }

    /* xorshift32 */
    x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}
