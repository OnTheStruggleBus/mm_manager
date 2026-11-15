/*
 * Minimal OS abstraction (sockets, threads, time) for the SIP virtual modem.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#ifndef MM_OSAL_H_
#define MM_OSAL_H_

#include <stdint.h>

#ifdef _WIN32
# if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
#  undef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600   /* Vista or later: condition variables, GetTickCount64 */
# endif
# ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#  define _WINSOCK_DEPRECATED_NO_WARNINGS
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
# include <windows.h>

typedef SOCKET              osal_socket_t;
typedef int                 osal_socklen_t;
# define OSAL_INVALID_SOCKET INVALID_SOCKET
# define osal_close_socket  closesocket

typedef CRITICAL_SECTION    osal_mutex_t;
typedef CONDITION_VARIABLE  osal_cond_t;
typedef HANDLE              osal_thread_t;
typedef DWORD               osal_thread_ret_t;
# define OSAL_THREAD_CALL   WINAPI

#else  /* POSIX */
# include <sys/types.h>
# include <sys/socket.h>
# include <sys/select.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>
# include <unistd.h>
# include <pthread.h>

typedef int                 osal_socket_t;
typedef socklen_t           osal_socklen_t;
# define OSAL_INVALID_SOCKET (-1)
# define osal_close_socket  close

typedef pthread_mutex_t     osal_mutex_t;
typedef pthread_cond_t      osal_cond_t;
typedef pthread_t           osal_thread_t;
typedef void *              osal_thread_ret_t;
# define OSAL_THREAD_CALL
#endif /* _WIN32 */

typedef osal_thread_ret_t (OSAL_THREAD_CALL *osal_thread_fn)(void *arg);

int      osal_net_init(void);
int      osal_thread_create(osal_thread_t *thread, osal_thread_fn fn, void *arg);
void     osal_thread_join(osal_thread_t thread);
void     osal_mutex_init(osal_mutex_t *m);
void     osal_mutex_destroy(osal_mutex_t *m);
void     osal_mutex_lock(osal_mutex_t *m);
void     osal_mutex_unlock(osal_mutex_t *m);
void     osal_cond_init(osal_cond_t *c);
void     osal_cond_destroy(osal_cond_t *c);
void     osal_cond_broadcast(osal_cond_t *c);
/* Wait on condition with timeout, mutex must be held.  Returns 0 if signalled, 1 on timeout. */
int      osal_cond_timedwait(osal_cond_t *c, osal_mutex_t *m, uint32_t timeout_ms);
uint64_t osal_time_ms(void);
void     osal_sleep_ms(uint32_t ms);
uint32_t osal_random32(void);

#endif /* MM_OSAL_H_ */
