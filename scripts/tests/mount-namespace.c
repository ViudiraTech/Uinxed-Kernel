#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line=%d %s errno=%d\n", __LINE__, #x, errno); failures++; } } while (0)

static void check_dir(int fd, const char *name)
{
    struct stat st;
    CHECK(fstatat(fd, name, &st, 0) == 0 && S_ISDIR(st.st_mode));
}

static void child(const char *target)
{
    CHECK(unshare(CLONE_NEWNS) == 0);
    CHECK(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
    int from = open("/", O_PATH | O_DIRECTORY);
    int to = open(target, O_PATH | O_DIRECTORY);
    CHECK(from >= 0 && to >= 0);
    char src[64], dst[64], path[256];
    snprintf(src, sizeof(src), "/proc/self/fd/%d", from);
    snprintf(dst, sizeof(dst), "/proc/self/fd/%d", to);
    CHECK(mount(src, dst, NULL, MS_BIND | MS_REC, NULL) == 0);
    close(to);
    to = open(target, O_PATH | O_DIRECTORY);
    snprintf(dst, sizeof(dst), "/proc/self/fd/%d", to);
    CHECK(mount(dst, dst, NULL, MS_BIND | MS_REC, NULL) == 0);
    close(to);
    to = open(target, O_PATH | O_DIRECTORY);
    CHECK(to >= 0);
    check_dir(to, "etc");
    check_dir(to, "proc");
    check_dir(to, "dev/pts");
    struct statx stx;
    CHECK(statx(to, "", AT_EMPTY_PATH, STATX_BASIC_STATS | STATX_MNT_ID, &stx) == 0);
    CHECK((stx.stx_attributes & STATX_ATTR_MOUNT_ROOT) != 0);
    struct statx etc, original;
    CHECK(statx(to, "etc", 0, STATX_MNT_ID, &etc) == 0);
    CHECK(statx(AT_FDCWD, "/etc", 0, STATX_MNT_ID, &original) == 0);
    CHECK(etc.stx_mnt_id == stx.stx_mnt_id && etc.stx_mnt_id != original.stx_mnt_id);
    int etcfd = openat(to, "etc", O_PATH | O_DIRECTORY);
    CHECK(etcfd >= 0);
    snprintf(dst, sizeof(dst), "/proc/self/fd/%d", etcfd);
    char link[256];
    ssize_t length = readlink(dst, link, sizeof(link) - 1);
    CHECK(length > 0);
    if (length > 0) {
        link[length] = 0;
        snprintf(path, sizeof(path), "%s/etc", target);
        CHECK(!strcmp(link, path));
    }
    if (etcfd >= 0) close(etcfd);
    char stage[] = "/run/proc-stage-XXXXXX";
    CHECK(mkdtemp(stage) != NULL);
    CHECK(mount("proc", stage, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0);
    snprintf(path, sizeof(path), "%s/proc", target);
    CHECK(umount2(path, MNT_DETACH) == 0);
    CHECK(mount(stage, path, NULL, MS_MOVE, NULL) == 0);
    snprintf(path, sizeof(path), "%s/proc/self/mountinfo", target);
    int info = open(path, O_RDONLY);
    char buffer[1024];
    CHECK(info >= 0 && read(info, buffer, sizeof(buffer)) > 0);
    if (info >= 0) close(info);
    snprintf(path, sizeof(path), "%s/etc", target);
    struct statfs fs;
    CHECK(statfs(path, &fs) == 0 && fs.f_type != 0x9fa0);
    CHECK(rmdir(stage) == 0);
    CHECK(chdir(target) == 0);
    CHECK(mount(".", "/", NULL, MS_MOVE, NULL) == 0);
    CHECK(chroot(".") == 0);
    info = open("/proc/sys/kernel/domainname", O_RDONLY);
    CHECK(info >= 0 && read(info, buffer, sizeof(buffer)) > 0);
    if (info >= 0) close(info);
    close(to);
    close(from);
    _exit(failures ? 1 : 0);
}

int main(void)
{
    setbuf(stdout, NULL);
    char target[] = "/run/mount-namespace-XXXXXX";
    CHECK(mkdtemp(target) != NULL);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) child(target);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char path[256];
    snprintf(path, sizeof(path), "%s/etc", target);
    struct stat st;
    CHECK(stat(path, &st) < 0 && errno == ENOENT);
    CHECK(rmdir(target) == 0);
    printf("MOUNT_NAMESPACE failures=%u\n", failures);
    return failures != 0;
}
