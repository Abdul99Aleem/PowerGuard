/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * .milestone-ham-suite.c
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#define _QNX_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ham-suite-assets.h"

extern char **environ;
static const char *ham;
static const char *hamctrl;

static const char *find_utility(const char *name, char *path, size_t size)
{
    static const char *const dirs[] = {"/usr/sbin", "/proc/boot", "/sbin", "/usr/bin", "/bin"};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        snprintf(path, size, "%s/%s", dirs[i], name);
        if (access(path, X_OK) == 0) {
            return path;
        }
        printf("Target utility %s: %s\n", path, strerror(errno));
    }
    return NULL;
}

static int run(char *const argv[])
{
    pid_t pid;
    int error = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);
    if (error) {
        errno = error;
        perror(argv[0]);
        return 1;
    }
    int status;
    do {
        if (waitpid(pid, &status, 0) >= 0) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
    } while (errno == EINTR);
    perror("waitpid");
    return 1;
}

static int start_ham(void)
{
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t files;
    int error = posix_spawnattr_init(&attr);
    if (error) {
        return error;
    }
    error = posix_spawn_file_actions_init(&files);
    if (!error) {
        error = posix_spawnattr_setxflags(&attr, POSIX_SPAWN_SETSID);
        if (!error) {
            error =
                posix_spawn_file_actions_addopen(&files, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        }
        if (!error) {
            error =
                posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        }
        if (!error) {
            error =
                posix_spawn_file_actions_addopen(&files, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        }
        if (!error) {
            char *argv[] = {(char *)ham, "-v", "-f", "/tmp/powerguard-ham.log", NULL};
            pid_t pid;
            error = posix_spawn(&pid, argv[0], &files, &attr, argv, environ);
            if (!error) {
                printf("Started HAM as a separate session, PID %ld.\n", (long)pid);
            }
        }
        posix_spawn_file_actions_destroy(&files);
    }
    posix_spawnattr_destroy(&attr);
    return error;
}

static int show_ham_state(void)
{
    FILE *info = fopen("/proc/ham/.info", "r");
    if (!info) {
        perror("HAM status");
        return 1;
    }
    char line[256];
    while (fgets(line, sizeof line, info)) {
        fputs(line, stdout);
    }
    if (ferror(info)) {
        perror("reading HAM status");
        fclose(info);
        return 1;
    }
    fclose(info);
    char *query[] = {(char *)hamctrl, "=heartbeat", NULL};
    return run(query);
}

static int run_trial(const char *label, const char *name, const unsigned char *bytes, size_t count)
{
    printf("\n=== %s ===\n", label);
    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0700);
    if (fd < 0) {
        perror(name);
        return 1;
    }
    while (count) {
        ssize_t written = write(fd, bytes, count);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            perror(name);
            close(fd);
            return 1;
        }
        bytes += written;
        count -= (size_t)written;
    }
    if (close(fd) != 0) {
        perror(name);
        return 1;
    }
    char *argv[] = {(char *)name, NULL};
    int status = run(argv);
    printf("HAM-on %s exit=%d\n", label, status);
    return status != 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct utsname board;
    if (uname(&board) == 0) {
        printf("Target %s, %s %s, %s\n", board.nodename, board.sysname, board.release,
               board.machine);
    }
    char ham_path[128];
    char hamctrl_path[128];
    ham = find_utility("ham", ham_path, sizeof ham_path);
    hamctrl = find_utility("hamctrl", hamctrl_path, sizeof hamctrl_path);
    if (!ham || !hamctrl) {
        fputs(
            "HAM activation blocked: this boot image has no executable HAM utilities in the standard paths.\n",
            stderr);
        return 2;
    }
    printf("HAM utilities: %s and %s\n", ham, hamctrl);
    if (access("/dev/null", R_OK | W_OK) != 0) {
        perror("checking HAM file redirection");
        return 2;
    }
    struct stat info;
    if (stat("/proc/ham/.info", &info) != 0) {
        if (errno != ENOENT) {
            perror("checking HAM");
            return 2;
        }
        int error = start_ham();
        if (error) {
            errno = error;
            perror("starting HAM");
            return 2;
        }
        struct timespec delay = {0, 100000000};
        for (int attempt = 0; attempt < 50 && stat("/proc/ham/.info", &info) != 0; ++attempt) {
            nanosleep(&delay, NULL);
        }
        if (stat("/proc/ham/.info", &info) != 0) {
            perror("waiting for HAM");
            return 2;
        }
    } else {
        puts("HAM was already running.");
    }
    char *enable[] = {(char *)hamctrl, "+heartbeat", NULL};
    if (run(enable) || show_ham_state()) {
        return 2;
    }

    char directory[] = "/tmp/pg-t3-ham-suite-XXXXXX";
    if (!mkdtemp(directory) || chdir(directory) != 0) {
        perror("HAM test directory");
        return 2;
    }
    printf("HAM suite directory=%s\n", directory);
    int failed = run_trial("Throttled", "./throttled", throttled, sizeof throttled);
    failed |= run_trial("Critical", "./critical", critical, sizeof critical);
    failed |= run_trial("Recovery", "./recovery", recovery, sizeof recovery);

    puts("\n=== HAM status after the suite ===");
    failed |= show_ham_state();
    printf("HAM-on suite result=%s\n", failed ? "FAIL" : "PASS");
    return failed != 0;
}
