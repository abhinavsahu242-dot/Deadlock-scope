# DeadlockScope

A lightweight, non-invasive runtime deadlock prediction and validation tool for pthread-based C programs on Linux x86_64.

DeadlockScope couples proactive lock-order prediction with automated dynamic validation using an end-to-end **Predict $\to$ Analyze $\to$ Validate** workflow:
1. **Predict:** Intercepts mutex operations at runtime via `LD_PRELOAD` to construct an attempt-time Lock-Order Graph (LOG) and discover structural cycles.
2. **Analyze:** Filters candidate cycles using credibility heuristics (same-thread exclusion, common gate-lock filtering, and thread lifecycle barriers).
3. **Validate:** Injects targeted micro-timing perturbations (`--stress`) specifically around locks involved in predicted cycles to deliberately provoke race conditions, turning theoretical predictions into confirmed, reproducible deadlocks.

---

## Why DeadlockScope Outclasses & Differentiates from Linux `lockdep`

| Capability | Linux `lockdep` / `liblockdep` | DeadlockScope |
|---|---|---|
| **Live Deadlock Detection** | **None.** Passive order validator only. Cannot detect when threads are frozen. | **Live Wait-For Graph (WFG)** with a background Watchdog thread detecting active unyielding thread freezes. |
| **Provocation & Closed Loop** | **None.** Only prints warnings; cannot validate whether an inversion can deadlock. | **Targeted Stress Engine** that widens the race window to prove predictions into `CONFIRMED` deadlocks. |
| **Condition Variable Safety** | **Broken/Stale.** Mutex release inside libc `cond_wait` corrupts held-lock sets. | **Wrapped `pthread_cond_wait` and `timedwait`** maintaining exact held sets across libc suspensions. |
| **Timed Wait Taxonomy** | **None.** Treats all locks identically. | Distinguishes **`ACTIVE`** (unyielding) from **`ACTIVE_TIMED`** (timedlock waits that may break on timeout). |
| **Credibility Heuristics** | Basic class order checking; noisy on single-thread or gate-guarded critical sections. | **Multi-tier Credibility Filters** (Same-Thread, Gate-Lock, Spawn/Join Lifecycle Barriers). |
| **Evaluation & Ablation** | Monolithic on/off switch. | **Configurable Filter Ablation Matrix (L0 $\to$ L3)** measuring empirical false-positive reduction ratios. |
| **Visualization** | Text warnings in dmesg. | **Dual-Cluster Graphviz (.dot)** visualizing the Lock-Order Graph alongside the live Wait-For Graph. |

---

## Architecture Overview

```
+-----------------------------------------------------------------------------+
|                               TARGET PROGRAM                                |
+-----------------------------------------------------------------------------+
       | (pthread_mutex_lock, trylock, timedlock, unlock, cond_wait/timedwait)
       v
+-----------------------------------------------------------------------------+
| INTERCEPTION LAYER (dlsym RTLD_NEXT, TLS re-entrancy protection)           |
+-----------------------------------------------------------------------------+
       |
       +------------------------------------+
       v                                    v
+-------------------------------+  +------------------------------------------+
| LOCK-ORDER GRAPH (LOG)        |  | LIVE WAIT-FOR GRAPH (WFG)                |
|  - Attempt-time edges         |  |  - Ephemeral thread blocking state       |
|  - Depth-First Search cycles  |  |  - Current owner resolution              |
|  - Credibility Filters:       |  +------------------------------------------+
|    * Same-Thread Filter (L1)  |                   ^
|    * Gate-Lock Filter (L2)    |                   | (polled by daemon)
|    * Lifecycle Barriers (L3)  |  +------------------------------------------+
+-------------------------------+  | WATCHDOG THREAD (ds_watchdog)            |
       |                           |  - Periodic sampling (default 50ms)      |
       | (registers targets)       |  - Two-tick stability re-validation      |
       v                           |  - Tri-state finding classification:     |
+-------------------------------+  |    POTENTIAL -> ACTIVE -> CONFIRMED      |
| TARGETED STRESS ENGINE        |  |    + ACTIVE_TIMED (for timed waits)      |
|  - Injects micro-delays on    |  +------------------------------------------+
|    predicted candidate locks  |                   |
+-------------------------------+                   v
                                   +------------------------------------------+
                                   | REPORTING & GRAPHVIZ EXPORT              |
                                   |  - Terminal alerts & summary table       |
                                   |  - Dual-cluster .dot export              |
                                   +------------------------------------------+
```

---

## Directory Layout

