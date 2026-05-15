#pragma once

#include <stdio.h>
#include "flow.h"
#include "worker.h"

void stats_write_header(FILE *f);
void stats_write_row(FILE *f, const struct flow_entry *e, const char *ts);
void stats_export_and_expire(struct worker_ctx *ctx, uint64_t now_tsc);
