#ifndef DS_FILTERS_H
#define DS_FILTERS_H

#include "ds_common.h"

SuppressReason ds_evaluate_filters(const Cycle *cycle);
bool ds_filter_same_thread(const Cycle *cycle);
bool ds_filter_gate_lock(const Cycle *cycle, MutexId *out_gate);
bool ds_filter_lifecycle(const Cycle *cycle);

#endif /* DS_FILTERS_H */
