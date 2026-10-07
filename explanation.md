# DeadlockScope: Comprehensive Architectural & Codebase Guide
*A Complete Reference for Understanding, Explaining, and Tutoring the DeadlockScope Runtime System*

---

## 1. Executive Summary & Problem Formulation

### 1.1 The Fundamental Concurrency Problem
In concurrent programming using POSIX Threads (`pthreads`), synchronization bugs—specifically **deadlocks**—are among the most insidious software defects. A deadlock occurs when a set of concurrent execution threads are permanently blocked because each thread holds a lock that another thread needs, and none can proceed without acquiring its next lock.

Formally, a deadlock requires all four **Coffman Conditions** to hold simultaneously:
1. **Mutual Exclusion:** Resources (mutexes) cannot be shared simultaneously.
2. **Hold and Wait:** A thread holds at least one resource while waiting to acquire another.
3. **No Preemption:** Resources can only be released voluntarily by the holding thread.
4. **Circular Wait:** A closed chain of threads $\{T_1, T_2, \dots, T_k\}$ exists such that $T_i$ is waiting for a lock held by $T_{(i \bmod k) + 1}$.

### 1.2 State-of-the-Art Limitations
Traditional deadlock analysis tools suffer from severe trade-offs:
- **Static Analysis (e.g., Clang Thread Safety Analysis, Coverity):** Lacks runtime pointer alias information. Suffers from high false-positive rates on complex pointer aliasing and cannot reason about thread lifecycles or conditional branches.
- **Linux Kernel `lockdep` / `liblockdep`:** Tracks lock acquisition classes. It operates purely on **lock acquisition order**. However:
  1. It generates **massive false positives** on benign patterns (e.g., gate locks, sequential reentrancy within the same thread, thread lifecycle barriers).
  2. It has **no live deadlock awareness**: it cannot tell if a program is actually frozen or just executing a harmless inversion.
  3. It cannot validate whether a predicted inversion is truly reproducible.
- **Dynamic Sanitizers (Valgrind `Helgrind`, LLVM `ThreadSanitizer`):**
  - Impose extreme runtime overhead (10x to 50x slowdown).
  - Helgrind flags benign lock inversions as errors.
  - ThreadSanitizer frequently misses multi-thread circular hangs entirely because it focuses primarily on data races, not lock-cycle graph topology.

### 1.3 What DeadlockScope Is
**DeadlockScope** is a high-performance, non-invasive runtime dynamic analysis and provocation tool for POSIX threads. Implemented in C99 as an `LD_PRELOAD` shared library (`libdeadlockscope.so`), it introduces four primary innovations:
1. **Dual-Graph Architecture:** Combines a historical **Lock-Order Graph (LOG)** for predictive cycle detection with a live **Wait-For Graph (WFG)** for real-time confirmation of actual thread freezes.
2. **Three-Tier Credibility Filtering:** Eliminates false alarms caused by gate locks, same-thread sequential acquisitions, and thread spawn/join lifecycle barriers.
3. **Active Race Provocation Engine:** Uses a closed-loop feedback mechanism to inject targeted microsecond delays on predicted lock cycle boundaries, widening the race window to actively prove whether a latent deadlock can manifest.
4. **Taxonomic State Classification:** Distinguishes between fatal unyielding deadlocks (`ACTIVE`), self-resolving timed stalls (`ACTIVE_TIMED`), benign serialized patterns (`SUPPRESSED`), and unconfirmed risks (`POTENTIAL`).

---

## 2. System Architecture & Codebase Map

