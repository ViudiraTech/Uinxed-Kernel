/*
 *
 *      evdev.c
 *      Linux-compatible evdev input event subsystem
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <acpi.h>
#include <alloc.h>
#include <errno.h>
#include <evdev.h>
#include <input_event.h>
#include <intrusive_list.h>
#include <printk.h>
#include <spin_lock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <task.h>

/* ---- _IOC extraction macros ---- */

#define _IOC_DIR(nr)	(((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr)	(((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)	(((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr)	(((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

#define _IOC_DIRMASK	((1U << _IOC_DIRBITS) - 1)
#define _IOC_TYPEMASK	((1U << _IOC_TYPEBITS) - 1)
#define _IOC_NRMASK	((1U << _IOC_NRBITS) - 1)
#define _IOC_SIZEMASK	((1U << _IOC_SIZEBITS) - 1)

/* ---- poll flags ---- */

#define POLLIN		0x001
#define POLLOUT		0x004
#define POLLHUP		0x010

/* ---- bit operations ---- */

static inline void set_bit(unsigned int nr, uint32_t *addr)
{
	addr[nr / 32] |= (1U << (nr % 32));
}

static inline void clear_bit(unsigned int nr, uint32_t *addr)
{
	addr[nr / 32] &= ~(1U << (nr % 32));
}

static inline bool test_bit(unsigned int nr, const uint32_t *addr)
{
	return (addr[nr / 32] >> (nr % 32)) & 1U;
}

static inline bool is_power_of_2(unsigned int n)
{
	return n && !(n & (n - 1));
}

static inline unsigned int roundup_pow_of_two(unsigned int n)
{
	unsigned int r = 1;

	while (r < n)
		r <<= 1;
	return r;
}

/* ---- helpers ---- */

static inline size_t evdev_event_size(void)
{
	return sizeof(input_event_t);
}

/* ---- global state ---- */

static evdev_t *evdev_table[EVDEV_MAX_DEVICES];
static spinlock_t evdev_table_lock = {0};
static bool evdev_initialized = false;

/* ---- evdev_get_mask_cnt ---- */

static size_t evdev_get_mask_cnt(unsigned int type)
{
	static const size_t counts[EV_CNT] = {
		[EV_SYN] = EV_CNT,
		[EV_KEY] = KEY_CNT,
		[EV_REL] = REL_CNT,
		[EV_ABS] = ABS_CNT,
		[EV_MSC] = MSC_CNT,
		[EV_SW]  = SW_CNT,
		[EV_LED] = LED_CNT,
		[EV_SND] = SND_CNT,
		[EV_FF]  = FF_CNT,
	};

	return (type < EV_CNT) ? counts[type] : 0;
}

/* ---- evdev_compute_buffer_size ---- */

static unsigned int evdev_compute_buffer_size(input_dev_t *dev)
{
	unsigned int n_events = dev->hint_events_per_packet * EVDEV_BUF_PACKETS;

	if (n_events < EVDEV_MIN_BUFFER_SIZE)
		n_events = EVDEV_MIN_BUFFER_SIZE;
	return roundup_pow_of_two(n_events);
}

/* ---- __evdev_is_filtered ---- */

static bool __evdev_is_filtered(evdev_client_t *client, unsigned int type,
				unsigned int code)
{
	uint32_t *mask;
	size_t cnt;

	if (type == EV_SYN || type >= EV_CNT)
		return false;

	mask = client->evmasks[0];
	if (mask && !test_bit(type, mask))
		return true;

	cnt = evdev_get_mask_cnt(type);
	if (!cnt || code >= cnt)
		return false;

	mask = client->evmasks[type];
	return mask && !test_bit(code, mask);
}

/* ---- __evdev_queue_syn_dropped ---- */

static void __evdev_queue_syn_dropped(evdev_client_t *client)
{
	unsigned int mask = client->bufsize - 1;
	uint64_t ns = nano_time();
	input_event_t ev;

	ev.sec  = ns / 1000000000ULL;
	ev.usec = (ns / 1000ULL) % 1000000ULL;
	ev.type = EV_SYN;
	ev.code = SYN_DROPPED;
	ev.value = 0;

	client->buffer[client->head & mask] = ev;
	client->head = (client->head + 1) & mask;

	/*
	 * If the buffer is full, drop all unconsumed events but keep
	 * the SYN_DROPPED we just inserted.
	 */
	if (client->head == client->tail) {
		client->tail = (client->head - 1) & mask;
		client->packet_head = client->tail;
	}
}

