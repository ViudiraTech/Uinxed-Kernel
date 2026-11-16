/*
 *
 *      control.c
 *      ULSA control interface (/dev/snd/controlC<n>)
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <kernel/debug/vsprintf.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <process/process.h>
#include <process/uaccess.h>
#include <syscall/fcntl.h>
#include <syscall/poll.h>

#if CONFIG_AUDIO

/*
 * The control device is how alsa-lib finds cards (CARD_INFO), enumerates
 * PCMs (PCM_NEXT_DEVICE / PCM_INFO) and drives mixers (ELEM_*).  Elements
 * come from drivers (ulsa_ctl_add) or from user space (ELEM_ADD, used for
 * example by alsa-lib's softvol plugin), and changes are reported to
 * subscribed files as snd_ctl_event records.
 */

#    define CTL_EVENT_QUEUE    64
#    define MAX_CONTROL_COUNT  1028
#    define MAX_USER_CTL_BYTES (8UL * 1024UL * 1024UL)
#    define IEC958_VALUE_BYTES 176

struct ulsa_ctl_event {
        unsigned int           mask;
        struct snd_ctl_elem_id id;
};

struct ulsa_ctl_file {
        ulsa_ctl_file_t      *next;
        ulsa_card_t          *card;
        bool                  subscribed;
        spinlock_t            lock;
        struct ulsa_ctl_event events[CTL_EVENT_QUEUE];
        unsigned int          head;
        unsigned int          count;
        wait_queue_t          wait;
        vfs_poll_source_t     poll_source;
        int                   preferred_subdevice[2];
        int                   pid;
};

/* A control created through ELEM_ADD. */
struct user_element {
        struct snd_ctl_elem_info info;
        size_t                   elem_data_size;
        uint8_t                 *elem_data;
        unsigned int            *tlv_data;
        size_t                   tlv_data_size;
        char                    *enum_names;
};

/* Length of s, looking at no more than max bytes. */
static size_t ctl_strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

/* ------------------------------------------------------ element ids */

/* Index of id inside a multi-element control. */
static unsigned int ctl_get_ioff(const ulsa_kcontrol_t *kctl, const struct snd_ctl_elem_id *id)
{
    if (id->numid) return id->numid - kctl->id.numid;
    return id->index - kctl->id.index;
}

/* The id of element `ioff` of a control. */
static void ctl_build_ioff(struct snd_ctl_elem_id *id, const ulsa_kcontrol_t *kctl, unsigned int ioff)
{
    *id = kctl->id;
    id->index += ioff;
    id->numid += ioff;
}

/* True when id (by name) falls inside kctl. */
static bool elem_id_matches(const ulsa_kcontrol_t *kctl, const struct snd_ctl_elem_id *id)
{
    return kctl->id.iface == id->iface && kctl->id.device == id->device && kctl->id.subdevice == id->subdevice && !strncmp((const char *)kctl->id.name, (const char *)id->name, sizeof(kctl->id.name))
           && kctl->id.index <= id->index && kctl->id.index + kctl->count > id->index;
}

/* Find the control holding an element id; controls_lock held. */
static ulsa_kcontrol_t *ctl_find_id(ulsa_card_t *card, const struct snd_ctl_elem_id *id)
{
    for (ulsa_kcontrol_t *k = card->controls; k; k = k->next) {
        if (id->numid) {
            if (k->id.numid <= id->numid && k->id.numid + k->count > id->numid) return k;
        } else if (elem_id_matches(k, id)) {
            return k;
        }
    }
    return NULL;
}

/* Find a control by name. */
ulsa_kcontrol_t *ulsa_ctl_find(ulsa_card_t *card, snd_ctl_elem_iface_t iface, const char *name, unsigned int index)
{
    struct snd_ctl_elem_id id;
    ulsa_kcontrol_t       *k;

    memset(&id, 0, sizeof(id));
    id.iface = iface;
    id.index = index;
    strncpy((char *)id.name, name, sizeof(id.name) - 1);
    mutex_lock(&card->controls_lock);
    k = ctl_find_id(card, &id);
    mutex_unlock(&card->controls_lock);
    return k;
}

/* ------------------------------------------------------------ events */

/* Queue an event for every subscriber, merging repeated events of one element. */
void ulsa_ctl_notify(ulsa_card_t *card, unsigned int mask, const struct snd_ctl_elem_id *id)
{
    uint64_t flags;

    if (!card || !id) return;
    flags = spin_lock_irqsave(&card->files_lock);
    for (ulsa_ctl_file_t *f = card->ctl_files; f; f = f->next) {
        uint64_t fflags;
        bool     merged = false;

        if (!f->subscribed) continue;
        fflags = spin_lock_irqsave(&f->lock);
        for (unsigned int i = 0; i < f->count; i++) {
            struct ulsa_ctl_event *ev = &f->events[(f->head + i) % CTL_EVENT_QUEUE];
            if (ev->id.numid == id->numid) {
                ev->mask |= mask;
                merged = true;
                break;
            }
        }
        if (!merged && f->count < CTL_EVENT_QUEUE) {
            struct ulsa_ctl_event *ev = &f->events[(f->head + f->count) % CTL_EVENT_QUEUE];
            ev->mask                  = mask;
            ev->id                    = *id;
            f->count++;
        }
        spin_unlock_irqrestore(&f->lock, fflags);
        wait_queue_wake_all(&f->wait);
        vfs_poll_source_notify(&f->poll_source, POLLIN | POLLRDNORM);
    }
    spin_unlock_irqrestore(&card->files_lock, flags);
}

/* Notify one element of a control. */
static void ctl_notify_one(ulsa_card_t *card, unsigned int mask, const ulsa_kcontrol_t *kctl, unsigned int ioff)
{
    struct snd_ctl_elem_id id;

    ctl_build_ioff(&id, kctl, ioff);
    ulsa_ctl_notify(card, mask, &id);
}

/* -------------------------------------------------- control objects */

