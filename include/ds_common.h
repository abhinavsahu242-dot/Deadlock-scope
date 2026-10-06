#ifndef DS_COMMON_H
#define DS_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define MAX_THREADS       64
#define MAX_LOCKS         512
#define MAX_HELD_LOCKS    16
#define MAX_EDGES         1024
#define MAX_WITNESSES     4
#define MAX_CYCLE_DEPTH   8
#define MAX_FINDINGS      64

typedef uintptr_t MutexId;
typedef uint16_t  ThreadId;
typedef uint16_t  LockIndex;

typedef enum {
    WK_NONE = 0,
    WK_LOCK,
    WK_TIMEDLOCK,
    WK_COND
} WaitKind;

typedef struct {
    MutexId locks[MAX_HELD_LOCKS];
    void   *sites[MAX_HELD_LOCKS];
    uint8_t count;
} HeldLockSet;

typedef struct {
    ThreadId        tid;
    pthread_t       pthread_id;
    atomic_int      active;
    HeldLockSet     held;
    
    /* Live wait state */
    atomic_int      is_waiting;
    atomic_int      wait_kind;
    atomic_uintptr_t waiting_for; /* MutexId */
    void           *wait_site;
    uint64_t        wait_start_ns;
    
    /* Lifecycle ancestry */
    ThreadId        parent_tid;
    uint64_t        spawn_seq;
    uint64_t        join_seq;
} ThreadSlot;

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

typedef struct {
    uint8_t length;
    MutexId locks[MAX_CYCLE_DEPTH];
    Witness witnesses[MAX_CYCLE_DEPTH];
    bool    contains_timed_wait;
} Cycle;

typedef enum {
    FINDING_POTENTIAL,
    FINDING_SUPPRESSED,
    FINDING_ACTIVE,
    FINDING_ACTIVE_TIMED,
    FINDING_CONFIRMED
} FindingKind;

typedef enum {
    SUPPRESS_NONE = 0,
    SUPPRESS_SAME_THREAD,
    SUPPRESS_GATE_LOCK,
    SUPPRESS_LIFECYCLE
} SuppressReason;

typedef struct {
    FindingKind    kind;
    SuppressReason suppress_reason;
    Cycle          cycle;
    uint64_t       detected_time_ms;
} Finding;

#define FILTER_SAME_THREAD (1 << 0)
#define FILTER_GATE_LOCK   (1 << 1)
#define FILTER_LIFECYCLE   (1 << 2)
#define FILTER_ALL         (FILTER_SAME_THREAD | FILTER_GATE_LOCK | FILTER_LIFECYCLE)

typedef struct {
    int stress_enabled;
    int stress_delay_us;
    int poll_ms;
    int confirm_ms;
    int debug;
    int filter_mask;
    char dot_file[256];
} DSConfig;

/* Global externs */
extern DSConfig g_config;
extern ThreadSlot g_threads[MAX_THREADS];
extern atomic_uintptr_t g_lock_owners[MAX_LOCKS];
extern MutexId g_lock_table[MAX_LOCKS];
extern int g_lock_count;
extern atomic_uint_fast64_t g_event_seq;

extern __thread int ds_in_hook;
extern __thread ThreadId ds_my_tid;
extern __thread int ds_tid_initialized;

void ds_spin_lock(void);
void ds_spin_unlock(void);
uint64_t ds_get_time_ns(void);
uint64_t ds_get_time_ms(void);
const char *ds_symbolize_site(void *addr, char *buf, size_t buflen);

#endif /* DS_COMMON_H */
