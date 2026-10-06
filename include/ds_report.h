#ifndef DS_REPORT_H
#define DS_REPORT_H

#include "ds_common.h"

void ds_report_init(void);
void ds_report_add_finding(FindingKind kind, SuppressReason reason, const Cycle *cycle);
bool ds_report_has_finding(FindingKind kind, const Cycle *cycle);
void ds_report_print_finding(const Finding *f);
void ds_report_dump_summary(void);
void ds_report_export_dot(const char *filename);

#endif /* DS_REPORT_H */