### 2.1 Directory Structure
```
deadlockscope/
├── include/                     # Public and internal headers
│   ├── ds_common.h              # Global data structures, types, configuration, spinlocks
│   ├── ds_interceptor.h         # Function pointer typedefs and hook prototypes
│   ├── ds_lock_graph.h          # Lock-Order Graph (LOG) management & cycle finding
│   ├── ds_wait_graph.h          # Wait-For Graph (WFG) & live cycle detection
│   ├── ds_filters.h             # Credibility heuristics (gate, same-thread, lifecycle)
│   ├── ds_watchdog.h            # Background asynchronous watchdog thread
│   ├── ds_stress.h              # Closed-loop provocation engine & delay injection
│   └── ds_report.h              # Text summary, Graphviz .dot, and standalone HTML export
├── src/                         # Core implementation files
│   ├── core.c                   # Lifecycle constructors/destructors, config, spinlocks
│   ├── interceptor.c            # LD_PRELOAD POSIX hooks (mutex, thread, condvar)
│   ├── lock_graph.c             # Directed graph edge tracking, DFS cycle search
│   ├── wait_graph.c             # Live thread wait-state tracking & WFG cycle walk
│   ├── filters.c                # Sound mathematical filter validation
│   ├── watchdog.c               # Polling loop, live deadlock verification, alert dump
│   ├── stress.c                 # Target lock registration & targeted nanosleep delays
│   └── report.c                 # Terminal tables, DOT generator, interactive HTML generator
├── cli/
│   └── deadlock-scope           # User-facing wrapper script with argument parsing
├── tests/                       # 12 targeted validation scenarios + test runner
│   ├── 01_simple_deadlock.c     # Classic 2-thread AB-BA live deadlock
│   ├── 02_cycle_3thread.c       # 3-thread circular deadlock (A->B, B->C, C->A)
│   ├── 03_potential_benign.c    # Predictive cycle (AB then BA, no concurrent race)
│   ├── 04_gate_lock.c           # Inverted worker locks protected by outer gate mutex
│   ├── 05_same_thread.c         # Sequential lock inversion inside a single thread
│   ├── 06_lifecycle_ordered.c   # Inversion separated by pthread_join lifecycle barrier
│   ├── 07_trylock_recovery.c    # Lock acquisition using non-blocking pthread_mutex_trylock
│   ├── 08_provocation_test.c    # Latent timing-dependent deadlock forced by --stress
│   ├── 09_recursive_mutex.c     # Reentrant PTHREAD_MUTEX_RECURSIVE safety
│   ├── 10_clean_parallel.c      # Clean hierarchical lock acquisition under high load
│   ├── 11_cond_wait_held_set.c  # pthread_cond_wait held-lock restoration validation
│   ├── 12_timedlock_deadlock.c  # Deadlock containing pthread_mutex_timedlock (ACTIVE_TIMED)
│   ├── run_tests.sh             # Automated 13-case test harness & ablation benchmark
│   └── compare/
│       └── run_comparison.sh    # Multi-tool benchmark (DeadlockScope vs Lockdep vs Helgrind vs TSan)
├── reports/                     # Standalone interactive HTML reports
└── Makefile                     # Build system with GCC flags, -rdynamic, test targets
```

---

## 3. Deep Dive into Data Structures (`include/ds_common.h`)

All data structures in DeadlockScope are statically allocated and cache-aligned where possible to avoid memory allocator reentrancy during hook execution.

### 3.1 Lock and Thread Identification
```c
typedef uintptr_t MutexId;   // Virtual memory address of the pthread_mutex_t
typedef uint16_t  ThreadId;  // Internal dense thread index [0 .. MAX_THREADS-1]
```
- **Why `uintptr_t`?** The virtual memory address uniquely identifies a mutex in the process's address space. No heap metadata needs to be attached to the user's mutex struct.
- **Why dense `ThreadId`?** OS thread IDs (`pthread_t`) are opaque large numbers or pointers. Dense integer IDs (`0, 1, 2...`) enable $O(1)$ bitmask operations and array indexing.

### 3.2 Held-Lock Tracking (`HeldLockSet`)
```c
typedef struct {
    MutexId locks[MAX_HELD_LOCKS];  // Stack of currently held mutexes
    void   *sites[MAX_HELD_LOCKS];  // Return addresses (call sites) of acquisitions
    uint8_t count;                  // Number of currently held locks
} HeldLockSet;
```
- Each thread maintains a per-thread `HeldLockSet`.
- When thread $T$ acquires lock $L_B$ while already holding $L_A$, an edge $L_A \to L_B$ is established in the Lock-Order Graph.
- `sites[]` records `__builtin_return_address(0)` to map the exact binary call site for symbolization via `dladdr`.