/* Allocate a control with `count` elements. */
static ulsa_kcontrol_t *ctl_alloc(unsigned int count, unsigned int access)
{
    ulsa_kcontrol_t *k;

    if (!count || count > MAX_CONTROL_COUNT) return NULL;
    k = calloc(1, sizeof(*k) + count * sizeof(ulsa_kcontrol_volatile_t));
    if (!k) return NULL;
    k->count = count;
    for (unsigned int i = 0; i < count; i++) k->vd[i].access = access;
    return k;
}

/* Build a control from a template. */
ulsa_kcontrol_t *ulsa_ctl_new1(const ulsa_kcontrol_new_t *knew, void *private_data)
{
    unsigned int     access = knew->access ? knew->access : SNDRV_CTL_ELEM_ACCESS_READWRITE;
    ulsa_kcontrol_t *k;

    access &= SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_VOLATILE | SNDRV_CTL_ELEM_ACCESS_INACTIVE | SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_COMMAND
              | SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK;
    if (knew->tlv) access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ;
    k = ctl_alloc(knew->count ? knew->count : 1, access);
    if (!k) return NULL;
    k->id.iface     = knew->iface;
    k->id.device    = knew->device;
    k->id.subdevice = knew->subdevice;
    k->id.index     = knew->index;
    if (knew->name) strncpy((char *)k->id.name, knew->name, sizeof(k->id.name) - 1);
    k->info          = knew->info;
    k->get           = knew->get;
    k->put           = knew->put;
    k->tlv           = knew->tlv;
    k->private_value = knew->private_value;
    k->private_data  = private_data;
    return k;
}

/* Free a control. */
static void ctl_free_one(ulsa_kcontrol_t *k)
{
    if (!k) return;
    if (k->private_free) k->private_free(k);
    free(k);
}

/* Link a control into the card; controls_lock held. */
static int ctl_add_locked(ulsa_card_t *card, ulsa_kcontrol_t *k)
{
    struct snd_ctl_elem_id id = k->id;

    id.numid = 0;
    for (unsigned int i = 0; i < k->count; i++) {
        id.index = k->id.index + i;
        if (ctl_find_id(card, &id)) return -EBUSY;
    }
    if (card->controls_count + k->count > 0xffffU * 16) return -ENOMEM;

    k->id.numid = card->last_numid + 1;
    card->last_numid += k->count;
    card->controls_count += k->count;

    ulsa_kcontrol_t **link = &card->controls;
    while (*link) link = &(*link)->next;
    k->next = NULL;
    *link   = k;
    return EOK;
}

/* Attach a control to a card. */
int ulsa_ctl_add(ulsa_card_t *card, ulsa_kcontrol_t *kcontrol)
{
    int err;

    if (!kcontrol) return -ENOMEM;
    if (!card || !kcontrol->info) {
        ctl_free_one(kcontrol);
        return -EINVAL;
    }
    mutex_lock(&card->controls_lock);
    err = ctl_add_locked(card, kcontrol);
    mutex_unlock(&card->controls_lock);
    if (err < 0) {
        ctl_free_one(kcontrol);
        return err;
    }
    for (unsigned int i = 0; i < kcontrol->count; i++) ctl_notify_one(card, SNDRV_CTL_EVENT_MASK_ADD, kcontrol, i);
    return EOK;
}

/* Unlink a control; controls_lock held. */
static void ctl_unlink_locked(ulsa_card_t *card, ulsa_kcontrol_t *k)
{
    for (ulsa_kcontrol_t **link = &card->controls; *link; link = &(*link)->next) {
        if (*link == k) {
            *link = k->next;
            card->controls_count -= k->count;
            return;
        }
    }
}

/* Remove and free a control. */
int ulsa_ctl_remove(ulsa_card_t *card, ulsa_kcontrol_t *kcontrol)
{
    if (!card || !kcontrol) return -EINVAL;
    mutex_lock(&card->controls_lock);
    ctl_unlink_locked(card, kcontrol);
    mutex_unlock(&card->controls_lock);
    for (unsigned int i = 0; i < kcontrol->count; i++) ctl_notify_one(card, SNDRV_CTL_EVENT_MASK_REMOVE, kcontrol, i);
    ctl_free_one(kcontrol);
    return EOK;
}

/* Free every control of a card. */
void ulsa_ctl_free_all(ulsa_card_t *card)
{
    mutex_lock(&card->controls_lock);
    while (card->controls) {
        ulsa_kcontrol_t *k = card->controls;
        card->controls     = k->next;
        ctl_free_one(k);
    }
    card->controls_count = 0;
    mutex_unlock(&card->controls_lock);
}

/* --------------------------------------------------- info helpers */

/* Info for a mono on/off switch. */
int ulsa_ctl_boolean_mono_info(ulsa_kcontrol_t *kcontrol, struct snd_ctl_elem_info *uinfo)
{
    (void)kcontrol;
    uinfo->type              = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
    uinfo->count             = 1;
    uinfo->value.integer.min = 0;
    uinfo->value.integer.max = 1;
    return EOK;
}

/* Info for a stereo on/off switch. */
int ulsa_ctl_boolean_stereo_info(ulsa_kcontrol_t *kcontrol, struct snd_ctl_elem_info *uinfo)
{
    (void)kcontrol;
    uinfo->type              = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
    uinfo->count             = 2;
    uinfo->value.integer.min = 0;
    uinfo->value.integer.max = 1;
    return EOK;
}

/* Info for an enumerated control. */
int ulsa_ctl_enum_info(struct snd_ctl_elem_info *info, unsigned int channels, unsigned int items, const char *const names[])
{
    info->type                   = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
    info->count                  = channels;
    info->value.enumerated.items = items;
    if (!items) return EOK;
    if (info->value.enumerated.item >= items) info->value.enumerated.item = items - 1;
    memset(info->value.enumerated.name, 0, sizeof(info->value.enumerated.name));
    strncpy(info->value.enumerated.name, names[info->value.enumerated.item], sizeof(info->value.enumerated.name) - 1);
    return EOK;
}

/* ------------------------------------------------------ user elements */

