#include "ds_report.h"
#include "ds_lock_graph.h"
#include "ds_interceptor.h"

static Finding g_findings[MAX_FINDINGS];
static int g_finding_count = 0;

void ds_report_init(void) {
    ds_spin_lock();
    g_finding_count = 0;
    memset(g_findings, 0, sizeof(g_findings));
    ds_spin_unlock();
}

static bool same_cycle(const Cycle *c1, const Cycle *c2) {
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

bool ds_report_has_finding(FindingKind kind, const Cycle *cycle) {
    ds_spin_lock();
    for (int i = 0; i < g_finding_count; i++) {
        if (g_findings[i].kind == kind && same_cycle(&g_findings[i].cycle, cycle)) {
            ds_spin_unlock();
            return true;
        }
    }
    ds_spin_unlock();
    return false;
}

void ds_report_add_finding(FindingKind kind, SuppressReason reason, const Cycle *cycle) {
    ds_spin_lock();
    for (int i = 0; i < g_finding_count; i++) {
        if (same_cycle(&g_findings[i].cycle, cycle)) {
            if ((g_findings[i].kind == FINDING_POTENTIAL || g_findings[i].kind == FINDING_ACTIVE) &&
                (kind == FINDING_CONFIRMED || kind == FINDING_ACTIVE_TIMED)) {
                g_findings[i].kind = kind;
                ds_spin_unlock();
                return;
            }
            if (g_findings[i].kind == kind) {
                ds_spin_unlock();
                return;
            }
        }
    }

    if (g_finding_count < MAX_FINDINGS) {
        Finding *f = &g_findings[g_finding_count++];
        f->kind = kind;
        f->suppress_reason = reason;
        f->cycle = *cycle;
        f->detected_time_ms = ds_get_time_ms();
    }
    ds_spin_unlock();
}

static const char *suppress_reason_str(SuppressReason r) {
    switch (r) {
        case SUPPRESS_SAME_THREAD: return "same_thread (all edges witnessed by same thread)";
        case SUPPRESS_GATE_LOCK:   return "gate_lock (common lock serializes critical sections)";
        case SUPPRESS_LIFECYCLE:   return "lifecycle (spawn/join temporal ordering barrier)";
        default:                   return "none";
    }
}

void ds_report_print_finding(const Finding *f) {
    char sym_from[128], sym_to[128];
    switch (f->kind) {
        case FINDING_POTENTIAL:
            fprintf(stderr, "\n[DS-FINDING] POTENTIAL DEADLOCK PREDICTED (Cycle length %d)\n", f->cycle.length);
            for (int i = 0; i < f->cycle.length; i++) {
                ds_symbolize_site(f->cycle.witnesses[i].site_from, sym_from, sizeof(sym_from));
                ds_symbolize_site(f->cycle.witnesses[i].site_to, sym_to, sizeof(sym_to));
                fprintf(stderr, "  Edge %d: Mutex 0x%lx -> Mutex 0x%lx [Witness Thread %u: from %s to %s]\n",
                        i + 1,
                        (unsigned long)f->cycle.locks[i],
                        (unsigned long)f->cycle.locks[(i + 1) % f->cycle.length],
                        f->cycle.witnesses[i].tid,
                        sym_from, sym_to);
            }
            break;
        case FINDING_ACTIVE:
            fprintf(stderr, "\n[DS-FINDING] ACTIVE DEADLOCK DETECTED (Live wait cycle length %d)\n", f->cycle.length);
            for (int i = 0; i < f->cycle.length; i++) {
                ds_symbolize_site(f->cycle.witnesses[i].site_to, sym_to, sizeof(sym_to));
                fprintf(stderr, "  Thread %u blocked at %s waiting for Mutex 0x%lx\n",
                        f->cycle.witnesses[i].tid,
                        sym_to,
                        (unsigned long)f->cycle.locks[i]);
            }
            break;
        case FINDING_ACTIVE_TIMED:
            fprintf(stderr, "\n[DS-FINDING] ACTIVE_TIMED DEADLOCK DETECTED (Live wait cycle length %d, contains timed wait)\n", f->cycle.length);
            for (int i = 0; i < f->cycle.length; i++) {
                ds_symbolize_site(f->cycle.witnesses[i].site_to, sym_to, sizeof(sym_to));
                fprintf(stderr, "  Thread %u blocked at %s waiting for Mutex 0x%lx (timed/cancellable)\n",
                        f->cycle.witnesses[i].tid,
                        sym_to,
                        (unsigned long)f->cycle.locks[i]);
            }
            break;
        case FINDING_CONFIRMED:
            fprintf(stderr, "\n[DS-FINDING] CONFIRMED DEADLOCK (Predicted POTENTIAL cycle validated via execution!)\n");
            for (int i = 0; i < f->cycle.length; i++) {
                ds_symbolize_site(f->cycle.witnesses[i].site_from, sym_from, sizeof(sym_from));
                ds_symbolize_site(f->cycle.witnesses[i].site_to, sym_to, sizeof(sym_to));
                fprintf(stderr, "  Thread %u holding Mutex 0x%lx (%s), waiting for Mutex 0x%lx (%s)\n",
                        f->cycle.witnesses[i].tid,
                        (unsigned long)f->cycle.locks[i],
                        sym_from,
                        (unsigned long)f->cycle.locks[(i + 1) % f->cycle.length],
                        sym_to);
            }
            break;
        case FINDING_SUPPRESSED:
            if (g_config.debug) {
                fprintf(stderr, "\n[DS-DEBUG] SUPPRESSED CANDIDATE CYCLE: %s\n", suppress_reason_str(f->suppress_reason));
            }
            break;
    }
}

void ds_report_dump_summary(void) {
    int cnt_potential = 0;
    int cnt_active = 0;
    int cnt_active_timed = 0;
    int cnt_confirmed = 0;
    int cnt_suppressed = 0;

    for (int i = 0; i < g_finding_count; i++) {
        switch (g_findings[i].kind) {
            case FINDING_POTENTIAL: cnt_potential++; break;
            case FINDING_ACTIVE: cnt_active++; break;
            case FINDING_ACTIVE_TIMED: cnt_active_timed++; break;
            case FINDING_CONFIRMED: cnt_confirmed++; break;
            case FINDING_SUPPRESSED: cnt_suppressed++; break;
        }
    }

    fprintf(stderr, "\n================================================================================\n");
    fprintf(stderr, "                           DEADLOCKSCOPE SUMMARY REPORT\n");
    fprintf(stderr, "================================================================================\n");
    fprintf(stderr, "  POTENTIAL DEADLOCKS    : %d\n", cnt_potential);
    fprintf(stderr, "  ACTIVE DEADLOCKS       : %d\n", cnt_active);
    fprintf(stderr, "  ACTIVE_TIMED DEADLOCKS : %d\n", cnt_active_timed);
    fprintf(stderr, "  CONFIRMED DEADLOCKS    : %d\n", cnt_confirmed);
    fprintf(stderr, "  SUPPRESSED CYCLES      : %d\n", cnt_suppressed);
    fprintf(stderr, "--------------------------------------------------------------------------------\n");

    for (int i = 0; i < g_finding_count; i++) {
        ds_report_print_finding(&g_findings[i]);
    }
    fprintf(stderr, "================================================================================\n\n");
}

void ds_report_export_dot(const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) return;

    fprintf(fp, "digraph DeadlockScope {\n");
    fprintf(fp, "  compound=true;\n");
    fprintf(fp, "  rankdir=LR;\n");
    fprintf(fp, "  fontname=\"Helvetica\";\n\n");

    /* Subgraph 1: Lock-Order Dependency Graph */
    fprintf(fp, "  subgraph cluster_lock_order {\n");
    fprintf(fp, "    label=\"Lock-Order Dependency Graph (Acquisitions)\";\n");
    fprintf(fp, "    style=dashed;\n");
    fprintf(fp, "    color=darkblue;\n");
    fprintf(fp, "    node [shape=box, style=filled, fillcolor=lightblue, fontname=\"Helvetica\"];\n");

    int edge_cnt = ds_log_get_edge_count();
    for (int i = 0; i < edge_cnt; i++) {
        const LockOrderEdge *edge = ds_log_get_edge(i);
        if (!edge) continue;

        const char *color = "black";
        int penwidth = 1;

        for (int f = 0; f < g_finding_count; f++) {
            if (g_findings[f].kind == FINDING_POTENTIAL || g_findings[f].kind == FINDING_CONFIRMED) {
                const Cycle *c = &g_findings[f].cycle;
                for (int k = 0; k < c->length; k++) {
                    if (c->locks[k] == edge->from && c->locks[(k + 1) % c->length] == edge->to) {
                        color = (g_findings[f].kind == FINDING_CONFIRMED) ? "red" : "orange";
                        penwidth = 2;
                        break;
                    }
                }
            }
        }

        fprintf(fp, "    \"L_0x%lx\" -> \"L_0x%lx\" [color=\"%s\", penwidth=%d, label=\"seen\"];\n",
                (unsigned long)edge->from, (unsigned long)edge->to, color, penwidth);
    }
    fprintf(fp, "  }\n\n");

    /* Subgraph 2: Active Wait-For Graph (Threads) */
    fprintf(fp, "  subgraph cluster_wait_for {\n");
    fprintf(fp, "    label=\"Live Wait-For Graph (Threads)\";\n");
    fprintf(fp, "    style=dashed;\n");
    fprintf(fp, "    color=crimson;\n");
    fprintf(fp, "    node [shape=ellipse, style=filled, fillcolor=lightpink, fontname=\"Helvetica\"];\n");

    for (int f = 0; f < g_finding_count; f++) {
        if (g_findings[f].kind == FINDING_ACTIVE || g_findings[f].kind == FINDING_ACTIVE_TIMED || g_findings[f].kind == FINDING_CONFIRMED) {
            const Cycle *c = &g_findings[f].cycle;
            for (int k = 0; k < c->length; k++) {
                ThreadId from_t = c->witnesses[k].tid;
                ThreadId to_t = c->witnesses[(k + 1) % c->length].tid;
                const char *wcolor = (g_findings[f].kind == FINDING_ACTIVE_TIMED) ? "purple" : "red";
                fprintf(fp, "    \"T_%u\" -> \"T_%u\" [color=\"%s\", penwidth=2, label=\"waits_on_0x%lx\"];\n",
                        from_t, to_t, wcolor, (unsigned long)c->locks[k]);
            }
        }
    }
    fprintf(fp, "  }\n");

    fprintf(fp, "}\n");
    fclose(fp);
}
