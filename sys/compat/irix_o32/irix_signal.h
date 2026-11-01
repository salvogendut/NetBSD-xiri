/* Xiri's explicitly serialized, traditional IRIX signal subset. */
#ifndef _COMPAT_IRIX_SIGNAL_H_
#define _COMPAT_IRIX_SIGNAL_H_
struct irix_sigaction_args {
	syscallarg(int32_t) signo;
	syscallarg(uint32_t) action;
	syscallarg(uint32_t) old;
	syscallarg(uint32_t) trampoline;
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
extern const uint8_t irix_signal_map[32];
int irix_prda_maskout(struct lwp *);
int irix_sigaction(struct lwp *, const void *, register_t *);
int irix_sigreturn(struct lwp *, const void *, register_t *);
int irix_kill(struct lwp *, const void *, register_t *);
void irix_sendsig(const ksiginfo_t *, const sigset_t *);
#endif
