#include <fs/sysfs.h>
#include <fs/vfs.h>
#include <ipc/netlink.h>
#include <kernel/errno.h>
#include <kernel/kobject.h>
#include <libs/glist/circular_list.h>
#include <libs/std/stdarg.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

extern int             printf(const char *format, ...);
extern struct kobject *sysfs_root_kobj;

static int              tests_run;
static int              tests_failed;
static int              recursive_lock_detected;
static vfs_callback_t   registered_sysfs;
static struct kobj_type test_ktype;
static vfs_node_t       sysfs_mountpoint;
static int              netlink_broadcasts;
static char             last_uevent[256];

#define EXPECT_TRUE(condition)                                                             \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            printf("    assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 0;                                                                      \
        }                                                                                  \
    } while (0)

#define EXPECT_EQ(actual, expected)                                                                                                         \
    do {                                                                                                                                    \
        long long actual_value   = (long long)(actual);                                                                                     \
        long long expected_value = (long long)(expected);                                                                                   \
        if (actual_value != expected_value) {                                                                                               \
            printf("    assertion failed at %s:%d: %s == %s (got %lld, want %lld)\n", __FILE__, __LINE__, #actual, #expected, actual_value, \
                   expected_value);                                                                                                         \
            return 0;                                                                                                                       \
        }                                                                                                                                   \
    } while (0)

static void run_test(const char *name, int (*test)(void))
{
    tests_run++;
    if (test()) {
        printf("PASS %s\n", name);
    } else {
        tests_failed++;
        printf("FAIL %s\n", name);
    }
}

int streq(const char *left, const char *right)
{
    return left && right && strcmp(left, right) == 0;
}

void printk(const char *format, ...)
{
    (void)format;
}

void plogk(const char *format, ...)
{
    (void)format;
}

int netlink_broadcast(uint32_t protocol, uint32_t group, const void *data, uint32_t len, int flags)
{
    (void)flags;
    if (protocol != NETLINK_KOBJECT_UEVENT || group != 1 || !data || len < NLMSG_HDRLEN) return -EINVAL;
    const char *payload = (const char *)data + NLMSG_HDRLEN;
    size_t      length  = strlen(payload);
    if (length >= sizeof(last_uevent)) length = sizeof(last_uevent) - 1;
    memcpy(last_uevent, payload, length);
    last_uevent[length] = '\0';
    netlink_broadcasts++;
    return EOK;
}

void spin_lock(spinlock_t *lock)
{
    if (lock->lock) recursive_lock_detected = 1;
    lock->lock = 1;
}

void spin_unlock(spinlock_t *lock)
{
    lock->lock = 0;
}

struct vfs_callback vfs_empty_callback;
vfs_callback_t      fs_callbacks[4];
vfs_node_t          rootdir;
uint64_t            KERNEL_HEAP_START;
uint64_t            KERNEL_HEAP_SIZE;

int vfs_regist_fs(const char *name, vfs_callback_t callback)
{
    EXPECT_TRUE(streq(name, "sysfs"));
    registered_sysfs = callback;
    fs_callbacks[1]  = callback;
    return 1;
}

vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name)
{
    vfs_node_t node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    node->parent      = parent;
    node->name        = name ? strdup(name) : NULL;
    node->type        = file_none;
    node->fsid        = parent ? parent->fsid : 1;
    node->root        = parent ? parent->root : node;
    node->permissions = 0777;
    if (parent) parent->child = clist_prepend(parent->child, node);
    return node;
}

void vfs_free(vfs_node_t node)
{
    if (!node) return;
    while (node->child) {
        vfs_node_t child = node->child->data;
        node->child      = clist_delete(node->child, child);
        vfs_free(child);
    }
    if (node->handle && registered_sysfs && registered_sysfs->free) registered_sysfs->free(node->handle);
    free(node->name);
    free(node->linkname);
    free(node);
}

void vfs_namespace_detach(vfs_node_t node)
{
    if (!node) return;
    if (node->parent) node->parent->child = clist_delete(node->parent->child, node);
    node->parent = NULL;
    if (!node->refcount) vfs_free(node);
}

