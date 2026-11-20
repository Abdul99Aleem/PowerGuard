/*
 * Optional one-time HAM registration utility for the standalone pgbench.
 * QNX HAM must be started separately. Use an absolute executable path.
 * This process exits after registering a restart-on-death action.
 */
#include <ha/ham.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s unique_entity_name '/absolute/path/pgbench serve /absolute/path/results.csv'\n", argv[0]);
        return 2;
    }
    if (argv[2][0] != '/') {
        fprintf(stderr, "Restart command must start with an absolute executable path\n");
        return 2;
    }
    if (ham_connect(0) == -1) { perror("ham_connect (start HAM first)"); return 1; }
    ham_entity_t *e = ham_attach(argv[1], 0, -1, argv[2], 0);
    if (!e) { perror("ham_attach"); ham_disconnect(0); return 1; }
    ham_condition_t *d = ham_condition(e, CONDDEATH, "death", HREARMAFTERRESTART);
    if (!d) { perror("ham_condition"); ham_disconnect(0); return 1; }
    ham_action_t *a = ham_action_restart(d, "restart", argv[2], HREARMAFTERRESTART);
    if (!a) { perror("ham_action_restart"); ham_disconnect(0); return 1; }
    printf("HAM: attached entity %s and registered persistent restart-on-death action\n", argv[1]);
    ham_disconnect(0);
    return 0;
}