/* ---- __evdev_flush_queue ---- */

/*
 * __evdev_flush_queue - flush queued events of type @type.
 * Caller must hold client->buffer_lock.
 *
 * This compacts the ring buffer by removing all events that match
 * the newly-installed filter mask.  After flushing, a SYN_DROPPED
 * is inserted to inform userspace that events were dropped.
 */
static void __evdev_flush_queue(evdev_client_t *client, unsigned int type)
{
	unsigned int head = client->head;
	unsigned int tail = client->tail;
	unsigned int mask = client->bufsize - 1;
	unsigned int write, read;
	bool dropped = false;

	/*
	 * Compact the ring buffer: walk from tail to head,
	 * copying unfiltered events to the front.
	 */
	write = tail;
	for (read = tail; read != head; read = (read + 1) & mask) {
		input_event_t *ev = &client->buffer[read & mask];

		if (__evdev_is_filtered(client, ev->type, ev->code)) {
			dropped = true;
			continue;
		}
		if (write != read)
			client->buffer[write & mask] = *ev;
		write = (write + 1) & mask;
	}

	client->head = write;
	client->packet_head = write;

	/* If we dropped any events, queue a SYN_DROPPED */
	if (dropped && type != EV_SYN)
		__evdev_queue_syn_dropped(client);
}

/* ---- __pass_event ---- */

static void __pass_event(evdev_client_t *client, const input_event_t *event)
{
	unsigned int mask = client->bufsize - 1;

	client->buffer[client->head & mask] = *event;
	client->head = (client->head + 1) & mask;

	if (client->head == client->tail) {
		/*
		 * Buffer overflow: drop all unconsumed events, leaving
		 * EV_SYN/SYN_DROPPED plus the newest event in the queue.
		 */
		client->tail = (client->head - 2) & mask;
		client->buffer[client->tail & mask] = (input_event_t){
			.sec   = event->sec,
			.usec  = event->usec,
			.type  = EV_SYN,
			.code  = SYN_DROPPED,
			.value = 0,
		};
		client->packet_head = client->tail;
	}

	if (event->type == EV_SYN && event->code == SYN_REPORT)
		client->packet_head = client->head;
}

/* ---- evdev_pass_values ---- */

static void evdev_pass_values(evdev_client_t *client,
			      const input_event_t *values,
			      unsigned int count, uint64_t time_ns)
{
	unsigned int i;
	bool wake = false;
	input_event_t event;

	spin_lock(&client->buffer_lock);
	for (i = 0; i < count; i++) {
		event = values[i];

		/* Timestamp the event */
		event.sec  = time_ns / 1000000000ULL;
		event.usec = (time_ns / 1000ULL) % 1000000ULL;

		/* Filter check */
		if (__evdev_is_filtered(client, event.type, event.code))
			continue;

		__pass_event(client, &event);

		if (event.type == EV_SYN && event.code == SYN_REPORT)
			wake = true;
	}
	spin_unlock(&client->buffer_lock);

	if (wake)
		wait_queue_wake_one(&client->wait);
}

/* ---- evdev_events ---- */

static void evdev_events(input_dev_t *dev, const input_event_t *values,
		  unsigned int count)
{
	evdev_t *evdev;
	evdev_client_t *client;
	evdev_client_t *grab;
	ilist_node_t *node;
	uint64_t time_ns;
	unsigned int i;

	/* Find the evdev associated with this input_dev */
	spin_lock(&evdev_table_lock);
	evdev = NULL;
	for (i = 0; i < EVDEV_MAX_DEVICES; i++) {
		if (evdev_table[i] && evdev_table[i]->input_dev == dev) {
			evdev = evdev_table[i];
			break;
		}
	}
	spin_unlock(&evdev_table_lock);

	if (!evdev || !evdev->exist)
		return;

	time_ns = nano_time();

	spin_lock(&evdev->client_lock);

	grab = evdev->grab;
	if (grab) {
		/* Exclusive grab: only the grab client gets events */
		spin_unlock(&evdev->client_lock);
		evdev_pass_values(grab, values, count, time_ns);
		return;
	}

	/* Distribute to all clients */
	for (node = evdev->client_list.next;
	     node != &evdev->client_list;
	     node = node->next) {
		client = (evdev_client_t *)((uintptr_t)node -
					    offsetof(evdev_client_t, node));
		evdev_pass_values(client, values, count, time_ns);
	}

	spin_unlock(&evdev->client_lock);
}

