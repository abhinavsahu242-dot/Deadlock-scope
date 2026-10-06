#include "ds_common.h"
#include "ds_interceptor.h"
#include "ds_lock_graph.h"
#include "ds_wait_graph.h"
#include "ds_stress.h"

fn_mutex_lock      real_pthread_mutex_lock = NULL;
fn_mutex_trylock   real_pthread_mutex_trylock = NULL;
fn_mutex_timedlock real_pthread_mutex_timedlock = NULL;
fn_mutex_unlock    real_pthread_mutex_unlock = NULL;
fn_mutex_init      real_pthread_mutex_init = NULL;
fn_mutex_destroy   real_pthread_mutex_destroy = NULL;
fn_pthread_create  real_pthread_create = NULL;
fn_pthread_join    real_pthread_join = NULL;
fn_cond_wait       real_pthread_cond_wait = NULL;
fn_cond_timedwait  real_pthread_cond_timedwait = NULL;

atomic_int g_thread_counter = 1;

void ds_resolve_symbols(void) {
    if (real_pthread_mutex_lock) return;

    real_pthread_mutex_lock = (fn_mutex_lock)dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_pthread_mutex_trylock = (fn_mutex_trylock)dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_pthread_mutex_timedlock = (fn_mutex_timedlock)dlsym(RTLD_NEXT, "pthread_mutex_timedlock");
    real_pthread_mutex_unlock = (fn_mutex_unlock)dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    real_pthread_mutex_init = (fn_mutex_init)dlsym(RTLD_NEXT, "pthread_mutex_init");
    real_pthread_mutex_destroy = (fn_mutex_destroy)dlsym(RTLD_NEXT, "pthread_mutex_destroy");
    real_pthread_create = (fn_pthread_create)dlsym(RTLD_NEXT, "pthread_create");
    real_pthread_join = (fn_pthread_join)dlsym(RTLD_NEXT, "pthread_join");
    real_pthread_cond_wait = (fn_cond_wait)dlsym(RTLD_NEXT, "pthread_cond_wait");
    real_pthread_cond_timedwait = (fn_cond_timedwait)dlsym(RTLD_NEXT, "pthread_cond_timedwait");
}

ThreadId ds_register_thread(pthread_t pt, ThreadId parent_tid) {
    int idx = atomic_fetch_add(&g_thread_counter, 1);
    if (idx >= MAX_THREADS) {
        idx = MAX_THREADS - 1; /* clamp */
    }
    ThreadId tid = (ThreadId)idx;
    g_threads[tid].tid = tid;
    g_threads[tid].pthread_id = pt;
    atomic_store(&g_threads[tid].active, 1);
    atomic_store(&g_threads[tid].is_waiting, 0);
    atomic_store(&g_threads[tid].wait_kind, WK_NONE);
    atomic_store(&g_threads[tid].waiting_for, 0);
    g_threads[tid].held.count = 0;
    g_threads[tid].parent_tid = parent_tid;
    g_threads[tid].spawn_seq = atomic_fetch_add(&g_event_seq, 1);
    g_threads[tid].join_seq = 0;
    return tid;
}

ThreadId ds_get_or_register_thread(void) {
    if (ds_tid_initialized) return ds_my_tid;

    ds_resolve_symbols();
    ds_my_tid = ds_register_thread(pthread_self(), 0);
    ds_tid_initialized = 1;
    return ds_my_tid;
}

int ds_get_or_create_lock_index(MutexId m) {
    for (int i = 0; i < g_lock_count; i++) {
        if (g_lock_table[i] == m) return i;
    }
    if (g_lock_count < MAX_LOCKS) {
        int idx = g_lock_count++;
        g_lock_table[idx] = m;
        atomic_store(&g_lock_owners[idx], 0);
        return idx;
    }
    return -1;
}

