#!/usr/bin/env python3
"""Exercise the actual VFS child-reclaim loop when namespace detach returns early.

The detach stub models an already unlinked/busy node; its list entry must be
removed before the final reference frees it. --baseline REF expects ASan failure.
"""
import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--baseline", metavar="REF")
args = parser.parse_args()


def function(source, name):
    start = source.index(name + "(")
    # Skip declarations and earlier calls: find the function definition's line.
    while True:
        line = source[source.rfind("\n", 0, start) + 1:source.find("\n", start)]
        if line.startswith(("void " + name, "clist_t " + name)):
            break
        start = source.index(name + "(", start + 1)
    start = source.rfind("\n", 0, start) + 1
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


source = (ROOT / "fs/core/vfs.c").read_text()
if args.baseline:
    source = subprocess.check_output(["git", "show", args.baseline + ":fs/core/vfs.c"], cwd=ROOT, text=True)
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct clist *clist_t;
struct clist { void *data; clist_t prev, next; };
typedef struct vfs_node *vfs_node_t;
struct vfs_node { clist_t child; uint32_t refcount; };
static unsigned releases, invalidations, held;
static int vfs_namespace_lock;
static void mutex_lock(int *lock) { (void)lock; assert(!held); held = 1; }
static void mutex_unlock(int *lock) { (void)lock; assert(held); held = 0; }
void vfs_dcache_remove(vfs_node_t node) { assert(node && held); invalidations++; }
static void vfs_namespace_detach(vfs_node_t node) { assert(node && !held); }
static int vfs_close(vfs_node_t node) {
    assert(!held && node->refcount == 1);
    node->refcount--;
    free(node);
    releases++;
    return 0;
}
'''
code += function((ROOT / "libs/list/circular_list.c").read_text(), "clist_delete_node")
code += function(source, "vfs_free_child")
code += r'''
int main(void) {
    struct vfs_node parent = {0};
    for (unsigned i = 0; i < 4; i++) {
        clist_t entry = calloc(1, sizeof(*entry));
        assert(entry);
        if (i != 1) entry->data = calloc(1, sizeof(struct vfs_node));
        entry->next = parent.child;
        if (parent.child) parent.child->prev = entry;
        parent.child = entry;
    }
    vfs_free_child(&parent);
    assert(!parent.child && !held && releases == 3 && invalidations == 3);
    vfs_free_child(&parent);
    vfs_free_child(NULL);
    puts("VFS_CHILD_RECLAIM released=3 dangling_list_entries=0");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="vfs-child-reclaim-") as tmp:
    source_file, binary = pathlib.Path(tmp) / "fixture.c", pathlib.Path(tmp) / "fixture"
    source_file.write_text(code)
    subprocess.run(["cc", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(source_file), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    if args.baseline:
        if result.returncode == 0 or "heap-use-after-free" not in result.stderr:
            raise SystemExit("Expected baseline heap-use-after-free was not reproduced")
        print("Baseline dangling-child use-after-free reproduced by ASan")
    else:
        print(result.stdout, end="")
        if result.returncode:
            print(result.stderr)
            raise SystemExit(result.returncode)