/* Bytes one value of each element type occupies. */
static size_t value_size(snd_ctl_elem_type_t type)
{
    switch (type) {
        case SNDRV_CTL_ELEM_TYPE_BOOLEAN :
        case SNDRV_CTL_ELEM_TYPE_INTEGER :
            return sizeof(long);
        case SNDRV_CTL_ELEM_TYPE_ENUMERATED :
            return sizeof(unsigned int);
        case SNDRV_CTL_ELEM_TYPE_BYTES :
            return 1;
        case SNDRV_CTL_ELEM_TYPE_IEC958 :
            return IEC958_VALUE_BYTES;
        case SNDRV_CTL_ELEM_TYPE_INTEGER64 :
            return sizeof(long long);
        default :
            return 0;
    }
}

/* Maximum value count per element of each type. */
static unsigned int value_count_max(snd_ctl_elem_type_t type)
{
    switch (type) {
        case SNDRV_CTL_ELEM_TYPE_BOOLEAN :
        case SNDRV_CTL_ELEM_TYPE_INTEGER :
        case SNDRV_CTL_ELEM_TYPE_ENUMERATED :
            return 128;
        case SNDRV_CTL_ELEM_TYPE_BYTES :
            return 512;
        case SNDRV_CTL_ELEM_TYPE_IEC958 :
            return 1;
        case SNDRV_CTL_ELEM_TYPE_INTEGER64 :
            return 64;
        default :
            return 0;
    }
}

/* User element info. */
static int user_elem_info(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_info *uinfo)
{
    struct user_element *ue   = kctl->private_data;
    unsigned int         ioff = ctl_get_ioff(kctl, &uinfo->id);
    unsigned int         item = uinfo->value.enumerated.item;

    *uinfo = ue->info;
    ctl_build_ioff(&uinfo->id, kctl, ioff);
    if (ue->info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
        const char *name = ue->enum_names;
        if (item >= uinfo->value.enumerated.items) item = uinfo->value.enumerated.items - 1;
        uinfo->value.enumerated.item = item;
        for (; item > 0; item--) name += strlen(name) + 1;
        memset(uinfo->value.enumerated.name, 0, sizeof(uinfo->value.enumerated.name));
        strncpy(uinfo->value.enumerated.name, name, sizeof(uinfo->value.enumerated.name) - 1);
    }
    return EOK;
}

/* User element get. */
static int user_elem_get(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    struct user_element *ue = kctl->private_data;

    memcpy(&ucontrol->value, ue->elem_data + ctl_get_ioff(kctl, &ucontrol->id) * ue->elem_data_size, ue->elem_data_size);
    return EOK;
}

/* Check a written value against the element's range. */
static int user_elem_check(const struct user_element *ue, const struct snd_ctl_elem_value *v)
{
    for (unsigned int i = 0; i < ue->info.count; i++) {
        switch (ue->info.type) {
            case SNDRV_CTL_ELEM_TYPE_BOOLEAN :
                if (v->value.integer.value[i] < 0 || v->value.integer.value[i] > 1) return -EINVAL;
                break;
            case SNDRV_CTL_ELEM_TYPE_INTEGER :
                if (v->value.integer.value[i] < ue->info.value.integer.min || v->value.integer.value[i] > ue->info.value.integer.max) return -EINVAL;
                break;
            case SNDRV_CTL_ELEM_TYPE_INTEGER64 :
                if (v->value.integer64.value[i] < ue->info.value.integer64.min || v->value.integer64.value[i] > ue->info.value.integer64.max) return -EINVAL;
                break;
            case SNDRV_CTL_ELEM_TYPE_ENUMERATED :
                if (v->value.enumerated.item[i] >= ue->info.value.enumerated.items) return -EINVAL;
                break;
            default :
                break;
        }
    }
    return EOK;
}

/* User element put. */
static int user_elem_put(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    struct user_element *ue  = kctl->private_data;
    uint8_t             *dst = ue->elem_data + ctl_get_ioff(kctl, &ucontrol->id) * ue->elem_data_size;
    int                  err = user_elem_check(ue, ucontrol);

    if (err < 0) return err;
    if (!memcmp(&ucontrol->value, dst, ue->elem_data_size)) return 0;
    memcpy(dst, &ucontrol->value, ue->elem_data_size);
    return 1;
}

/* User element TLV read/write. */
static int user_elem_tlv(ulsa_kcontrol_t *kctl, int op_flag, unsigned int size, unsigned int *user_tlv)
{
    struct user_element *ue = kctl->private_data;

    if (op_flag == SNDRV_CTL_TLV_OP_READ) {
        if (!ue->tlv_data) return -ENXIO;
        if (size < ue->tlv_data_size) return -ENOSPC;
        return copy_to_user(user_tlv, ue->tlv_data, ue->tlv_data_size) ? -EFAULT : EOK;
    }
    if (op_flag == SNDRV_CTL_TLV_OP_WRITE) {
        unsigned int *data;
        bool          changed;

        if (size > 1024 * 128 || size < 2 * sizeof(unsigned int)) return -EINVAL;
        data = malloc(size);
        if (!data) return -ENOMEM;
        if (copy_from_user(data, user_tlv, size)) {
            free(data);
            return -EFAULT;
        }
        if (data[1] + 2 * sizeof(unsigned int) > size) {
            free(data);
            return -EINVAL;
        }
        changed = !ue->tlv_data || ue->tlv_data_size != size || memcmp(ue->tlv_data, data, size) != 0;
        free(ue->tlv_data);
        ue->tlv_data      = data;
        ue->tlv_data_size = size;
        return changed ? 1 : 0;
    }
    return -ENXIO;
}

/* Free a user element's storage. */
static void user_elem_free(ulsa_kcontrol_t *kctl)
{
    struct user_element *ue = kctl->private_data;

    if (!ue) return;
    free(ue->tlv_data);
    free(ue->enum_names);
    free(ue->elem_data);
    free(ue);
}