static vfs_node_t find_vnode(vfs_node_t parent, const char *name)
{
    for (clist_t node = parent ? parent->child : NULL; node; node = node->next) {
        vfs_node_t child = node->data;
        if (child && streq(child->name, name)) return child;
    }
    return NULL;
}

static struct kobject *find_kobject(struct kobject *parent, const char *name)
{
    for (clist_t node = parent ? parent->children : NULL; node; node = node->next) {
        struct kobject *child = node->data;
        if (child && streq(kobject_name(child), name)) return child;
    }
    return NULL;
}

static int test_create_and_add_returns_creator_reference_only(void)
{
    struct kobject *object = kobject_create_and_add("reference", sysfs_root_kobj);
    EXPECT_TRUE(object != NULL);
    EXPECT_EQ(kref_read(&object->kref), 1);
    kobject_put(object);
    return 1;
}

static int test_registered_child_pins_parent(void)
{
    struct kobject *parent = kobject_create_and_add("parent-pin", sysfs_root_kobj);
    EXPECT_TRUE(parent != NULL);
    uint32_t        before = kref_read(&parent->kref);
    struct kobject *child  = kobject_create_and_add("child-pin", parent);
    EXPECT_TRUE(child != NULL);
    EXPECT_EQ(kref_read(&parent->kref), before + 1);
    kobject_put(child);
    EXPECT_EQ(kref_read(&parent->kref), before);
    kobject_put(parent);
    return 1;
}

static int test_duplicate_sibling_names_are_rejected(void)
{
    struct kobject *first = kobject_create_and_add("duplicate-name", sysfs_root_kobj);
    EXPECT_TRUE(first != NULL);

    struct kobject *second = calloc(1, sizeof(*second));
    EXPECT_TRUE(second != NULL);
    kobject_init(second, &test_ktype);
    EXPECT_EQ(kobject_add(second, sysfs_root_kobj, "duplicate-name"), -EEXIST);
    EXPECT_EQ(second->state_in_sysfs, 0);
    kobject_put(second);
    kobject_put(first);
    return 1;
}

static int test_kset_unregister_does_not_unregister_members(void)
{
    struct kset *set = kset_create_and_add("member-set", NULL, sysfs_root_kobj);
    EXPECT_TRUE(set != NULL);
    struct kobject *member = calloc(1, sizeof(*member));
    EXPECT_TRUE(member != NULL);
    kobject_init(member, &test_ktype);
    member->kset = set;
    EXPECT_EQ(kobject_add(member, NULL, "member"), EOK);

    recursive_lock_detected = 0;
    kset_unregister(set);

    EXPECT_EQ(recursive_lock_detected, 0);
    EXPECT_EQ(member->state_in_sysfs, 1);
    EXPECT_TRUE(member->ktype == &test_ktype);
    kobject_put(member);
    return 1;
}

static int uevent_filter_calls;
static int uevent_hook_calls;
static int uevent_environment_valid;

static int test_uevent_filter(struct kset *kset, struct kobject *kobj)
{
    (void)kset;
    (void)kobj;
    uevent_filter_calls++;
    return 1;
}

static const char *test_uevent_name(struct kset *kset, struct kobject *kobj)
{
    (void)kset;
    (void)kobj;
    return "test-subsystem";
}

static int test_uevent_hook(struct kset *kset, struct kobject *kobj, char *envp[], int nenv)
{
    (void)kset;
    (void)kobj;
    int action_ok    = 0;
    int path_ok      = 0;
    int subsystem_ok = 0;
    for (int i = 0; i < nenv; i++) {
        if (streq(envp[i], "ACTION=add")) action_ok = 1;
        if (streq(envp[i], "DEVPATH=/uevent-set/member")) path_ok = 1;
        if (streq(envp[i], "SUBSYSTEM=test-subsystem")) subsystem_ok = 1;
    }
    uevent_environment_valid = action_ok && path_ok && subsystem_ok;
    uevent_hook_calls++;
    return EOK;
}

