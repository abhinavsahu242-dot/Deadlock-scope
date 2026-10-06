#ifndef DS_INTERCEPTOR_H
#define DS_INTERCEPTOR_H

#include "ds_common.h"

typedef int (*fn_mutex_lock)(pthread_mutex_t *);
typedef int (*fn_mutex_trylock)(pthread_mutex_t *);
typedef int (*fn_mutex_timedlock)(pthread_mutex_t *, const struct timespec *);
typedef int (*fn_mutex_unlock)(pthread_mutex_t *);
typedef int (*fn_mutex_init)(pthread_mutex_t *, const pthread_mutexattr_t *);
typedef int (*fn_mutex_destroy)(pthread_mutex_t *);
typedef int (*fn_pthread_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
typedef int (*fn_pthread_join)(pthread_t, void **);
typedef int (*fn_cond_wait)(pthread_cond_t *, pthread_mutex_t *);
typedef int (*fn_cond_timedwait)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);

extern fn_mutex_lock      real_pthread_mutex_lock;
extern fn_mutex_trylock   real_pthread_mutex_trylock;
extern fn_mutex_timedlock real_pthread_mutex_timedlock;
extern fn_mutex_unlock    real_pthread_mutex_unlock;
extern fn_mutex_init      real_pthread_mutex_init;
extern fn_mutex_destroy   real_pthread_mutex_destroy;
extern fn_pthread_create  real_pthread_create;
extern fn_pthread_join    real_pthread_join;
extern fn_cond_wait       real_pthread_cond_wait;
extern fn_cond_timedwait  real_pthread_cond_timedwait;

void ds_resolve_symbols(void);
ThreadId ds_register_thread(pthread_t pt, ThreadId parent_tid);
ThreadId ds_get_or_register_thread(void);
int ds_get_or_create_lock_index(MutexId m);
void ds_take_ownership(ThreadId tid, MutexId m, void *site);
void ds_release_ownership(ThreadId tid, MutexId m);
int ds_is_owner(ThreadId tid, MutexId m);

int ds_release_for_cond(ThreadId tid, MutexId m);
void ds_restore_after_cond(ThreadId tid, MutexId m, void *site);

#endif /* DS_INTERCEPTOR_H */