/* Copy and validate the NUL-separated item names of an enumerated user element. */
static int user_elem_enum_names(struct user_element *ue)
{
    size_t len = ue->info.value.enumerated.names_length;
    char  *names;
    char  *p;

    if (!ue->info.value.enumerated.names_ptr || !len || len > 64UL * 1024UL) return -EINVAL;
    names = malloc(len);
    if (!names) return -ENOMEM;
    if (copy_from_user(names, (const void *)(uintptr_t)ue->info.value.enumerated.names_ptr, len)) {
        free(names);
        return -EFAULT;
    }
    p = names;
    for (unsigned int i = 0; i < ue->info.value.enumerated.items; i++) {
        size_t n = ctl_strnlen(p, (size_t)(names + len - p));
        if (n == 0 || n >= 64 || p + n >= names + len) {
            free(names);
            return -EINVAL;
        }
        p += n + 1;
    }
    ue->enum_names                         = names;
    ue->info.value.enumerated.names_ptr    = 0;
    ue->info.value.enumerated.names_length = 0;
    return EOK;
}

/* Remove a user element (REPLACE / REMOVE); controls_lock held. */
static int ctl_remove_user_locked(ulsa_ctl_file_t *file, const struct snd_ctl_elem_id *id, ulsa_kcontrol_t **removed)
{
    ulsa_kcontrol_t *k = ctl_find_id(file->card, id);

    if (!k) return -ENOENT;
    if (!(k->vd[0].access & SNDRV_CTL_ELEM_ACCESS_USER)) return -EINVAL;
    for (unsigned int i = 0; i < k->count; i++)
        if (k->vd[i].owner && k->vd[i].owner != file) return -EBUSY;
    ctl_unlink_locked(file->card, k);
    *removed = k;
    return EOK;
}

/* SNDRV_CTL_IOCTL_ELEM_ADD / ELEM_REPLACE */
static int ctl_elem_add(ulsa_ctl_file_t *file, struct snd_ctl_elem_info *info, bool replace)
{
    ulsa_card_t         *card = file->card;
    ulsa_kcontrol_t     *k, *old = NULL;
    struct user_element *ue;
    unsigned int         count  = info->owner ? (unsigned int)info->owner : 1;
    unsigned int         access = info->access ? info->access : SNDRV_CTL_ELEM_ACCESS_READWRITE;
    size_t               vsize, alloc;
    int                  err;

    if (!info->id.name[0] || ctl_strnlen((const char *)info->id.name, sizeof(info->id.name)) >= sizeof(info->id.name)) return -EINVAL;
    if (count > MAX_CONTROL_COUNT) return -EINVAL;
    if (info->type < SNDRV_CTL_ELEM_TYPE_BOOLEAN || info->type > SNDRV_CTL_ELEM_TYPE_LAST) return -EINVAL;
    if (info->count < 1 || info->count > value_count_max(info->type)) return -EINVAL;
    if (info->type == SNDRV_CTL_ELEM_TYPE_INTEGER && (info->value.integer.min > info->value.integer.max || info->value.integer.step < 0)) return -EINVAL;
    if (info->type == SNDRV_CTL_ELEM_TYPE_INTEGER64 && (info->value.integer64.min > info->value.integer64.max || info->value.integer64.step < 0)) return -EINVAL;
    if (info->type == SNDRV_CTL_ELEM_TYPE_ENUMERATED && (!info->value.enumerated.items || info->value.enumerated.items > 128)) return -EINVAL;

    access &= SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_INACTIVE | SNDRV_CTL_ELEM_ACCESS_TLV_WRITE;
    if (access & SNDRV_CTL_ELEM_ACCESS_TLV_WRITE) access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ | SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK;
    access |= SNDRV_CTL_ELEM_ACCESS_USER;
    info->id.numid = 0;

    vsize = value_size(info->type) * info->count;
    alloc = vsize * count;
    if (card->user_ctl_alloc_size + alloc > MAX_USER_CTL_BYTES) return -ENOMEM;

    ue = calloc(1, sizeof(*ue));
    if (!ue) return -ENOMEM;
    ue->info           = *info;
    ue->info.access    = 0;
    ue->info.owner     = 0;
    ue->elem_data_size = vsize;
    ue->elem_data      = calloc(count, vsize);
    if (!ue->elem_data) {
        free(ue);
        return -ENOMEM;
    }
    if (info->type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
        err = user_elem_enum_names(ue);
        if (err < 0) {
            free(ue->elem_data);
            free(ue);
            return err;
        }
    }

    k = ctl_alloc(count, access);
    if (!k) {
        free(ue->enum_names);
        free(ue->elem_data);
        free(ue);
        return -ENOMEM;
    }
    k->id           = info->id;
    k->info         = user_elem_info;
    k->get          = (access & SNDRV_CTL_ELEM_ACCESS_READ) ? user_elem_get : NULL;
    k->put          = (access & SNDRV_CTL_ELEM_ACCESS_WRITE) ? user_elem_put : NULL;
    k->tlv_callback = (access & SNDRV_CTL_ELEM_ACCESS_TLV_WRITE) ? user_elem_tlv : NULL;
    k->private_data = ue;
    k->private_free = user_elem_free;

    mutex_lock(&card->controls_lock);
    if (replace) {
        err = ctl_remove_user_locked(file, &info->id, &old);
        if (err < 0 && err != -ENOENT) {
            mutex_unlock(&card->controls_lock);
            ctl_free_one(k);
            return err;
        }
    }
    err = ctl_add_locked(card, k);
    if (err == EOK) {
        if (old) {
            struct user_element *old_ue = old->private_data;
            card->user_ctl_alloc_size -= old_ue->elem_data_size * old->count;
        }
        card->user_ctl_alloc_size += alloc;
    }
    mutex_unlock(&card->controls_lock);
    if (old) {
        for (unsigned int i = 0; i < old->count; i++) ctl_notify_one(card, SNDRV_CTL_EVENT_MASK_REMOVE, old, i);
        ctl_free_one(old);
    }
    if (err < 0) {
        ctl_free_one(k);
        return err;
    }
    ctl_build_ioff(&info->id, k, ctl_get_ioff(k, &info->id));
    for (unsigned int i = 0; i < k->count; i++) ctl_notify_one(card, SNDRV_CTL_EVENT_MASK_ADD, k, i);
    return EOK;
}

