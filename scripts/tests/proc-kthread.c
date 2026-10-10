/* Guest regression for systemd's /proc/<pid>/stat kernel-thread filter.
 * cc -O2 -Wall -Wextra -Werror scripts/tests/proc-kthread.c -o proc-kthread
 * Run in the initial PID namespace before testing systemctl poweroff/reboot.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LINUX_PF_KTHREAD 0x00200000ULL

static unsigned failures;

static void check(int good, const char *what, long pid)
{
    if (!good) {
        fprintf(stderr, "FAIL pid=%ld %s\n", pid, what);
        failures++;
    }
}

static int read_stat(long pid, unsigned long long *flags, char *name, size_t capacity)
{
    char path[64], line[2048];
    snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    FILE *file = fopen(path, "re");
    if (!file) return -1;
    char *result = fgets(line, sizeof(line), file);
    fclose(file);
    if (!result) return -1;
    char *open = strchr(line, '('), *close = strrchr(line, ')');
    if (!open || !close || close <= open) return -1;
    size_t length = (size_t)(close - open - 1);
    if (length >= capacity) return -1;
    memcpy(name, open + 1, length);
    name[length] = 0;

    /* Locate flags exactly as systemd v257 pid_is_kernel_thread() does:
     * skip state, ppid, pgrp, session, tty_nr and tpgid after the final ')'.
     * Also check all 52 ABI fields, so inserting flags cannot shift accounting.
     */
    unsigned field = 3;
    char    *save  = NULL;
    for (char *word = strtok_r(close + 1, " \t\n", &save); word; word = strtok_r(NULL, " \t\n", &save), field++) {
        if (field == 9) {
            char *end;
            errno  = 0;
            *flags = strtoull(word, &end, 10);
            if (errno || *end) return -1;
        }
    }
    return field == 53 ? 0 : -1;
}

static void inspect(long pid, int kernel)
{
    char               name[128];
    unsigned long long flags  = 0;
    int                result = read_stat(pid, &flags, name, sizeof(name));
    check(result == 0, "read complete 52-field stat", pid);
    if (result) return;
    printf("PROC_KTHREAD pid=%ld name=%s flags=%llu kernel=%d\n", pid, name, flags, kernel);
    check(!!(flags & LINUX_PF_KTHREAD) == kernel, "stat PF_KTHREAD classification", pid);

    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%ld/status", pid);
    FILE *file = fopen(path, "re");
    check(file != NULL, "open status", pid);
    if (!file) return;
    int value = -1;
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "Kthread: %d", &value) == 1) break;
    fclose(file);
    check(value == kernel, "status Kthread classification", pid);

    if (kernel) {
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        file = fopen(path, "re");
        check(file != NULL, "open kernel cmdline", pid);
        if (file) {
            check(fgetc(file) == EOF && !ferror(file), "empty kernel cmdline", pid);
            fclose(file);
        }
        check(kill((pid_t)pid, 0) == 0, "kernel thread still exists", pid);
    }
}

int main(void)
{
    inspect(1, 0);
    inspect((long)getpid(), 0);
    inspect(2, 1);

    static const char *const workers[] = {"net-loopback", "drm-atomic", "e1000-poll", "video-refresh", "timer-deferred"};
    DIR                     *directory = opendir("/proc");
    check(directory != NULL, "enumerate procfs", 0);
    unsigned       found = 0;
    struct dirent *entry;
    while (directory && (entry = readdir(directory))) {
        char *end;
        long  pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 2) continue;
        unsigned long long flags = 0;
        char               name[128];
        if (read_stat(pid, &flags, name, sizeof(name))) continue;
        for (size_t i = 0; i < sizeof(workers) / sizeof(workers[0]); i++) {
            if (strcmp(name, workers[i])) continue;
            inspect(pid, 1);
            found++;
            break;
        }
    }
    if (directory) closedir(directory);
    check(found >= 2, "found persistent subsystem kernel workers", 0);
    printf("PROC_KTHREAD_PASS workers=%u failures=%u\n", found, failures);
    return failures != 0;
}
