/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef DAV1D_NDLESS_THREAD_H
#define DAV1D_NDLESS_THREAD_H
/* The player calls decoder instances serially. dav1d_open rejects worker
 * threads/frame parallelism; unexpected waits fail instead of spinning. */
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
typedef unsigned Dav1dSerialMutex;
typedef unsigned Dav1dSerialCond;
typedef unsigned Dav1dSerialOnce;
typedef uintptr_t Dav1dSerialThread;
typedef struct {size_t stack_size;} Dav1dSerialAttr;
#define pthread_mutex_t Dav1dSerialMutex
#define pthread_cond_t Dav1dSerialCond
#define pthread_once_t Dav1dSerialOnce
#define pthread_t Dav1dSerialThread
#define pthread_attr_t Dav1dSerialAttr
#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_ONCE_INIT 0
#define dav1d_init_thread() ((void)0)
#define dav1d_set_thread_name(name) ((void)0)
static inline int pthread_attr_init(pthread_attr_t *p){p->stack_size=0;return 0;}
static inline int pthread_attr_destroy(pthread_attr_t *p){(void)p;return 0;}
static inline int pthread_attr_setstacksize(pthread_attr_t *p,size_t n){p->stack_size=n;return 0;}
static inline int pthread_mutex_init(pthread_mutex_t *p,const void *a){(void)a;*p=0;return 0;}
static inline int pthread_mutex_destroy(pthread_mutex_t *p){(void)p;return 0;}
static inline int pthread_mutex_lock(pthread_mutex_t *p){(void)p;return 0;}
static inline int pthread_mutex_unlock(pthread_mutex_t *p){(void)p;return 0;}
static inline int pthread_cond_init(pthread_cond_t *p,const void *a){(void)a;*p=0;return 0;}
static inline int pthread_cond_destroy(pthread_cond_t *p){(void)p;return 0;}
static inline int pthread_cond_wait(pthread_cond_t *p,pthread_mutex_t *m){(void)p;(void)m;abort();return EINVAL;}
static inline int pthread_cond_signal(pthread_cond_t *p){(void)p;return 0;}
static inline int pthread_cond_broadcast(pthread_cond_t *p){(void)p;return 0;}
static inline int pthread_once(pthread_once_t *p,void (*f)(void)){if(!*p){*p=1;f();}return 0;}
static inline int pthread_create(pthread_t *p,const pthread_attr_t *a,void *(*f)(void*),void *v){(void)p;(void)a;(void)f;(void)v;return ENOSYS;}
static inline int pthread_join(pthread_t p,void **r){(void)p;(void)r;return ENOSYS;}
#endif
