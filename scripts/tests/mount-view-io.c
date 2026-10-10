#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line=%d %s errno=%d\n", __LINE__, #x, errno); failures++; } } while (0)

static void namespace_child(const char *workspace, int inherited, const char *parent_ns)
{
    unsigned before = failures;
    char ns[128];
    ssize_t n = readlink("/proc/self/ns/mnt", ns, sizeof(ns) - 1);
    CHECK(n > 0);
    if (n > 0) { ns[n] = 0; CHECK(strcmp(ns, parent_ns) != 0); }
    CHECK(mount(NULL, workspace, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) == 0);
    CHECK(write(inherited, "i", 1) == 1);
    CHECK(fchmod(inherited, 0600) == 0);
    char path[256];
    snprintf(path, sizeof(path), "%s/payload", workspace);
    CHECK(open(path, O_WRONLY) < 0 && errno == EROFS);
    _exit(failures != before);
}

static void clone_view(const char *workspace, int inherited, int modern)
{
    char parent_ns[128];
    ssize_t n = readlink("/proc/self/ns/mnt", parent_ns, sizeof(parent_ns) - 1);
    CHECK(n > 0);
    if (n <= 0) return;
    parent_ns[n] = 0;
    struct clone_args args = {.flags = CLONE_NEWNS, .exit_signal = SIGCHLD};
    pid_t pid = modern ? syscall(SYS_clone3, &args, sizeof(args)) : syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
    CHECK(pid >= 0);
    if (!pid) namespace_child(workspace, inherited, parent_ns);
    if (pid > 0) {
        int status;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(write(inherited, "p", 1) == 1);
    }
}

int main(void)
{
    setbuf(stdout, NULL);
    if (unshare(CLONE_NEWNS) || mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL)) { perror("isolate"); return 2; }
    char base[] = "/run/view-io-XXXXXX";
    if (!mkdtemp(base)) return 2;
    char store[128], workspace[128], payload[256], published[256];
    snprintf(store, sizeof(store), "%s/store", base);
    snprintf(workspace, sizeof(workspace), "%s/workspace", base);
    CHECK(mkdir(store, 0700) == 0 && mkdir(workspace, 0700) == 0);
    CHECK(mount("tmpfs", store, "tmpfs", 0, NULL) == 0);
    snprintf(published, sizeof(published), "%s/payload", store);
    int old = open(published, O_CREAT | O_RDWR, 0600);
    CHECK(old >= 0 && write(old, "seed", 4) == 4);
    CHECK(mount(store, workspace, NULL, MS_BIND, NULL) == 0);
    CHECK(mount(NULL, store, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) == 0);
    snprintf(payload, sizeof(payload), "%s/payload", workspace);
    int rw = open(payload, O_RDWR);
    CHECK(rw >= 0);
    struct stat a, b;
    CHECK(fstat(rw, &a) == 0 && fstat(old, &b) == 0 && a.st_ino == b.st_ino);
    CHECK(write(rw, "w", 1) == 1);
    CHECK(pwrite(rw, "p", 1, 0) == 1);
    struct iovec iov = {.iov_base = "v", .iov_len = 1};
    CHECK(writev(rw, &iov, 1) == 1);
    CHECK(fchmod(rw, 0600) == 0 && chmod(payload, 0600) == 0);
    CHECK(fchown(rw, 0, 0) == 0 && chown(payload, 0, 0) == 0);
    CHECK(futimens(rw, NULL) == 0 && utimensat(AT_FDCWD, payload, NULL, 0) == 0);
    CHECK(ftruncate(rw, 4) == 0 && truncate(payload, 4) == 0);
    int input = memfd_create("view-input", 0);
    CHECK(input >= 0 && write(input, "copy", 4) == 4);
    off_t offset = 0;
    CHECK(sendfile(rw, input, &offset, 4) == 4);
    if (input >= 0) close(input);
    CHECK(write(old, "r", 1) < 0 && errno == EROFS);
    CHECK(fchmod(old, 0600) < 0 && errno == EROFS);
    CHECK(ftruncate(old, 0) < 0 && errno == EROFS);
    CHECK(open(published, O_WRONLY) < 0 && errno == EROFS);
    int dir = open(workspace, O_PATH | O_DIRECTORY);
    int tmp = openat(dir, ".", O_TMPFILE | O_RDWR, 0600);
    CHECK(tmp >= 0 && write(tmp, "tmp", 3) == 3 && fchmod(tmp, 0600) == 0);
    if (tmp >= 0) close(tmp);
    tmp = syscall(SYS_open, workspace, O_TMPFILE | O_RDWR, 0600);
    CHECK(tmp >= 0 && write(tmp, "tmp", 3) == 3 && ftruncate(tmp, 1) == 0);
    if (tmp >= 0) close(tmp);
    close(dir);
    clone_view(workspace, rw, 0);
    clone_view(workspace, rw, 1);
    CHECK(mount("tmpfs", store, "tmpfs", 0, NULL) == 0);
    CHECK(access(published, F_OK) < 0 && errno == ENOENT);
    CHECK(umount2(store, MNT_DETACH) == 0);
    CHECK(umount2(store, MNT_DETACH) == 0);
    CHECK(write(old, "r", 1) < 0 && errno == EROFS);
    CHECK(fchmod(old, 0600) < 0 && errno == EROFS);
    CHECK(write(rw, "w", 1) == 1);
    close(old); close(rw);
    printf("MOUNT_VIEW_IO checks=%u failures=%u\n", checks, failures);
    return failures != 0;
}
