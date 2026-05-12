#pragma once

#ifndef UNIT_TEST
#include "worker.h"
void stats_export_and_expire(struct worker_ctx *ctx, uint64_t now_tsc);
#else
#include <stdio.h>
#include "flow.h"
/* Minimal signature for unit tests */
void stats_write_row(FILE *f, const struct flow_entry *e);
void stats_write_header(FILE *f);
#endif
