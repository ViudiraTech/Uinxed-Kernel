/*
 * Mount propagation: does a mount made in one namespace actually reach the
 * namespaces Linux says it should, and only those?
 *
 *   shared   - peers in one peer group see each other's mounts
 *   slave    - receives from its master, never sends back
 *   private  - neither sends nor receives
 *   rbind    - a recursive bind owns its own copy; unmounting the source
 *              must not take the copy with it
 *
 * Every case runs the second namespace in a forked child so one failure cannot
 * contaminate the next.  Pipes order the two sides; without them the test
 * would race and pass or fail at random.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line=%d %s errno=%d\n", __LINE__, #x, errno); failures++; } } while (0)

static int send_byte(int fd)
{
    char c = 'x';
    return write(fd, &c, 1) == 1;
}

static int wait_byte(int fd)
{
    char c;
    return read(fd, &c, 1) == 1;
}

/* Is `path` a mount point (its mount id differs from its parent directory's)? */
static int mount_id_of(const char *path, unsigned long long *out)
{
    struct statx stx;
    if (statx(AT_FDCWD, path, 0, STATX_MNT_ID, &stx) != 0) return -1;
    *out = stx.stx_mnt_id;
    return 0;
}

static int is_separate_mount(const char *path, const char *over)
{
    unsigned long long a = 0, b = 0;
    if (mount_id_of(path, &a) != 0 || mount_id_of(over, &b) != 0) return 0;
    return a != b;
}

/*
 * A mount created under a shared mount propagates to the peer groups, so a
 * child that unshared its namespace (thereby joining the group) must see it.
 */