### 3.3 Thread State Descriptor (`ThreadSlot`)
```c
typedef struct {
    ThreadId        tid;
    pthread_t       pthread_id;
    atomic_int      active;
    HeldLockSet     held;
    
    /* Live wait state (Wait-For Graph) */
    atomic_int      is_waiting;
    atomic_int      wait_kind;      /* WK_NONE, WK_LOCK, WK_TIMEDLOCK, WK_COND */
    atomic_uintptr_t waiting_for;   /* MutexId thread is currently blocked on */
    void           *wait_site;
    uint64_t        wait_start_ns;
    
    /* Lifecycle ancestry */
    ThreadId        parent_tid;
    uint64_t        spawn_seq;      /* Monotonic event counter when spawned */
    uint64_t        join_seq;       /* Monotonic event counter when joined */
} ThreadSlot;
```
- Atomic fields allow the **Watchdog background thread** to inspect any thread's live wait status without acquiring intrusive locks that could cause metadata deadlocks.
- `spawn_seq` and `join_seq` provide monotonic logical timestamps for the **Lifecycle Barrier Filter**.

### 3.4 Witnesses and Edges (`LockOrderEdge`)
```c
typedef struct {
    ThreadId    tid;
    HeldLockSet held_before;
    void       *site_from;
    void       *site_to;
} Witness;

typedef struct {
    MutexId from;
    MutexId to;
    Witness witnesses[MAX_WITNESSES];
    uint8_t witness_count;
} LockOrderEdge;
```
- An edge represents an observed acquisition order: a thread acquired `to` while already holding `from`.
- **Witnesses:** DeadlockScope stores the exact context (`held_before`, `tid`, and call sites) under which the edge was observed. Storing `held_before` is critical for verifying the **Gate-Lock Filter**.

### 3.5 Finding Taxonomy (`FindingKind`)
```c
typedef enum {
    FINDING_POTENTIAL,     // Lock inversion seen in LOG, but no concurrent race occurred
    FINDING_SUPPRESSED,    // Inversion detected, but proven benign by credibility filters
    FINDING_ACTIVE,        // Live circular wait currently blocking threads in WFG
    FINDING_ACTIVE_TIMED,  // Live circular wait containing pthread_mutex_timedlock
    FINDING_CONFIRMED      // Predicted POTENTIAL inversion actively confirmed via WFG
} FindingKind;
```

---

## 4. Subsystem Execution & Line-by-Line Mechanics

### 4.1 Low-Level Interception Plumbing (`src/interceptor.c`)
DeadlockScope intercepts POSIX calls via `LD_PRELOAD`. 

#### A. Symbol Resolution (`ds_resolve_symbols`)
Lines 20–33 of `interceptor.c`:
```c
real_pthread_mutex_lock = (fn_mutex_lock)dlsym(RTLD_NEXT, "pthread_mutex_lock");
```
Using `RTLD_NEXT` allows DeadlockScope to locate the next occurrence of `pthread_mutex_lock` in the dynamic library search order (which resolves to GNU `libc.so`).

#### B. Recursion Prevention (`ds_in_hook`)
Intercepting libc calls is notoriously susceptible to infinite recursion:
- If `dlsym`, `malloc`, or internal logging attempts to acquire an internal mutex, it re-enters `pthread_mutex_lock`.
- To prevent this, DeadlockScope uses a thread-local reentrancy flag:
```c
__thread int ds_in_hook = 0;
```
At the entry of every intercepted function:
```c
if (ds_in_hook) return real_pthread_mutex_lock(mutex);
ds_in_hook = 1;
/* ... instrumentation ... */
ds_in_hook = 0;
```

#### C. Mutex Lock Interception Flow
In `pthread_mutex_lock(pthread_mutex_t *mutex)` (Lines 226–270):
1. **Recursion Guard:** If `ds_in_hook` is set, calls `real_pthread_mutex_lock` directly.
2. **Thread Identification:** Retrieves dense `ThreadId` via `ds_get_or_register_thread()`.
3. **Lock-Order Recording:** Calls `ds_log_record_order(tid, m, site)`. For every mutex currently held in `g_threads[tid].held`, an edge `held_lock -> m` is added to the Lock-Order Graph along with the thread's held-set witness.
4. **Stress Delay Injection:** Calls `ds_stress_maybe_delay(tid, m)`. If stress testing is active, delays the thread slightly before acquisition to maximize race exposure.
5. **Wait-For Graph Registration:** Calls `ds_wait_begin(tid, m, site, WK_LOCK)`. Marks the thread as actively waiting for mutex `m`.
6. **Real Invocation:** Calls `real_pthread_mutex_lock(mutex)`.
7. **Wait Completion:** Calls `ds_wait_end(tid)`.
8. **Ownership Transfer:** Calls `ds_take_ownership(tid, m, site)`. Updates `g_lock_owners` and pushes `m` into `g_threads[tid].held`.

