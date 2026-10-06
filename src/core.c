#include "ds_common.h"
#include "ds_interceptor.h"
#include "ds_lock_graph.h"
#include "ds_filters.h"
#include "ds_wait_graph.h"
#include "ds_watchdog.h"
#include "ds_stress.h"
#include "ds_report.h"

DSConfig g_config;
ThreadSlot g_threads[MAX_THREADS];
atomic_uintptr_t g_lock_owners[MAX_LOCKS];
MutexId g_lock_table[MAX_LOCKS];
int g_lock_count = 0;
atomic_uint_fast64_t g_event_seq = 0;

static atomic_flag g_monitor_flag = ATOMIC_FLAG_INIT;
static atomic_int g_next_tid = 0;

__thread int ds_in_hook = 0;
__thread ThreadId ds_my_tid = 0;
__thread int ds_tid_initialized = 0;

void ds_spin_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_monitor_flag, memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#else
        sched_yield();
#endif
    }
}

void ds_spin_unlock(void) {
    atomic_flag_clear_explicit(&g_monitor_flag, memory_order_release);
}

uint64_t ds_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t ds_get_time_ms(void) {
    return ds_get_time_ns() / 1000000ULL;
}

const char *ds_symbolize_site(void *addr, char *buf, size_t buflen) {
    if (!addr) {
        snprintf(buf, buflen, "unknown");
        return buf;
    }
    Dl_info info;
    if (dladdr(addr, &info) && info.dli_sname) {
        uintptr_t offset = (uintptr_t)addr - (uintptr_t)info.dli_saddr;
        snprintf(buf, buflen, "%s+0x%lx", info.dli_sname, (unsigned long)offset);
    } else {
        snprintf(buf, buflen, "%p", addr);
    }
    return buf;
}

static void ds_load_config(void) {
    memset(&g_config, 0, sizeof(g_config));
    
    const char *stress_env = getenv("DEADLOCKSCOPE_STRESS");
    if (stress_env) {
        g_config.stress_enabled = 1;
        int ms = atoi(stress_env);
        g_config.stress_delay_us = (ms > 0 ? ms : 5) * 1000;
    } else {
        g_config.stress_enabled = 0;
        g_config.stress_delay_us = 5000;
    }

    const char *poll_env = getenv("DEADLOCKSCOPE_POLL_MS");
    g_config.poll_ms = poll_env ? atoi(poll_env) : 50;
    if (g_config.poll_ms <= 0) g_config.poll_ms = 50;

    const char *conf_env = getenv("DEADLOCKSCOPE_CONFIRM_MS");
    g_config.confirm_ms = conf_env ? atoi(conf_env) : 100;
    if (g_config.confirm_ms <= 0) g_config.confirm_ms = 100;

    const char *dbg_env = getenv("DEADLOCKSCOPE_DEBUG");
    g_config.debug = (dbg_env && atoi(dbg_env) > 0) ? 1 : 0;

    const char *dot_env = getenv("DEADLOCKSCOPE_DOT");
    if (dot_env && strlen(dot_env) > 0) {
        strncpy(g_config.dot_file, dot_env, sizeof(g_config.dot_file) - 1);
    }

    /* Filter configuration */
    const char *filt_env = getenv("DEADLOCKSCOPE_FILTERS");
    if (filt_env) {
        g_config.filter_mask = 0;
        if (strstr(filt_env, "all") != NULL) {
            g_config.filter_mask = FILTER_ALL;
        } else {
            if (strstr(filt_env, "thread") != NULL)    g_config.filter_mask |= FILTER_SAME_THREAD;
            if (strstr(filt_env, "gate") != NULL)      g_config.filter_mask |= FILTER_GATE_LOCK;
            if (strstr(filt_env, "lifecycle") != NULL) g_config.filter_mask |= FILTER_LIFECYCLE;
        }
    } else {
        g_config.filter_mask = FILTER_ALL;
    }
}

__attribute__((constructor(101)))
void ds_init(void) {
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;

    ds_resolve_symbols();
    ds_load_config();

    memset(g_threads, 0, sizeof(g_threads));
    for (int i = 0; i < MAX_LOCKS; i++) {
        atomic_init(&g_lock_owners[i], 0);
        g_lock_table[i] = 0;
    }

    /* Register main thread */
    ds_my_tid = 0;
    ds_tid_initialized = 1;
    g_threads[0].tid = 0;
    g_threads[0].pthread_id = pthread_self();
    atomic_store(&g_threads[0].active, 1);
    atomic_store(&g_threads[0].is_waiting, 0);
    atomic_store(&g_threads[0].wait_kind, WK_NONE);
    atomic_store(&g_threads[0].waiting_for, 0);
    g_threads[0].parent_tid = 0;
    g_threads[0].spawn_seq = 0;
    atomic_store(&g_next_tid, 1);

    ds_log_init();
    ds_stress_init();
    ds_report_init();

    ds_watchdog_start();
}

__attribute__((destructor(101)))
void ds_fini(void) {
    static int finished = 0;
    if (finished) return;
    finished = 1;

    ds_watchdog_stop();

    /* Final lock-order cycle search */
    Cycle cycles[MAX_FINDINGS];
    int n = ds_log_find_cycles(cycles, MAX_FINDINGS);
    for (int i = 0; i < n; i++) {
        SuppressReason reason = ds_evaluate_filters(&cycles[i]);
        if (reason == SUPPRESS_NONE) {
            ds_report_add_finding(FINDING_POTENTIAL, SUPPRESS_NONE, &cycles[i]);
        } else {
            ds_report_add_finding(FINDING_SUPPRESSED, reason, &cycles[i]);
        }
    }

    ds_report_dump_summary();

    if (g_config.dot_file[0] != '\0') {
        ds_report_export_dot(g_config.dot_file);
    }
}