/* ---- evdev_set_clk_type ---- */

static void evdev_set_clk_type(evdev_client_t *client, int clk_type)
{
	if (client->clk_type == clk_type)
		return;

	spin_lock(&client->buffer_lock);
	client->clk_type = clk_type;
	/* Flush pending events and queue SYN_DROPPED */
	client->tail = client->head;
	client->packet_head = client->head;
	__evdev_queue_syn_dropped(client);
	spin_unlock(&client->buffer_lock);
	wait_queue_wake_one(&client->wait);
}

/* ---- evdev_grab / evdev_ungrab ---- */

static int evdev_grab(evdev_t *evdev, evdev_client_t *client)
{
	spin_lock(&evdev->client_lock);
	if (evdev->grab) {
		spin_unlock(&evdev->client_lock);
		return -EBUSY;
	}
	evdev->grab = client;
	spin_unlock(&evdev->client_lock);
	return 0;
}

static int evdev_ungrab(evdev_t *evdev, evdev_client_t *client)
{
	spin_lock(&evdev->client_lock);
	if (evdev->grab != client) {
		spin_unlock(&evdev->client_lock);
		return -EINVAL;
	}
	evdev->grab = NULL;
	spin_unlock(&evdev->client_lock);
	return 0;
}

/* ---- evdev_attach_client / evdev_detach_client ---- */

static void evdev_attach_client(evdev_t *evdev, evdev_client_t *client)
{
	spin_lock(&evdev->client_lock);
	ilist_insert_after(&evdev->client_list, &client->node);
	spin_unlock(&evdev->client_lock);
}

static void evdev_detach_client(evdev_t *evdev, evdev_client_t *client)
{
	spin_lock(&evdev->client_lock);
	ilist_remove(&client->node);
	spin_unlock(&evdev->client_lock);
}

/* ---- evdev_open_device / evdev_close_device ---- */

static int evdev_open_device(evdev_t *evdev)
{
	int ret = 0;

	spin_lock(&evdev->mutex);
	if (!evdev->exist) {
		ret = -ENODEV;
		goto out;
	}
	if (evdev->open_count == 0) {
		/*
		 * input_open_device would go here.
		 * For now, just count opens.
		 */
	}
	evdev->open_count++;
out:
	spin_unlock(&evdev->mutex);
	return ret;
}

static void evdev_close_device(evdev_t *evdev)
{
	spin_lock(&evdev->mutex);
	if (evdev->exist) {
		if (evdev->open_count > 0)
			evdev->open_count--;
		/*
		 * input_close_device would go here when open_count
		 * reaches 0.  For now, just decrement.
		 */
	}
	spin_unlock(&evdev->mutex);
}

/* ---- evdev_hangup ---- */

static void evdev_hangup(evdev_t *evdev)
{
	ilist_node_t *node;
	evdev_client_t *client;

	spin_lock(&evdev->client_lock);
	evdev->exist = false;

	for (node = evdev->client_list.next;
	     node != &evdev->client_list;
	     node = node->next) {
		client = (evdev_client_t *)((uintptr_t)node -
					    offsetof(evdev_client_t, node));
		wait_queue_wake_one(&client->wait);
	}
	spin_unlock(&evdev->client_lock);
}

/* ---- evdev_create ---- */