#### D. Condition Variable Safety (`pthread_cond_wait`)
Lines 391–434:
When a thread calls `pthread_cond_wait(cond, mutex)`:
- Standard `libc` automatically releases `mutex` while sleeping on `cond`, and re-acquires it before returning.
- If an interceptor does not account for this, the thread's `HeldLockSet` remains corrupted (claiming it holds the lock while sleeping).
- **DeadlockScope Solution:**
  1. Calls `ds_release_for_cond(tid, m)` to remove the lock from the held set before entering the condition wait.
  2. Invokes `real_pthread_cond_wait(cond, mutex)`.
  3. Upon waking, calls `ds_restore_after_cond(tid, m, site)` to re-register ownership and record lock-order dependencies.

---

### 4.2 The Dual-Graph Engine

DeadlockScope maintains two entirely separate graphs with distinct semantic meanings:

```
+-----------------------------------------------------------------------------+
|                               DEADLOCKSCOPE                                 |
|                                                                             |
|   1. HISTORICAL / PREDICTIVE               2. REAL-TIME / EMPIRICAL         |
|   +-----------------------------+          +----------------------------+   |
|   |  Lock-Order Graph (LOG)     |          |   Wait-For Graph (WFG)     |   |
|   |  Nodes = Mutexes (L_A, L_B) |          |   Nodes = Threads (T1, T2) |   |
|   |  Edges = Acquisition Order  |          |   Edges = Blocked On Mutex |   |
|   |  Algorithm = Depth-First    |          |   Algorithm = Directed     |   |
|   |              Search (DFS)   |          |               Cycle Walk   |   |
|   +--------------+--------------+          +-------------+--------------+   |
|                  |                                       |                  |
|                  v                                       v                  |
|        POTENTIAL CYCLE DETECTED                LIVE BLOCKED CYCLE?          |
|                  |                                       |                  |
|                  +------------------+--------------------+                  |
|                                     |                                       |
|                                     v                                       |
|                  CONFIRMED ACTIVE DEADLOCK IDENTIFIED                       |
+-----------------------------------------------------------------------------+
```

#### A. Lock-Order Graph (LOG) Cycle Search (`src/lock_graph.c`)
- **Nodes:** Unique mutex memory addresses ($M \in \text{MutexId}$).
- **Edges:** Directed edge $M_A \to M_B$ indicates at least one thread acquired $M_B$ while holding $M_A$.
- **Detection Algorithm:** Depth-First Search (`dfs_find`, lines 111–161) bounded by `MAX_CYCLE_DEPTH = 8`.
- **Normalization:** Cycles are normalized by rotating the sequence so the lowest numerical pointer address appears first (`normalize_cycle`). This guarantees identical cycles are deduplicated regardless of discovery order.

#### B. Live Wait-For Graph (WFG) Cycle Walk (`src/wait_graph.c`)
- **Nodes:** Active threads ($T \in [0 \dots \text{MAX\_THREADS}-1]$).
- **Edges:** Directed edge $T_1 \to T_2$ indicates $T_1$ is currently blocked waiting on mutex $M$, and $M$ is currently held by $T_2$.
- **Detection Algorithm:** In `ds_wfg_detect_active_cycle()` (lines 18–105):
  1. Constructs an instantaneous successor map:
     $$\text{successor}[T_i] = \text{owner}(\text{waiting\_for}[T_i])$$
  2. For each thread, walks the successor chain tracking visited nodes with a 64-bit mask `path_mask`.
  3. If $\text{path\_mask} \ \& \ (1 \ll \text{curr})$ is true, a circular wait exists!
  4. Inspects `wait_kind` across all threads in the cycle. If any thread is in `WK_TIMEDLOCK`, sets `contains_timed_wait = true`.

---

### 4.3 Credibility Filters & False-Alarm Elimination (`src/filters.c`)

Standard tools like `lockdep` and `helgrind` report every cycle in the Lock-Order Graph as an error. In reality, many lock inversions can never deadlock. DeadlockScope implements three formal mathematical credibility filters:

