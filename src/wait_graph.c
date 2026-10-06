#include "ds_wait_graph.h"
#include "ds_interceptor.h"

void ds_wait_begin(ThreadId tid, MutexId m, void *site, WaitKind kind) {
    g_threads[tid].wait_site = site;
    g_threads[tid].wait_start_ns = ds_get_time_ns();
    atomic_store(&g_threads[tid].wait_kind, kind);
    atomic_store(&g_threads[tid].waiting_for, m);
    atomic_store(&g_threads[tid].is_waiting, 1);
}

void ds_wait_end(ThreadId tid) {
    atomic_store(&g_threads[tid].is_waiting, 0);
    atomic_store(&g_threads[tid].wait_kind, WK_NONE);
    atomic_store(&g_threads[tid].waiting_for, 0);
}

int ds_wfg_detect_active_cycle(Cycle *out_cycle) {
    ThreadId successor[MAX_THREADS];
    MutexId waiting_lock[MAX_THREADS];
    void *wait_sites[MAX_THREADS];
    WaitKind wait_kinds[MAX_THREADS];

    for (int i = 0; i < MAX_THREADS; i++) {
        successor[i] = (ThreadId)-1;
        waiting_lock[i] = 0;
        wait_sites[i] = NULL;
        wait_kinds[i] = WK_NONE;
    }

    ds_spin_lock();
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!atomic_load(&g_threads[i].active)) continue;
        if (!atomic_load(&g_threads[i].is_waiting)) continue;

        WaitKind wk = (WaitKind)atomic_load(&g_threads[i].wait_kind);
        if (wk != WK_LOCK && wk != WK_TIMEDLOCK) continue; /* Cond waits don't have mutex successors */

        MutexId m = (MutexId)atomic_load(&g_threads[i].waiting_for);
        if (!m) continue;

        waiting_lock[i] = m;
        wait_sites[i] = g_threads[i].wait_site;
        wait_kinds[i] = wk;

        int l_idx = ds_get_or_create_lock_index(m);
        if (l_idx >= 0) {
            uintptr_t owner_val = atomic_load(&g_lock_owners[l_idx]);
            if (owner_val > 0) {
                ThreadId owner_tid = (ThreadId)(owner_val - 1);
                if (owner_tid < MAX_THREADS && atomic_load(&g_threads[owner_tid].active)) {
                    successor[i] = owner_tid;
                }
            }
        }
    }
    ds_spin_unlock();

    uint64_t global_visited = 0;

    for (int start = 0; start < MAX_THREADS; start++) {
        if (successor[start] == (ThreadId)-1) continue;
        if (global_visited & (1ULL << start)) continue;

        ThreadId path[MAX_CYCLE_DEPTH + 1];
        int depth = 0;
        ThreadId curr = (ThreadId)start;
        uint64_t path_mask = 0;

        while (curr != (ThreadId)-1 && depth < MAX_CYCLE_DEPTH) {
            if (path_mask & (1ULL << curr)) {
                /* Cycle found */
                int cycle_start = 0;
                while (cycle_start < depth && path[cycle_start] != curr) {
                    cycle_start++;
                }

                int cycle_len = depth - cycle_start;
                if (cycle_len >= 2 && cycle_len <= MAX_CYCLE_DEPTH) {
                    out_cycle->length = (uint8_t)cycle_len;
                    out_cycle->contains_timed_wait = false;
                    for (int k = 0; k < cycle_len; k++) {
                        ThreadId t = path[cycle_start + k];
                        out_cycle->locks[k] = waiting_lock[t];
                        out_cycle->witnesses[k].tid = t;
                        out_cycle->witnesses[k].site_to = wait_sites[t];
                        out_cycle->witnesses[k].held_before = g_threads[t].held;
                        if (wait_kinds[t] == WK_TIMEDLOCK) {
                            out_cycle->contains_timed_wait = true;
                        }
                    }
                    return 1;
                }
                break;
            }

            path[depth++] = curr;
            path_mask |= (1ULL << curr);
            global_visited |= (1ULL << curr);
            curr = successor[curr];
        }
    }

    return 0;
}
