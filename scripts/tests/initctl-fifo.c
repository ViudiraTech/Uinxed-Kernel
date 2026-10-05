#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned failures;
static void     check(int good, const char *what)
{
    if (!good) {
        fprintf(stderr, "FAIL %s errno=%d\n", what, errno);
        failures++;
    }
}

static void credentials(void)
{
    FILE *status = fopen("/proc/1/status", "r");
    check(status != NULL, "PID 1 status");
    if (!status) return;
    char     line[512];
    unsigned seen = 0, real, effective, saved, filesystem;
    while (fgets(line, sizeof(line), status)) {
        if (sscanf(line, "Uid: %u %u %u %u", &real, &effective, &saved, &filesystem) == 4 || sscanf(line, "Gid: %u %u %u %u", &real, &effective, &saved, &filesystem) == 4) {
            printf("PID1 %s", line);
            check(!real && !effective && !saved && !filesystem, "PID 1 root credentials");
            seen++;
        }
    }
    fclose(status);
    check(seen == 2, "PID 1 uid/gid records");
}

static void fifo(const char *directory)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/initctl-probe-%ld", directory, (long)getpid());
    mode_t previous = umask(~(mode_t)0600);
    umask(previous | ~(mode_t)0600);
    int created = mkfifo(path, 0600);
    umask(previous);
    check(created == 0, "create FIFO");
    if (created) return;
    errno = 0;
    check(mkfifo(path, 0600) == -1 && errno == EEXIST, "duplicate FIFO");
    for (unsigned i = 0; i < 2; i++) {
        int fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW);
        check(fd >= 0, "open FIFO");
        if (fd < 0) continue;
        struct stat st;
        int         rc = fstat(fd, &st);
        check(rc == 0, "fstat FIFO");
        if (!rc) {
            printf("FIFO %s mode=%o uid=%u gid=%u caller=%u:%u\n", path, st.st_mode, st.st_uid, st.st_gid, getuid(), getgid());
            check(S_ISFIFO(st.st_mode), "FIFO type");
            check((st.st_mode & 0777) == (0600 & ~previous), "FIFO mode");
            check(st.st_uid == getuid() && st.st_gid == getgid(), "FIFO credentials");
        }
        close(fd);
    }
    check(unlink(path) == 0, "remove FIFO");
}

int main(void)
{
    credentials();
    fifo("/run");
    fifo("/tmp");
    printf("INITCTL_FIFO failures=%u\n", failures);
    return failures != 0;
}