/* SNDRV_CTL_IOCTL_ELEM_REMOVE */
static int ctl_elem_remove(ulsa_ctl_file_t *file, const struct snd_ctl_elem_id *id)
{
    ulsa_card_t     *card = file->card;
    ulsa_kcontrol_t *k    = NULL;
    int              err;

    mutex_lock(&card->controls_lock);
    err = ctl_remove_user_locked(file, id, &k);
    if (err == EOK) {
        struct user_element *ue = k->private_data;
        card->user_ctl_alloc_size -= ue->elem_data_size * k->count;
    }
    mutex_unlock(&card->controls_lock);
    if (err < 0) return err;
    for (unsigned int i = 0; i < k->count; i++) ctl_notify_one(card, SNDRV_CTL_EVENT_MASK_REMOVE, k, i);
    ctl_free_one(k);
    return EOK;
}

/* ------------------------------------------------------- ioctls */

/* SNDRV_CTL_IOCTL_CARD_INFO */
static int ctl_card_info(ulsa_card_t *card, void *arg)
{
    struct snd_ctl_card_info info;

    memset(&info, 0, sizeof(info));
    info.card = card->number;
    strncpy((char *)info.id, card->id, sizeof(info.id) - 1);
    strncpy((char *)info.driver, card->driver, sizeof(info.driver) - 1);
    strncpy((char *)info.name, card->shortname, sizeof(info.name) - 1);
    strncpy((char *)info.longname, card->longname, sizeof(info.longname) - 1);
    strncpy((char *)info.mixername, card->mixername, sizeof(info.mixername) - 1);
    strncpy((char *)info.components, card->components, sizeof(info.components) - 1);
    return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : EOK;
}

/* SNDRV_CTL_IOCTL_CARD_BYTES */
static int ctl_card_bytes(ulsa_card_t *card, struct snd_ctl_card_bytes *uinfo)
{
    struct snd_ctl_card_bytes info;
    uint32_t                  len = (uint32_t)strlen(card->components) + 1;

    if (copy_from_user(&info, uinfo, sizeof(info))) return -EFAULT;
    if (info.type != SND_CTL_CARD_BTYPE_COMPONENTS) return -EINVAL;
    if (info.data && info.data_allocated) {
        if (info.data_allocated < len) return -ENOMEM;
        if (copy_to_user((void *)(uintptr_t)info.data, card->components, len)) return -EFAULT;
    }
    return copy_to_user(&uinfo->data_len, &len, sizeof(len)) ? -EFAULT : EOK;
}

/* SNDRV_CTL_IOCTL_ELEM_LIST */
static int ctl_elem_list(ulsa_card_t *card, struct snd_ctl_elem_list *ulist)
{
    struct snd_ctl_elem_list list;
    unsigned int             offset, space;
    int                      err = EOK;

    if (copy_from_user(&list, ulist, sizeof(list))) return -EFAULT;
    offset = list.offset;
    space  = list.space;

    mutex_lock(&card->controls_lock);
    list.count = card->controls_count;
    list.used  = 0;
    for (ulsa_kcontrol_t *k = card->controls; k && space; k = k->next) {
        if (offset >= k->count) {
            offset -= k->count;
            continue;
        }
        for (unsigned int j = offset; j < k->count && space; j++) {
            struct snd_ctl_elem_id id;
            ctl_build_ioff(&id, k, j);
            if (copy_to_user(list.pids + list.used, &id, sizeof(id))) {
                err = -EFAULT;
                goto out;
            }
            list.used++;
            space--;
        }
        offset = 0;
    }
out:
    mutex_unlock(&card->controls_lock);
    if (err) return err;
    return copy_to_user(ulist, &list, sizeof(list)) ? -EFAULT : EOK;
}

/* SNDRV_CTL_IOCTL_ELEM_INFO */
static int ctl_elem_info(ulsa_ctl_file_t *file, struct snd_ctl_elem_info *uinfo)
{
    ulsa_card_t             *card = file->card;
    struct snd_ctl_elem_info info;
    ulsa_kcontrol_t         *k;
    unsigned int             ioff;
    int                      err;

    if (copy_from_user(&info, uinfo, sizeof(info))) return -EFAULT;
    mutex_lock(&card->controls_lock);
    k = ctl_find_id(card, &info.id);
    if (!k) {
        mutex_unlock(&card->controls_lock);
        return -ENOENT;
    }
    ioff = ctl_get_ioff(k, &info.id);
    ctl_build_ioff(&info.id, k, ioff);
    err = k->info(k, &info);
    if (err >= 0) {
        ctl_build_ioff(&info.id, k, ioff);
        info.access = k->vd[ioff].access;
        if (k->vd[ioff].owner) {
            info.access |= SNDRV_CTL_ELEM_ACCESS_LOCK;
            if (k->vd[ioff].owner == file) info.access |= SNDRV_CTL_ELEM_ACCESS_OWNER;
            info.owner = k->vd[ioff].owner->pid;
        } else {
            info.owner = -1;
        }
    }
    mutex_unlock(&card->controls_lock);
    if (err < 0) return err;
    return copy_to_user(uinfo, &info, sizeof(info)) ? -EFAULT : EOK;
}

/* SNDRV_CTL_IOCTL_ELEM_READ */
static int ctl_elem_read(ulsa_card_t *card, struct snd_ctl_elem_value *uvalue)
{
    struct snd_ctl_elem_value *value = malloc(sizeof(*value));
    ulsa_kcontrol_t           *k;
    unsigned int               ioff;
    int                        err;

    if (!value) return -ENOMEM;
    if (copy_from_user(value, uvalue, sizeof(*value))) {
        free(value);
        return -EFAULT;
    }
    mutex_lock(&card->controls_lock);
    k = ctl_find_id(card, &value->id);
    if (!k) {
        err = -ENOENT;
    } else {
        ioff = ctl_get_ioff(k, &value->id);
        if (!(k->vd[ioff].access & SNDRV_CTL_ELEM_ACCESS_READ) || !k->get) {
            err = -EPERM;
        } else {
            ctl_build_ioff(&value->id, k, ioff);
            memset(&value->value, 0, sizeof(value->value));
            err = k->get(k, value);
        }
    }
    mutex_unlock(&card->controls_lock);
    if (err >= 0 && copy_to_user(uvalue, value, sizeof(*value))) err = -EFAULT;
    free(value);
    return err < 0 ? err : EOK;
}