evdev_t *evdev_create(input_dev_t *dev)
{
	evdev_t *evdev;

	evdev = malloc(sizeof(evdev_t));
	if (!evdev)
		return NULL;

	memset(evdev, 0, sizeof(evdev_t));
	evdev->input_dev = dev;
	evdev->exist = true;
	evdev->client_list = (ilist_node_t){&evdev->client_list,
					    &evdev->client_list};
	evdev->client_lock = (spinlock_t){0};
	evdev->mutex = (spinlock_t){0};
	evdev->grab = NULL;
	evdev->open_count = 0;
	evdev->minor = -1;

	return evdev;
}

/* ---- evdev_destroy ---- */

void evdev_destroy(evdev_t *evdev)
{
	if (!evdev)
		return;

	evdev_hangup(evdev);
	free(evdev);
}

/* ---- evdev_register ---- */

int evdev_register(evdev_t *evdev)
{
	int minor;
	int i;

	if (!evdev || !evdev_initialized)
		return -EINVAL;

	spin_lock(&evdev_table_lock);
	minor = -1;
	for (i = 0; i < EVDEV_MAX_DEVICES; i++) {
		if (!evdev_table[i]) {
			minor = i;
			break;
		}
	}
	if (minor < 0) {
		spin_unlock(&evdev_table_lock);
		return -ENFILE;
	}

	evdev->minor = minor;
	evdev_table[minor] = evdev;
	spin_unlock(&evdev_table_lock);

	return 0;
}

/* ---- evdev_unregister ---- */

void evdev_unregister(evdev_t *evdev)
{
	if (!evdev)
		return;

	spin_lock(&evdev_table_lock);
	if (evdev->minor >= 0 && evdev->minor < EVDEV_MAX_DEVICES &&
	    evdev_table[evdev->minor] == evdev)
		evdev_table[evdev->minor] = NULL;
	spin_unlock(&evdev_table_lock);

	evdev_destroy(evdev);
}

/* ---- evdev_find_by_minor ---- */

evdev_t *evdev_find_by_minor(int minor)
{
	evdev_t *evdev;

	if (minor < 0 || minor >= EVDEV_MAX_DEVICES)
		return NULL;

	spin_lock(&evdev_table_lock);
	evdev = evdev_table[minor];
	spin_unlock(&evdev_table_lock);

	return evdev;
}

/* ---- evdev_init ---- */

void evdev_init(void)
{
	int i;

	spin_lock(&evdev_table_lock);
	for (i = 0; i < EVDEV_MAX_DEVICES; i++)
		evdev_table[i] = NULL;
	evdev_initialized = true;
	spin_unlock(&evdev_table_lock);
}

/* ---- evdev_inject_event ---- */

void evdev_inject_event(input_dev_t *dev, uint16_t type, uint16_t code,
			int32_t value)
{
	input_event_t event;

	event.sec  = 0;
	event.usec = 0;
	event.type = type;
	event.code = code;
	event.value = value;

	/*
	 * Update the input device's state bitmaps for key/led/snd/sw
	 * types so that EVIOCGKEY et al. return correct state.
	 */
	switch (type) {
	case EV_KEY:
		if (code < KEY_CNT) {
			if (value)
				set_bit(code, dev->key_state);
			else
				clear_bit(code, dev->key_state);
		}
		break;
	case EV_LED:
		if (code < LED_CNT) {
			if (value)
				set_bit(code, dev->led_state);
			else
				clear_bit(code, dev->led_state);
		}
		break;
	case EV_SND:
		if (code < SND_CNT) {
			if (value)
				set_bit(code, dev->snd_state);
			else
				clear_bit(code, dev->snd_state);
		}
		break;
	case EV_SW:
		if (code < SW_CNT) {
			if (value)
				set_bit(code, dev->sw_state);
			else
				clear_bit(code, dev->sw_state);
		}
		break;
	default:
		break;
	}

	evdev_events(dev, &event, 1);
}

/* ---- evdev_inject_syn ---- */

void evdev_inject_syn(input_dev_t *dev)
{
	evdev_inject_event(dev, EV_SYN, SYN_REPORT, 0);
}

/* ---- evdev_fop_open ---- */