#### 1. Same-Thread Sequential Filter (`ds_filter_same_thread`)
- **Rule:** If all edges comprising the cycle were witnessed exclusively by the **exact same thread**, no concurrency race is possible.
- **Proof:** A single thread of execution cannot be in two places at once. If Thread 1 acquires $A \to B$ in function `foo()`, releases both, and later acquires $B \to A$ in function `bar()`, there is an inversion in the LOG. But because only Thread 1 ever touches these locks, circular wait between two concurrent entities is physically impossible.
- **Implementation (Lines 3–12):**
  ```c
  ThreadId first_tid = cycle->witnesses[0].tid;
  for (int i = 1; i < cycle->length; i++) {
      if (cycle->witnesses[i].tid != first_tid) return false;
  }
  return true;
  ```

#### 2. Gate-Lock Serialized Filter (`ds_filter_gate_lock`)
- **Rule:** If every conflicting acquisition sequence in the cycle occurred while holding a common outer mutex $G$ (where $G$ is not part of the cycle itself), the cycle is serialized.
- **Proof:** Suppose Thread 1 acquires $A \to B$ while holding $G$, and Thread 2 acquires $B \to A$ while holding $G$. 
  Because $G$ is a mutually exclusive lock, Thread 1 and Thread 2 can never enter their inner critical sections simultaneously. Either Thread 1 acquires $G$ first and completes both $A$ and $B$, or Thread 2 acquires $G$ first and completes both $B$ and $A$. The inner inversion is benign.
- **Implementation (Lines 14–56):**
  DeadlockScope iterates through all mutexes held in `witness[0].held_before`. For each candidate $G$, it verifies:
  1. $G \notin \{L_1, \dots, L_k\}$ (the gate lock cannot be an inner cycle lock).
  2. $\forall i \in [1 \dots k-1], G \in \text{witness}[i].\text{held\_before}$.
  If any $G$ satisfies this for all witnesses, the cycle is suppressed as `SUPPRESS_GATE_LOCK`.

#### 3. Lifecycle Barrier Filter (`ds_filter_lifecycle`)
- **Rule:** If thread lifetimes are strictly separated by synchronization barriers (`pthread_join`), the cycle cannot deadlock.
- **Proof:** Suppose Thread 1 acquires $A \to B$ and terminates. The main thread calls `pthread_join(t1)`. Subsequently, Thread 2 is spawned and acquires $B \to A$. Even though two distinct threads executed inverted orders, Thread 1's execution was finished before Thread 2 was created. They never coexisted in time.
- **Implementation (Lines 58–78):**
  Uses the global monotonic event sequence `g_event_seq`.
  If $T_1.\text{join\_seq} > 0$ and $T_2.\text{spawn\_seq} > T_1.\text{join\_seq}$, the threads are causally ordered, suppressing the cycle as `SUPPRESS_LIFECYCLE`.

---

### 4.4 The Background Watchdog Subsystem (`src/watchdog.c`)
Deadlocks by definition freeze threads. In a traditional tool, if threads deadlock, `exit()` is never reached and `__attribute__((destructor))` never fires. The tool hangs silently alongside the deadlocked application.

DeadlockScope solves this using an independent **Watchdog Thread**:
1. Spawned at startup inside `ds_init()` (`core.c:145`).
2. Runs in a loop with sleep interval `g_config.poll_ms` (default 50ms).
3. **Lock-Free WFG Polling:** Inspects the live Wait-For Graph via atomic loads.
4. **Hysteresis / Confirmation Timer:** When a cycle is detected, the watchdog records `first_seen_ms`. It requires the cycle to persist continuously for `confirm_ms` (default 100ms) to ensure it is not an ephemeral transition.
5. **Immediate Autonomous Dump:** Once confirmed, the watchdog directly prints the deadlock diagnostics to `stderr` and writes both the Graphviz `.dot` and interactive `.html` reports to disk—guaranteeing that diagnostic data is never lost when the process freezes.

---

### 4.5 The Closed-Loop Provocation Engine (`src/stress.c`)
Most concurrency bugs are timing-dependent Heisenbugs that do not manifest in development runs.
DeadlockScope provides a **Closed-Loop Provocation Engine**:
1. When the Watchdog or LOG DFS detects a latent `POTENTIAL` cycle, it registers the constituent locks into `g_target_locks[]` (`ds_stress_register_target_locks`).
2. When any thread enters `pthread_mutex_lock(m)` for an identified target lock, `ds_stress_maybe_delay()` intercepts the thread.
3. It injects a configurable microsecond sleep (`nanosleep(&req)`).
4. This artificially widens the race window, allowing the companion thread to acquire the second lock and forcing the latent order inversion into a reproducible `CONFIRMED` live deadlock.

---

