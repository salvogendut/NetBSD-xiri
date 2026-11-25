/* Xiri's bounded IRIX share-process state. */
#ifndef _COMPAT_IRIX_SPROC_H_
#define _COMPAT_IRIX_SPROC_H_

#define IRIX_PRDA_ADDRESS 0x00200000U
#define IRIX_PRDA_SYS_OFFSET 0xe00U
#define IRIX_PRDA_FLAGS (IRIX_PRDA_ADDRESS + 0xe6cU)
#define IRIX_PRDA_HOLD (IRIX_PRDA_ADDRESS + 0xe70U)
#define IRIX_LOCAL_MAX 32

struct irix_range { vaddr_t start, end; };
struct irix_localset {
	unsigned count;
	struct irix_range range[IRIX_LOCAL_MAX];
};
struct irix_share;
struct irix_spawn;
struct irix_process {
	struct { uint32_t flags, mask, trampoline; } signal[32];
	bool threads, reset_hold;
	vaddr_t prda_window;
	vaddr_t heap_floor;	/* break at first pthread initialization */
	krwlock_t maplock;	/* live with the pthread PRDA window */
	struct irix_worker *active_worker;
	struct irix_usync_state *usync;	/* private to this process image */
	uint32_t thread_serial;
	struct irix_localset local;
	struct irix_share *share;
	struct irix_spawn *spawning;
	struct proc *proc;
	struct vmspace *vm;
	struct irix_process *unblock;
	bool blocked;
	uint32_t sharemask, entry, argument;
	vaddr_t stack_base, stack_top;
	vsize_t stack_size;
};
struct irix_nsproc_args {
	syscallarg(uint32_t) entry;
	syscallarg(uint32_t) flags;
	syscallarg(uint32_t) desc;
	syscallarg(uint32_t) sched;
};
struct irix_usync_args {
	syscallarg(uint32_t) operation;
	syscallarg(uint32_t) request;
};
int irix_usync_cntl(struct lwp *, const void *, register_t *);
void irix_usync_release(struct irix_process *);
int irix_nsproc(struct lwp *, const void *, register_t *);
int irix_thread_ctl(struct lwp *, uint32_t, uint32_t, register_t *);
void irix_thread_exit(struct lwp *);
void irix_threads_release(struct proc *);
struct irix_sproc_args {
	syscallarg(uint32_t) entry;
	syscallarg(uint32_t) flags;
	syscallarg(uint32_t) arg;
};
struct irix_sprocsp_args {
	syscallarg(uint32_t) entry;
	syscallarg(uint32_t) flags;
	syscallarg(uint32_t) arg;
	syscallarg(uint32_t) sp;
	syscallarg(uint32_t) len;
};
struct irix_procblk_args {
	syscallarg(int32_t) command;
	syscallarg(int32_t) pid;
	syscallarg(int32_t) count;
};

void irix_o32_child_return(void *);
int irix_sproc(struct lwp *, const void *, register_t *);
int irix_sprocsp(struct lwp *, const void *, register_t *);
int irix_procblk(struct lwp *, const void *, register_t *);
int irix_share_prctl(struct lwp *, uint32_t, uint32_t, register_t *);
void irix_share_fork(struct proc *, struct lwp *);
void irix_share_leave(struct proc *);
void irix_share_child(struct lwp *);
bool irix_share_single(struct proc *);
int irix_local_plan(const struct irix_localset *, vaddr_t, vaddr_t, bool,
    struct irix_localset *);
int irix_share_mmap(struct lwp *, struct sys_mmap_args *, register_t *);

#endif
