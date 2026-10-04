#ifndef SAMARA_IO_URING_H
#define SAMARA_IO_URING_H
#include "core/types.h"

/* linux/io_uring.h uapi, the part we speak. same layout as linux so liburing works */

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t  __s32;
typedef int64_t  __s64;

struct io_uring_sqe {
    __u8  opcode;
    __u8  flags;
    __u16 ioprio;
    __s32 fd;
    union { __u64 off; __u64 addr2; };
    union { __u64 addr; __u64 splice_off_in; };
    __u32 len;
    union {
        __u32 rw_flags, fsync_flags, msg_flags, timeout_flags, accept_flags, cancel_flags;
        __u32 open_flags, statx_flags, poll32_events, fadvise_advice, splice_flags;
        __u16 poll_events;
    };
    __u64 user_data;
    union { __u16 buf_index; __u16 buf_group; } __attribute__((packed));
    __u16 personality;
    union { __s32 splice_fd_in; __u32 file_index; };
    __u64 addr3;
    __u64 pad2;
};

struct io_uring_cqe {
    __u64 user_data;
    __s32 res;
    __u32 flags;
};

#define IOSQE_FIXED_FILE        1
#define IOSQE_IO_DRAIN          2
#define IOSQE_IO_LINK           4
#define IOSQE_IO_HARDLINK       8
#define IOSQE_ASYNC             16
#define IOSQE_BUFFER_SELECT     32
#define IOSQE_CQE_SKIP_SUCCESS  64

#define IORING_SETUP_IOPOLL     (1U << 0)
#define IORING_SETUP_SQPOLL     (1U << 1)
#define IORING_SETUP_SQ_AFF     (1U << 2)
#define IORING_SETUP_CQSIZE     (1U << 3)
#define IORING_SETUP_CLAMP      (1U << 4)
#define IORING_SETUP_ATTACH_WQ  (1U << 5)
#define IORING_SETUP_R_DISABLED (1U << 6)
#define IORING_SETUP_SUBMIT_ALL (1U << 7)
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#define IORING_SETUP_TASKRUN_FLAG (1U << 9)
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)

enum {
    IORING_OP_NOP, IORING_OP_READV, IORING_OP_WRITEV, IORING_OP_FSYNC,
    IORING_OP_READ_FIXED, IORING_OP_WRITE_FIXED, IORING_OP_POLL_ADD, IORING_OP_POLL_REMOVE,
    IORING_OP_SYNC_FILE_RANGE, IORING_OP_SENDMSG, IORING_OP_RECVMSG, IORING_OP_TIMEOUT,
    IORING_OP_TIMEOUT_REMOVE, IORING_OP_ACCEPT, IORING_OP_ASYNC_CANCEL, IORING_OP_LINK_TIMEOUT,
    IORING_OP_CONNECT, IORING_OP_FALLOCATE, IORING_OP_OPENAT, IORING_OP_CLOSE,
    IORING_OP_FILES_UPDATE, IORING_OP_STATX, IORING_OP_READ, IORING_OP_WRITE,
    IORING_OP_FADVISE, IORING_OP_MADVISE, IORING_OP_SEND, IORING_OP_RECV,
    IORING_OP_OPENAT2, IORING_OP_EPOLL_CTL, IORING_OP_SPLICE, IORING_OP_PROVIDE_BUFFERS,
    IORING_OP_REMOVE_BUFFERS, IORING_OP_TEE, IORING_OP_SHUTDOWN, IORING_OP_RENAMEAT,
    IORING_OP_UNLINKAT, IORING_OP_MKDIRAT, IORING_OP_SYMLINKAT, IORING_OP_LINKAT,
    IORING_OP_MSG_RING, IORING_OP_FSETXATTR, IORING_OP_SETXATTR, IORING_OP_FGETXATTR,
    IORING_OP_GETXATTR, IORING_OP_SOCKET,
    IORING_OP_LAST = 64,        /* real linux has fewer, the probe cuts it at the last one we know */
};

#define IORING_FSYNC_DATASYNC   1

#define IORING_TIMEOUT_ABS      (1U << 0)
#define IORING_TIMEOUT_UPDATE   (1U << 1)
#define IORING_TIMEOUT_BOOTTIME (1U << 2)
#define IORING_TIMEOUT_REALTIME (1U << 3)
#define IORING_LINK_TIMEOUT_UPDATE (1U << 4)
#define IORING_TIMEOUT_ETIME_SUCCESS (1U << 5)
#define IORING_TIMEOUT_MULTISHOT (1U << 6)