void ds_take_ownership(ThreadId tid, MutexId m, void *site) {
    ds_spin_lock();
    int idx = ds_get_or_create_lock_index(m);
    if (idx >= 0) {
        atomic_store(&g_lock_owners[idx], (uintptr_t)tid + 1);
    }
    HeldLockSet *held = &g_threads[tid].held;
    for (int i = 0; i < held->count; i++) {
        if (held->locks[i] == m) {
            ds_spin_unlock();
            return; /* already held (e.g. recursive mutex) */
        }
    }
    if (held->count < MAX_HELD_LOCKS) {
        held->locks[held->count] = m;
        held->sites[held->count] = site;
        held->count++;
    }
    ds_spin_unlock();
}

void ds_release_ownership(ThreadId tid, MutexId m) {
    ds_spin_lock();
    int idx = ds_get_or_create_lock_index(m);
    if (idx >= 0) {
        if (atomic_load(&g_lock_owners[idx]) == (uintptr_t)tid + 1) {
            atomic_store(&g_lock_owners[idx], 0);
        }
    }
    HeldLockSet *held = &g_threads[tid].held;
    for (int i = 0; i < held->count; i++) {
        if (held->locks[i] == m) {
            for (int j = i; j < held->count - 1; j++) {
                held->locks[j] = held->locks[j + 1];
                held->sites[j] = held->sites[j + 1];
            }
            held->count--;
            break;
        }
    }
    ds_spin_unlock();
}

int ds_release_for_cond(ThreadId tid, MutexId m) {
    ds_spin_lock();
    int idx = ds_get_or_create_lock_index(m);
    if (idx >= 0) {
        if (atomic_load(&g_lock_owners[idx]) == (uintptr_t)tid + 1) {
            atomic_store(&g_lock_owners[idx], 0);
        }
    }
    int had = 0;
    HeldLockSet *held = &g_threads[tid].held;
    for (int i = 0; i < held->count; i++) {
        if (held->locks[i] == m) {
            had = 1;
            for (int j = i; j < held->count - 1; j++) {
                held->locks[j] = held->locks[j + 1];
                held->sites[j] = held->sites[j + 1];
            }
            held->count--;
            break;
        }
    }
    ds_spin_unlock();
    return had;
}

void ds_restore_after_cond(ThreadId tid, MutexId m, void *site) {
    ds_log_record_order(tid, m, site);
    ds_take_ownership(tid, m, site);
}

int ds_is_owner(ThreadId tid, MutexId m) {
    ds_spin_lock();
    HeldLockSet *held = &g_threads[tid].held;
    for (int i = 0; i < held->count; i++) {
        if (held->locks[i] == m) {
            ds_spin_unlock();
            return 1;
        }
    }
    ds_spin_unlock();
    return 0;
}

typedef struct {
    void *(*real_fn)(void *);
    void *real_arg;
    ThreadId parent_tid;
} SpawnTrampolineArg;

static void *ds_thread_trampoline(void *arg) {
    SpawnTrampolineArg *targ = (SpawnTrampolineArg *)arg;
    void *(*real_fn)(void *) = targ->real_fn;
    void *real_arg = targ->real_arg;
    ThreadId parent_tid = targ->parent_tid;
    free(targ);

    ds_resolve_symbols();
    ds_my_tid = ds_register_thread(pthread_self(), parent_tid);
    ds_tid_initialized = 1;

    return real_fn(real_arg);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    ds_resolve_symbols();
    if (ds_in_hook) return real_pthread_create(thread, attr, start_routine, arg);

    ds_in_hook = 1;
    ThreadId parent = ds_get_or_register_thread();

    SpawnTrampolineArg *targ = (SpawnTrampolineArg *)malloc(sizeof(SpawnTrampolineArg));
    if (!targ) {
        ds_in_hook = 0;
        return real_pthread_create(thread, attr, start_routine, arg);
    }
    targ->real_fn = start_routine;
    targ->real_arg = arg;
    targ->parent_tid = parent;

    ds_in_hook = 0;
    return real_pthread_create(thread, attr, ds_thread_trampoline, targ);
}

