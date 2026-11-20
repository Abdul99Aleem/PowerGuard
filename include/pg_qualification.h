/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * include/pg_qualification.h
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#ifndef PG_QUALIFICATION_H
#define PG_QUALIFICATION_H
#include <stdint.h>

static inline int pg_overhead_result(char mode, int valid, int job_errors, uint64_t cpu_ns,
                                     uint64_t wall_ns)
{
    if (!valid || !wall_ns || (mode != 'A' && mode != 'B' && mode != 'C')) {
        return 2;
    }
    if (job_errors || (mode != 'A' && (double)cpu_ns > 0.02 * wall_ns)) {
        return 1;
    }
    return 0;
}
#endif
