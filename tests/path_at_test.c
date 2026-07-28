#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <fs/vfs.h>
#include <kernel/errno.h>

static void expect_path(const char *base, const char *path, const char *expected)
{
    char resolved[VFS_PATH_MAX];
    int  ret = vfs_resolve_path(base, path, resolved, sizeof(resolved));

    assert(ret == 0);
    assert(strcmp(resolved, expected) == 0);
}

int main(void)
{
    expect_path("/home/firefox", "profile/prefs.js", "/home/firefox/profile/prefs.js");
    expect_path("/home/firefox/profile", "../cache/./startup/", "/home/firefox/cache/startup");
    expect_path("/home/firefox", "../../../etc/fonts", "/etc/fonts");
    expect_path("/ignored/base", "/usr//lib/../share/fonts", "/usr/share/fonts");
    expect_path("/", ".", "/");
    expect_path("/", "", "/");

    char small[8];
    assert(vfs_resolve_path("/", "too-long", small, sizeof(small)) == -ENAMETOOLONG);
    assert(vfs_resolve_path("relative", "file", small, sizeof(small)) == -EINVAL);

    struct vfs_node root = {.name = "/"};
    struct vfs_node home = {.parent = &root, .name = "home"};
    struct vfs_node user = {.parent = &home, .name = "firefox"};
    char            node_path[32];
    assert(vfs_node_path(&user, node_path, sizeof(node_path)) == 0);
    assert(strcmp(node_path, "/home/firefox") == 0);
    assert(vfs_node_path(&root, node_path, 1) == -EINVAL);

    puts("path_at_test: ok");
    return 0;
}
