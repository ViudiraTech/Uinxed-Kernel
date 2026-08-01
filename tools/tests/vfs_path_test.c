/*
 * VFS pathname normalization regression tests.
 */

#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_path(const char *base, const char *path, const char *expected)
{
    char output[VFS_PATH_MAX];
    int  status = vfs_resolve_path(base, path, output, sizeof(output));
    if (status != EOK || strcmp(output, expected)) {
        fprintf(stderr, "vfs_resolve_path(%s, %s): status=%d path=%s expected=%s\n", base, path, status,
                status == EOK ? output : "<error>", expected);
        exit(1);
    }
}

int main(void)
{
    char output[VFS_PATH_MAX];

    expect_path("/work/tree", "../file", "/work/file");
    expect_path("/work/tree", "/etc///./config", "/etc/config");
    expect_path("/work/tree", "/", "/");
    expect_path("/work/tree", "////", "/");
    expect_path("/", "../../etc", "/etc");

    if (vfs_resolve_path("/work", "", output, sizeof(output)) != -ENOENT) {
        fputs("empty pathname must fail with -ENOENT\n", stderr);
        return 1;
    }

    puts("PASS VFS pathname normalization and empty-path Linux semantics");
    return 0;
}