/* SNDRV_CTL_IOCTL_ELEM_WRITE */
static int ctl_elem_write(ulsa_ctl_file_t *file, struct snd_ctl_elem_value *uvalue)
{
    ulsa_card_t               *card  = file->card;
    struct snd_ctl_elem_value *value = malloc(sizeof(*value));
    struct snd_ctl_elem_id     notify_id;
    ulsa_kcontrol_t           *k;
    unsigned int               ioff = 0;
    int                        err;

    if (!value) return -ENOMEM;
    if (copy_from_user(value, uvalue, sizeof(*value))) {
        free(value);
        return -EFAULT;
    }
    memset(&notify_id, 0, sizeof(notify_id));
    mutex_lock(&card->controls_lock);
    k = ctl_find_id(card, &value->id);
    if (!k) {
        err = -ENOENT;
    } else {
        ioff = ctl_get_ioff(k, &value->id);
        if (!(k->vd[ioff].access & SNDRV_CTL_ELEM_ACCESS_WRITE) || !k->put || (k->vd[ioff].owner && k->vd[ioff].owner != file)) {
            err = -EPERM;
        } else {
            ctl_build_ioff(&value->id, k, ioff);
            notify_id = value->id;
            err       = k->put(k, value);
        }
    }
    mutex_unlock(&card->controls_lock);
    if (err > 0) ulsa_ctl_notify(card, SNDRV_CTL_EVENT_MASK_VALUE, &notify_id);
    if (err >= 0 && copy_to_user(uvalue, value, sizeof(*value))) err = -EFAULT;
    free(value);
    return err < 0 ? err : EOK;
}

/* SNDRV_CTL_IOCTL_ELEM_LOCK / UNLOCK */
static int ctl_elem_lock(ulsa_ctl_file_t *file, const void *uid, bool lock)
{
    ulsa_card_t           *card = file->card;
    struct snd_ctl_elem_id id;
    ulsa_kcontrol_t       *k;
    int                    err = EOK;

    if (copy_from_user(&id, uid, sizeof(id))) return -EFAULT;
    mutex_lock(&card->controls_lock);
    k = ctl_find_id(card, &id);
    if (!k) {
        err = -ENOENT;
    } else {
        ulsa_kcontrol_volatile_t *vd = &k->vd[ctl_get_ioff(k, &id)];
        if (lock) {
            if (vd->owner) {
                err = -EBUSY;
            } else {
                vd->owner = file;
            }
        } else if (!vd->owner) {
            err = -EINVAL;
        } else if (vd->owner != file) {
            err = -EPERM;
        } else {
            vd->owner = NULL;
        }
    }
    mutex_unlock(&card->controls_lock);
    return err;
}

/* SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS */
static int ctl_subscribe(ulsa_ctl_file_t *file, int *uarg)
{
    int      subscribe;
    uint64_t flags;

    if (copy_from_user(&subscribe, uarg, sizeof(subscribe))) return -EFAULT;
    if (subscribe < 0) {
        subscribe = file->subscribed ? 1 : 0;
        return copy_to_user(uarg, &subscribe, sizeof(subscribe)) ? -EFAULT : EOK;
    }
    flags = spin_lock_irqsave(&file->lock);
    if (subscribe) {
        file->subscribed = true;
    } else {
        file->subscribed = false;
        file->count      = 0;
    }
    spin_unlock_irqrestore(&file->lock, flags);
    return EOK;
}

/* SNDRV_CTL_IOCTL_TLV_READ / WRITE / COMMAND */
static int ctl_tlv(ulsa_ctl_file_t *file, struct snd_ctl_tlv *utlv, int op_flag)
{
    ulsa_card_t              *card = file->card;
    struct snd_ctl_tlv        header;
    ulsa_kcontrol_t          *k;
    ulsa_kcontrol_volatile_t *vd;
    struct snd_ctl_elem_id    notify_id;
    int                       err;

    if (copy_from_user(&header, utlv, sizeof(header))) return -EFAULT;
    if (!header.numid) return -EINVAL;
    if (header.length < 2 * sizeof(unsigned int)) return -EINVAL;

    memset(&notify_id, 0, sizeof(notify_id));
    mutex_lock(&card->controls_lock);
    {
        struct snd_ctl_elem_id id;
        memset(&id, 0, sizeof(id));
        id.numid = header.numid;
        k        = ctl_find_id(card, &id);
        vd       = k ? &k->vd[header.numid - k->id.numid] : NULL;
        if (k) ctl_build_ioff(&notify_id, k, header.numid - k->id.numid);
    }
    if (!k) {
        err = -ENOENT;
    } else if (vd->access & SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK) {
        unsigned int perm = SNDRV_CTL_ELEM_ACCESS_TLV_COMMAND;
        if (op_flag == SNDRV_CTL_TLV_OP_READ) {
            perm = SNDRV_CTL_ELEM_ACCESS_TLV_READ;
        } else if (op_flag == SNDRV_CTL_TLV_OP_WRITE) {
            perm = SNDRV_CTL_ELEM_ACCESS_TLV_WRITE;
        }
        if (!(vd->access & perm) || !k->tlv_callback) {
            err = -ENXIO;
        } else if (op_flag != SNDRV_CTL_TLV_OP_READ && vd->owner && vd->owner != file) {
            err = -EPERM;
        } else {
            err = k->tlv_callback(k, op_flag, header.length, utlv->tlv);
        }
    } else if (op_flag == SNDRV_CTL_TLV_OP_READ && (vd->access & SNDRV_CTL_ELEM_ACCESS_TLV_READ) && k->tlv) {
        unsigned int len = 2 * sizeof(unsigned int) + k->tlv[1];
        if (header.length < len) {
            err = -ENOMEM;
        } else {
            err = copy_to_user(utlv->tlv, k->tlv, len) ? -EFAULT : EOK;
        }
    } else {
        err = -ENXIO;
    }
    mutex_unlock(&card->controls_lock);
    if (err > 0) {
        ulsa_ctl_notify(card, SNDRV_CTL_EVENT_MASK_TLV, &notify_id);
        err = EOK;
    }
    return err;
}