## 5. Verification Test Suite Matrix

The project includes 12 dedicated C test scenarios in `tests/`, each designed to test a specific concurrency behavior:

| Binary | Description | Lockdep Verdict | DeadlockScope Verdict | What This Proves |
|---|---|---|---|---|
| `01_simple_deadlock` | Classic 2-thread AB-BA lock inversion | `ORDER INVERSION` | `CONFIRMED` | Detects live process freezes using WFG and Watchdog. |
| `02_cycle_3thread` | 3-thread circular deadlock ($T_1 \to T_2 \to T_3 \to T_1$) | `ORDER INVERSION` | `CONFIRMED` | Handles multi-hop graph cycles that break simple pairwise checks. |
| `03_potential_benign` | Inverse acquisitions separated by sleep; no race occurs | `ORDER INVERSION` | `POTENTIAL` | Accurately identifies inversions without falsely claiming a live hang. |
| `04_gate_lock` | Inverse acquisitions protected by outer gate lock | `ORDER INVERSION` *(False Alarm)* | `SUPPRESSED (gate_lock)` | Demonstrates gate-lock intersection heuristic. |
| `05_same_thread` | Inverse acquisitions within the same sequential thread | `ORDER INVERSION` *(False Alarm)* | `SUPPRESSED (same_thread)` | Proves reentrant single-thread safety. |
| `06_lifecycle_ordered` | Inversion across two threads separated by `pthread_join` | `ORDER INVERSION` *(False Alarm)* | `SUPPRESSED (lifecycle)` | Proves spawn/join causal ordering elimination. |
| `07_trylock_recovery` | Lock acquisition using non-blocking `pthread_mutex_trylock` | `CLEAN` | `CLEAN` | Confirms trylock failure does not establish rigid order edges. |
| `08_provocation_test` | Latent inversion that only deadlocks under stress injection | `ORDER INVERSION` | `POTENTIAL` $\to$ `CONFIRMED` (with `--stress 50`) | Proves the closed-loop provocation engine can force Heisenbugs. |
| `09_recursive_mutex` | Multiple nested acquisitions of `PTHREAD_MUTEX_RECURSIVE` | `CLEAN` | `CLEAN` | Confirms recursive mutexes do not produce self-loop cycles. |
| `10_clean_parallel` | 8 threads performing 50,000 hierarchical lock acquisitions | `CLEAN` | `CLEAN` | Validates low runtime overhead and zero false positives under concurrency. |
| `11_cond_wait_held_set`| `pthread_cond_wait` releasing and re-acquiring mutex | `CLEAN` | `CLEAN` | Verifies held-set tracking integrity across libc condvar suspensions. |
| `12_timedlock` | Deadlock on `pthread_mutex_timedlock` that unblocks on timeout | `ORDER INVERSION` | `ACTIVE_TIMED` | Distinguishes temporary timed stalls from fatal freezes. |

---

## 6. Guide for LLM Tutoring Prompts

When using this document to teach or study DeadlockScope, the following prompts can be used with an LLM to explore the codebase in depth:

1. **"Explain how `interceptor.c` intercepts `pthread_mutex_lock` without causing infinite recursion."**  
   *Target Concept:* Focus on `__thread int ds_in_hook`, `RTLD_NEXT`, and why static allocation prevents `malloc` reentrancy.
2. **"Walk me through the mathematical proof behind `ds_filter_gate_lock` in `filters.c`."**  
   *Target Concept:* Focus on mutex mutual exclusion, set intersection of `held_before`, and why the gate lock cannot be a cycle member.
3. **"Why does DeadlockScope use an asynchronous Watchdog thread instead of checking for deadlocks only at process exit?"**  
   *Target Concept:* Focus on the fact that deadlocked processes never reach `exit()`, so `__attribute__((destructor))` never executes.
4. **"Compare how Linux `lockdep` and DeadlockScope handle `04_gate_lock.c`."**  
   *Target Concept:* Focus on why lockdep treats lock classes blindly while DeadlockScope inspects the witness held set to suppress the false alarm.
5. **"How does DeadlockScope distinguish between `ACTIVE` and `ACTIVE_TIMED` findings?"**  
   *Target Concept:* Focus on `WaitKind` (`WK_TIMEDLOCK`) and why timeouts mean a stall is self-resolving rather than permanent.

---
*Document compiled and verified against DeadlockScope v1.0.0 (Master).*