#define IORING_POLL_ADD_MULTI   (1U << 0)
#define IORING_POLL_UPDATE_EVENTS (1U << 1)
#define IORING_POLL_UPDATE_USER_DATA (1U << 2)

#define IORING_ASYNC_CANCEL_ALL (1U << 0)
#define IORING_ASYNC_CANCEL_FD  (1U << 1)
#define IORING_ASYNC_CANCEL_ANY (1U << 2)
#define IORING_ASYNC_CANCEL_FD_FIXED (1U << 3)

#define IORING_CQE_F_BUFFER     (1U << 0)
#define IORING_CQE_F_MORE       (1U << 1)

#define IORING_OFF_SQ_RING      0ULL
#define IORING_OFF_CQ_RING      0x8000000ULL
#define IORING_OFF_SQES         0x10000000ULL

struct io_sqring_offsets {
    __u32 head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
    __u64 user_addr;
};

#define IORING_SQ_NEED_WAKEUP   (1U << 0)
#define IORING_SQ_CQ_OVERFLOW   (1U << 1)

struct io_cqring_offsets {
    __u32 head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
    __u64 user_addr;
};

#define IORING_CQ_EVENTFD_DISABLED (1U << 0)

#define IORING_ENTER_GETEVENTS  (1U << 0)
#define IORING_ENTER_SQ_WAKEUP  (1U << 1)
#define IORING_ENTER_SQ_WAIT    (1U << 2)
#define IORING_ENTER_EXT_ARG    (1U << 3)
#define IORING_ENTER_REGISTERED_RING (1U << 4)

struct io_uring_params {
    __u32 sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle, features, wq_fd;
    __u32 resv[3];
    struct io_sqring_offsets sq_off;
    struct io_cqring_offsets cq_off;
};

#define IORING_FEAT_SINGLE_MMAP     (1U << 0)
#define IORING_FEAT_NODROP          (1U << 1)
#define IORING_FEAT_SUBMIT_STABLE   (1U << 2)
#define IORING_FEAT_RW_CUR_POS      (1U << 3)
#define IORING_FEAT_CUR_PERSONALITY (1U << 4)
#define IORING_FEAT_FAST_POLL       (1U << 5)
#define IORING_FEAT_POLL_32BITS     (1U << 6)
#define IORING_FEAT_SQPOLL_NONFIXED (1U << 7)
#define IORING_FEAT_EXT_ARG         (1U << 8)
#define IORING_FEAT_NATIVE_WORKERS  (1U << 9)
#define IORING_FEAT_CQE_SKIP        (1U << 11)

enum {
    IORING_REGISTER_BUFFERS = 0, IORING_UNREGISTER_BUFFERS, IORING_REGISTER_FILES,
    IORING_UNREGISTER_FILES, IORING_REGISTER_EVENTFD, IORING_UNREGISTER_EVENTFD,
    IORING_REGISTER_FILES_UPDATE, IORING_REGISTER_EVENTFD_ASYNC, IORING_REGISTER_PROBE,
    IORING_REGISTER_PERSONALITY, IORING_UNREGISTER_PERSONALITY, IORING_REGISTER_RESTRICTIONS,
    IORING_REGISTER_ENABLE_RINGS, IORING_REGISTER_FILES2, IORING_REGISTER_FILES_UPDATE2,
    IORING_REGISTER_BUFFERS2, IORING_REGISTER_BUFFERS_UPDATE,
    IORING_REGISTER_IOWQ_MAX_WORKERS = 19,
};

struct io_uring_getevents_arg {
    __u64 sigmask;
    __u32 sigmask_sz;
    __u32 min_wait_usec;
    __u64 ts;
};

struct io_uring_probe_op { __u8 op, resv; __u16 flags; __u32 resv2; };
struct io_uring_probe {
    __u8 last_op, ops_len;
    __u16 resv;
    __u32 resv2[3];
    struct io_uring_probe_op ops[];
};
#define IO_URING_OP_SUPPORTED   1

struct io_uring_files_update { __u32 offset, resv; __u64 fds; };
struct io_uring_rsrc_register { __u32 nr, flags; __u64 resv2, data, tags; };
struct io_uring_rsrc_update { __u32 offset, resv; __u64 data; };
struct io_uring_rsrc_update2 { __u32 offset, resv; __u64 data, tags; __u32 nr, resv2; };

struct kts { __s64 sec, nsec; };    /* __kernel_timespec */

#endif