/* PCM-related control ioctls. */
static int ctl_pcm_ioctl(ulsa_ctl_file_t *file, size_t req, void *arg)
{
    ulsa_card_t *card = file->card;

    switch (req) {
        case SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE : {
            int device;
            if (copy_from_user(&device, arg, sizeof(device))) return -EFAULT;
            device = ulsa_pcm_next_device(card, device);
            return copy_to_user(arg, &device, sizeof(device)) ? -EFAULT : EOK;
        }
        case SNDRV_CTL_IOCTL_PCM_INFO : {
            struct snd_pcm_info   info;
            ulsa_pcm_t           *pcm;
            ulsa_pcm_str_t       *pstr;
            ulsa_pcm_substream_t *s;
            int                   err;

            if (copy_from_user(&info, arg, sizeof(info))) return -EFAULT;
            if (info.stream < 0 || info.stream > 1) return -EINVAL;
            if (info.device >= SNDRV_MINOR_PCMS) return -ENXIO;
            pcm = card->pcms[info.device];
            if (!pcm) return -ENXIO;
            pstr = &pcm->streams[info.stream];
            if (!pstr->substream_count) return -ENOENT;
            if (info.subdevice >= pstr->substream_count) return -ENXIO;
            for (s = pstr->substream; s && s->number != (int)info.subdevice; s = s->next) {}
            if (!s) return -ENXIO;
            mutex_lock(&pcm->open_mutex);
            err = ulsa_pcm_info(s, &info);
            mutex_unlock(&pcm->open_mutex);
            if (err < 0) return err;
            return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : EOK;
        }
        case SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE : {
            int val;
            if (copy_from_user(&val, arg, sizeof(val))) return -EFAULT;
            file->preferred_subdevice[0] = val;
            return EOK;
        }
        default :
            return -ENOTTY;
    }
}

/* Control ioctl dispatcher. */
static int ctl_dev_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg)
{
    ulsa_ctl_file_t *file = private_data;
    ulsa_card_t     *card = file ? file->card : NULL;

    (void)ctx;
    (void)flags;
    if (!card) return -ENXIO;
    switch (req) {
        case SNDRV_CTL_IOCTL_PVERSION : {
            int version = SNDRV_CTL_VERSION;
            return copy_to_user(arg, &version, sizeof(version)) ? -EFAULT : EOK;
        }
        case SNDRV_CTL_IOCTL_CARD_INFO :
            return ctl_card_info(card, arg);
        case SNDRV_CTL_IOCTL_CARD_BYTES :
            return ctl_card_bytes(card, arg);
        case SNDRV_CTL_IOCTL_ELEM_LIST :
            return ctl_elem_list(card, arg);
        case SNDRV_CTL_IOCTL_ELEM_INFO :
            return ctl_elem_info(file, arg);
        case SNDRV_CTL_IOCTL_ELEM_READ :
            return ctl_elem_read(card, arg);
        case SNDRV_CTL_IOCTL_ELEM_WRITE :
            return ctl_elem_write(file, arg);
        case SNDRV_CTL_IOCTL_ELEM_LOCK :
            return ctl_elem_lock(file, arg, true);
        case SNDRV_CTL_IOCTL_ELEM_UNLOCK :
            return ctl_elem_lock(file, arg, false);
        case SNDRV_CTL_IOCTL_ELEM_ADD :
        case SNDRV_CTL_IOCTL_ELEM_REPLACE : {
            struct snd_ctl_elem_info info;
            int                      err;

            if (copy_from_user(&info, arg, sizeof(info))) return -EFAULT;
            err = ctl_elem_add(file, &info, req == SNDRV_CTL_IOCTL_ELEM_REPLACE);
            if (err < 0) return err;
            if (copy_to_user(arg, &info, sizeof(info))) {
                (void)ctl_elem_remove(file, &info.id);
                return -EFAULT;
            }
            return EOK;
        }
        case SNDRV_CTL_IOCTL_ELEM_REMOVE : {
            struct snd_ctl_elem_id id;
            if (copy_from_user(&id, arg, sizeof(id))) return -EFAULT;
            return ctl_elem_remove(file, &id);
        }
        case SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS :
            return ctl_subscribe(file, arg);
        case SNDRV_CTL_IOCTL_TLV_READ :
            return ctl_tlv(file, arg, SNDRV_CTL_TLV_OP_READ);
        case SNDRV_CTL_IOCTL_TLV_WRITE :
            return ctl_tlv(file, arg, SNDRV_CTL_TLV_OP_WRITE);
        case SNDRV_CTL_IOCTL_TLV_COMMAND :
            return ctl_tlv(file, arg, SNDRV_CTL_TLV_OP_CMD);
        case SNDRV_CTL_IOCTL_POWER :
            return -ENOPROTOOPT;
        case SNDRV_CTL_IOCTL_POWER_STATE : {
            int state = SNDRV_CTL_POWER_D0;
            return copy_to_user(arg, &state, sizeof(state)) ? -EFAULT : EOK;
        }
        case SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE :
        case SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE :
        case SNDRV_CTL_IOCTL_UMP_NEXT_DEVICE : {
            /* No hwdep, rawmidi or UMP devices exist on ULSA cards. */
            int none = -1;
            return copy_to_user(arg, &none, sizeof(none)) ? -EFAULT : EOK;
        }
        case SNDRV_CTL_IOCTL_RAWMIDI_PREFER_SUBDEVICE : {
            int val;
            if (copy_from_user(&val, arg, sizeof(val))) return -EFAULT;
            file->preferred_subdevice[1] = val;
            return EOK;
        }
        default :
            return ctl_pcm_ioctl(file, req, arg);
    }
}