int pthread_join(pthread_t thread, void **retval) {
    ds_resolve_symbols();
    if (ds_in_hook) return real_pthread_join(thread, retval);

    int rc = real_pthread_join(thread, retval);
    if (rc == 0) {
        ds_in_hook = 1;
        ds_spin_lock();
        for (int i = 0; i < MAX_THREADS; i++) {
            if (pthread_equal(g_threads[i].pthread_id, thread) && g_threads[i].join_seq == 0) {
                g_threads[i].join_seq = atomic_fetch_add(&g_event_seq, 1);
                atomic_store(&g_threads[i].active, 0);
                break;
            }
        }
        ds_spin_unlock();
        ds_in_hook = 0;
    }
    return rc;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    ds_resolve_symbols();
    if (ds_in_hook || !real_pthread_mutex_lock) {
        return real_pthread_mutex_lock ? real_pthread_mutex_lock(mutex) : 0;
    }

    ds_in_hook = 1;
    ThreadId tid = ds_get_or_register_thread();
    MutexId m = (MutexId)mutex;
    void *site = __builtin_return_address(0);

    /* Check if already held (e.g. recursive mutex) */
    if (ds_is_owner(tid, m)) {
        ds_in_hook = 0;
        return real_pthread_mutex_lock(mutex);
    }

    /* Fast path: probe with trylock */
    int rc = real_pthread_mutex_trylock(mutex);
    if (rc == 0) {
        ds_log_record_order(tid, m, site);
        ds_take_ownership(tid, m, site);
        ds_in_hook = 0;
        ds_stress_maybe_delay(tid, m);
        return 0;
    }

    if (rc == EBUSY) {
        /* Contended: record attempt-time edge */
        ds_log_record_order(tid, m, site);
        ds_wait_begin(tid, m, site, WK_LOCK);
        ds_in_hook = 0;

        rc = real_pthread_mutex_lock(mutex);

        ds_in_hook = 1;
        ds_wait_end(tid);
        if (rc == 0) {
            ds_take_ownership(tid, m, site);
        }
        ds_in_hook = 0;
        ds_stress_maybe_delay(tid, m);
        return rc;
    }

    ds_in_hook = 0;
    return rc;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    ds_resolve_symbols();
    if (ds_in_hook || !real_pthread_mutex_trylock) {
        return real_pthread_mutex_trylock ? real_pthread_mutex_trylock(mutex) : 0;
    }

    ds_in_hook = 1;
    ThreadId tid = ds_get_or_register_thread();
    MutexId m = (MutexId)mutex;
    void *site = __builtin_return_address(0);

    int rc = real_pthread_mutex_trylock(mutex);
    if (rc == 0) {
        /* Successful trylock does NOT add attempt edges, but joins held set */
        ds_take_ownership(tid, m, site);
    }
    ds_in_hook = 0;
    return rc;
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime) {
    ds_resolve_symbols();
    if (ds_in_hook || !real_pthread_mutex_timedlock) {
        return real_pthread_mutex_timedlock ? real_pthread_mutex_timedlock(mutex, abstime) : 0;
    }

    ds_in_hook = 1;
    ThreadId tid = ds_get_or_register_thread();
    MutexId m = (MutexId)mutex;
    void *site = __builtin_return_address(0);

    if (ds_is_owner(tid, m)) {
        ds_in_hook = 0;
        return real_pthread_mutex_timedlock(mutex, abstime);
    }

    int rc = real_pthread_mutex_trylock(mutex);
    if (rc == 0) {
        ds_log_record_order(tid, m, site);
        ds_take_ownership(tid, m, site);
        ds_in_hook = 0;
        ds_stress_maybe_delay(tid, m);
        return 0;
    }

    if (rc == EBUSY) {
        ds_log_record_order(tid, m, site);
        ds_wait_begin(tid, m, site, WK_TIMEDLOCK);
        ds_in_hook = 0;

        rc = real_pthread_mutex_timedlock(mutex, abstime);

        ds_in_hook = 1;
        ds_wait_end(tid);
        if (rc == 0) {
            ds_take_ownership(tid, m, site);
        }
        ds_in_hook = 0;
        ds_stress_maybe_delay(tid, m);
        return rc;
    }

    ds_in_hook = 0;
    return rc;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    ds_resolve_symbols();
    if (ds_in_hook || !real_pthread_mutex_unlock) {
        return real_pthread_mutex_unlock ? real_pthread_mutex_unlock(mutex) : 0;
    }

    ds_in_hook = 1;
    ThreadId tid = ds_get_or_register_thread();
    MutexId m = (MutexId)mutex;

    ds_release_ownership(tid, m);
    ds_in_hook = 0;
    return real_pthread_mutex_unlock(mutex);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    ds_resolve_symbols();
    int rc = real_pthread_mutex_init ? real_pthread_mutex_init(mutex, attr) : 0;
    if (rc == 0 && !ds_in_hook) {
        ds_in_hook = 1;
        ds_spin_lock();
        ds_get_or_create_lock_index((MutexId)mutex);
        ds_spin_unlock();
        ds_in_hook = 0;
    }
    return rc;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    ds_resolve_symbols();
    if (!ds_in_hook) {
        ds_in_hook = 1;
        ds_log_retire_lock((MutexId)mutex);
        ds_in_hook = 0;
    }
    return real_pthread_mutex_destroy ? real_pthread_mutex_destroy(mutex) : 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    ds_resolve_symbols();
    if (ds_in_hook || !real_pthread_cond_wait) {
        return real_pthread_cond_wait ? real_pthread_cond_wait(cond, mutex) : 0;
    }

    ds_in_hook = 1;
    ThreadId tid = ds_get_or_register_thread();
    MutexId m = (MutexId)mutex;
    void *site = __builtin_return_address(0);

    int had = ds_release_for_cond(tid, m);
    atomic_store(&g_threads[tid].wait_kind, WK_COND);
    atomic_store(&g_threads[tid].is_waiting, 1);
    atomic_store(&g_threads[tid].waiting_for, (uintptr_t)cond);
    ds_in_hook = 0;

    int rc = real_pthread_cond_wait(cond, mutex);

    ds_in_hook = 1;
    atomic_store(&g_threads[tid].is_waiting, 0);
    atomic_store(&g_threads[tid].wait_kind, WK_NONE);
    atomic_store(&g_threads[tid].waiting_for, 0);
    if (had) {
        ds_restore_after_cond(tid, m, site);
    }
    ds_in_hook = 0;
    return rc;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) {
    ds_resolve_symbols();
    if (ds_in_hook || !real_pthread_cond_timedwait) {
        return real_pthread_cond_timedwait ? real_pthread_cond_timedwait(cond, mutex, abstime) : 0;
    }

    ds_in_hook = 1;
    ThreadId tid = ds_get_or_register_thread();
    MutexId m = (MutexId)mutex;
    void *site = __builtin_return_address(0);

    int had = ds_release_for_cond(tid, m);
    atomic_store(&g_threads[tid].wait_kind, WK_COND);
    atomic_store(&g_threads[tid].is_waiting, 1);
    atomic_store(&g_threads[tid].waiting_for, (uintptr_t)cond);
    ds_in_hook = 0;

    int rc = real_pthread_cond_timedwait(cond, mutex, abstime);

    ds_in_hook = 1;
    atomic_store(&g_threads[tid].is_waiting, 0);
    atomic_store(&g_threads[tid].wait_kind, WK_NONE);
    atomic_store(&g_threads[tid].waiting_for, 0);
    if (had) {
        ds_restore_after_cond(tid, m, site);
    }
    ds_in_hook = 0;
    return rc;
}
