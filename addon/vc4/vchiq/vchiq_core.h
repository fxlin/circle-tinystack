/**
 * Copyright (c) 2010-2012 Broadcom. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2, as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef VCHIQ_CORE_H
#define VCHIQ_CORE_H

#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/kthread.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/printk.h>
#include <linux/kernel.h>

#include "vchiq_cfg.h"

#include "vchiq.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run time control of log level, based on KERN_XXX level. */
#define VCHIQ_LOG_DEFAULT  4
#define VCHIQ_LOG_ERROR    3
#define VCHIQ_LOG_WARNING  4
#define VCHIQ_LOG_INFO     6
#define VCHIQ_LOG_TRACE    7

#define VCHIQ_LOG_PREFIX   KERN_INFO "vchiq: "

#ifndef vchiq_log_error
#define vchiq_log_error(cat, fmt, ...) \
	do { if (cat >= VCHIQ_LOG_ERROR) \
		printk(VCHIQ_LOG_PREFIX fmt "\n", ##__VA_ARGS__); } while (0)
#endif
#ifndef vchiq_log_warning
#define vchiq_log_warning(cat, fmt, ...) \
	do { if (cat >= VCHIQ_LOG_WARNING) \
		 printk(VCHIQ_LOG_PREFIX fmt "\n", ##__VA_ARGS__); } while (0)
#endif
#ifndef vchiq_log_info
#define vchiq_log_info(cat, fmt, ...) \
	do { if (cat >= VCHIQ_LOG_INFO) \
		printk(VCHIQ_LOG_PREFIX fmt "\n", ##__VA_ARGS__); } while (0)
#endif
#ifndef vchiq_log_trace
#define vchiq_log_trace(cat, fmt, ...) \
	do { if (cat >= VCHIQ_LOG_TRACE) \
		printk(VCHIQ_LOG_PREFIX fmt "\n", ##__VA_ARGS__); } while (0)
#endif

#define vchiq_loud_error(...) \
	vchiq_log_error(vchiq_core_log_level, "===== " __VA_ARGS__)

#ifndef vchiq_static_assert
#define vchiq_static_assert(cond) __attribute__((unused)) \
	extern int vchiq_static_assert[(cond) ? 1 : -1]
#endif

#define IS_POW2(x) (x && ((x & (x - 1)) == 0))

/* Ensure that the slot size and maximum number of slots are powers of 2 */
vchiq_static_assert(IS_POW2(VCHIQ_SLOT_SIZE));
vchiq_static_assert(IS_POW2(VCHIQ_MAX_SLOTS));
vchiq_static_assert(IS_POW2(VCHIQ_MAX_SLOTS_PER_SIDE));

#define VCHIQ_SLOT_MASK        (VCHIQ_SLOT_SIZE - 1)
#define VCHIQ_SLOT_QUEUE_MASK  (VCHIQ_MAX_SLOTS_PER_SIDE - 1)
#define VCHIQ_SLOT_ZERO_SLOTS  ((sizeof(VCHIQ_SLOT_ZERO_T) + \
	VCHIQ_SLOT_SIZE - 1) / VCHIQ_SLOT_SIZE) // xzl: # of "meta" blocks which can > 1?

#define VCHIQ_MSG_PADDING            0  /* -                                 */
#define VCHIQ_MSG_CONNECT            1  /* -                                 */
#define VCHIQ_MSG_OPEN               2  /* + (srcport, -), fourcc, client_id */
#define VCHIQ_MSG_OPENACK            3  /* + (srcport, dstport)              */
#define VCHIQ_MSG_CLOSE              4  /* + (srcport, dstport)              */
#define VCHIQ_MSG_DATA               5  /* + (srcport, dstport)              */
#define VCHIQ_MSG_BULK_RX            6  /* + (srcport, dstport), data, size  */
#define VCHIQ_MSG_BULK_TX            7  /* + (srcport, dstport), data, size  */
#define VCHIQ_MSG_BULK_RX_DONE       8  /* + (srcport, dstport), actual      */
#define VCHIQ_MSG_BULK_TX_DONE       9  /* + (srcport, dstport), actual      */
#define VCHIQ_MSG_PAUSE             10  /* -                                 */
#define VCHIQ_MSG_RESUME            11  /* -                                 */
#define VCHIQ_MSG_REMOTE_USE        12  /* -                                 */
#define VCHIQ_MSG_REMOTE_RELEASE    13  /* -                                 */
#define VCHIQ_MSG_REMOTE_USE_ACTIVE 14  /* -                                 */

#define VCHIQ_PORT_MAX                 (VCHIQ_MAX_SERVICES - 1)
#define VCHIQ_PORT_FREE                0x1000
#define VCHIQ_PORT_IS_VALID(port)      (port < VCHIQ_PORT_FREE)
#define VCHIQ_MAKE_MSG(type, srcport, dstport) \
	((type<<24) | (srcport<<12) | (dstport<<0))
#define VCHIQ_MSG_TYPE(msgid)          ((unsigned int)msgid >> 24)
#define VCHIQ_MSG_SRCPORT(msgid) \
	(unsigned short)(((unsigned int)msgid >> 12) & 0xfff)
#define VCHIQ_MSG_DSTPORT(msgid) \
	((unsigned short)msgid & 0xfff)

#define VCHIQ_FOURCC_AS_4CHARS(fourcc)	\
	((fourcc) >> 24) & 0xff, \
	((fourcc) >> 16) & 0xff, \
	((fourcc) >>  8) & 0xff, \
	(fourcc) & 0xff

/* Ensure the fields are wide enough */
vchiq_static_assert(VCHIQ_MSG_SRCPORT(VCHIQ_MAKE_MSG(0, 0, VCHIQ_PORT_MAX))
	== 0);
vchiq_static_assert(VCHIQ_MSG_TYPE(VCHIQ_MAKE_MSG(0, VCHIQ_PORT_MAX, 0)) == 0);
vchiq_static_assert((unsigned int)VCHIQ_PORT_MAX <
	(unsigned int)VCHIQ_PORT_FREE);

#define VCHIQ_MSGID_PADDING            VCHIQ_MAKE_MSG(VCHIQ_MSG_PADDING, 0, 0)
#define VCHIQ_MSGID_CLAIMED            0x40000000

// xzl: what's fourcc?
#define VCHIQ_FOURCC_INVALID           0x00000000
#define VCHIQ_FOURCC_IS_LEGAL(fourcc)  (fourcc != VCHIQ_FOURCC_INVALID)

#define VCHIQ_BULK_ACTUAL_ABORTED -1

typedef uint32_t BITSET_T;

vchiq_static_assert((sizeof(BITSET_T) * 8) == 32);

#define BITSET_SIZE(b)        ((b + 31) >> 5)
#define BITSET_WORD(b)        (b >> 5)
#define BITSET_BIT(b)         (1 << (b & 31))
#define BITSET_ZERO(bs)       memset(bs, 0, sizeof(bs))
#define BITSET_IS_SET(bs, b)  (bs[BITSET_WORD(b)] & BITSET_BIT(b))
#define BITSET_SET(bs, b)     (bs[BITSET_WORD(b)] |= BITSET_BIT(b))
#define BITSET_CLR(bs, b)     (bs[BITSET_WORD(b)] &= ~BITSET_BIT(b))

#if VCHIQ_ENABLE_STATS
#define VCHIQ_STATS_INC(state, stat) (state->stats. stat++)
#define VCHIQ_SERVICE_STATS_INC(service, stat) (service->stats. stat++)
#define VCHIQ_SERVICE_STATS_ADD(service, stat, addend) \
	(service->stats. stat += addend)
#else
#define VCHIQ_STATS_INC(state, stat) ((void)0)
#define VCHIQ_SERVICE_STATS_INC(service, stat) ((void)0)
#define VCHIQ_SERVICE_STATS_ADD(service, stat, addend) ((void)0)
#endif

enum {
	DEBUG_ENTRIES,
#if VCHIQ_ENABLE_DEBUG
	DEBUG_SLOT_HANDLER_COUNT,
	DEBUG_SLOT_HANDLER_LINE,
	DEBUG_PARSE_LINE,
	DEBUG_PARSE_HEADER,
	DEBUG_PARSE_MSGID,
	DEBUG_AWAIT_COMPLETION_LINE,
	DEBUG_DEQUEUE_MESSAGE_LINE,
	DEBUG_SERVICE_CALLBACK_LINE,
	DEBUG_MSG_QUEUE_FULL_COUNT,
	DEBUG_COMPLETION_QUEUE_FULL_COUNT,
#endif
	DEBUG_MAX
};

#if VCHIQ_ENABLE_DEBUG

#define DEBUG_INITIALISE(local) int *debug_ptr = (local)->debug;
#define DEBUG_TRACE(d) \
	do { debug_ptr[DEBUG_ ## d] = __LINE__; dsb(); } while (0)
#define DEBUG_VALUE(d, v) \
	do { debug_ptr[DEBUG_ ## d] = (v); dsb(); } while (0)
#define DEBUG_COUNT(d) \
	do { debug_ptr[DEBUG_ ## d]++; dsb(); } while (0)

#else /* VCHIQ_ENABLE_DEBUG */

#define DEBUG_INITIALISE(local)
#define DEBUG_TRACE(d)
#define DEBUG_VALUE(d, v)
#define DEBUG_COUNT(d)

#endif /* VCHIQ_ENABLE_DEBUG */

typedef enum {
	VCHIQ_CONNSTATE_DISCONNECTED,
	VCHIQ_CONNSTATE_CONNECTING,
	VCHIQ_CONNSTATE_CONNECTED,
	VCHIQ_CONNSTATE_PAUSING,
	VCHIQ_CONNSTATE_PAUSE_SENT,
	VCHIQ_CONNSTATE_PAUSED,
	VCHIQ_CONNSTATE_RESUMING,
	VCHIQ_CONNSTATE_PAUSE_TIMEOUT,
	VCHIQ_CONNSTATE_RESUME_TIMEOUT
} VCHIQ_CONNSTATE_T;

enum {
	VCHIQ_SRVSTATE_FREE,
	VCHIQ_SRVSTATE_HIDDEN,
	VCHIQ_SRVSTATE_LISTENING,
	VCHIQ_SRVSTATE_OPENING,
	VCHIQ_SRVSTATE_OPEN,
	VCHIQ_SRVSTATE_OPENSYNC,
	VCHIQ_SRVSTATE_CLOSESENT,
	VCHIQ_SRVSTATE_CLOSERECVD,
	VCHIQ_SRVSTATE_CLOSEWAIT,
	VCHIQ_SRVSTATE_CLOSED
};

enum {
	VCHIQ_POLL_TERMINATE, // mutual exclusive from the below?
	VCHIQ_POLL_REMOVE, // xzl: the service must be polled for removal??
	VCHIQ_POLL_TXNOTIFY,
	VCHIQ_POLL_RXNOTIFY,
	VCHIQ_POLL_COUNT
};

typedef enum {
	VCHIQ_BULK_TRANSMIT,
	VCHIQ_BULK_RECEIVE
} VCHIQ_BULK_DIR_T;

typedef void (*VCHIQ_USERDATA_TERM_T)(void *userdata);

typedef struct vchiq_bulk_struct {
	short mode;
	short dir;
	void *userdata;
	VCHI_MEM_HANDLE_T handle;
	void *data;
	int size;
	void *remote_data;
	int remote_size;
	int actual;
} VCHIQ_BULK_T; // xzl: what's this? bulk transfer?

typedef struct vchiq_bulk_queue_struct {
	int local_insert;  /* Where to insert the next local bulk */
	int remote_insert; /* Where to insert the next remote bulk (master) */
	int process;       /* Bulk to transfer next */
	int remote_notify; /* Bulk to notify the remote client of next (mstr) */
	int remove;        /* Bulk to notify the local client of, and remove,
			   ** next */
	VCHIQ_BULK_T bulks[VCHIQ_NUM_SERVICE_BULKS];
} VCHIQ_BULK_QUEUE_T; // xzl: for xfer bulks? not slots?

typedef struct remote_event_struct {
	int armed;
	int fired;
#if AARCH == 32
	struct semaphore *event;
#else
	unsigned event;		/* Must be 32 bit */
#endif
} REMOTE_EVENT_T;

typedef struct opaque_platform_state_t *VCHIQ_PLATFORM_STATE_T;

typedef struct vchiq_state_struct VCHIQ_STATE_T;

typedef struct vchiq_slot_struct {
	char data[VCHIQ_SLOT_SIZE];
} VCHIQ_SLOT_T;

typedef struct vchiq_slot_info_struct {
	/* Use two counters rather than one to avoid the need for a mutex. */
	short use_count;
	short release_count;
} VCHIQ_SLOT_INFO_T;

typedef struct vchiq_service_struct {
	VCHIQ_SERVICE_BASE_T base;
	VCHIQ_SERVICE_HANDLE_T handle;
	unsigned int ref_count;
	int srvstate;
	VCHIQ_USERDATA_TERM_T userdata_term;
	unsigned int localport;
	unsigned int remoteport;
	int public_fourcc; // xzl:??
	int client_id;
	char auto_close;
	char sync;
	char closing;
	char trace;
	atomic_t poll_flags; // xzl:actions of the service that need polling (?)
	short version;
	short version_min;
	short peer_version;

	VCHIQ_STATE_T *state;
	VCHIQ_INSTANCE_T instance;

	int service_use_count;

	VCHIQ_BULK_QUEUE_T bulk_tx;
	VCHIQ_BULK_QUEUE_T bulk_rx;

	struct semaphore remove_event;
	struct semaphore bulk_remove_event;
	struct mutex bulk_mutex;

	struct service_stats_struct {
		int quota_stalls;
		int slot_stalls;
		int bulk_stalls;
		int error_count;
		int ctrl_tx_count;
		int ctrl_rx_count;
		int bulk_tx_count;
		int bulk_rx_count;
		int bulk_aborted_count;
		uint64_t ctrl_tx_bytes;
		uint64_t ctrl_rx_bytes;
		uint64_t bulk_tx_bytes;
		uint64_t bulk_rx_bytes;
	} stats;
} VCHIQ_SERVICE_T;

/* The quota information is outside VCHIQ_SERVICE_T so that it can be
	statically allocated, since for accounting reasons a service's slot
	usage is carried over between users of the same port number.
 */
typedef struct vchiq_service_quota_struct {
	unsigned short slot_quota;
	unsigned short slot_use_count;
	unsigned short message_quota;
	unsigned short message_use_count;
	struct semaphore quota_event;
	int previous_tx_index;
} VCHIQ_SERVICE_QUOTA_T;

typedef struct vchiq_shared_state_struct {

	/* A non-zero value here indicates that the content is valid. */
	int initialised;

	/* The first and last (inclusive) slots allocated to the owner. */
	int slot_first;
	int slot_last;

	/* The slot allocated to synchronous messages from the owner. */
	int slot_sync;

	/* Signalling this event indicates that owner's slot handler thread
	** should run. */
	REMOTE_EVENT_T trigger;

	/* Indicates the byte position within the stream where the next message
	** will be written. The least significant bits are an index into the
	** slot. The next bits are the index of the slot in slot_queue. */
	int tx_pos;

	/* This event should be signalled when a slot is recycled. */
	REMOTE_EVENT_T recycle;

	/* The slot_queue index where the next recycled slot will be written. */
	int slot_queue_recycle;

	/* This event should be signalled when a synchronous message is sent. */
	REMOTE_EVENT_T sync_trigger;

	/* This event should be signalled when a synchronous message has been
	** released. */
	REMOTE_EVENT_T sync_release;

	/* A circular buffer of slot indexes. */
	int slot_queue[VCHIQ_MAX_SLOTS_PER_SIDE];
	// xzl: the driver maintains a slot queue which contains indexes of slots?

	/* Debugging state */
	int debug[DEBUG_MAX];
} VCHIQ_SHARED_STATE_T;

// xzl: the "meta" slot?
typedef struct vchiq_slot_zero_struct {
	int magic;
	short version;
	short version_min;
	int slot_zero_size;
	int slot_size;
	int max_slots;
	int max_slots_per_side;
	int platform_data[2];
	VCHIQ_SHARED_STATE_T master; // xzl: gpu?
	VCHIQ_SHARED_STATE_T slave; // xzl: cpu?
	VCHIQ_SLOT_INFO_T slots[VCHIQ_MAX_SLOTS];
} VCHIQ_SLOT_ZERO_T;

struct vchiq_state_struct {
	int id;
	int initialised;
	VCHIQ_CONNSTATE_T conn_state;
	int is_master;
	short version_common;

	VCHIQ_SHARED_STATE_T *local;
	VCHIQ_SHARED_STATE_T *remote;
	VCHIQ_SLOT_T *slot_data;

	unsigned short default_slot_quota;
	unsigned short default_message_quota;

	/* Event indicating connect message received */
	struct semaphore connect;

	/* Mutex protecting services */
	struct mutex mutex;
	VCHIQ_INSTANCE_T *instance;

	/* Processes incoming messages */
	struct task_struct *slot_handler_thread;

	/* Processes recycled slots */
	struct task_struct *recycle_thread;

	/* Processes synchronous messages */
	struct task_struct *sync_thread;

	/* Local implementation of the trigger remote event */
	struct semaphore trigger_event;

	/* Local implementation of the recycle remote event */
	struct semaphore recycle_event;

	/* Local implementation of the sync trigger remote event */
	struct semaphore sync_trigger_event;

	/* Local implementation of the sync release remote event */
	struct semaphore sync_release_event;

	char *tx_data;
	char *rx_data;
	VCHIQ_SLOT_INFO_T *rx_info;

	struct mutex slot_mutex;

	struct mutex recycle_mutex;

	struct mutex sync_mutex;

	struct mutex bulk_transfer_mutex;

	/* Indicates the byte position within the stream from where the next
	** message will be read. The least significant bits are an index into
	** the slot.The next bits are the index of the slot in
	** remote->slot_queue. */
	int rx_pos;

	/* A cached copy of local->tx_pos. Only write to local->tx_pos, and read
		from remote->tx_pos. */
	int local_tx_pos;

	/* The slot_queue index of the slot to become available next. */
	int slot_queue_available;

	/* A flag to indicate if any poll has been requested */
	int poll_needed; // xzl: what does this mean?

	/* Ths index of the previous slot used for data messages. */
	int previous_data_index;

	/* The number of slots occupied by data messages. */
	unsigned short data_use_count;

	/* The maximum number of slots to be occupied by data messages. */
	unsigned short data_quota;

	/* An array of bit sets indicating which services must be polled. */
	atomic_t poll_services[BITSET_SIZE(VCHIQ_MAX_SERVICES)];

	/* The number of the first unused service */
	int unused_service;

	/* Signalled when a free slot becomes available. */
	struct semaphore slot_available_event;

	struct semaphore slot_remove_event;

	/* Signalled when a free data slot becomes available. */
	struct semaphore data_quota_event;

	/* Incremented when there are bulk transfers which cannot be processed
	 * whilst paused and must be processed on resume */
	int deferred_bulks;

	struct state_stats_struct {
		int slot_stalls;
		int data_stalls;
		int ctrl_tx_count;
		int ctrl_rx_count;
		int error_count;
	} stats;

	VCHIQ_SERVICE_T * services[VCHIQ_MAX_SERVICES];
	VCHIQ_SERVICE_QUOTA_T service_quotas[VCHIQ_MAX_SERVICES];
	VCHIQ_SLOT_INFO_T slot_info[VCHIQ_MAX_SLOTS];

	VCHIQ_PLATFORM_STATE_T platform_state;
};

struct bulk_waiter {
	VCHIQ_BULK_T *bulk;
	struct semaphore event;
	int actual;
};

extern spinlock_t bulk_waiter_spinlock;

extern int vchiq_core_log_level;
extern int vchiq_core_msg_log_level;
extern int vchiq_sync_log_level;

extern VCHIQ_STATE_T *vchiq_states[VCHIQ_MAX_STATES];

extern const char *
get_conn_state_name(VCHIQ_CONNSTATE_T conn_state);

extern VCHIQ_SLOT_ZERO_T *
vchiq_init_slots(void *mem_base, int mem_size);

extern VCHIQ_STATUS_T
vchiq_init_state(VCHIQ_STATE_T *state, VCHIQ_SLOT_ZERO_T *slot_zero,
	int is_master);

extern VCHIQ_STATUS_T
vchiq_connect_internal(VCHIQ_STATE_T *state, VCHIQ_INSTANCE_T instance);

extern VCHIQ_SERVICE_T *
vchiq_add_service_internal(VCHIQ_STATE_T *state,
	const VCHIQ_SERVICE_PARAMS_T *params, int srvstate,
	VCHIQ_INSTANCE_T instance, VCHIQ_USERDATA_TERM_T userdata_term);

extern VCHIQ_STATUS_T
vchiq_open_service_internal(VCHIQ_SERVICE_T *service, int client_id);

extern VCHIQ_STATUS_T
vchiq_close_service_internal(VCHIQ_SERVICE_T *service, int close_recvd);

extern void
vchiq_terminate_service_internal(VCHIQ_SERVICE_T *service);

extern void
vchiq_free_service_internal(VCHIQ_SERVICE_T *service);

extern VCHIQ_STATUS_T
vchiq_shutdown_internal(VCHIQ_STATE_T *state, VCHIQ_INSTANCE_T instance);

extern VCHIQ_STATUS_T
vchiq_pause_internal(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_resume_internal(VCHIQ_STATE_T *state);

extern void
remote_event_pollall(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_bulk_transfer(VCHIQ_SERVICE_HANDLE_T handle,
	VCHI_MEM_HANDLE_T memhandle, void *offset, int size, void *userdata,
	VCHIQ_BULK_MODE_T mode, VCHIQ_BULK_DIR_T dir);

extern void
vchiq_dump_state(void *dump_context, VCHIQ_STATE_T *state);

extern void
vchiq_dump_service_state(void *dump_context, VCHIQ_SERVICE_T *service);

extern void
vchiq_loud_error_header(void);

extern void
vchiq_loud_error_footer(void);

extern void
request_poll(VCHIQ_STATE_T *state, VCHIQ_SERVICE_T *service, int poll_type);

static inline VCHIQ_SERVICE_T *
handle_to_service(VCHIQ_SERVICE_HANDLE_T handle)
{
	VCHIQ_STATE_T *state = vchiq_states[(handle / VCHIQ_MAX_SERVICES) &
		(VCHIQ_MAX_STATES - 1)];
	if (!state)
		return NULL;

	return state->services[handle & (VCHIQ_MAX_SERVICES - 1)];
}

extern VCHIQ_SERVICE_T *
find_service_by_handle(VCHIQ_SERVICE_HANDLE_T handle);

extern VCHIQ_SERVICE_T *
find_service_by_port(VCHIQ_STATE_T *state, int localport);

extern VCHIQ_SERVICE_T *
find_service_for_instance(VCHIQ_INSTANCE_T instance,
	VCHIQ_SERVICE_HANDLE_T handle);

extern VCHIQ_SERVICE_T *
find_closed_service_for_instance(VCHIQ_INSTANCE_T instance,
	VCHIQ_SERVICE_HANDLE_T handle);

extern VCHIQ_SERVICE_T *
next_service_by_instance(VCHIQ_STATE_T *state, VCHIQ_INSTANCE_T instance,
	int *pidx);

extern void
lock_service(VCHIQ_SERVICE_T *service);

extern void
unlock_service(VCHIQ_SERVICE_T *service);

/* The following functions are called from vchiq_core, and external
** implementations must be provided. */

extern VCHIQ_STATUS_T
vchiq_prepare_bulk_data(VCHIQ_BULK_T *bulk,
	VCHI_MEM_HANDLE_T memhandle, void *offset, int size, int dir);

extern void
vchiq_transfer_bulk(VCHIQ_BULK_T *bulk);

extern void
vchiq_complete_bulk(VCHIQ_BULK_T *bulk);

extern int
vchiq_copy_from_user(void *dst, const void *src, int size);

extern void
remote_event_signal(REMOTE_EVENT_T *event);

void
vchiq_platform_check_suspend(VCHIQ_STATE_T *state);

extern void
vchiq_platform_paused(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_platform_resume(VCHIQ_STATE_T *state);

extern void
vchiq_platform_resumed(VCHIQ_STATE_T *state);

extern void
vchiq_dump(void *dump_context, const char *str, int len);

extern void
vchiq_dump_platform_state(void *dump_context);

extern void
vchiq_dump_platform_instances(void *dump_context);

extern void
vchiq_dump_platform_service_state(void *dump_context,
	VCHIQ_SERVICE_T *service);

extern VCHIQ_STATUS_T
vchiq_use_service_internal(VCHIQ_SERVICE_T *service);

extern VCHIQ_STATUS_T
vchiq_release_service_internal(VCHIQ_SERVICE_T *service);

extern void
vchiq_on_remote_use(VCHIQ_STATE_T *state);

extern void
vchiq_on_remote_release(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_platform_init_state(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_check_service(VCHIQ_SERVICE_T *service);

extern void
vchiq_on_remote_use_active(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_send_remote_use(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_send_remote_release(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_send_remote_use_active(VCHIQ_STATE_T *state);

extern void
vchiq_platform_conn_state_changed(VCHIQ_STATE_T *state,
	VCHIQ_CONNSTATE_T oldstate, VCHIQ_CONNSTATE_T newstate);

extern void
vchiq_platform_handle_timeout(VCHIQ_STATE_T *state);

extern void
vchiq_set_conn_state(VCHIQ_STATE_T *state, VCHIQ_CONNSTATE_T newstate);


extern void
vchiq_log_dump_mem(const char *label, uint32_t addr, const void *voidMem,
	size_t numBytes);

#ifdef __cplusplus
}
#endif

#endif
