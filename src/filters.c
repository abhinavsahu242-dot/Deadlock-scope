#include "ds_filters.h"

bool ds_filter_same_thread(const Cycle *cycle) {
    if (cycle->length < 2) return false;
    ThreadId first_tid = cycle->witnesses[0].tid;
    for (int i = 1; i < cycle->length; i++) {
        if (cycle->witnesses[i].tid != first_tid) {
            return false; /* multiple distinct threads */
        }
    }
    return true; /* all edges witnessed by the exact same thread */
}

bool ds_filter_gate_lock(const Cycle *cycle, MutexId *out_gate) {
    if (cycle->length < 2) return false;

    /* Find candidate gate locks from the first witness */
    const HeldLockSet *first_held = &cycle->witnesses[0].held_before;
    for (int h = 0; h < first_held->count; h++) {
        MutexId cand = first_held->locks[h];

        /* Candidate gate must not be one of the cycle's internal locks */
        bool is_cycle_lock = false;
        for (int c = 0; c < cycle->length; c++) {
            if (cycle->locks[c] == cand) {
                is_cycle_lock = true;
                break;
            }
        }
        if (is_cycle_lock) continue;

        /* Verify cand is held in all other witnesses */
        bool common_to_all = true;
        for (int i = 1; i < cycle->length; i++) {
            const HeldLockSet *other_held = &cycle->witnesses[i].held_before;
            bool found = false;
            for (int k = 0; k < other_held->count; k++) {
                if (other_held->locks[k] == cand) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                common_to_all = false;
                break;
            }
        }

        if (common_to_all) {
            if (out_gate) *out_gate = cand;
            return true;
        }
    }

    return false;
}

bool ds_filter_lifecycle(const Cycle *cycle) {
    if (cycle->length < 2) return false;

    for (int i = 0; i < cycle->length; i++) {
        ThreadId t1 = cycle->witnesses[i].tid;
        uint64_t t1_join = g_threads[t1].join_seq;

        for (int j = 0; j < cycle->length; j++) {
            if (i == j) continue;
            ThreadId t2 = cycle->witnesses[j].tid;
            uint64_t t2_spawn = g_threads[t2].spawn_seq;

            /* If t1 joined before t2 was spawned, they cannot overlap */
            if (t1_join > 0 && t2_spawn > t1_join) {
                return true;
            }
        }
    }

    return false;
}

SuppressReason ds_evaluate_filters(const Cycle *cycle) {
    if ((g_config.filter_mask & FILTER_SAME_THREAD) && ds_filter_same_thread(cycle)) {
        return SUPPRESS_SAME_THREAD;
    }
    if ((g_config.filter_mask & FILTER_GATE_LOCK) && ds_filter_gate_lock(cycle, NULL)) {
        return SUPPRESS_GATE_LOCK;
    }
    if ((g_config.filter_mask & FILTER_LIFECYCLE) && ds_filter_lifecycle(cycle)) {
        return SUPPRESS_LIFECYCLE;
    }
    return SUPPRESS_NONE;
}
