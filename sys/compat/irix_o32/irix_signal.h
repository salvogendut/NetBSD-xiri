/* Xiri's explicitly serialized, traditional IRIX signal subset. */
#ifndef _COMPAT_IRIX_SIGNAL_H_
#define _COMPAT_IRIX_SIGNAL_H_
struct irix_sigaction_args {
	syscallarg(int32_t) signo;
	syscallarg(uint32_t) action;
	syscallarg(uint32_t) old;
	syscallarg(uint32_t) trampoline;
};
struct irix_sigpoll_args {
	syscallarg(uint32_t) set;
	syscallarg(uint32_t) info;
	syscallarg(uint32_t) timeout;
};
struct irix_sigreturn_args {
	syscallarg(uint32_t) sc;
	syscallarg(uint32_t) uc;
	syscallarg(uint32_t) signo;
};
struct irix_kill_args {
	syscallarg(int32_t) pid;
	syscallarg(int32_t) signo;
};
/* SGI libpthread uses this private directed signal for its event worker.
 * Reserve one native realtime signal for the foreign personality. */
#define IRIX_SIGPTINTR 47
#define IRIX_NATIVE_PTINTR SIGRTMIN
#define IRIX_PTINTR_BIT (1U << ((IRIX_SIGPTINTR - 1) % 32))
extern const uint8_t irix_signal_map[32];
void irix_mask_export(const sigset_t *, uint32_t *, uint32_t *);
void irix_mask_import(uint32_t, uint32_t, sigset_t *);
int irix_prda_maskout(struct lwp *);
int irix_sigaction(struct lwp *, const void *, register_t *);
int irix_sigpoll(struct lwp *, const void *, register_t *);
int irix_sigreturn(struct lwp *, const void *, register_t *);
int irix_kill(struct lwp *, const void *, register_t *);
void irix_sendsig(const ksiginfo_t *, const sigset_t *);
#endif
