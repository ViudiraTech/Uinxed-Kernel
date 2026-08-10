/*
 *
 *      console.c
 *      Console driver table and `console=` command-line handling
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/tty/console.h>
#include <kernel/cmdline/cmdline.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

console_cmdline_t console_cmdline[NR_CONSOLES];
int               console_cmdline_count;

static console_t *console_list;
static console_t *console_active;
static spinlock_t console_lock;
static bool       console_cmdline_parsed;

static void console_cmdline_add(const char *arg)
{
    console_cmdline_t *cc;
    const char        *p;
    const char        *end;
    size_t             name_len;

    if (console_cmdline_count >= NR_CONSOLES) return;
    if (!arg || !*arg) return;
    cc = &console_cmdline[console_cmdline_count];

    /* "tty0", "ttyS2", "ttyD0,115200n8" -> name "tty"/"ttyS"/"ttyD", index, options */
    p = arg;
    while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) p++;
    name_len = (size_t)(p - arg);
    if (name_len >= sizeof(cc->name)) name_len = sizeof(cc->name) - 1;
    memcpy(cc->name, arg, name_len);
    cc->name[name_len] = '\0';

    cc->index      = 0;
    cc->options[0] = '\0';
    if (*p >= '0' && *p <= '9') {
        cc->index = (int)strtol(p, (char **)&end, 10);
        p         = end;
    }
    if (*p == ',') strncpy(cc->options, p + 1, sizeof(cc->options) - 1);
    console_cmdline_count++;
}

void console_cmdline_parse(void)
{
    const char *cmdline;
    char       *copy;
    char       *token;

    if (console_cmdline_parsed) return;
    console_cmdline_parsed = true;

    cmdline = get_cmdline();
    if (!cmdline) return;
    copy = strdup(cmdline);
    if (!copy) return;

    token = strtok(copy, " ");
    while (token) {
        if (!strncmp(token, "console=", 8)) console_cmdline_add(token + 8);
        token = strtok(NULL, " ");
    }
    free(copy);
}

static bool console_matches_cmdline(const console_t *c)
{
    for (int i = 0; i < console_cmdline_count; i++) {
        if (streq(console_cmdline[i].name, c->name) && console_cmdline[i].index == (int)c->index) return true;
    }
    return false;
}

int register_console(console_t *c)
{
    console_t *cur;
    bool       enabled = false;

    if (!c || !c->name) return -EINVAL;

    console_cmdline_parse();

    spin_lock(&console_lock);
    for (cur = console_list; cur; cur = cur->next) {
        if (streq(cur->name, c->name) && cur->index == c->index) {
            spin_unlock(&console_lock);
            return -EEXIST;
        }
    }

    if (console_matches_cmdline(c)) {
        enabled = true;
    } else if (console_cmdline_count == 0 && streq(c->name, "tty") && c->index == 0) {
        /* No console= given: the vt console is the default boot console. */
        enabled = true;
    }
    if (enabled) c->flags |= CON_ENABLED;

    /*
     * CON_CONSDEV goes to the console matching the last `console=` entry,
     * or to the default vt console when nothing was specified.
     */
    if (console_matches_cmdline(c) && console_cmdline_count > 0) {
        int last = console_cmdline_count - 1;
        if (streq(console_cmdline[last].name, c->name) && console_cmdline[last].index == (int)c->index) {
            c->flags |= CON_CONSDEV;
            console_active = c;
        }
    } else if (console_cmdline_count == 0 && enabled) {
        c->flags |= CON_CONSDEV;
        console_active = c;
    }

    c->next      = console_list;
    console_list = c;
    spin_unlock(&console_lock);

    plogk("console: Registered \"%s%u\" %s.\n", c->name, c->index, enabled ? "(enabled)" : "");
    return 0;
}

void unregister_console(console_t *c)
{
    console_t **link;

    if (!c) return;
    spin_lock(&console_lock);
    link = &console_list;
    while (*link && *link != c) link = &(*link)->next;
    if (*link) *link = c->next;
    if (console_active == c) console_active = NULL;
    spin_unlock(&console_lock);
}

void console_write_all(const uint8_t *buf, size_t len)
{
    console_t *c;

    /*
     * Consoles are static objects registered once at boot; iterate without
     * the registry lock so a console driver may safely call back into the
     * console layer from its write path.
     */
    if (!buf || !len) return;
    for (c = console_list; c; c = c->next) {
        if ((c->flags & CON_ENABLED) && c->write) c->write(c, buf, len);
    }
}

console_t *console_get_active(void)
{
    console_t *c = NULL;

    spin_lock(&console_lock);
    if (console_active) {
        c = console_active;
    } else {
        for (c = console_list; c; c = c->next)
            if (c->flags & CON_ENABLED) break;
    }
    spin_unlock(&console_lock);
    return c;
}

tty_core_t *console_get_tty(void)
{
    console_t *c = console_get_active();
    return c && c->get_tty ? c->get_tty(c) : NULL;
}

tty_device_t console_derive_boot_tty(void)
{
    tty_device_t dev = {.type = TTY_DEVICE_VGA, .port = 0};
    console_t   *c   = console_get_active();

    if (c) {
        if (streq(c->name, "ttyS"))
            dev.type = TTY_DEVICE_SERIAL;
        else if (streq(c->name, "ttyD"))
            dev.type = TTY_DEVICE_DRM;
        else
            dev.type = TTY_DEVICE_VGA;
        dev.port = c->index;
    }
    return dev;
}