evdev_client_t *evdev_fop_open(evdev_t *evdev)
{
	evdev_client_t *client;
	unsigned int bufsize;
	size_t client_size;
	int ret;

	if (!evdev || !evdev->exist)
		return NULL;

	ret = evdev_open_device(evdev);
	if (ret < 0)
		return NULL;

	bufsize = evdev_compute_buffer_size(evdev->input_dev);
	client_size = sizeof(evdev_client_t) + bufsize * sizeof(input_event_t);

	client = malloc(client_size);
	if (!client) {
		evdev_close_device(evdev);
		return NULL;
	}

	memset(client, 0, client_size);
	client->bufsize = bufsize;
	client->evdev = evdev;
	client->head = 0;
	client->tail = 0;
	client->packet_head = 0;
	client->buffer_lock = (spinlock_t){0};
	client->clk_type = CLOCK_REALTIME;
	client->revoked = false;
	wait_queue_init(&client->wait);

	evdev_attach_client(evdev, client);

	return client;
}

/* ---- evdev_fop_release ---- */

void evdev_fop_release(evdev_client_t *client)
{
	evdev_t *evdev;
	int i;

	if (!client)
		return;

	evdev = client->evdev;

	evdev_detach_client(evdev, client);

	/* Release grab if this client held it */
	if (evdev->grab == client)
		evdev->grab = NULL;

	/* Free event filter masks */
	for (i = 0; i < EV_CNT; i++) {
		if (client->evmasks[i]) {
			free(client->evmasks[i]);
			client->evmasks[i] = NULL;
		}
	}

	evdev_close_device(evdev);
	free(client);
}

/* ---- evdev_fop_read ---- */

ssize_t evdev_fop_read(evdev_client_t *client, void *buf, size_t count,
		       bool nonblock)
{
	evdev_t *evdev = client->evdev;
	input_event_t *dst = (input_event_t *)buf;
	size_t max_events = count / sizeof(input_event_t);
	size_t read_count = 0;

	if (max_events == 0)
		return -EINVAL;

	if (!evdev->exist)
		return -ENODEV;

	if (client->revoked)
		return -ENODEV;

	spin_lock(&client->buffer_lock);

	while (client->tail == client->head) {
		spin_unlock(&client->buffer_lock);

		if (nonblock)
			return -EAGAIN;

		if (!evdev->exist)
			return -ENODEV;

		if (client->revoked)
			return -ENODEV;

		wait_queue_wait(&client->wait);

		if (!evdev->exist)
			return -ENODEV;

		if (client->revoked)
			return -ENODEV;

		spin_lock(&client->buffer_lock);
	}

	/* Read events from ring buffer */
	while (read_count < max_events && client->tail != client->head) {
		unsigned int mask = client->bufsize - 1;

		dst[read_count] = client->buffer[client->tail & mask];
		read_count++;
		client->tail = (client->tail + 1) & mask;

		/*
		 * Stop at packet boundary if we've read at least one
		 * complete SYN_REPORT packet.
		 */
		if (client->packet_head == client->tail)
			break;
	}

	spin_unlock(&client->buffer_lock);

	return (ssize_t)(read_count * sizeof(input_event_t));
}

/* ---- evdev_fop_write ---- */

ssize_t evdev_fop_write(evdev_client_t *client, const void *buf, size_t count)
{
	evdev_t *evdev = client->evdev;
	const input_event_t *src = (const input_event_t *)buf;
	size_t n_events = count / sizeof(input_event_t);
	size_t i;

	if (n_events == 0)
		return -EINVAL;

	if (!evdev->exist)
		return -ENODEV;

	if (client->revoked)
		return -ENODEV;

	for (i = 0; i < n_events; i++)
		evdev_inject_event(evdev->input_dev, src[i].type,
				   src[i].code, src[i].value);

	return (ssize_t)(n_events * sizeof(input_event_t));
}

/* ---- evdev_fop_poll ---- */

int evdev_fop_poll(evdev_client_t *client, int events)
{
	evdev_t *evdev = client->evdev;
	int revents = 0;

	(void)events;

	if (client->revoked || !evdev->exist)
		return POLLHUP;

	spin_lock(&client->buffer_lock);
	if (client->tail != client->head)
		revents |= POLLIN;
	spin_unlock(&client->buffer_lock);

	revents |= POLLOUT;

	return revents;
}

/* ---- evdev_fop_ioctl ---- */