```
deadlockscope/
├── include/
│   ├── ds_common.h       # Shared types, limits, WaitKind, filter masks
│   ├── ds_interceptor.h  # Interceptor prototypes, condvar & timedlock hooks
│   ├── ds_lock_graph.h   # Lock-Order Graph and DFS cycle detection
│   ├── ds_filters.h      # Credibility heuristics (same-thread, gate-lock, lifecycle)
│   ├── ds_wait_graph.h   # Live Wait-For Graph and active cycle detector
│   ├── ds_watchdog.h     # Watchdog background monitoring daemon
│   ├── ds_stress.h       # Targeted timing perturbation engine
│   └── ds_report.h       # Diagnostics, finding table, and DOT exporter
├── src/
│   ├── core.c            # Constructor, destructor, configuration, and spinlocks
│   ├── interceptor.c     # LD_PRELOAD hooks for mutex, condvar, and thread routines
│   ├── lock_graph.c      # Graph edges, witness tracking, cycle finder
│   ├── filters.c         # Credibility filter implementations (L0 -> L3)
│   ├── wait_graph.c      # Live wait state snapshotting and pointer chase
│   ├── watchdog.c        # Polling daemon and confirmation re-validation
│   ├── stress.c          # Target-lock delay injection
│   └── report.c          # Report formatting and dual-cluster Graphviz export
├── cli/
│   └── deadlock-scope    # CLI runner script
├── tests/
│   ├── 01_simple_deadlock.c    # 2-thread classic deadlock (ACTIVE)
│   ├── 02_cycle_3thread.c      # 3-thread cyclic deadlock (ACTIVE)
│   ├── 03_potential_benign.c   # Staggered opposite orders (POTENTIAL)
│   ├── 04_gate_lock.c          # Common gate lock (SUPPRESSED)
│   ├── 05_same_thread.c        # Sequential single thread (SUPPRESSED)
│   ├── 06_lifecycle_ordered.c  # Join-barrier ordered (SUPPRESSED)
│   ├── 07_trylock_recovery.c   # Backoff with trylock (Clean)
│   ├── 08_provocation_test.c   # Predict -> Provoke -> Confirm hero test
│   ├── 09_recursive_mutex.c    # Re-entrant recursive mutex (Clean)
│   ├── 10_clean_parallel.c     # Highly concurrent clean lock order (Clean)
│   ├── 11_cond_wait_held_set.c # Condvar mutex release/restore verification
│   ├── 12_timedlock_deadlock.c # Timedlock hang classification (ACTIVE_TIMED)
│   └── run_tests.sh            # Automated test runner & ablation table
└── Makefile
```

---

## Building

Requires `gcc` and `make` on Linux x86_64:

```bash
make clean
make
```

This compiles `libdeadlockscope.so` and builds all 12 test binaries into `tests/bin/`.

---

## Usage

Run any multithreaded executable under DeadlockScope without recompiling:

```bash
# Basic observation and prediction
./cli/deadlock-scope -- ./your_program [args...]

# Enable targeted stress perturbation (injects micro-delays on predicted locks)
./cli/deadlock-scope --stress 50 -- ./your_program [args...]

# Configure active credibility filters (e.g. disable filters to mimic lockdep)
./cli/deadlock-scope --filters none -- ./your_program [args...]
./cli/deadlock-scope --filters thread,gate,lifecycle -- ./your_program [args...]

# Export dual-cluster Graphviz lock graph
./cli/deadlock-scope --dot lock_graph.dot -- ./your_program [args...]

# Render the graph
dot -Tpng lock_graph.dot -o lock_graph.png
```

---

## Automated Verification & Ablation Study

Run the automated verification suite and Filter Ablation Study:

```bash
make check
```

Output includes:
- All 13 test execution assertions passing.
- An empirical **Filter Ablation Study** demonstrating false-positive elimination across L0 (lockdep mode) vs L3 (DeadlockScope):

```text
================================================================================
               FILTER ABLATION STUDY: DEADLOCKSCOPE VS LOCKDEP                  
================================================================================
Test Case                      | L0 (Raw lockdep: none) | L3 (DeadlockScope: all)
--------------------------------------------------------------------------------
04_gate_lock                   | POTENTIAL (False Alarm) | SUPPRESSED (Accurate)
05_same_thread                 | POTENTIAL (False Alarm) | SUPPRESSED (Accurate)
06_lifecycle_ordered           | POTENTIAL (False Alarm) | SUPPRESSED (Accurate)
================================================================================
```
