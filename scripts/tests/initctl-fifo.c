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
        if (i == 0) {
            check(write(fd, "x", 1) == 1, "buffer FIFO data");
        } else {
            char byte;
            errno = 0;
            check(read(fd, &byte, 1) == -1 && errno == EAGAIN, "discard data after last close");
        }
        close(fd);
    }
    check(unlink(path) == 0, "remove FIFO");
}

int main(void)
{
    int descriptors[2];
    check(pipe(descriptors) == 0, "anonymous pipe");
    check(write(descriptors[1], "x", 1) == 1, "anonymous pipe write");
    close(descriptors[1]);
    char byte;
    check(read(descriptors[0], &byte, 1) == 1 && byte == 'x', "anonymous pipe buffered read");
    check(read(descriptors[0], &byte, 1) == 0, "anonymous pipe EOF");
    close(descriptors[0]);
    credentials();
    fifo("/run");
    fifo("/tmp");
    printf("INITCTL_FIFO failures=%u\n", failures);
    return failures != 0;
}