static void case_shared(const char *base)
{
    char dir[256], sub[256];
    snprintf(dir, sizeof(dir), "%s/shared", base);
    snprintf(sub, sizeof(sub), "%s/shared/sub", base);
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(mount("tmpfs", dir, "tmpfs", 0, NULL) == 0);
    CHECK(mount(NULL, dir, NULL, MS_SHARED, NULL) == 0);
    CHECK(mkdir(sub, 0700) == 0);

    int down[2], up[2];
    CHECK(pipe(down) == 0 && pipe(up) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        close(down[1]);
        close(up[0]);
        unsigned before = failures;
        CHECK(unshare(CLONE_NEWNS) == 0);
        send_byte(up[1]);
        if (!wait_byte(down[0])) _exit(1);
        CHECK(is_separate_mount(sub, dir));
        _exit(failures != before);
    }
    close(down[0]);
    close(up[1]);
    CHECK(wait_byte(up[0]) == 1);
    CHECK(mount("tmpfs", sub, "tmpfs", 0, NULL) == 0);
    send_byte(down[1]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(umount2(sub, MNT_DETACH) == 0);
    CHECK(rmdir(sub) == 0); /* inside the tmpfs, so before dir goes away */
    CHECK(umount2(dir, MNT_DETACH) == 0);
    CHECK(rmdir(dir) == 0);
}

/*
 * A slave receives its master's events but sends nothing back, so the same
 * mount must be visible in the child and the child's own mount invisible in
 * the parent.
 */
static void case_slave(const char *base)
{
    char dir[256], sub[256], own[256];
    snprintf(dir, sizeof(dir), "%s/slave", base);
    snprintf(sub, sizeof(sub), "%s/slave/sub", base);
    snprintf(own, sizeof(own), "%s/slave/own", base);
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(mount("tmpfs", dir, "tmpfs", 0, NULL) == 0);
    CHECK(mount(NULL, dir, NULL, MS_SHARED, NULL) == 0);
    CHECK(mkdir(sub, 0700) == 0);
    CHECK(mkdir(own, 0700) == 0);

    int down[2], up[2];
    CHECK(pipe(down) == 0 && pipe(up) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        close(down[1]);
        close(up[0]);
        unsigned before = failures;
        CHECK(unshare(CLONE_NEWNS) == 0);
        /* Leave the peer group: receive from the old group, send to nobody. */
        CHECK(mount(NULL, dir, NULL, MS_SLAVE, NULL) == 0);
        send_byte(up[1]);
        if (!wait_byte(down[0])) _exit(1);
        CHECK(is_separate_mount(sub, dir)); /* received from the master */
        CHECK(mount("tmpfs", own, "tmpfs", 0, NULL) == 0);
        send_byte(up[1]);
        _exit(failures != before);
    }
    close(down[0]);
    close(up[1]);
    CHECK(wait_byte(up[0]) == 1);
    CHECK(mount("tmpfs", sub, "tmpfs", 0, NULL) == 0);
    send_byte(down[1]);
    CHECK(wait_byte(up[0]) == 1);
    /* The child's mount must not have come back up to us. */
    CHECK(!is_separate_mount(own, dir));
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(umount2(sub, MNT_DETACH) == 0);
    CHECK(rmdir(sub) == 0);
    CHECK(rmdir(own) == 0); /* both live inside the tmpfs */
    CHECK(umount2(dir, MNT_DETACH) == 0);
    CHECK(rmdir(dir) == 0);
}

/* A private mount shares with nobody: a sibling namespace must not see it. */
static void case_private(const char *base)
{
    char dir[256], sub[256];
    snprintf(dir, sizeof(dir), "%s/priv", base);
    snprintf(sub, sizeof(sub), "%s/priv/sub", base);
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(mount("tmpfs", dir, "tmpfs", 0, NULL) == 0);
    CHECK(mount(NULL, dir, NULL, MS_PRIVATE, NULL) == 0);
    CHECK(mkdir(sub, 0700) == 0);

    int down[2], up[2];
    CHECK(pipe(down) == 0 && pipe(up) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        close(down[1]);
        close(up[0]);
        unsigned before = failures;
        CHECK(unshare(CLONE_NEWNS) == 0);
        send_byte(up[1]);
        if (!wait_byte(down[0])) _exit(1);
        CHECK(!is_separate_mount(sub, dir));
        _exit(failures != before);
    }
    close(down[0]);
    close(up[1]);
    CHECK(wait_byte(up[0]) == 1);
    CHECK(mount("tmpfs", sub, "tmpfs", 0, NULL) == 0);
    send_byte(down[1]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(umount2(sub, MNT_DETACH) == 0);
    CHECK(rmdir(sub) == 0);
    CHECK(umount2(dir, MNT_DETACH) == 0);
    CHECK(rmdir(dir) == 0);
}

/*
 * A recursive bind replicates the subtree into mounts of its own: unmounting
 * the original must leave the copy mounted.  If the two shared one object they
 * would go away together.
 */
static void case_recursive_bind(const char *base)
{
    char src[256], inner[256], copy[256], copied_inner[256];
    snprintf(src, sizeof(src), "%s/rb", base);
    snprintf(inner, sizeof(inner), "%s/rb/inner", base);
    snprintf(copy, sizeof(copy), "%s/rbcopy", base);
    snprintf(copied_inner, sizeof(copied_inner), "%s/rbcopy/inner", base);
    CHECK(mkdir(src, 0700) == 0);
    CHECK(mkdir(copy, 0700) == 0);
    CHECK(mkdir(inner, 0700) == 0);
    CHECK(mount("tmpfs", inner, "tmpfs", 0, NULL) == 0);

    CHECK(mount(src, copy, NULL, MS_BIND | MS_REC, NULL) == 0);
    CHECK(is_separate_mount(copied_inner, copy)); /* the submount was replicated */

    /* Unmounting the original inner mount must not disturb the copy's. */
    CHECK(umount2(inner, MNT_DETACH) == 0);
    CHECK(is_separate_mount(copied_inner, copy));

    CHECK(umount2(copied_inner, MNT_DETACH) == 0);
    CHECK(umount2(copy, MNT_DETACH) == 0);
    /* The bind shares the tree, so copied_inner *is* inner: remove it once. */
    CHECK(rmdir(inner) == 0);
    CHECK(rmdir(copy) == 0 && rmdir(src) == 0);
}

int main(void)
{
    setbuf(stdout, NULL);
    /* Work in our own namespace so the cases cannot leak into the host tree. */
    if (unshare(CLONE_NEWNS) || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) {
        perror("isolate");
        return 2;
    }
    char base[] = "/run/propagation-XXXXXX";
    if (!mkdtemp(base)) return 2;

    case_shared(base);
    case_slave(base);
    case_private(base);
    case_recursive_bind(base);

    CHECK(rmdir(base) == 0);
    printf("MOUNT_PROPAGATION failures=%u\n", failures);
    return failures != 0;
}
