#include "ds_watchdog.h"
#include "ds_interceptor.h"
#include "ds_wait_graph.h"
#include "ds_lock_graph.h"
#include "ds_filters.h"
#include "ds_stress.h"
#include "ds_report.h"

static pthread_t g_watchdog_thread;
static atomic_int g_watchdog_running = 0;

static bool same_lock_set(const Cycle *c1, const Cycle *c2) {
    if (c1->length != c2->length) return false;
    for (int i = 0; i < c1->length; i++) {
        bool found = false;
        for (int j = 0; j < c2->length; j++) {
            if (c1->locks[i] == c2->locks[j]) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static void *watchdog_main(void *arg) {
    (void)arg;
    ds_in_hook = 1;

    Cycle last_active_cycle = {0};
    uint64_t first_seen_ms = 0;
    bool already_reported = false;

    while (atomic_load(&g_watchdog_running)) {
        struct timespec req = {
            .tv_sec = g_config.poll_ms / 1000,
            .tv_nsec = (g_config.poll_ms % 1000) * 1000000L
        };
        nanosleep(&req, NULL);

        /* 1. Periodic Lock-Order Graph scan to update predictions & stress targets */
        Cycle log_cycles[16];
        int num_log = ds_log_find_cycles(log_cycles, 16);
        for (int i = 0; i < num_log; i++) {
            SuppressReason reason = ds_evaluate_filters(&log_cycles[i]);
            if (reason == SUPPRESS_NONE) {
                ds_report_add_finding(FINDING_POTENTIAL, SUPPRESS_NONE, &log_cycles[i]);
                ds_stress_register_target_locks(&log_cycles[i]);
            } else {
                ds_report_add_finding(FINDING_SUPPRESSED, reason, &log_cycles[i]);
            }
        }

        /* 2. Live Wait-For Graph cycle detection */
        Cycle active_cycle;
        if (ds_wfg_detect_active_cycle(&active_cycle)) {
            uint64_t now_ms = ds_get_time_ms();
            if (same_lock_set(&active_cycle, &last_active_cycle)) {
                /* Same cycle persists across ticks */
                if (!already_reported && (now_ms - first_seen_ms >= (uint64_t)g_config.confirm_ms)) {
                    FindingKind kind = FINDING_ACTIVE;
                    if (active_cycle.contains_timed_wait) {
                        kind = FINDING_ACTIVE_TIMED;
                    } else if (ds_report_has_finding(FINDING_POTENTIAL, &active_cycle)) {
                        kind = FINDING_CONFIRMED;
                    }
                    ds_report_add_finding(kind, SUPPRESS_NONE, &active_cycle);
                    already_reported = true;

                    /* Live deadlocked process will never exit; emit immediately */
                    fprintf(stderr, "\n=== DEADLOCK DETECTED BY WATCHDOG ===\n");
                    if (kind == FINDING_CONFIRMED) {
                        fprintf(stderr, "[!] STATUS: CONFIRMED (Predicted POTENTIAL cycle successfully validated)\n");
                    } else if (kind == FINDING_ACTIVE_TIMED) {
                        fprintf(stderr, "[!] STATUS: ACTIVE_TIMED (Contains timed wait; cycle may break on timeout)\n");
                    } else {
                        fprintf(stderr, "[!] STATUS: ACTIVE (Live deadlock observed in wait-for graph)\n");
                    }
                    fprintf(stderr, "Cycle length: %d threads/locks involved.\n", active_cycle.length);
                    for (int k = 0; k < active_cycle.length; k++) {
                        char sym_buf[128];
                        ds_symbolize_site(active_cycle.witnesses[k].site_to, sym_buf, sizeof(sym_buf));
                        fprintf(stderr, "  Thread %u waiting for Mutex 0x%lx (at %s, held by Thread %u)\n",
                                active_cycle.witnesses[k].tid,
                                (unsigned long)active_cycle.locks[k],
                                sym_buf,
                                active_cycle.witnesses[(k + 1) % active_cycle.length].tid);
                    }
                    fprintf(stderr, "======================================\n\n");
                }
            } else {
                /* New candidate cycle sighting */
                last_active_cycle = active_cycle;
                first_seen_ms = now_ms;
                already_reported = false;
            }
        } else {
            memset(&last_active_cycle, 0, sizeof(last_active_cycle));
            already_reported = false;
        }
    }

    return NULL;
}

void ds_watchdog_start(void) {
    if (atomic_load(&g_watchdog_running)) return;
    atomic_store(&g_watchdog_running, 1);

    if (real_pthread_create) {
        real_pthread_create(&g_watchdog_thread, NULL, watchdog_main, NULL);
    }
}

void ds_watchdog_stop(void) {
    if (!atomic_load(&g_watchdog_running)) return;
    atomic_store(&g_watchdog_running, 0);
    if (real_pthread_join && g_watchdog_thread) {
        real_pthread_join(g_watchdog_thread, NULL);
    }
}