int evdev_fop_ioctl(evdev_client_t *client, uint32_t request, void *arg)
{
	evdev_t *evdev = client->evdev;
	input_dev_t *dev = evdev->input_dev;
	size_t len;
	unsigned int ev_type;
	int ival;

	if (!evdev->exist)
		return -ENODEV;

	if (client->revoked)
		return -ENODEV;

	switch (request) {
	case EVIOCGVERSION: {
		int32_t version = EV_VERSION;

		memcpy(arg, &version, sizeof(version));
		return 0;
	}

	case EVIOCGID:
		memcpy(arg, &dev->id, sizeof(dev->id));
		return 0;

	case EVIOCGREP:
		memcpy(arg, dev->rep, sizeof(dev->rep));
		return 0;

	case EVIOCSREP:
		memcpy(dev->rep, arg, sizeof(dev->rep));
		return 0;

	case EVIOCGKEYCODE:
	case EVIOCGKEYCODE_V2:
	case EVIOCSKEYCODE:
	case EVIOCSKEYCODE_V2:
		return -EINVAL;

	case EVIOCSFF:
	case EVIOCRMFF:
	case EVIOCGEFFECTS:
		return -EINVAL;
	}

	/* Handle variable-length ioctls by extracting the _IOC_NR */

	switch (_IOC_NR(request)) {
	case 0x06: /* EVIOCGNAME(len) */
		len = _IOC_SIZE(request);
		if (len > EVDEV_MAX_NAME_LEN)
			len = EVDEV_MAX_NAME_LEN;
		memcpy(arg, dev->name, len);
		return 0;

	case 0x07: /* EVIOCGPHYS(len) */
		len = _IOC_SIZE(request);
		if (len > EVDEV_MAX_NAME_LEN)
			len = EVDEV_MAX_NAME_LEN;
		memcpy(arg, dev->phys, len);
		return 0;

	case 0x08: /* EVIOCGUNIQ(len) */
		len = _IOC_SIZE(request);
		if (len > EVDEV_MAX_NAME_LEN)
			len = EVDEV_MAX_NAME_LEN;
		memcpy(arg, dev->uniq, len);
		return 0;

	case 0x09: /* EVIOCGPROP(len) */
		len = _IOC_SIZE(request);
		{
			size_t nbytes = ((INPUT_PROP_CNT) + 7) / 8;

			if (len > nbytes)
				len = nbytes;
			memcpy(arg, dev->propbit, len);
		}
		return 0;

	case 0x18: /* EVIOCGKEY(len) */
		len = _IOC_SIZE(request);
		{
			size_t nbytes = ((KEY_CNT) + 7) / 8;

			if (len > nbytes)
				len = nbytes;
			memcpy(arg, dev->key_state, len);
		}
		return 0;

	case 0x19: /* EVIOCGLED(len) */
		len = _IOC_SIZE(request);
		{
			size_t nbytes = ((LED_CNT) + 7) / 8;

			if (len > nbytes)
				len = nbytes;
			memcpy(arg, dev->led_state, len);
		}
		return 0;

	case 0x1a: /* EVIOCGSND(len) */
		len = _IOC_SIZE(request);
		{
			size_t nbytes = ((SND_CNT) + 7) / 8;

			if (len > nbytes)
				len = nbytes;
			memcpy(arg, dev->snd_state, len);
		}
		return 0;

	case 0x1b: /* EVIOCGSW(len) */
		len = _IOC_SIZE(request);
		{
			size_t nbytes = ((SW_CNT) + 7) / 8;

			if (len > nbytes)
				len = nbytes;
			memcpy(arg, dev->sw_state, len);
		}
		return 0;
	}

	/* EVIOCGBIT(ev, len) and EVIOCGABS/EVIOCSABS */
	ev_type = _IOC_NR(request);
	if (ev_type >= 0x20 && ev_type <= 0x3f) {
		/* EVIOCGBIT(ev, len) */
		unsigned int ev = ev_type - 0x20;
		size_t cnt = evdev_get_mask_cnt(ev);
		const uint32_t *src = NULL;

		len = _IOC_SIZE(request);

		switch (ev) {
		case 0:
			src = dev->evbit;
			cnt = EV_CNT;
			break;
		case EV_KEY:
			src = dev->keybit;
			break;
		case EV_REL:
			src = dev->relbit;
			break;
		case EV_ABS:
			src = dev->absbit;
			break;
		case EV_MSC:
			src = dev->mscbit;
			break;
		case EV_LED:
			src = dev->ledbit;
			break;
		case EV_SND:
			src = dev->sndbit;
			break;
		case EV_SW:
			src = dev->swbit;
			break;
		case EV_FF:
			src = dev->ffbit;
			break;
		default:
			src = NULL;
			cnt = 0;
			break;
		}

		if (!src || cnt == 0) {
			memset(arg, 0, len);
			return 0;
		}

		{
			size_t nbytes = (cnt + 7) / 8;

			if (len > nbytes)
				len = nbytes;
			memcpy(arg, src, len);
		}
		return 0;
	}

	if (ev_type >= 0x40 && ev_type <= 0x5f) {
		/* EVIOCGABS(abs) */
		unsigned int abs = ev_type - 0x40;

		if (abs >= ABS_CNT)
			return -EINVAL;
		memcpy(arg, &dev->absinfo[abs], sizeof(input_absinfo_t));
		return 0;
	}

	if (ev_type >= 0xc0 && ev_type <= 0xdf) {
		/* EVIOCSABS(abs) */
		unsigned int abs = ev_type - 0xc0;

		if (abs >= ABS_CNT)
			return -EINVAL;
		memcpy(&dev->absinfo[abs], arg, sizeof(input_absinfo_t));
		return 0;
	}

	/* EVIOCGRAB, EVIOCREVOKE, EVIOCGMASK, EVIOCSMASK, EVIOCSCLOCKID */
	switch (request) {
	case EVIOCGRAB:
		memcpy(&ival, arg, sizeof(ival));
		if (ival)
			return evdev_grab(evdev, client);
		else
			return evdev_ungrab(evdev, client);

	case EVIOCREVOKE:
		memcpy(&ival, arg, sizeof(ival));
		if (ival) {
			client->revoked = true;
			wait_queue_wake_one(&client->wait);
		}
		return 0;

	case EVIOCSCLOCKID:
		memcpy(&ival, arg, sizeof(ival));
		evdev_set_clk_type(client, ival);
		return 0;

	case EVIOCGMASK: {
		input_mask_t mask;

		memcpy(&mask, arg, sizeof(mask));
		{
			size_t cnt = evdev_get_mask_cnt(mask.type);
			uint32_t *client_mask;

			if (!cnt)
				return -EINVAL;
			if (mask.codes_size > (cnt + 7) / 8)
				return -EINVAL;

			client_mask = client->evmasks[mask.type];
			if (!client_mask) {
				memset(arg, 0, sizeof(input_mask_t));
				return 0;
			}
			memcpy((void *)(uintptr_t)mask.codes_ptr,
			       client_mask, mask.codes_size);
		}
		return 0;
	}

	case EVIOCSMASK: {
		input_mask_t mask;

		memcpy(&mask, arg, sizeof(mask));
		{
			size_t cnt = evdev_get_mask_cnt(mask.type);
			size_t nbytes = (cnt + 7) / 8;
			uint32_t *client_mask;

			if (!cnt)
				return -EINVAL;

			if (mask.codes_size == 0) {
				/* Remove mask: allow all events */
				if (client->evmasks[mask.type]) {
					free(client->evmasks[mask.type]);
					client->evmasks[mask.type] = NULL;
				}
				__evdev_flush_queue(client, mask.type);
				return 0;
			}

			if (mask.codes_size != nbytes)
				return -EINVAL;

			client_mask = client->evmasks[mask.type];
			if (!client_mask) {
				client_mask = malloc(nbytes);
				if (!client_mask)
					return -ENOMEM;
				memset(client_mask, 0, nbytes);
				client->evmasks[mask.type] = client_mask;
			}

			memcpy(client_mask,
			       (void *)(uintptr_t)mask.codes_ptr,
			       nbytes);

			__evdev_flush_queue(client, mask.type);
		}
		return 0;
	}

	default:
		return -EINVAL;
	}
}