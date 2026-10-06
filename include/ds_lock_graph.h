#ifndef DS_LOCK_GRAPH_H
#define DS_LOCK_GRAPH_H

#include "ds_common.h"

void ds_log_init(void);
void ds_log_record_order(ThreadId tid, MutexId to, void *site_to);
int ds_log_find_cycles(Cycle out_cycles[], int max_cycles);
bool ds_cycles_equal(const Cycle *c1, const Cycle *c2);
void ds_log_retire_lock(MutexId m);
int ds_log_get_edge_count(void);
const LockOrderEdge* ds_log_get_edge(int index);

#endif /* DS_LOCK_GRAPH_H */