static int test_uevent_uses_linux_payload_and_kset_hooks(void)
{
    static const struct kset_uevent_ops ops = {
        .filter = test_uevent_filter,
        .name   = test_uevent_name,
        .uevent = test_uevent_hook,
    };
    struct kset *set = kset_create_and_add("uevent-set", &ops, sysfs_root_kobj);
    EXPECT_TRUE(set != NULL);
    struct kobject *member = calloc(1, sizeof(*member));
    EXPECT_TRUE(member != NULL);
    kobject_init(member, &test_ktype);
    member->kset = set;
    EXPECT_EQ(kobject_add(member, NULL, "member"), EOK);

    uevent_filter_calls      = 0;
    uevent_hook_calls        = 0;
    uevent_environment_valid = 0;
    netlink_broadcasts       = 0;
    last_uevent[0]           = '\0';
    EXPECT_EQ(kobject_uevent(member, KOBJ_ADD), EOK);
    EXPECT_EQ(uevent_filter_calls, 1);
    EXPECT_EQ(uevent_hook_calls, 1);
    EXPECT_EQ(uevent_environment_valid, 1);
    EXPECT_EQ(netlink_broadcasts, 1);
    EXPECT_TRUE(streq(last_uevent, "add@/uevent-set/member"));

    kobject_put(member);
    kset_unregister(set);
    return 1;
}

static ssize_t binary_read(struct kobject *kobj, struct bin_attribute *attr, char *buffer, int64_t pos, size_t count)
{
    (void)kobj;
    (void)attr;
    static const char value[] = "binary";
    size_t            length  = sizeof(value) - 1;
    if ((size_t)pos >= length) return 0;
    if (count > length - (size_t)pos) count = length - (size_t)pos;
    memcpy(buffer, value + pos, count);
    return (ssize_t)count;
}

static struct bin_attribute deferred_binary = {
    .attr = {.name = "blob", .mode = 0444},
    .size = 6,
    .read = binary_read,
};

static int test_binary_attribute_survives_deferred_mount(void)
{
    struct kobject *object = kobject_create_and_add("deferred-bin", sysfs_root_kobj);
    EXPECT_TRUE(object != NULL);
    EXPECT_EQ(sysfs_create_bin_file(object, &deferred_binary), EOK);

    sysfs_mountpoint = vfs_node_alloc(NULL, "sys");
    EXPECT_TRUE(sysfs_mountpoint != NULL);
    sysfs_mountpoint->type = file_dir;
    EXPECT_EQ(registered_sysfs->mount(NULL, sysfs_mountpoint), EOK);
    EXPECT_TRUE(object->sd != NULL);
    EXPECT_EQ(registered_sysfs->stat(object->sd->handle, object->sd), EOK);

    vfs_node_t file = find_vnode(object->sd, "blob");
    EXPECT_TRUE(file != NULL);
    void *open_file = NULL;
    EXPECT_EQ(registered_sysfs->file_open(file, 0, &open_file), EOK);
    char buffer[8] = {0};
    EXPECT_EQ(registered_sysfs->file_read(file, open_file, 0, buffer, 0, sizeof(buffer)), 6);
    EXPECT_TRUE(streq(buffer, "binary"));
    registered_sysfs->file_release(file, open_file);
    kobject_put(object);
    return 1;
}

static int test_kset_unregister_unbinds_member_namespace(void)
{
    struct kset *set = kset_create_and_add("mounted-member-set", NULL, sysfs_root_kobj);
    EXPECT_TRUE(set != NULL);
    struct kobject *member = calloc(1, sizeof(*member));
    EXPECT_TRUE(member != NULL);
    kobject_init(member, &test_ktype);
    member->kset = set;
    EXPECT_EQ(kobject_add(member, NULL, "mounted-member"), EOK);
    EXPECT_TRUE(member->sd != NULL);

    kset_unregister(set);
    EXPECT_EQ(member->state_in_sysfs, 1);
    EXPECT_TRUE(member->sd == NULL);

    kobject_put(member);
    return 1;
}

