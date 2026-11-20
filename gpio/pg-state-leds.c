/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * gpio/pg-state-leds.c
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#include "ipc.h"

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/wait.h>
#include <time.h>

extern char **environ;

static char *const pins[] = {
    [NORMAL] = "17", [WARNING] = "27", [THROTTLED] = "22", [CRITICAL] = "23", [RECOVERY] = "24",
};
static char all_pins[] = "17,27,22,23,24";
static volatile sig_atomic_t stopping;

static void stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int set_gpio(char *driver, char *pin, char *mode, char *level)
{
    char *args[] = {driver, "set", pin, mode, level, NULL};
    pid_t child;
    int error = posix_spawnp(&child, driver, NULL, NULL, args, environ);
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", driver, strerror(error));
        return -1;
    }

    int status;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        perror("waitpid");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s failed for GPIO %s\n", driver, pin);
        return -1;
    }
    return 0;
}

static int read_state(int coid, int *state)
{
    const pg_message_v1 request = {.type = PG_READ, .version = PG_VERSION};
    pg_reply_v1 reply = {0};
    uint64_t timeout = 100 * PG_MS;

    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout,
                     NULL) == -1 ||
        MsgSend(coid, &request, sizeof(request), &reply, sizeof(reply)) == -1 ||
        reply.version != PG_VERSION || reply.decision.state >= sizeof(pins) / sizeof(pins[0])) {
        return -1;
    }
    *state = (int)reply.decision.state;
    return 0;
}

int main(int argc, char **argv)
{
    char *driver = NULL;
    if (argc == 3 && strcmp(argv[1], "--board") == 0) {
        if (strcmp(argv[2], "pi4") == 0) {
            driver = "gpio-bcm2711";
        } else if (strcmp(argv[2], "pi5") == 0) {
            driver = "gpio-rp1";
        }
    }
    if (driver == NULL) {
        fprintf(stderr, "Usage: %s --board pi4|pi5\n", argv[0]);
        return 2;
    }

    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 || sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    if (set_gpio(driver, all_pins, "op", "dl") == -1) {
        return 1;
    }

    const struct timespec interval = {.tv_nsec = 100 * PG_MS};
    int coid = -1;
    int displayed = -1;
    int result = 0;
    while (!stopping) {
        int state = -1;
        if (coid == -1) {
            coid = name_open(PG_NAME, 0);
        }
        if (coid != -1 && read_state(coid, &state) == -1) {
            name_close(coid);
            coid = -1;
        }
        if (stopping) {
            break;
        }

        if (state != displayed) {
            if ((displayed >= 0 && set_gpio(driver, pins[displayed], "dl", NULL) == -1) ||
                (state >= 0 && set_gpio(driver, pins[state], "dh", NULL) == -1)) {
                result = 1;
                break;
            }
            displayed = state;
        }
        nanosleep(&interval, NULL);
    }

    if (coid != -1) {
        name_close(coid);
    }
    if (set_gpio(driver, all_pins, "dl", NULL) == -1) {
        result = 1;
    }
    return result;
}
