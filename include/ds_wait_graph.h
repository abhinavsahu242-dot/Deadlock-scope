#ifndef DS_WAIT_GRAPH_H
#define DS_WAIT_GRAPH_H

#include "ds_common.h"

void ds_wait_begin(ThreadId tid, MutexId m, void *site, WaitKind kind);
void ds_wait_end(ThreadId tid);
int ds_wfg_detect_active_cycle(Cycle *out_cycle);

#endif /* DS_WAIT_GRAPH_H */
