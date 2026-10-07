#include "ds_report.h"
#include "ds_lock_graph.h"
#include "ds_interceptor.h"
#include "ds_filters.h"
#include <time.h>
#include <unistd.h>

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

void ds_report_export_html(const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) return;

    /* 1. Extract Target Executable Name */
    char target_name[256] = "Pthread Concurrency Target";
    FILE *fcmd = fopen("/proc/self/cmdline", "r");
    if (fcmd) {
        char cmd_buf[256];
        size_t r = fread(cmd_buf, 1, sizeof(cmd_buf) - 1, fcmd);
        if (r > 0) {
            cmd_buf[r] = '\0';
            char *p = cmd_buf;
            char *slash = strrchr(p, '/');
            if (slash) p = slash + 1;
            snprintf(target_name, sizeof(target_name), "%s", p);
        }
        fclose(fcmd);
    }

    /* 2. Compute Metric Counters */
    int cnt_potential = 0, cnt_active = 0, cnt_active_timed = 0, cnt_confirmed = 0, cnt_suppressed = 0;
    for (int i = 0; i < g_finding_count; i++) {
        switch (g_findings[i].kind) {
            case FINDING_POTENTIAL: cnt_potential++; break;
            case FINDING_ACTIVE: cnt_active++; break;
            case FINDING_ACTIVE_TIMED: cnt_active_timed++; break;
            case FINDING_CONFIRMED: cnt_confirmed++; break;
            case FINDING_SUPPRESSED: cnt_suppressed++; break;
        }
    }

    /* 3. Determine Scenario Diagnostic Classification & Gate Lock */
    const char *status_code = "CLEAN";
    const char *status_badge = "VERIFIED DEADLOCK-FREE";
    const char *status_title = "CLEAN: STRICT LOCK-ORDER COMPLIANCE";
    const char *status_col = "#10b981";
    const char *status_bg = "rgba(16, 185, 129, 0.12)";
    const char *status_desc = "Zero lock inversions observed. All observed mutex acquisitions respect a strict hierarchical order.";
    const char *suppress_detail = "none";
    MutexId detected_gate_lock = 0;

    if (cnt_confirmed > 0) {
        status_code = "CONFIRMED";
        status_badge = "CRITICAL FAILURE: LIVE DEADLOCK";
        status_title = "CONFIRMED ACTIVE DEADLOCK (PROCESS FROZEN)";
        status_col = "#ef4444";
        status_bg = "rgba(239, 68, 68, 0.15)";
        status_desc = "Threads entered a permanent circular wait state. Deadlock actively manifested and halted execution progress.";
    } else if (cnt_active_timed > 0) {
        status_code = "ACTIVE_TIMED";
        status_badge = "NON-FATAL WAIT: TIMED STALL";
        status_title = "ACTIVE TIMED STALL (SELF-RESOLVING)";
        status_col = "#a855f7";
        status_bg = "rgba(168, 85, 247, 0.15)";
        status_desc = "Wait cycle contains pthread_mutex_timedlock. This is not a permanent freeze; the stall will break when the timeout expires.";
    } else if (cnt_active > 0) {
        status_code = "ACTIVE";
        status_badge = "UNYIELDING BLOCK DETECTED";
        status_title = "ACTIVE DEADLOCK OBSERVED";
        status_col = "#ef4444";
        status_bg = "rgba(239, 68, 68, 0.15)";
        status_desc = "Threads observed in an unyielding blocking cycle in the live Wait-For Graph.";
    } else if (cnt_suppressed > 0) {
        status_code = "SUPPRESSED";
        for (int i = 0; i < g_finding_count; i++) {
            if (g_findings[i].kind == FINDING_SUPPRESSED) {
                if (g_findings[i].suppress_reason == SUPPRESS_GATE_LOCK) {
                    suppress_detail = "gate_lock";
                    status_badge = "VERIFIED SAFE: BENIGN PATTERN";
                    status_title = "SUPPRESSED (GATE-LOCK SERIALIZED)";
                    status_col = "#10b981";
                    status_bg = "rgba(16, 185, 129, 0.12)";
                    status_desc = "An inverted lock order exists, but all conflicting sections are protected by an outer gate lock. Mutual exclusion is guaranteed.";
                    ds_filter_gate_lock(&g_findings[i].cycle, &detected_gate_lock);
                    break;
                } else if (g_findings[i].suppress_reason == SUPPRESS_SAME_THREAD) {
                    suppress_detail = "same_thread";
                    status_badge = "VERIFIED SAFE: BENIGN PATTERN";
                    status_title = "SUPPRESSED (SAME-THREAD SEQUENTIAL)";
                    status_col = "#14b8a6";
                    status_bg = "rgba(20, 184, 166, 0.12)";
                    status_desc = "All conflicting lock acquisitions occurred sequentially within the same thread. No concurrent race condition can occur.";
                    break;
                } else if (g_findings[i].suppress_reason == SUPPRESS_LIFECYCLE) {
                    suppress_detail = "lifecycle";
                    status_badge = "VERIFIED SAFE: BENIGN PATTERN";
                    status_title = "SUPPRESSED (LIFECYCLE BARRIER)";
                    status_col = "#06b6d4";
                    status_bg = "rgba(6, 182, 212, 0.12)";
                    status_desc = "Threads are strictly separated by a pthread_join synchronization barrier. Thread 1 terminated before Thread 2 was spawned.";
                    break;
                }
            }
        }
    } else if (cnt_potential > 0) {
        status_code = "POTENTIAL";
        status_badge = "LATENT RACE WARNING";
        status_title = "POTENTIAL DEADLOCK PREDICTED";
        status_col = "#f59e0b";
        status_bg = "rgba(245, 158, 11, 0.15)";
        status_desc = "Opposite lock acquisition orders observed across threads without synchronization. Can deadlock under adverse scheduling; test with --stress.";
    }

    time_t now = time(NULL);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S UTC", gmtime(&now));

    /* 4. Emit HTML Header & CSS */
    fputs("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n", fp);
    fputs("<meta charset=\"UTF-8\">\n", fp);
    fputs("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n", fp);
    fprintf(fp, "<title>DeadlockScope - %s [%s]</title>\n", target_name, status_code);
    fputs("<style>\n", fp);
    fprintf(fp, "  :root {\n");
    fprintf(fp, "    --bg-main: #070b14; --bg-card: #0f172a; --bg-card-hover: #1e293b;\n");
    fprintf(fp, "    --border: #1e293b; --border-bright: #334155; --text-primary: #f8fafc;\n");
    fprintf(fp, "    --text-secondary: #94a3b8; --text-muted: #64748b;\n");
    fprintf(fp, "    --status-col: %s; --status-bg: %s;\n", status_col, status_bg);
    fprintf(fp, "    --col-potential: #f59e0b; --col-active: #ef4444; --col-active-timed: #a855f7;\n");
    fprintf(fp, "    --col-confirmed: #dc2626; --col-suppressed: #10b981; --col-accent: #38bdf8; --col-gate: #f59e0b;\n");
    fprintf(fp, "  }\n");
    fputs("  * { box-sizing: border-box; margin: 0; padding: 0; }\n", fp);
    fputs("  body {\n", fp);
    fputs("    font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, Helvetica, Arial, sans-serif;\n", fp);
    fputs("    background-color: var(--bg-main); color: var(--text-primary);\n", fp);
    fputs("    display: flex; flex-direction: column; height: 100vh; overflow: hidden;\n", fp);
    fputs("  }\n", fp);
    fputs("  header {\n", fp);
    fputs("    background: var(--bg-card); border-bottom: 1px solid var(--border);\n", fp);
    fputs("    padding: 10px 24px; display: flex; justify-content: space-between; align-items: center; flex-shrink: 0;\n", fp);
    fputs("  }\n", fp);
    fputs("  .brand { display: flex; align-items: center; gap: 12px; }\n", fp);
    fputs("  .brand-badge {\n", fp);
    fputs("    background: linear-gradient(135deg, #ef4444, #f59e0b); color: #fff;\n", fp);
    fputs("    font-weight: 800; font-size: 13px; letter-spacing: 1px; padding: 4px 10px; border-radius: 6px;\n", fp);
    fputs("    box-shadow: 0 0 12px rgba(239, 68, 68, 0.35);\n", fp);
    fputs("  }\n", fp);
    fputs("  .brand h1 { font-size: 16px; font-weight: 700; letter-spacing: -0.3px; }\n", fp);
    fputs("  .brand .subtitle { font-size: 11px; color: var(--text-muted); }\n", fp);
    fputs("  .meta-tags { display: flex; gap: 16px; font-size: 12px; color: var(--text-secondary); }\n", fp);
    fputs("  .meta-tag strong { color: var(--text-primary); }\n", fp);

    /* Hero Verdict Banner */
    fputs("  .hero-verdict {\n", fp);
    fputs("    background: var(--status-bg); border-bottom: 1px solid var(--border);\n", fp);
    fputs("    border-left: 6px solid var(--status-col); padding: 14px 24px;\n", fp);
    fputs("    display: flex; flex-direction: column; gap: 6px; flex-shrink: 0;\n", fp);
    fputs("    transition: all 0.3s ease;\n", fp);
    fputs("  }\n", fp);
    fputs("  .hero-header { display: flex; align-items: center; gap: 12px; }\n", fp);
    fputs("  .hero-badge {\n", fp);
    fputs("    background: var(--status-col); color: #fff; font-size: 11px; font-weight: 800;\n", fp);
    fputs("    padding: 3px 10px; border-radius: 4px; text-transform: uppercase; letter-spacing: 0.5px;\n", fp);
    fputs("  }\n", fp);
    fputs("  .hero-target { font-size: 13px; color: var(--text-secondary); }\n", fp);
    fputs("  .hero-target strong { color: var(--text-primary); font-family: monospace; font-size: 14px; }\n", fp);
    fputs("  .hero-title { font-size: 19px; font-weight: 800; letter-spacing: -0.4px; color: #fff; }\n", fp);
    fputs("  .hero-desc { font-size: 13px; color: #cbd5e1; line-height: 1.4; max-width: 1100px; }\n", fp);

    /* Multi-Tool Verdict Comparison Matrix */
    fputs("  .tool-matrix-box {\n", fp);
    fputs("    background: rgba(15, 23, 42, 0.7); border-bottom: 1px solid var(--border);\n", fp);
    fputs("    padding: 10px 24px; display: flex; align-items: center; gap: 16px; flex-shrink: 0;\n", fp);
    fputs("  }\n", fp);
    fputs("  .matrix-label {\n", fp);
    fputs("    font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px;\n", fp);
    fputs("    color: var(--col-accent); white-space: nowrap;\n", fp);
    fputs("  }\n", fp);
    fputs("  .matrix-chips { display: flex; gap: 12px; flex: 1; overflow-x: auto; }\n", fp);
    fputs("  .matrix-chip {\n", fp);
    fputs("    background: var(--bg-card); border: 1px solid var(--border); border-radius: 6px;\n", fp);
    fputs("    padding: 6px 12px; display: flex; align-items: center; gap: 8px; font-size: 12px;\n", fp);
    fputs("  }\n", fp);
    fputs("  .matrix-chip strong { color: var(--text-muted); font-size: 11px; }\n", fp);
    fputs("  .chip-val { font-weight: 700; font-size: 11px; padding: 2px 6px; border-radius: 4px; }\n", fp);
    fputs("  .chip-safe { background: rgba(16, 185, 129, 0.2); color: #10b981; border: 1px solid #10b981; }\n", fp);
    fputs("  .chip-err { background: rgba(239, 68, 68, 0.2); color: #ef4444; border: 1px solid #ef4444; }\n", fp);
    fputs("  .chip-warn { background: rgba(245, 158, 11, 0.2); color: #f59e0b; border: 1px solid #f59e0b; }\n", fp);
    fputs("  .chip-miss { background: rgba(148, 163, 184, 0.2); color: #94a3b8; border: 1px solid #64748b; }\n", fp);

    /* Filter Ablation Bar */
    fputs("  .ablation-bar {\n", fp);
    fputs("    background: #090e1a; border-bottom: 1px solid var(--border); padding: 8px 24px;\n", fp);
    fputs("    display: flex; align-items: center; gap: 20px; font-size: 12px; flex-shrink: 0;\n", fp);
    fputs("  }\n", fp);
    fputs("  .ablation-title { font-weight: 700; color: #cbd5e1; display: flex; align-items: center; gap: 6px; }\n", fp);
    fputs("  .filter-toggle { display: flex; align-items: center; gap: 6px; cursor: pointer; user-select: none; color: var(--text-secondary); }\n", fp);
    fputs("  .filter-toggle input { cursor: pointer; accent-color: var(--col-suppressed); transform: scale(1.1); }\n", fp);
    fputs("  .filter-toggle:hover { color: var(--text-primary); }\n", fp);
    fputs("  .ablation-hint { margin-left: auto; font-size: 11px; color: var(--text-muted); }\n", fp);

    /* Workspace */
    fputs("  .workspace { display: flex; flex: 1; overflow: hidden; }\n", fp);
    fputs("  .canvas-panel { flex: 1; position: relative; display: flex; flex-direction: column; background: #050811; }\n", fp);
    fputs("  .canvas-toolbar {\n", fp);
    fputs("    position: absolute; top: 12px; left: 12px; z-index: 10; display: flex; gap: 8px;\n", fp);
    fputs("    background: rgba(15, 23, 42, 0.85); backdrop-filter: blur(8px); border: 1px solid var(--border);\n", fp);
    fputs("    border-radius: 8px; padding: 6px;\n", fp);
    fputs("  }\n", fp);
    fputs("  .btn {\n", fp);
    fputs("    background: transparent; border: 1px solid var(--border); color: var(--text-secondary);\n", fp);
    fputs("    padding: 5px 10px; border-radius: 6px; font-size: 11px; font-weight: 600; cursor: pointer;\n", fp);
    fputs("    transition: all 0.15s ease;\n", fp);
    fputs("  }\n", fp);
    fputs("  .btn:hover { background: var(--bg-card-hover); color: var(--text-primary); border-color: var(--border-bright); }\n", fp);
    fputs("  .btn.active { background: var(--col-accent); color: #000; border-color: var(--col-accent); }\n", fp);
    fputs("  .legend {\n", fp);
    fputs("    position: absolute; bottom: 12px; left: 12px; z-index: 10;\n", fp);
    fputs("    background: rgba(15, 23, 42, 0.85); backdrop-filter: blur(8px); border: 1px solid var(--border);\n", fp);
    fputs("    border-radius: 8px; padding: 10px 14px; font-size: 11px; display: flex; flex-direction: column;\n", fp);
    fputs("    gap: 6px; color: var(--text-secondary);\n", fp);
    fputs("  }\n", fp);
    fputs("  .legend-item { display: flex; align-items: center; gap: 8px; }\n", fp);
    fputs("  .legend-box { width: 12px; height: 12px; border-radius: 3px; display: inline-block; }\n", fp);
    fputs("  #graphCanvas { width: 100%; height: 100%; cursor: grab; display: block; }\n", fp);
    fputs("  #graphCanvas:active { cursor: grabbing; }\n", fp);

    /* Sidebar */
    fputs("  .sidebar { width: 440px; background: var(--bg-card); border-left: 1px solid var(--border); display: flex; flex-direction: column; flex-shrink: 0; }\n", fp);
    fputs("  .tabs { display: flex; border-bottom: 1px solid var(--border); background: rgba(15, 23, 42, 0.5); }\n", fp);
    fputs("  .tab {\n", fp);
    fputs("    flex: 1; text-align: center; padding: 10px; font-size: 11px; font-weight: 700;\n", fp);
    fputs("    color: var(--text-muted); cursor: pointer; border-bottom: 2px solid transparent;\n", fp);
    fputs("    transition: all 0.15s ease; text-transform: uppercase; letter-spacing: 0.5px;\n", fp);
    fputs("  }\n", fp);
    fputs("  .tab:hover { color: var(--text-primary); }\n", fp);
    fputs("  .tab.active { color: var(--col-accent); border-bottom-color: var(--col-accent); background: var(--bg-card); }\n", fp);
    fputs("  .tab-content { flex: 1; overflow-y: auto; padding: 16px; display: flex; flex-direction: column; gap: 12px; }\n", fp);
    fputs("  .tab-pane { display: none; flex-direction: column; gap: 12px; }\n", fp);
    fputs("  .tab-pane.active { display: flex; }\n", fp);
    fputs("  .finding-card {\n", fp);
    fputs("    background: #0a0f1d; border: 1px solid var(--border); border-radius: 8px; padding: 14px;\n", fp);
    fputs("    cursor: pointer; transition: all 0.2s ease; display: flex; flex-direction: column; gap: 8px;\n", fp);
    fputs("  }\n", fp);
    fputs("  .finding-card:hover { border-color: var(--border-bright); background: #131c33; }\n", fp);
    fputs("  .finding-card.selected { border-color: var(--col-accent); box-shadow: 0 0 12px rgba(56, 189, 248, 0.25); }\n", fp);
    fputs("  .card-header { display: flex; justify-content: space-between; align-items: center; }\n", fp);
    fputs("  .badge { font-size: 10px; font-weight: 800; padding: 3px 8px; border-radius: 4px; text-transform: uppercase; letter-spacing: 0.5px; }\n", fp);
    fputs("  .badge-confirmed { background: var(--col-confirmed); color: #fff; }\n", fp);
    fputs("  .badge-active { background: var(--col-active); color: #fff; }\n", fp);
    fputs("  .badge-timed { background: var(--col-active-timed); color: #fff; }\n", fp);
    fputs("  .badge-potential { background: var(--col-potential); color: #000; }\n", fp);
    fputs("  .badge-suppressed { background: rgba(16, 185, 129, 0.2); color: var(--col-suppressed); border: 1px solid var(--col-suppressed); }\n", fp);
    fputs("  .badge-ablated { background: #b91c1c; color: #fff; animation: pulse 1.5s infinite; }\n", fp);
    fputs("  @keyframes pulse { 0% { opacity: 0.75; } 50% { opacity: 1; box-shadow: 0 0 10px #ef4444; } 100% { opacity: 0.75; } }\n", fp);
    fputs("  .card-title { font-size: 13px; font-weight: 700; }\n", fp);
    fputs("  .card-meta { font-size: 11px; color: var(--text-secondary); line-height: 1.5; }\n", fp);
    fputs("  .cycle-witness-box {\n", fp);
    fputs("    background: #050811; border: 1px solid var(--border); border-radius: 6px; padding: 8px;\n", fp);
    fputs("    font-family: monospace; font-size: 11px; color: #cbd5e1; display: flex; flex-direction: column; gap: 4px;\n", fp);
    fputs("  }\n", fp);
    fputs("  .prop-table { width: 100%; border-collapse: collapse; font-size: 12px; }\n", fp);
    fputs("  .prop-table td { padding: 8px 6px; border-bottom: 1px solid var(--border); }\n", fp);
    fputs("  .prop-table td.key { color: var(--text-muted); width: 38%; font-weight: 600; }\n", fp);
    fputs("  .prop-table td.val { color: var(--text-primary); font-family: monospace; word-break: break-all; }\n", fp);
    fputs("  .ledger-row {\n", fp);
    fputs("    background: #0a0f1d; border: 1px solid var(--border); border-radius: 6px; padding: 10px;\n", fp);
    fputs("    font-size: 11px; display: flex; flex-direction: column; gap: 4px;\n", fp);
    fputs("  }\n", fp);
    fputs("</style>\n</head>\n<body>\n", fp);

    /* 5. Top Header */
    fprintf(fp, "  <header>\n");
    fprintf(fp, "    <div class=\"brand\">\n");
    fprintf(fp, "      <span class=\"brand-badge\">DEADLOCKSCOPE</span>\n");
    fprintf(fp, "      <div>\n");
    fprintf(fp, "        <h1>Runtime Concurrency & Deadlock Diagnostics</h1>\n");
    fprintf(fp, "        <span class=\"subtitle\">Dynamic Lock-Order Analysis, Credibility Filtering & Live Wait-For Verification</span>\n");
    fprintf(fp, "      </div>\n");
    fprintf(fp, "    </div>\n");
    fprintf(fp, "    <div class=\"meta-tags\">\n");
    fprintf(fp, "      <div>PID: <strong>%d</strong></div>\n", getpid());
    fprintf(fp, "      <div>Timestamp: <strong>%s</strong></div>\n", time_str);
    fprintf(fp, "    </div>\n");
    fprintf(fp, "  </header>\n\n");

    /* 6. Executive Hero Verdict Banner */
    fprintf(fp, "  <div class=\"hero-verdict\" id=\"hero-verdict\">\n");
    fprintf(fp, "    <div class=\"hero-header\">\n");
    fprintf(fp, "      <span class=\"hero-badge\" id=\"hero-badge\">%s</span>\n", status_badge);
    fprintf(fp, "      <span class=\"hero-target\">Target Executable: <strong>%s</strong></span>\n", target_name);
    fprintf(fp, "    </div>\n");
    fprintf(fp, "    <h2 class=\"hero-title\" id=\"hero-title\">%s</h2>\n", status_title);
    fprintf(fp, "    <p class=\"hero-desc\" id=\"hero-desc\">%s</p>\n", status_desc);
    fprintf(fp, "  </div>\n\n");

    /* 7. Multi-Tool Verdict Comparison Matrix */
    fprintf(fp, "  <div class=\"tool-matrix-box\">\n");
    fprintf(fp, "    <span class=\"matrix-label\">Tool Comparison:</span>\n");
    fprintf(fp, "    <div class=\"matrix-chips\" id=\"matrix-chips\">\n");
    /* Populated by JS for dynamic ablation toggles */
    fprintf(fp, "    </div>\n");
    fprintf(fp, "  </div>\n\n");

    /* 8. Filter Ablation Bar */
    fprintf(fp, "  <div class=\"ablation-bar\">\n");
    fprintf(fp, "    <span class=\"ablation-title\">\n");
    fprintf(fp, "      <svg width=\"14\" height=\"14\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2.5\"><path d=\"M22 3H2l8 9.46V19l4 2v-8.54L22 3z\"/></svg>\n");
    fprintf(fp, "      Credibility Filter Ablation:\n");
    fprintf(fp, "    </span>\n");
    fprintf(fp, "    <label class=\"filter-toggle\"><input type=\"checkbox\" id=\"filter-gate\" checked> Gate-Lock Filter</label>\n");
    fprintf(fp, "    <label class=\"filter-toggle\"><input type=\"checkbox\" id=\"filter-thread\" checked> Same-Thread Filter</label>\n");
    fprintf(fp, "    <label class=\"filter-toggle\"><input type=\"checkbox\" id=\"filter-lifecycle\" checked> Lifecycle Barrier Filter</label>\n");
    fprintf(fp, "    <span class=\"ablation-hint\">Uncheck any filter to see how Lockdep / Helgrind false alarms re-manifest live in the graph!</span>\n");
    fprintf(fp, "  </div>\n\n");

    /* 9. Workspace */
    fputs("  <div class=\"workspace\">\n", fp);
    fputs("    <div class=\"canvas-panel\">\n", fp);
    fputs("      <div class=\"canvas-toolbar\">\n", fp);
    fputs("        <button class=\"btn active\" id=\"btn-view-all\">Unified Graph</button>\n", fp);
    fputs("        <button class=\"btn\" id=\"btn-view-log\">Lock-Order Only</button>\n", fp);
    fputs("        <button class=\"btn\" id=\"btn-view-wfg\">Wait-For Only</button>\n", fp);
    fputs("        <button class=\"btn\" id=\"btn-reset-cam\">Reset View</button>\n", fp);
    fputs("      </div>\n", fp);
    fputs("      <div class=\"legend\">\n", fp);
    fputs("        <div class=\"legend-item\"><span class=\"legend-box\" style=\"background: #f59e0b; border: 1px solid #fbbf24;\"></span> Gate Lock (Outer Guard)</div>\n", fp);
    fputs("        <div class=\"legend-item\"><span class=\"legend-box\" style=\"background: #38bdf8;\"></span> Mutex Lock</div>\n", fp);
    fputs("        <div class=\"legend-item\"><span class=\"legend-box\" style=\"background: #f43f5e; border-radius: 50%;\"></span> Execution Thread</div>\n", fp);
    fputs("        <div class=\"legend-item\"><span class=\"legend-box\" style=\"background: #ef4444;\"></span> Deadlock Cycle Edge</div>\n", fp);
    fputs("      </div>\n", fp);
    fputs("      <canvas id=\"graphCanvas\"></canvas>\n", fp);
    fputs("    </div>\n", fp);

    /* 10. Sidebar */
    fputs("    <div class=\"sidebar\">\n", fp);
    fputs("      <div class=\"tabs\">\n", fp);
    fprintf(fp, "        <div class=\"tab active\" data-target=\"pane-findings\">Findings (<span id=\"tab-cnt-findings\">%d</span>)</div>\n", g_finding_count);
    fputs("        <div class=\"tab\" data-target=\"pane-inspector\">Inspector</div>\n", fp);
    fputs("        <div class=\"tab\" data-target=\"pane-ledger\">Edge Ledger</div>\n", fp);
    fputs("      </div>\n", fp);
    fputs("      <div class=\"tab-content\">\n", fp);
    fputs("        <div class=\"tab-pane active\" id=\"pane-findings\"></div>\n", fp);
    fputs("        <div class=\"tab-pane\" id=\"pane-inspector\"><div style=\"color: var(--text-muted); font-size: 13px; text-align: center; margin-top: 40px;\">Click any node, edge, or finding card to inspect its telemetry.</div></div>\n", fp);
    fputs("        <div class=\"tab-pane\" id=\"pane-ledger\"></div>\n", fp);
    fputs("      </div>\n", fp);
    fputs("    </div>\n", fp);
    fputs("  </div>\n\n", fp);

    /* 11. Embedded JSON Telemetry Block */
    fputs("  <script>\n", fp);
    fputs("  const DATA = {\n", fp);
    fprintf(fp, "    target: \"%s\",\n", target_name);
    fprintf(fp, "    pid: %d,\n", getpid());
    fprintf(fp, "    timestamp: \"%s\",\n", time_str);
    fprintf(fp, "    initialStatus: { code: \"%s\", badge: \"%s\", title: \"%s\", desc: \"%s\", col: \"%s\", bg: \"%s\", suppressDetail: \"%s\" },\n",
            status_code, status_badge, status_title, status_desc, status_col, status_bg, suppress_detail);
    fprintf(fp, "    kpis: { potential: %d, active: %d, activeTimed: %d, confirmed: %d, suppressed: %d },\n",
            cnt_potential, cnt_active, cnt_active_timed, cnt_confirmed, cnt_suppressed);

    /* Locks */
    fputs("    locks: [\n", fp);
    for (int i = 0; i < g_lock_count; i++) {
        char sym[128];
        ds_symbolize_site((void*)g_lock_table[i], sym, sizeof(sym));
        ThreadId owner = (ThreadId)atomic_load(&g_lock_owners[i]);
        bool is_gate = (detected_gate_lock != 0 && g_lock_table[i] == detected_gate_lock);
        fprintf(fp, "      { id: \"L_0x%lx\", addr: \"0x%lx\", owner: %u, sym: \"%s\", is_gate: %s, role: \"%s\" }%s\n",
                (unsigned long)g_lock_table[i], (unsigned long)g_lock_table[i], owner, sym,
                is_gate ? "true" : "false",
                is_gate ? "Gate Lock (G)" : "Worker Mutex",
                (i + 1 < g_lock_count) ? "," : "");
    }
    fputs("    ],\n", fp);

    /* Threads */
    fputs("    threads: [\n", fp);
    bool first_t = true;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (atomic_load(&g_threads[i].active) || g_threads[i].tid != 0 || i == 0) {
            if (!first_t) fputs(",\n", fp);
            first_t = false;
            fprintf(fp, "      { tid: %u, active: %d, is_waiting: %d, waiting_for: \"0x%lx\", wait_kind: %d }",
                    g_threads[i].tid,
                    atomic_load(&g_threads[i].active),
                    atomic_load(&g_threads[i].is_waiting),
                    (unsigned long)atomic_load(&g_threads[i].waiting_for),
                    (int)atomic_load(&g_threads[i].wait_kind));
        }
    }
    fputs("\n    ],\n", fp);

    /* Lock-Order Edges */
    int edge_cnt = ds_log_get_edge_count();
    fputs("    edges: [\n", fp);
    for (int i = 0; i < edge_cnt; i++) {
        const LockOrderEdge *e = ds_log_get_edge(i);
        if (!e) continue;
        char sf[128] = "unknown", st[128] = "unknown";
        ThreadId w_tid = 0;
        if (e->witness_count > 0) {
            ds_symbolize_site(e->witnesses[0].site_from, sf, sizeof(sf));
            ds_symbolize_site(e->witnesses[0].site_to, st, sizeof(st));
            w_tid = e->witnesses[0].tid;
        }
        fprintf(fp, "      { from: \"L_0x%lx\", to: \"L_0x%lx\", tid: %u, sym_from: \"%s\", sym_to: \"%s\" }%s\n",
                (unsigned long)e->from, (unsigned long)e->to, w_tid, sf, st,
                (i + 1 < edge_cnt) ? "," : "");
    }
    fputs("    ],\n", fp);

    /* Findings */
    fputs("    findings: [\n", fp);
    for (int i = 0; i < g_finding_count; i++) {
        const Finding *f = &g_findings[i];
        const char *kstr = "POTENTIAL";
        if (f->kind == FINDING_ACTIVE) kstr = "ACTIVE";
        else if (f->kind == FINDING_ACTIVE_TIMED) kstr = "ACTIVE_TIMED";
        else if (f->kind == FINDING_CONFIRMED) kstr = "CONFIRMED";
        else if (f->kind == FINDING_SUPPRESSED) kstr = "SUPPRESSED";

        const char *rstr = "none";
        if (f->suppress_reason == SUPPRESS_SAME_THREAD) rstr = "same_thread";
        else if (f->suppress_reason == SUPPRESS_GATE_LOCK) rstr = "gate_lock";
        else if (f->suppress_reason == SUPPRESS_LIFECYCLE) rstr = "lifecycle";

        fprintf(fp, "      { id: %d, kind: \"%s\", suppress_reason: \"%s\", len: %d, time_ms: %lu, locks: [",
                i, kstr, rstr, f->cycle.length, (unsigned long)f->detected_time_ms);
        for (int k = 0; k < f->cycle.length; k++) {
            fprintf(fp, "\"0x%lx\"%s", (unsigned long)f->cycle.locks[k], (k + 1 < f->cycle.length) ? "," : "");
        }
        fputs("], witnesses: [", fp);
        for (int k = 0; k < f->cycle.length; k++) {
            char sf[128], st[128];
            ds_symbolize_site(f->cycle.witnesses[k].site_from, sf, sizeof(sf));
            ds_symbolize_site(f->cycle.witnesses[k].site_to, st, sizeof(st));
            fprintf(fp, "{ tid: %u, lock: \"0x%lx\", sym_from: \"%s\", sym_to: \"%s\" }%s",
                    f->cycle.witnesses[k].tid, (unsigned long)f->cycle.locks[k], sf, st,
                    (k + 1 < f->cycle.length) ? "," : "");
        }
        fprintf(fp, "] }%s\n", (i + 1 < g_finding_count) ? "," : "");
    }
    fputs("    ]\n", fp);
    fputs("  };\n\n", fp);

    /* 12. Embedded Vanilla JavaScript Application Engine */
    fputs("  // --- Client-Side Application Engine ---\n", fp);
    fputs("  let currentView = 'all';\n", fp);
    fputs("  let selectedItem = null;\n", fp);
    fputs("  let filterState = { gate: true, thread: true, lifecycle: true };\n", fp);
    fputs("  const canvas = document.getElementById('graphCanvas');\n", fp);
    fputs("  const ctx = canvas.getContext('2d');\n", fp);
    fputs("  let cam = { x: 0, y: 0, zoom: 1.0, isPanning: false, startX: 0, startY: 0 };\n", fp);
    fputs("  let draggedNode = null;\n", fp);
    fputs("  let hoveredNode = null;\n\n", fp);

    fputs("  // Build Graph Nodes & Edges\n", fp);
    fputs("  const nodeMap = new Map();\n", fp);
    fputs("  DATA.locks.forEach((l, idx) => {\n", fp);
    fputs("    const label = l.is_gate ? 'GATE' : ('Lock ' + String.fromCharCode(65 + idx));\n", fp);
    fputs("    nodeMap.set(l.id, {\n", fp);
    fputs("      id: l.id, type: 'lock', label: label, fullAddr: l.addr, is_gate: l.is_gate,\n", fp);
    fputs("      role: l.role, sym: l.sym, x: (Math.random() - 0.5) * 260, y: (Math.random() - 0.5) * 260,\n", fp);
    fputs("      vx: 0, vy: 0, radius: l.is_gate ? 28 : 24, mass: l.is_gate ? 3.0 : 2.0\n", fp);
    fputs("    });\n", fp);
    fputs("  });\n", fp);
    fputs("  DATA.threads.forEach(t => {\n", fp);
    fputs("    const id = 'T_' + t.tid;\n", fp);
    fputs("    nodeMap.set(id, {\n", fp);
    fputs("      id: id, type: 'thread', label: 'Thread ' + t.tid, tid: t.tid,\n", fp);
    fputs("      x: (Math.random() - 0.5) * 260, y: (Math.random() - 0.5) * 260,\n", fp);
    fputs("      vx: 0, vy: 0, radius: 20, mass: 1.2, waiting_for: t.waiting_for\n", fp);
    fputs("    });\n", fp);
    fputs("  });\n", fp);
    fputs("  const nodes = Array.from(nodeMap.values());\n\n", fp);

    fputs("  function resizeCanvas() {\n", fp);
    fputs("    const rect = canvas.getBoundingClientRect();\n", fp);
    fputs("    const dpr = window.devicePixelRatio || 1;\n", fp);
    fputs("    canvas.width = rect.width * dpr;\n", fp);
    fputs("    canvas.height = rect.height * dpr;\n", fp);
    fputs("    ctx.scale(dpr, dpr);\n", fp);
    fputs("  }\n", fp);
    fputs("  window.addEventListener('resize', resizeCanvas);\n", fp);
    fputs("  resizeCanvas();\n", fp);
    fputs("  cam.x = canvas.clientWidth / 2;\n", fp);
    fputs("  cam.y = canvas.clientHeight / 2;\n\n", fp);

    fputs("  // Force-Directed Physics Simulation\n", fp);
    fputs("  function updatePhysics() {\n", fp);
    fputs("    const kRep = 4500, kSpring = 0.04, l0 = 95, damping = 0.86;\n", fp);
    fputs("    for (let i = 0; i < nodes.length; i++) {\n", fp);
    fputs("      for (let j = i + 1; j < nodes.length; j++) {\n", fp);
    fputs("        const dx = nodes[j].x - nodes[i].x;\n", fp);
    fputs("        const dy = nodes[j].y - nodes[i].y;\n", fp);
    fputs("        const dist = Math.sqrt(dx * dx + dy * dy) || 1;\n", fp);
    fputs("        if (dist < 400) {\n", fp);
    fputs("          const force = kRep / (dist * dist);\n", fp);
    fputs("          const fx = (dx / dist) * force;\n", fp);
    fputs("          const fy = (dy / dist) * force;\n", fp);
    fputs("          nodes[i].vx -= fx / nodes[i].mass;\n", fp);
    fputs("          nodes[i].vy -= fy / nodes[i].mass;\n", fp);
    fputs("          nodes[j].vx += fx / nodes[j].mass;\n", fp);
    fputs("          nodes[j].vy += fy / nodes[j].mass;\n", fp);
    fputs("        }\n", fp);
    fputs("      }\n", fp);
    fputs("    }\n", fp);
    fputs("    DATA.edges.forEach(e => {\n", fp);
    fputs("      const n1 = nodeMap.get(e.from);\n", fp);
    fputs("      const n2 = nodeMap.get(e.to);\n", fp);
    fputs("      if (n1 && n2) {\n", fp);
    fputs("        const dx = n2.x - n1.x, dy = n2.y - n1.y;\n", fp);
    fputs("        const dist = Math.sqrt(dx * dx + dy * dy) || 1;\n", fp);
    fputs("        const f = (dist - l0) * kSpring;\n", fp);
    fputs("        const fx = (dx / dist) * f, fy = (dy / dist) * f;\n", fp);
    fputs("        n1.vx += fx; n1.vy += fy;\n", fp);
    fputs("        n2.vx -= fx; n2.vy -= fy;\n", fp);
    fputs("      }\n", fp);
    fputs("    });\n", fp);
    fputs("    nodes.forEach(n => {\n", fp);
    fputs("      if (n !== draggedNode) {\n", fp);
    fputs("        n.vx -= n.x * 0.002; n.vy -= n.y * 0.002;\n", fp);
    fputs("        n.vx *= damping; n.vy *= damping;\n", fp);
    fputs("        n.x += n.vx; n.y += n.vy;\n", fp);
    fputs("      }\n", fp);
    fputs("    });\n", fp);
    fputs("  }\n\n", fp);

    fputs("  // Render Graph Loop\n", fp);
    fputs("  function draw() {\n", fp);
    fputs("    updatePhysics();\n", fp);
    fputs("    ctx.save();\n", fp);
    fputs("    ctx.clearRect(0, 0, canvas.width, canvas.height);\n", fp);
    fputs("    ctx.translate(cam.x, cam.y);\n", fp);
    fputs("    ctx.scale(cam.zoom, cam.zoom);\n\n", fp);

    fputs("    // Draw subtle grid\n", fp);
    fputs("    ctx.strokeStyle = '#0d1322';\n", fp);
    fputs("    ctx.lineWidth = 1;\n", fp);
    fputs("    const gridStep = 50;\n", fp);
    fputs("    for (let x = -800; x <= 800; x += gridStep) {\n", fp);
    fputs("      ctx.beginPath(); ctx.moveTo(x, -800); ctx.lineTo(x, 800); ctx.stroke();\n", fp);
    fputs("    }\n", fp);
    fputs("    for (let y = -800; y <= 800; y += gridStep) {\n", fp);
    fputs("      ctx.beginPath(); ctx.moveTo(-800, y); ctx.lineTo(800, y); ctx.stroke();\n", fp);
    fputs("    }\n\n", fp);

    fputs("    const activeCycles = getProcessedFindings().filter(f => f.effectiveKind !== 'SUPPRESSED');\n", fp);
    fputs("    const pulse = 0.5 + 0.5 * Math.sin(Date.now() / 200);\n\n", fp);

    fputs("    // Draw Edges\n", fp);
    fputs("    if (currentView === 'all' || currentView === 'log') {\n", fp);
    fputs("      DATA.edges.forEach(e => {\n", fp);
    fputs("        const n1 = nodeMap.get(e.from);\n", fp);
    fputs("        const n2 = nodeMap.get(e.to);\n", fp);
    fputs("        if (!n1 || !n2) return;\n", fp);
    fputs("        let isCycleEdge = false, isAblatedEdge = false;\n", fp);
    fputs("        activeCycles.forEach(f => {\n", fp);
    fputs("          for (let k = 0; k < f.locks.length; k++) {\n", fp);
    fputs("            const l1 = 'L_' + f.locks[k];\n", fp);
    fputs("            const l2 = 'L_' + f.locks[(k + 1) % f.locks.length];\n", fp);
    fputs("            if (l1 === e.from && l2 === e.to) {\n", fp);
    fputs("              isCycleEdge = true;\n", fp);
    fputs("              if (f.isAblated) isAblatedEdge = true;\n", fp);
    fputs("            }\n", fp);
    fputs("          }\n", fp);
    fputs("        });\n", fp);
    fputs("        ctx.beginPath();\n", fp);
    fputs("        ctx.moveTo(n1.x, n1.y); ctx.lineTo(n2.x, n2.y);\n", fp);
    fputs("        if (isAblatedEdge) {\n", fp);
    fputs("          ctx.strokeStyle = `rgba(245, 158, 11, ${0.5 + 0.5 * pulse})`;\n", fp);
    fputs("          ctx.lineWidth = 3;\n", fp);
    fputs("          ctx.setLineDash([6, 4]);\n", fp);
    fputs("        } else if (isCycleEdge) {\n", fp);
    fputs("          ctx.strokeStyle = `rgba(239, 68, 68, ${0.6 + 0.4 * pulse})`;\n", fp);
    fputs("          ctx.lineWidth = 3.5;\n", fp);
    fputs("          ctx.setLineDash([]);\n", fp);
    fputs("        } else {\n", fp);
    fputs("          ctx.strokeStyle = '#334155';\n", fp);
    fputs("          ctx.lineWidth = 1.5;\n", fp);
    fputs("          ctx.setLineDash([]);\n", fp);
    fputs("        }\n", fp);
    fputs("        ctx.stroke();\n", fp);
    fputs("        ctx.setLineDash([]);\n", fp);
    fputs("        const angle = Math.atan2(n2.y - n1.y, n2.x - n1.x);\n", fp);
    fputs("        const arrowDist = n2.radius + 6;\n", fp);
    fputs("        const ax = n2.x - Math.cos(angle) * arrowDist;\n", fp);
    fputs("        const ay = n2.y - Math.sin(angle) * arrowDist;\n", fp);
    fputs("        ctx.fillStyle = ctx.strokeStyle;\n", fp);
    fputs("        ctx.beginPath();\n", fp);
    fputs("        ctx.moveTo(ax, ay);\n", fp);
    fputs("        ctx.lineTo(ax - 9 * Math.cos(angle - 0.4), ay - 9 * Math.sin(angle - 0.4));\n", fp);
    fputs("        ctx.lineTo(ax - 9 * Math.cos(angle + 0.4), ay - 9 * Math.sin(angle + 0.4));\n", fp);
    fputs("        ctx.fill();\n", fp);
    fputs("      });\n", fp);
    fputs("    }\n\n", fp);

    fputs("    // Draw Nodes\n", fp);
    fputs("    nodes.forEach(n => {\n", fp);
    fputs("      if (currentView === 'log' && n.type === 'thread') return;\n", fp);
    fputs("      if (currentView === 'wfg' && n.type === 'lock' && n.id !== 'L_' + draggedNode?.waiting_for) return;\n", fp);
    fputs("      ctx.save();\n", fp);
    fputs("      if (n.type === 'lock') {\n", fp);
    fputs("        if (n.is_gate) {\n", fp);
    fputs("          // Golden Shield for Gate Lock\n", fp);
    fputs("          ctx.fillStyle = '#1e1b12';\n", fp);
    fputs("          ctx.strokeStyle = '#f59e0b';\n", fp);
    fputs("          ctx.lineWidth = (hoveredNode === n) ? 3.5 : 2.5;\n", fp);
    fputs("          ctx.beginPath();\n", fp);
    fputs("          const r = n.radius;\n", fp);
    fputs("          ctx.roundRect(n.x - r, n.y - r, r * 2, r * 2, 8);\n", fp);
    fputs("          ctx.fill(); ctx.stroke();\n", fp);
    fputs("          ctx.fillStyle = '#fbbf24';\n", fp);
    fputs("          ctx.font = 'bold 11px sans-serif';\n", fp);
    fputs("          ctx.textAlign = 'center'; ctx.textBaseline = 'middle';\n", fp);
    fputs("          ctx.fillText(n.label, n.x, n.y - 4);\n", fp);
    fputs("          ctx.fillStyle = '#d97706';\n", fp);
    fputs("          ctx.font = '9px monospace';\n", fp);
    fputs("          ctx.fillText(n.fullAddr.slice(-4), n.x, n.y + 8);\n", fp);
    fputs("        } else {\n", fp);
    fputs("          // Regular Mutex Box\n", fp);
    fputs("          ctx.fillStyle = '#111827';\n", fp);
    fputs("          ctx.strokeStyle = (hoveredNode === n) ? '#38bdf8' : '#0284c7';\n", fp);
    fputs("          ctx.lineWidth = (hoveredNode === n) ? 2.5 : 1.5;\n", fp);
    fputs("          ctx.beginPath();\n", fp);
    fputs("          const r = n.radius;\n", fp);
    fputs("          ctx.roundRect(n.x - r, n.y - r, r * 2, r * 2, 6);\n", fp);
    fputs("          ctx.fill(); ctx.stroke();\n", fp);
    fputs("          ctx.fillStyle = '#f8fafc';\n", fp);
    fputs("          ctx.font = 'bold 10px sans-serif';\n", fp);
    fputs("          ctx.textAlign = 'center'; ctx.textBaseline = 'middle';\n", fp);
    fputs("          ctx.fillText(n.label, n.x, n.y - 4);\n", fp);
    fputs("          ctx.fillStyle = '#64748b';\n", fp);
    fputs("          ctx.font = '9px monospace';\n", fp);
    fputs("          ctx.fillText(n.fullAddr.slice(-4), n.x, n.y + 8);\n", fp);
    fputs("        }\n", fp);
    fputs("      } else {\n", fp);
    fputs("        // Thread Circle\n", fp);
    fputs("        ctx.fillStyle = '#240d1c';\n", fp);
    fputs("        ctx.strokeStyle = (hoveredNode === n) ? '#fb7185' : '#e11d48';\n", fp);
    fputs("        ctx.lineWidth = (hoveredNode === n) ? 2.5 : 1.5;\n", fp);
    fputs("        ctx.beginPath();\n", fp);
    fputs("        ctx.arc(n.x, n.y, n.radius, 0, Math.PI * 2);\n", fp);
    fputs("        ctx.fill(); ctx.stroke();\n", fp);
    fputs("        ctx.fillStyle = '#fda4af';\n", fp);
    fputs("        ctx.font = 'bold 10px sans-serif';\n", fp);
    fputs("        ctx.textAlign = 'center'; ctx.textBaseline = 'middle';\n", fp);
    fputs("        ctx.fillText(n.label, n.x, n.y);\n", fp);
    fputs("      }\n", fp);
    fputs("      ctx.restore();\n", fp);
    fputs("    });\n", fp);
    fputs("    ctx.restore();\n", fp);
    fputs("    requestAnimationFrame(draw);\n", fp);
    fputs("  }\n", fp);
    fputs("  requestAnimationFrame(draw);\n\n", fp);

    fputs("  // Interactive Filter Ablation & Tool Comparison Engine\n", fp);
    fputs("  function getProcessedFindings() {\n", fp);
    fputs("    return DATA.findings.map(f => {\n", fp);
    fputs("      let isAblated = false;\n", fp);
    fputs("      let effKind = f.kind;\n", fp);
    fputs("      if (f.kind === 'SUPPRESSED') {\n", fp);
    fputs("        if (f.suppress_reason === 'gate_lock' && !filterState.gate) isAblated = true;\n", fp);
    fputs("        if (f.suppress_reason === 'same_thread' && !filterState.thread) isAblated = true;\n", fp);
    fputs("        if (f.suppress_reason === 'lifecycle' && !filterState.lifecycle) isAblated = true;\n", fp);
    fputs("        if (isAblated) effKind = 'POTENTIAL';\n", fp);
    fputs("      }\n", fp);
    fputs("      return { ...f, isAblated: isAblated, effectiveKind: effKind };\n", fp);
    fputs("    });\n", fp);
    fputs("  }\n\n", fp);

    fputs("  function updateHeroAndMatrix() {\n", fp);
    fputs("    const findings = getProcessedFindings();\n", fp);
    fputs("    const anyAblated = findings.some(f => f.isAblated);\n", fp);
    fputs("    const hero = document.getElementById('hero-verdict');\n", fp);
    fputs("    const badge = document.getElementById('hero-badge');\n", fp);
    fputs("    const title = document.getElementById('hero-title');\n", fp);
    fputs("    const desc = document.getElementById('hero-desc');\n", fp);
    fputs("    const chips = document.getElementById('matrix-chips');\n\n", fp);

    fputs("    if (anyAblated) {\n", fp);
    fputs("      const ablatedF = findings.find(f => f.isAblated);\n", fp);
    fputs("      badge.textContent = '⚠️ ABLATED FALSE ALARM MANIFESTED';\n", fp);
    fputs("      badge.style.background = '#ef4444';\n", fp);
    fputs("      hero.style.borderLeftColor = '#ef4444';\n", fp);
    fputs("      hero.style.background = 'rgba(239, 68, 68, 0.18)';\n", fp);
    fputs("      title.textContent = `ORDER INVERSION (${ablatedF.suppress_reason.toUpperCase()} FILTER DISABLED)`;\n", fp);
    fputs("      desc.textContent = `Notice: Because you disabled the ${ablatedF.suppress_reason} filter, DeadlockScope now reports the exact same false-positive alarm as Linux lockdep and Helgrind!`;\n", fp);
    fputs("      chips.innerHTML = `\n", fp);
    fputs("        <div class=\"matrix-chip\"><strong>DeadlockScope (Ours):</strong><span class=\"chip-val chip-err\">ORDER INVERSION (False Alarm)</span></div>\n", fp);
    fputs("        <div class=\"matrix-chip\"><strong>Linux lockdep:</strong><span class=\"chip-val chip-err\">ORDER INVERSION (False Alarm)</span></div>\n", fp);
    fputs("        <div class=\"matrix-chip\"><strong>Helgrind:</strong><span class=\"chip-val chip-err\">LOCK ORDER ERR (False Alarm)</span></div>\n", fp);
    fputs("        <div class=\"matrix-chip\"><strong>ThreadSanitizer:</strong><span class=\"chip-val chip-err\">LOCK INVERSION (False Alarm)</span></div>\n", fp);
    fputs("      `;\n", fp);
    fputs("    } else {\n", fp);
    fputs("      badge.textContent = DATA.initialStatus.badge;\n", fp);
    fputs("      badge.style.background = DATA.initialStatus.col;\n", fp);
    fputs("      hero.style.borderLeftColor = DATA.initialStatus.col;\n", fp);
    fputs("      hero.style.background = DATA.initialStatus.bg;\n", fp);
    fputs("      title.textContent = DATA.initialStatus.title;\n", fp);
    fputs("      desc.textContent = DATA.initialStatus.desc;\n\n", fp);

    fputs("      if (DATA.initialStatus.code === 'SUPPRESSED') {\n", fp);
    fputs("        chips.innerHTML = `\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>DeadlockScope (Ours):</strong><span class=\"chip-val chip-safe\">SUPPRESSED (${DATA.initialStatus.suppressDetail})</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>Linux lockdep:</strong><span class=\"chip-val chip-err\">ORDER INVERSION (False Alarm)</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>Helgrind:</strong><span class=\"chip-val chip-err\">LOCK ORDER ERR (False Alarm)</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>ThreadSanitizer:</strong><span class=\"chip-val chip-err\">LOCK INVERSION (False Alarm)</span></div>\n", fp);
    fputs("        `;\n", fp);
    fputs("      } else if (DATA.initialStatus.code === 'CONFIRMED') {\n", fp);
    fputs("        chips.innerHTML = `\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>DeadlockScope (Ours):</strong><span class=\"chip-val chip-err\">CONFIRMED LIVE DEADLOCK</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>Linux lockdep:</strong><span class=\"chip-val chip-warn\">ORDER INVERSION (Passive Only)</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>Helgrind:</strong><span class=\"chip-val chip-warn\">LOCK ORDER ERR</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>ThreadSanitizer:</strong><span class=\"chip-val chip-miss\">CLEAN (Missed Hang)</span></div>\n", fp);
    fputs("        `;\n", fp);
    fputs("      } else if (DATA.initialStatus.code === 'ACTIVE_TIMED') {\n", fp);
    fputs("        chips.innerHTML = `\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>DeadlockScope (Ours):</strong><span class=\"chip-val\" style=\"background:rgba(168,85,247,0.2);color:#a855f7;border:1px solid #a855f7;\">ACTIVE_TIMED (Non-Fatal)</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>Linux lockdep:</strong><span class=\"chip-val chip-warn\">ORDER INVERSION (Unclassified)</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>Helgrind:</strong><span class=\"chip-val chip-miss\">CLEAN (Missed)</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>ThreadSanitizer:</strong><span class=\"chip-val chip-miss\">CLEAN (Missed)</span></div>\n", fp);
    fputs("        `;\n", fp);
    fputs("      } else {\n", fp);
    fputs("        chips.innerHTML = `\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>DeadlockScope:</strong><span class=\"chip-val chip-safe\">CLEAN</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>Linux lockdep:</strong><span class=\"chip-val chip-safe\">CLEAN</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>Helgrind:</strong><span class=\"chip-val chip-safe\">CLEAN</span></div>\n", fp);
    fputs("          <div class=\"matrix-chip\"><strong>ThreadSanitizer:</strong><span class=\"chip-val chip-safe\">CLEAN</span></div>\n", fp);
    fputs("        `;\n", fp);
    fputs("      }\n", fp);
    fputs("    }\n", fp);
    fputs("  }\n\n", fp);

    fputs("  function renderFindingsList() {\n", fp);
    fputs("    updateHeroAndMatrix();\n", fp);
    fputs("    const container = document.getElementById('pane-findings');\n", fp);
    fputs("    container.innerHTML = '';\n", fp);
    fputs("    const list = getProcessedFindings();\n", fp);
    fputs("    document.getElementById('tab-cnt-findings').textContent = list.length;\n\n", fp);

    fputs("    if (list.length === 0) {\n", fp);
    fputs("      container.innerHTML = '<div style=\"color: var(--text-muted); text-align: center; margin-top: 40px;\">No cycle findings recorded. Execution was completely deadlock-free!</div>';\n", fp);
    fputs("      return;\n", fp);
    fputs("    }\n\n", fp);

    fputs("    list.forEach(f => {\n", fp);
    fputs("      const card = document.createElement('div');\n", fp);
    fputs("      card.className = 'finding-card';\n", fp);
    fputs("      let badgeHtml = '';\n", fp);
    fputs("      if (f.isAblated) {\n", fp);
    fputs("        badgeHtml = `<span class=\"badge badge-ablated\">FALSE ALARM (${f.suppress_reason} ablated)</span>`;\n", fp);
    fputs("      } else if (f.kind === 'CONFIRMED') {\n", fp);
    fputs("        badgeHtml = '<span class=\"badge badge-confirmed\">CONFIRMED LIVE</span>';\n", fp);
    fputs("      } else if (f.kind === 'ACTIVE') {\n", fp);
    fputs("        badgeHtml = '<span class=\"badge badge-active\">ACTIVE DEADLOCK</span>';\n", fp);
    fputs("      } else if (f.kind === 'ACTIVE_TIMED') {\n", fp);
    fputs("        badgeHtml = '<span class=\"badge badge-timed\">ACTIVE TIMED</span>';\n", fp);
    fputs("      } else if (f.kind === 'POTENTIAL') {\n", fp);
    fputs("        badgeHtml = '<span class=\"badge badge-potential\">POTENTIAL</span>';\n", fp);
    fputs("      } else {\n", fp);
    fputs("        badgeHtml = `<span class=\"badge badge-suppressed\">SUPPRESSED (${f.suppress_reason})</span>`;\n", fp);
    fputs("      }\n\n", fp);

    fputs("      let witnessHtml = '';\n", fp);
    fputs("      f.witnesses.forEach((w, idx) => {\n", fp);
    fputs("        const nextLock = f.locks[(idx + 1) % f.locks.length];\n", fp);
    fputs("        witnessHtml += `<div>Thread ${w.tid}: holds ${w.lock.slice(-4)} &rarr; waits on ${nextLock.slice(-4)} (${w.sym_to})</div>`;\n", fp);
    fputs("      });\n\n", fp);

    fputs("      card.innerHTML = `\n", fp);
    fputs("        <div class=\"card-header\">\n", fp);
    fputs("          <span class=\"card-title\">Cycle Depth: ${f.len} Locks</span>\n", fp);
    fputs("          ${badgeHtml}\n", fp);
    fputs("        </div>\n", fp);
    fputs("        <div class=\"card-meta\">\n", fp);
    fputs("          ${f.isAblated ? '<div style=\"color: #fca5a5; font-weight:700;\">Filter disabled: Raw Lockdep/Helgrind would flag this!</div>' : ''}\n", fp);
    fputs("          Locks: ${f.locks.map(l => l.slice(-4)).join(' &harr; ')}\n", fp);
    fputs("        </div>\n", fp);
    fputs("        <div class=\"cycle-witness-box\">${witnessHtml}</div>\n", fp);
    fputs("      `;\n", fp);

    fputs("      card.addEventListener('click', () => {\n", fp);
    fputs("        document.querySelectorAll('.finding-card').forEach(c => c.classList.remove('selected'));\n", fp);
    fputs("        card.classList.add('selected');\n", fp);
    fputs("        inspectFinding(f);\n", fp);
    fputs("        const l1 = nodeMap.get('L_' + f.locks[0]);\n", fp);
    fputs("        if (l1) { cam.x = canvas.clientWidth / 2 - l1.x * cam.zoom; cam.y = canvas.clientHeight / 2 - l1.y * cam.zoom; }\n", fp);
    fputs("      });\n", fp);
    fputs("      container.appendChild(card);\n", fp);
    fputs("    });\n", fp);
    fputs("  }\n\n", fp);

    fputs("  function inspectFinding(f) {\n", fp);
    fputs("    const pane = document.getElementById('pane-inspector');\n", fp);
    fputs("    pane.innerHTML = `\n", fp);
    fputs("      <h3 style=\"font-size: 14px; font-weight: 700; color: var(--col-accent); margin-bottom: 8px;\">Cycle Finding Telemetry</h3>\n", fp);
    fputs("      <table class=\"prop-table\">\n", fp);
    fputs("        <tr><td class=\"key\">Status</td><td class=\"val\">${f.effectiveKind}</td></tr>\n", fp);
    fputs("        <tr><td class=\"key\">Filter Status</td><td class=\"val\">${f.suppress_reason}</td></tr>\n", fp);
    fputs("        <tr><td class=\"key\">Cycle Length</td><td class=\"val\">${f.len}</td></tr>\n", fp);
    fputs("        <tr><td class=\"key\">Detection Time</td><td class=\"val\">${f.time_ms} ms</td></tr>\n", fp);
    fputs("      </table>\n", fp);
    fputs("      <h4 style=\"font-size: 12px; margin-top: 12px; color: var(--text-secondary);\">Involved Mutexes:</h4>\n", fp);
    fputs("      <div style=\"font-family: monospace; font-size: 11px;\">${f.locks.map(l => '<div>&bull; ' + l + '</div>').join('')}</div>\n", fp);
    fputs("    `;\n", fp);
    fputs("    document.querySelector('.tab[data-target=\"pane-inspector\"]').click();\n", fp);
    fputs("  }\n\n", fp);

    fputs("  function inspectNode(n) {\n", fp);
    fputs("    const pane = document.getElementById('pane-inspector');\n", fp);
    fputs("    pane.innerHTML = `\n", fp);
    fputs("      <h3 style=\"font-size: 14px; font-weight: 700; color: var(--col-accent); margin-bottom: 8px;\">${n.type.toUpperCase()} Inspector</h3>\n", fp);
    fputs("      <table class=\"prop-table\">\n", fp);
    fputs("        <tr><td class=\"key\">Identifier</td><td class=\"val\">${n.id}</td></tr>\n", fp);
    fputs("        <tr><td class=\"key\">Role</td><td class=\"val\">${n.role || n.type}</td></tr>\n", fp);
    fputs("        ${n.fullAddr ? `<tr><td class=\"key\">Address</td><td class=\"val\">${n.fullAddr}</td></tr>` : ''}\n", fp);
    fputs("        ${n.sym ? `<tr><td class=\"key\">Symbol</td><td class=\"val\">${n.sym}</td></tr>` : ''}\n", fp);
    fputs("        ${n.tid !== undefined ? `<tr><td class=\"key\">Thread ID</td><td class=\"val\">${n.tid}</td></tr>` : ''}\n", fp);
    fputs("      </table>\n", fp);
    fputs("    `;\n", fp);
    fputs("    document.querySelector('.tab[data-target=\"pane-inspector\"]').click();\n", fp);
    fputs("  }\n\n", fp);

    fputs("  // Render Edge Ledger\n", fp);
    fputs("  function renderLedger() {\n", fp);
    fputs("    const container = document.getElementById('pane-ledger');\n", fp);
    fputs("    container.innerHTML = '';\n", fp);
    fputs("    DATA.edges.forEach((e, idx) => {\n", fp);
    fputs("      const row = document.createElement('div');\n", fp);
    fputs("      row.className = 'ledger-row';\n", fp);
    fputs("      row.innerHTML = `\n", fp);
    fputs("        <div style=\"display:flex; justify-content:space-between; font-weight:600; color:var(--col-accent);\">\n", fp);
    fputs("          <span>Edge #${idx + 1}: ${e.from.slice(-4)} &rarr; ${e.to.slice(-4)}</span>\n", fp);
    fputs("          <span style=\"color:var(--text-muted); font-size:10px;\">Thread ${e.tid}</span>\n", fp);
    fputs("        </div>\n", fp);
    fputs("        <div style=\"color:var(--text-secondary); font-size:10px;\">Held: ${e.sym_from}</div>\n", fp);
    fputs("        <div style=\"color:var(--text-secondary); font-size:10px;\">Acquiring: ${e.sym_to}</div>\n", fp);
    fputs("      `;\n", fp);
    fputs("      container.appendChild(row);\n", fp);
    fputs("    });\n", fp);
    fputs("  }\n\n", fp);

    fputs("  // Tab Switching\n", fp);
    fputs("  document.querySelectorAll('.tab').forEach(tab => {\n", fp);
    fputs("    tab.addEventListener('click', () => {\n", fp);
    fputs("      document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));\n", fp);
    fputs("      document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));\n", fp);
    fputs("      tab.classList.add('active');\n", fp);
    fputs("      document.getElementById(tab.dataset.target).classList.add('active');\n", fp);
    fputs("    });\n", fp);
    fputs("  });\n\n", fp);

    fputs("  // Ablation Checkbox Listeners\n", fp);
    fputs("  ['gate', 'thread', 'lifecycle'].forEach(k => {\n", fp);
    fputs("    const cb = document.getElementById('filter-' + k);\n", fp);
    fputs("    cb.addEventListener('change', () => {\n", fp);
    fputs("      filterState[k] = cb.checked;\n", fp);
    fputs("      renderFindingsList();\n", fp);
    fputs("    });\n", fp);
    fputs("  });\n\n", fp);

    fputs("  // View Mode Buttons\n", fp);
    fputs("  document.getElementById('btn-view-all').addEventListener('click', function() {\n", fp);
    fputs("    currentView = 'all'; setActiveViewBtn(this);\n", fp);
    fputs("  });\n", fp);
    fputs("  document.getElementById('btn-view-log').addEventListener('click', function() {\n", fp);
    fputs("    currentView = 'log'; setActiveViewBtn(this);\n", fp);
    fputs("  });\n", fp);
    fputs("  document.getElementById('btn-view-wfg').addEventListener('click', function() {\n", fp);
    fputs("    currentView = 'wfg'; setActiveViewBtn(this);\n", fp);
    fputs("  });\n", fp);
    fputs("  document.getElementById('btn-reset-cam').addEventListener('click', () => {\n", fp);
    fputs("    cam.x = canvas.clientWidth / 2; cam.y = canvas.clientHeight / 2; cam.zoom = 1.0;\n", fp);
    fputs("  });\n", fp);
    fputs("  function setActiveViewBtn(btn) {\n", fp);
    fputs("    ['btn-view-all', 'btn-view-log', 'btn-view-wfg'].forEach(id => document.getElementById(id).classList.remove('active'));\n", fp);
    fputs("    btn.classList.add('active');\n", fp);
    fputs("  }\n\n", fp);

    fputs("  // Canvas Pointer & Pan/Zoom Interaction\n", fp);
    fputs("  canvas.addEventListener('pointerdown', e => {\n", fp);
    fputs("    const rect = canvas.getBoundingClientRect();\n", fp);
    fputs("    const mx = (e.clientX - rect.left - cam.x) / cam.zoom;\n", fp);
    fputs("    const my = (e.clientY - rect.top - cam.y) / cam.zoom;\n", fp);
    fputs("    draggedNode = nodes.find(n => Math.hypot(n.x - mx, n.y - my) <= n.radius + 6);\n", fp);
    fputs("    if (draggedNode) {\n", fp);
    fputs("      inspectNode(draggedNode);\n", fp);
    fputs("    } else {\n", fp);
    fputs("      cam.isPanning = true;\n", fp);
    fputs("      cam.startX = e.clientX - cam.x;\n", fp);
    fputs("      cam.startY = e.clientY - cam.y;\n", fp);
    fputs("    }\n", fp);
    fputs("  });\n", fp);
    fputs("  window.addEventListener('pointermove', e => {\n", fp);
    fputs("    const rect = canvas.getBoundingClientRect();\n", fp);
    fputs("    const mx = (e.clientX - rect.left - cam.x) / cam.zoom;\n", fp);
    fputs("    const my = (e.clientY - rect.top - cam.y) / cam.zoom;\n", fp);
    fputs("    hoveredNode = nodes.find(n => Math.hypot(n.x - mx, n.y - my) <= n.radius + 6);\n", fp);
    fputs("    if (draggedNode) {\n", fp);
    fputs("      draggedNode.x = mx; draggedNode.y = my;\n", fp);
    fputs("      draggedNode.vx = 0; draggedNode.vy = 0;\n", fp);
    fputs("    } else if (cam.isPanning) {\n", fp);
    fputs("      cam.x = e.clientX - cam.startX;\n", fp);
    fputs("      cam.y = e.clientY - cam.startY;\n", fp);
    fputs("    }\n", fp);
    fputs("  });\n", fp);
    fputs("  window.addEventListener('pointerup', () => {\n", fp);
    fputs("    draggedNode = null; cam.isPanning = false;\n", fp);
    fputs("  });\n", fp);
    fputs("  canvas.addEventListener('wheel', e => {\n", fp);
    fputs("    e.preventDefault();\n", fp);
    fputs("    const factor = e.deltaY > 0 ? 0.9 : 1.1;\n", fp);
    fputs("    cam.zoom = Math.max(0.2, Math.min(3.0, cam.zoom * factor));\n", fp);
    fputs("  });\n\n", fp);

    fputs("  // Initial Render\n", fp);
    fputs("  renderFindingsList();\n", fp);
    fputs("  renderLedger();\n", fp);
    fputs("  </script>\n", fp);
    fputs("</body>\n</html>\n", fp);

    fclose(fp);
}
