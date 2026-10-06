#ifndef DS_STRESS_H
#define DS_STRESS_H

#include "ds_common.h"

void ds_stress_init(void);
void ds_stress_register_target_locks(const Cycle *cycle);
bool ds_stress_is_target(MutexId m);
void ds_stress_maybe_delay(ThreadId tid, MutexId m);

#endif /* DS_STRESS_H */
