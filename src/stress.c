#include "ds_stress.h"

static MutexId g_target_locks[MAX_LOCKS];
static int g_target_lock_count = 0;

void ds_stress_init(void) {
    ds_spin_lock();
    g_target_lock_count = 0;
    memset(g_target_locks, 0, sizeof(g_target_locks));
    ds_spin_unlock();
}

void ds_stress_register_target_locks(const Cycle *cycle) {
    ds_spin_lock();
    for (int i = 0; i < cycle->length; i++) {
        MutexId m = cycle->locks[i];
        bool exists = false;
        for (int k = 0; k < g_target_lock_count; k++) {
            if (g_target_locks[k] == m) {
                exists = true;
                break;
            }
        }
        if (!exists && g_target_lock_count < MAX_LOCKS) {
            g_target_locks[g_target_lock_count++] = m;
        }
    }
    ds_spin_unlock();
}

bool ds_stress_is_target(MutexId m) {
    if (!g_config.stress_enabled) return false;
    for (int i = 0; i < g_target_lock_count; i++) {
        if (g_target_locks[i] == m) return true;
    }
    return false;
}

void ds_stress_maybe_delay(ThreadId tid, MutexId m) {
    if (!g_config.stress_enabled) return;

    /* Inject delay if lock is target or thread currently holds locks */
    if (ds_stress_is_target(m) || g_threads[tid].held.count >= 1) {
        struct timespec req = {
            .tv_sec = g_config.stress_delay_us / 1000000,
            .tv_nsec = (g_config.stress_delay_us % 1000000) * 1000L
        };
        nanosleep(&req, NULL);
    }
}