/* ------------------------------------------------- file callbacks */

/* Open a control file. */
static int ctl_dev_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    tmpfs_file_t    *f     = node ? node->handle : NULL;
    ulsa_minor_t    *minor = f ? f->device.ctx : NULL;
    ulsa_ctl_file_t *file;
    uint64_t         lflags;

    (void)flags;
    if (!minor || !minor->card || !private_data) return -ENODEV;
    file = calloc(1, sizeof(*file));
    if (!file) return -ENOMEM;
    file->card                   = minor->card;
    file->preferred_subdevice[0] = -1;
    file->preferred_subdevice[1] = -1;
    {
        process_t *proc = process_current();
        file->pid       = proc && proc->task ? (int)proc->task->tgid : 0;
    }
    wait_queue_init(&file->wait);
    vfs_poll_source_init(&file->poll_source);

    lflags                = spin_lock_irqsave(&file->card->files_lock);
    file->next            = file->card->ctl_files;
    file->card->ctl_files = file;
    spin_unlock_irqrestore(&file->card->files_lock, lflags);
    *private_data = file;
    return EOK;
}

/* Release a control file: unlink it and drop the element locks it held. */
static void ctl_dev_release(vfs_node_t node, void *private_data)
{
    ulsa_ctl_file_t *file = private_data;
    ulsa_card_t     *card;
    uint64_t         lflags;

    (void)node;
    if (!file) return;
    card   = file->card;
    lflags = spin_lock_irqsave(&card->files_lock);
    for (ulsa_ctl_file_t **link = &card->ctl_files; *link; link = &(*link)->next) {
        if (*link == file) {
            *link = file->next;
            break;
        }
    }
    spin_unlock_irqrestore(&card->files_lock, lflags);

    mutex_lock(&card->controls_lock);
    for (ulsa_kcontrol_t *k = card->controls; k; k = k->next)
        for (unsigned int i = 0; i < k->count; i++)
            if (k->vd[i].owner == file) k->vd[i].owner = NULL;
    mutex_unlock(&card->controls_lock);

    vfs_poll_source_close(&file->poll_source, POLLHUP);
    free(file);
}

/* read(2): queued snd_ctl_event records of a subscribed file. */
static int64_t ctl_dev_read_user(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, struct process *proc)
{
    ulsa_ctl_file_t *file   = private_data;
    int64_t          result = 0;
    uint64_t         lflags;

    (void)ctx;
    (void)offset;
    (void)proc;
    if (!file) return -ENXIO;
    if (!file->subscribed) return -EBADFD;
    if (size < sizeof(struct snd_ctl_event)) return -EINVAL;

    lflags = spin_lock_irqsave(&file->lock);
    while (size >= sizeof(struct snd_ctl_event)) {
        struct snd_ctl_event ev;

        while (!file->count) {
            if ((flags & O_NONBLOCK) || result > 0) {
                spin_unlock_irqrestore(&file->lock, lflags);
                return result > 0 ? result : -EAGAIN;
            }
            if (signal_has_pending_current()) {
                spin_unlock_irqrestore(&file->lock, lflags);
                return -ERESTARTSYS;
            }
            wait_queue_prepare(&file->wait);
            spin_unlock_irqrestore(&file->lock, lflags);
            wait_queue_sleep();
            lflags = spin_lock_irqsave(&file->lock);
            if (!file->subscribed) {
                spin_unlock_irqrestore(&file->lock, lflags);
                return result > 0 ? result : -EBADFD;
            }
        }
        memset(&ev, 0, sizeof(ev));
        ev.type           = SNDRV_CTL_EVENT_ELEM;
        ev.data.elem.mask = file->events[file->head].mask;
        ev.data.elem.id   = file->events[file->head].id;
        file->head        = (file->head + 1) % CTL_EVENT_QUEUE;
        file->count--;
        spin_unlock_irqrestore(&file->lock, lflags);
        if (copy_to_user((uint8_t *)addr + result, &ev, sizeof(ev))) return result > 0 ? result : -EFAULT;
        result += (int64_t)sizeof(ev);
        size -= sizeof(ev);
        lflags = spin_lock_irqsave(&file->lock);
    }
    spin_unlock_irqrestore(&file->lock, lflags);
    return result;
}

/* Kernel-buffer reads are never used for control nodes. */
static int64_t ctl_dev_read_kernel(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)addr;
    (void)offset;
    (void)size;
    return -EINVAL;
}

/* poll(2): readable when events are queued. */
static int ctl_dev_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    ulsa_ctl_file_t *file = private_data;

    (void)ctx;
    (void)flags;
    if (!file || !file->subscribed) return 0;
    return file->count ? (int)(events & (POLLIN | POLLRDNORM)) : 0;
}

/* Readiness source for poll/epoll. */
static vfs_poll_source_t *ctl_dev_poll_source(void *ctx, void *private_data)
{
    ulsa_ctl_file_t *file = private_data;

    (void)ctx;
    return file ? &file->poll_source : NULL;
}

const tmpfs_device_ops_t ulsa_ctl_device_ops = {
    .open             = ctl_dev_open,
    .release          = ctl_dev_release,
    .file_read        = ctl_dev_read_kernel,
    .file_read_user   = ctl_dev_read_user,
    .file_poll        = ctl_dev_poll,
    .file_poll_source = ctl_dev_poll_source,
    .file_ioctl       = ctl_dev_ioctl,
};

/* Publish controlC<n>. */
int ulsa_ctl_register(ulsa_card_t *card)
{
    char name[24];

    if (card->ctl_minor) return EOK;
    (void)snprintf(name, sizeof(name), "controlC%d", card->number);
    return ulsa_register_device(ULSA_DEVICE_CONTROL, card, NULL, 0, 0, name, &ulsa_ctl_device_ops, &card->ctl_minor);
}

#endif
