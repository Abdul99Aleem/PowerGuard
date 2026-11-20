#ifndef PG_NAMES_H
#define PG_NAMES_H
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Optional instance names let target validation coexist with an installed demo. */
static inline const char *pg_instance_name(const char *base, char *out, size_t size) {
    const char *instance = getenv("PG_INSTANCE");
    if (!instance || !*instance) return base;
    if (strlen(instance) > 32) { fputs("PG_INSTANCE exceeds 32 characters\n", stderr); exit(2); }
    for (const char *p = instance; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            fputs("Invalid PG_INSTANCE\n", stderr); exit(2);
        }
    snprintf(out, size, "%s_%s", base, instance);
    return out;
}
static inline const char *pg_policy_name(void) {
    static char name[128]; return pg_instance_name("powerguard_k_v1", name, sizeof(name));
}
static inline const char *pg_bench_name(void) {
    static char name[128]; return pg_instance_name("powerguard_bench", name, sizeof(name));
}
#endif