static int test_group_failure_preserves_preexisting_files(void)
{
    static struct attribute existing  = {.name = "existing", .mode = 0444};
    static struct attribute duplicate = {.name = "existing", .mode = 0444};
    static struct attribute added     = {.name = "added-before-failure", .mode = 0444};
    struct attribute       *attrs[]   = {&added, &duplicate, NULL};
    struct attribute_group  group     = {.attrs = attrs};

    struct kobject *owner = calloc(1, sizeof(*owner));
    EXPECT_TRUE(owner != NULL);
    kobject_init(owner, &test_ktype);
    EXPECT_EQ(kobject_add(owner, sysfs_root_kobj, "group-rollback"), EOK);
    EXPECT_EQ(sysfs_create_file(owner, &existing), EOK);

    EXPECT_EQ(sysfs_create_group(owner, &group), -EEXIST);
    EXPECT_TRUE(find_vnode(owner->sd, "existing") != NULL);
    EXPECT_TRUE(find_vnode(owner->sd, "added-before-failure") == NULL);

    sysfs_remove_file(owner, &existing);
    kobject_put(owner);
    return 1;
}

static int             show_generation;
static struct kobject *expected_show_owner;
static int             show_owner_ok;

static ssize_t generation_show(struct kobject *kobj, struct attribute *attr, char *buffer)
{
    (void)kobj;
    (void)attr;
    if (expected_show_owner) show_owner_ok = kobj == expected_show_owner;
    show_generation++;
    return snprintf(buffer, SYSFS_PAGE_SIZE, "%d\n", show_generation);
}

static ssize_t failing_store(struct kobject *kobj, struct attribute *attr, const char *buffer, size_t count)
{
    (void)kobj;
    (void)attr;
    (void)buffer;
    (void)count;
    return -EINVAL;
}

static const struct sysfs_ops test_sysfs_ops = {
    .show  = generation_show,
    .store = failing_store,
};

static void test_kobject_release(struct kobject *kobj)
{
    free(kobj);
}

static struct kobj_type test_ktype = {
    .release   = test_kobject_release,
    .sysfs_ops = &test_sysfs_ops,
};

static struct attribute generation_attr = {
    .name = "generation",
    .mode = 0644,
};

static int test_text_attribute_uses_seekable_per_open_snapshot(void)
{
    struct kobject *object = calloc(1, sizeof(*object));
    EXPECT_TRUE(object != NULL);
    kobject_init(object, &test_ktype);
    EXPECT_EQ(kobject_add(object, sysfs_root_kobj, "io-object"), EOK);
    EXPECT_EQ(sysfs_create_file(object, &generation_attr), EOK);

    EXPECT_TRUE(sysfs_mountpoint != NULL);
    EXPECT_TRUE(object->sd != NULL);
    EXPECT_EQ(registered_sysfs->stat(object->sd->handle, object->sd), EOK);

    vfs_node_t file = find_vnode(object->sd, "generation");
    EXPECT_TRUE(file != NULL);
    EXPECT_TRUE((file->type & file_stream) == 0);
    EXPECT_TRUE(registered_sysfs->file_open != NULL);
    EXPECT_TRUE(registered_sysfs->file_read != NULL);
    EXPECT_TRUE(registered_sysfs->file_write != NULL);

    void *first_open = NULL;
    EXPECT_EQ(registered_sysfs->file_open(file, 0, &first_open), EOK);
    char first[16] = {0};
    EXPECT_EQ(registered_sysfs->file_read(file, first_open, 0, first, 0, sizeof(first)), 2);
    EXPECT_EQ(registered_sysfs->file_read(file, first_open, 0, first, 2, sizeof(first)), 0);
    registered_sysfs->file_release(file, first_open);

    void *second_open = NULL;
    EXPECT_EQ(registered_sysfs->file_open(file, 0, &second_open), EOK);
    char second[16] = {0};
    EXPECT_EQ(registered_sysfs->file_read(file, second_open, 0, second, 0, sizeof(second)), 2);
    EXPECT_TRUE(strcmp(first, second) != 0);
    EXPECT_EQ(registered_sysfs->file_write(file, second_open, 0, "bad", 0, 3), -EINVAL);
    registered_sysfs->file_release(file, second_open);
    kobject_put(object);
    return 1;
}

