#include "ds_lock_graph.h"
#include "ds_interceptor.h"

static LockOrderEdge g_edges[MAX_EDGES];
static int g_edge_count = 0;

void ds_log_init(void) {
    ds_spin_lock();
    g_edge_count = 0;
    memset(g_edges, 0, sizeof(g_edges));
    ds_spin_unlock();
}

void ds_log_record_order(ThreadId tid, MutexId to, void *site_to) {
    ds_spin_lock();
    HeldLockSet *held = &g_threads[tid].held;
    for (int i = 0; i < held->count; i++) {
        MutexId from = held->locks[i];
        void *site_from = held->sites[i];
        if (from == to) continue; /* self-edge */

        /* Search for existing edge */
        LockOrderEdge *edge = NULL;
        for (int e = 0; e < g_edge_count; e++) {
            if (g_edges[e].from == from && g_edges[e].to == to) {
                edge = &g_edges[e];
                break;
            }
        }

        if (!edge) {
            if (g_edge_count < MAX_EDGES) {
                edge = &g_edges[g_edge_count++];
                edge->from = from;
                edge->to = to;
                edge->witness_count = 0;
            }
        }

        if (edge) {
            /* Check if witness for this thread already exists */
            bool have_witness = false;
            for (int w = 0; w < edge->witness_count; w++) {
                if (edge->witnesses[w].tid == tid) {
                    have_witness = true;
                    break;
                }
            }
            if (!have_witness && edge->witness_count < MAX_WITNESSES) {
                Witness *wit = &edge->witnesses[edge->witness_count++];
                wit->tid = tid;
                wit->held_before = *held;
                wit->site_from = site_from;
                wit->site_to = site_to;
            }
        }
    }
    ds_spin_unlock();
}

int ds_log_get_edge_count(void) {
    return g_edge_count;
}

const LockOrderEdge* ds_log_get_edge(int index) {
    if (index >= 0 && index < g_edge_count) return &g_edges[index];
    return NULL;
}

void ds_log_retire_lock(MutexId m) {
    ds_spin_lock();
    int write_idx = 0;
    for (int i = 0; i < g_edge_count; i++) {
        if (g_edges[i].from != m && g_edges[i].to != m) {
            if (write_idx != i) {
                g_edges[write_idx] = g_edges[i];
            }
            write_idx++;
        }
    }
    g_edge_count = write_idx;
    ds_spin_unlock();
}

static void normalize_cycle(Cycle *c) {
    if (c->length <= 1) return;
    int min_idx = 0;
    for (int i = 1; i < c->length; i++) {
        if (c->locks[i] < c->locks[min_idx]) {
            min_idx = i;
        }
    }
    if (min_idx == 0) return;

    Cycle temp = *c;
    for (int i = 0; i < c->length; i++) {
        int src = (min_idx + i) % c->length;
        c->locks[i] = temp.locks[src];
        c->witnesses[i] = temp.witnesses[src];
    }
}

bool ds_cycles_equal(const Cycle *c1, const Cycle *c2) {
    if (c1->length != c2->length) return false;
    for (int i = 0; i < c1->length; i++) {
        if (c1->locks[i] != c2->locks[i]) return false;
    }
    return true;
}

static void dfs_find(MutexId start, MutexId curr, int depth,
                     MutexId path[], Witness path_w[], bool visited[],
                     Cycle out[], int *out_count, int max_cycles) {
    if (depth >= MAX_CYCLE_DEPTH) return;

    for (int e = 0; e < g_edge_count; e++) {
        if (g_edges[e].from != curr) continue;

        MutexId next = g_edges[e].to;
        Witness w = (g_edges[e].witness_count > 0) ? g_edges[e].witnesses[0] : (Witness){0};

        if (next == start && depth >= 1) {
            /* Cycle completed! */
            Cycle found;
            found.length = depth + 1;
            for (int i = 0; i <= depth; i++) {
                found.locks[i] = path[i];
                found.witnesses[i] = (i < depth) ? path_w[i] : w;
            }
            normalize_cycle(&found);

            /* Check duplicate */
            bool exists = false;
            for (int c = 0; c < *out_count; c++) {
                if (ds_cycles_equal(&out[c], &found)) {
                    exists = true;
                    break;
                }
            }
            if (!exists && *out_count < max_cycles) {
                out[(*out_count)++] = found;
            }
            continue;
        }

        /* Check if next already in path */
        bool in_path = false;
        for (int i = 0; i <= depth; i++) {
            if (path[i] == next) {
                in_path = true;
                break;
            }
        }

        if (!in_path && depth + 1 < MAX_CYCLE_DEPTH) {
            path[depth + 1] = next;
            path_w[depth] = w;
            dfs_find(start, next, depth + 1, path, path_w, visited, out, out_count, max_cycles);
        }
    }
}

int ds_log_find_cycles(Cycle out_cycles[], int max_cycles) {
    ds_spin_lock();
    int count = 0;
    MutexId path[MAX_CYCLE_DEPTH];
    Witness path_w[MAX_CYCLE_DEPTH];
    bool visited[MAX_LOCKS];
    memset(visited, 0, sizeof(visited));

    for (int e = 0; e < g_edge_count; e++) {
        MutexId start = g_edges[e].from;
        path[0] = start;
        dfs_find(start, start, 0, path, path_w, visited, out_cycles, &count, max_cycles);
    }
    ds_spin_unlock();
    return count;
}