static int test_unmount_clears_backpointers_and_remounts(void)
{
    EXPECT_TRUE(sysfs_mountpoint != NULL);
    struct kobject *bus = find_kobject(sysfs_root_kobj, "bus");
    EXPECT_TRUE(bus != NULL && bus->sd != NULL);

    while (sysfs_mountpoint->child) {
        vfs_node_t child        = sysfs_mountpoint->child->data;
        sysfs_mountpoint->child = clist_delete(sysfs_mountpoint->child, child);
        child->parent           = NULL;
        vfs_free(child);
    }
    void *root_handle = sysfs_mountpoint->handle;
    registered_sysfs->unmount(root_handle);
    sysfs_mountpoint->handle = NULL;
    vfs_free(sysfs_mountpoint);
    sysfs_mountpoint = NULL;

    EXPECT_TRUE(sysfs_root_kobj->sd == NULL);
    EXPECT_TRUE(bus->sd == NULL);

    sysfs_mountpoint = vfs_node_alloc(NULL, "sys");
    EXPECT_TRUE(sysfs_mountpoint != NULL);
    sysfs_mountpoint->type = file_dir;
    EXPECT_EQ(registered_sysfs->mount(NULL, sysfs_mountpoint), EOK);
    EXPECT_TRUE(sysfs_root_kobj->sd == sysfs_mountpoint);
    EXPECT_TRUE(bus->sd != NULL);
    return 1;
}

static int visible_read_only(struct kobject *kobj, struct attribute *attr, int index)
{
    (void)kobj;
    (void)attr;
    (void)index;
    return 0400;
}

static int test_named_group_uses_owner_and_visibility_mode(void)
{
    struct kobject *owner = calloc(1, sizeof(*owner));
    EXPECT_TRUE(owner != NULL);
    kobject_init(owner, &test_ktype);
    EXPECT_EQ(kobject_add(owner, sysfs_root_kobj, "group-owner"), EOK);

    struct attribute      *attrs[] = {&generation_attr, NULL};
    struct attribute_group group   = {
          .name       = "settings",
          .is_visible = visible_read_only,
          .attrs      = attrs,
    };
    EXPECT_EQ(sysfs_create_group(owner, &group), EOK);

    struct kobject *directory = find_kobject(owner, "settings");
    EXPECT_TRUE(directory != NULL && directory->sd != NULL);
    vfs_node_t file = find_vnode(directory->sd, "generation");
    EXPECT_TRUE(file != NULL);
    EXPECT_EQ(file->permissions, 0400);

    expected_show_owner = owner;
    show_owner_ok       = 0;
    void *open_file     = NULL;
    EXPECT_EQ(registered_sysfs->file_open(file, 0, &open_file), EOK);
    char buffer[16] = {0};
    EXPECT_TRUE(registered_sysfs->file_read(file, open_file, 0, buffer, 0, sizeof(buffer)) > 0);
    EXPECT_EQ(show_owner_ok, 1);
    registered_sysfs->file_release(file, open_file);
    expected_show_owner = NULL;

    sysfs_remove_group(owner, &group);
    kobject_put(owner);
    return 1;
}

static int test_open_attribute_survives_namespace_removal(void)
{
    struct kobject *owner = calloc(1, sizeof(*owner));
    EXPECT_TRUE(owner != NULL);
    kobject_init(owner, &test_ktype);
    EXPECT_EQ(kobject_add(owner, sysfs_root_kobj, "open-removal"), EOK);
    EXPECT_EQ(sysfs_create_file(owner, &generation_attr), EOK);
    vfs_node_t file = find_vnode(owner->sd, "generation");
    EXPECT_TRUE(file != NULL);

    file->refcount  = 1;
    void *open_file = NULL;
    EXPECT_EQ(registered_sysfs->file_open(file, 0, &open_file), EOK);
    uint32_t references = kref_read(&owner->kref);
    sysfs_remove_file(owner, &generation_attr);
    EXPECT_TRUE(find_vnode(owner->sd, "generation") == NULL);
    EXPECT_EQ(kref_read(&owner->kref), references);

    char buffer[16] = {0};
    EXPECT_TRUE(registered_sysfs->file_read(file, open_file, 0, buffer, 0, sizeof(buffer)) > 0);
    registered_sysfs->file_release(file, open_file);
    file->refcount = 0;
    vfs_free(file);
    kobject_put(owner);
    return 1;
}

static int test_rename_and_move_update_namespace_transactionally(void)
{
    struct kobject *left      = kobject_create_and_add("move-left", sysfs_root_kobj);
    struct kobject *right     = kobject_create_and_add("move-right", sysfs_root_kobj);
    struct kobject *object    = kobject_create_and_add("moving", left);
    struct kobject *collision = kobject_create_and_add("taken", left);
    EXPECT_TRUE(left && right && object && collision);
    EXPECT_EQ(kobject_rename(object, "taken"), -EEXIST);
    EXPECT_TRUE(streq(kobject_name(object), "moving"));
    EXPECT_TRUE(streq(object->sd->name, "moving"));

    EXPECT_EQ(kobject_rename(object, "renamed"), EOK);
    EXPECT_TRUE(streq(kobject_name(object), "renamed"));
    EXPECT_TRUE(find_vnode(left->sd, "renamed") == object->sd);
    EXPECT_EQ(kobject_move(object, right), EOK);
    EXPECT_TRUE(object->parent == right);
    EXPECT_TRUE(find_vnode(left->sd, "renamed") == NULL);
    EXPECT_TRUE(find_vnode(right->sd, "renamed") == object->sd);

    kobject_put(collision);
    kobject_put(object);
    kobject_put(left);
    kobject_put(right);
    return 1;
}

static int test_symlink_is_relative_and_pins_target(void)
{
    struct kobject *owner  = kobject_create_and_add("link-owner", sysfs_root_kobj);
    struct kobject *target = kobject_create_and_add("link-target", sysfs_root_kobj);
    EXPECT_TRUE(owner && target);
    uint32_t references = kref_read(&target->kref);
    EXPECT_EQ(sysfs_create_symlink(owner, target, "device"), EOK);
    EXPECT_EQ(kref_read(&target->kref), references + 2);

    vfs_node_t link = find_vnode(owner->sd, "device");
    EXPECT_TRUE(link != NULL);
    char path[64] = {0};
    EXPECT_EQ(registered_sysfs->readlink(link, path, 0, sizeof(path) - 1), 14);
    EXPECT_TRUE(streq(path, "../link-target"));

    EXPECT_EQ(kobject_rename(target, "link-target-renamed"), EOK);
    memset(path, 0, sizeof(path));
    EXPECT_EQ(registered_sysfs->readlink(link, path, 0, sizeof(path) - 1), 22);
    EXPECT_TRUE(streq(path, "../link-target-renamed"));

    sysfs_remove_symlink(owner, "device");
    EXPECT_EQ(kref_read(&target->kref), references);
    kobject_put(owner);
    kobject_put(target);
    return 1;
}

int main(void)
{
    sysfs_regist();
    if (!registered_sysfs || sysfs_init() != EOK || !sysfs_root_kobj) {
        printf("test setup failed\n");
        return 2;
    }

    run_test("kobject_create_and_add returns one creator reference", test_create_and_add_returns_creator_reference_only);
    run_test("registered child pins parent", test_registered_child_pins_parent);
    run_test("duplicate sibling names are rejected", test_duplicate_sibling_names_are_rejected);
    run_test("kset unregister leaves members registered", test_kset_unregister_does_not_unregister_members);
    run_test("uevent uses Linux payload and kset hooks", test_uevent_uses_linux_payload_and_kset_hooks);
    run_test("binary attribute survives deferred mount", test_binary_attribute_survives_deferred_mount);
    run_test("kset unregister unbinds member namespace", test_kset_unregister_unbinds_member_namespace);
    run_test("group rollback preserves preexisting files", test_group_failure_preserves_preexisting_files);
    run_test("text attribute uses seekable per-open snapshots", test_text_attribute_uses_seekable_per_open_snapshot);
    run_test("named group uses owner and visibility mode", test_named_group_uses_owner_and_visibility_mode);
    run_test("open attribute survives namespace removal", test_open_attribute_survives_namespace_removal);
    run_test("rename and move update namespace transactionally", test_rename_and_move_update_namespace_transactionally);
    run_test("symlink is relative and pins target", test_symlink_is_relative_and_pins_target);
    run_test("unmount clears vnode backpointers and remounts", test_unmount_clears_backpointers_and_remounts);

    printf("%d tests, %d failures\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
