/*
 * Xiri IRIX signal adapter. Wire layouts are measured by the independent
 * irix-signal lab on IRIX 6.5.22m. Native signal and machine-context routines
 * own disposition changes, pending signals and FPU ownership.
 */
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/pcu.h>
#include <sys/signalvar.h>
#include <sys/syscallargs.h>
#include <sys/systm.h>
#include <sys/ucontext.h>
#include <mips/locore.h>
#include <mips/frame.h>
#include <mips/regnum.h>
#include <compat/irix_o32/irix_signal.h>
#include <compat/irix_o32/irix_sproc.h>

static bool
n32_process(const struct proc *p)
{
#ifdef _LP64
	return p->p_md.md_abi == _MIPS_BSD_API_N32;
#else
	return false;
#endif
}

static void *
ptr32(uint32_t address)
{
	return (void *)(intptr_t)(int32_t)address;
}

static uint32_t
get32(const uint8_t *p, unsigned offset)
{
	uint32_t value;
	memcpy(&value, p + offset, sizeof(value));
	return value;
}

static uint64_t
get64(const uint8_t *p, unsigned offset)
{
	uint64_t value;
	memcpy(&value, p + offset, sizeof(value));
	return value;
}

static void
put32(uint8_t *p, unsigned offset, uint32_t value)
{
	memcpy(p + offset, &value, sizeof(value));
}

static void
put64(uint8_t *p, unsigned offset, uint64_t value)
{
	memcpy(p + offset, &value, sizeof(value));
}

static unsigned
foreign_signal(unsigned native)
{
	for (unsigned i = 1; i < __arraycount(irix_signal_map); i++)
		if (irix_signal_map[i] == native)
			return i;
	return 0;
}

static uint32_t
mask_to_word(const sigset_t *set)
{
	uint32_t word = 0;
	for (unsigned i = 1; i < __arraycount(irix_signal_map); i++)
		if (sigismember(set, irix_signal_map[i]))
			word |= 1U << (i - 1);
	return word;
}

static void
word_to_mask(uint32_t word, sigset_t *set)
{
	for (unsigned i = 1; i < __arraycount(irix_signal_map); i++) {
		if (word & (1U << (i - 1)))
			sigaddset(set, irix_signal_map[i]);
		else
			sigdelset(set, irix_signal_map[i]);
	}
}

int
irix_kill(struct lwp *l, const void *args, register_t *rval)
{
	const struct irix_kill_args *uap = args;
	struct sys_kill_args native;
	int sig = SCARG(uap, signo);
	if (sig < 0 || sig >= 32)
		return EINVAL;
	SCARG(&native, pid) = SCARG(uap, pid);
	SCARG(&native, signum) = irix_signal_map[sig];
	return sys_kill(l, &native, rval);
}

int
irix_sigaction(struct lwp *l, const void *args, register_t *rval)
{
	const struct irix_sigaction_args *uap = args;
	struct proc *p = l->l_proc;
	struct irix_process *ip = p->p_emuldata;
	struct sigaction native, previous;
	uint32_t in[8], out[8] = { 0 };
	int sig = SCARG(uap, signo), error;
	bool change = SCARG(uap, action) != 0;

	if (change && (error = copyin(ptr32(SCARG(uap, action)), in,
	    sizeof(in))) != 0)
		return error;
	if (sig <= 0 || sig > 64 || (change && (sig == 9 || sig == 23)))
		return EINVAL;
	/* Checkpoint/realtime signals and masks have no mapping in this subset. */
	if (sig >= 32)
		return EOPNOTSUPP;
	if (change && ((in[2] & 0x80000000U) || in[3] || in[4] || in[5] ||
	    (sig == 18 && (in[0] & 0x30000))))
		return EOPNOTSUPP;
	/* Per-process foreign metadata is serialized by the current one-LWP
	 * restriction. Native sigaction1 owns the actual disposition and locks. */
	if (p->p_nlwps != 1)
		return EOPNOTSUPP;
	error = sigaction1(l, irix_signal_map[sig], NULL, &previous, NULL, 0);
	if (error != 0)
		return error;
	out[0] = ip->signal[sig].flags;
	out[1] = (uintptr_t)previous.sa_handler;
	out[2] = ip->signal[sig].mask;
	/* Preserve IRIX's copyout-before-install ordering, including partial
	 * copyout on a boundary fault. No user pointer is retained in the kernel. */
	if (SCARG(uap, old) != 0) {
		/* The o32 bulk copyout may fault after an unrolled 16-byte
		 * block. Serialize bytes to retain the observed 4/28-byte
		 * prefixes at a page boundary on both kernel architectures. */
		for (size_t i = 0; i < sizeof(out); i++) {
			uint32_t address = SCARG(uap, old);
			if (address >= 0x80000000U || i >= 0x80000000U - address)
				return EFAULT;
			error = copyout((const uint8_t *)out + i,
			    ptr32(address + i), 1);
			if (error != 0)
				return error;
		}
	}
	if (change) {
		memset(&native, 0, sizeof(native));
		native.sa_handler = (sig_t)ptr32(in[1]);
		if (in[0] & 1) native.sa_flags |= SA_ONSTACK;
		if (in[0] & 2) native.sa_flags |= SA_RESETHAND;
		if (in[0] & 4) native.sa_flags |= SA_RESTART;
		if (in[0] & 8) native.sa_flags |= SA_SIGINFO;
		if (in[0] & 16) native.sa_flags |= SA_NODEFER;
		sigemptyset(&native.sa_mask);
		word_to_mask(in[2], &native.sa_mask);
		/* Native validation requires a nonnull trampoline for version 2.
		 * Our sendsig uses the separately stored foreign value, including
		 * null/bad addresses accepted at registration by the reference. */
		error = sigaction1(l, irix_signal_map[sig], &native, NULL,
		    ptr32(1), 2);
		if (error != 0)
			return error;
		ip->signal[sig].flags = in[0] & 0x1f;
		ip->signal[sig].mask = in[2] & 0x7fbffeff;
		ip->signal[sig].trampoline = SCARG(uap, trampoline);
	}
	*rval = 0;
	return 0;
}

/* Both native and IRIX o32 arrays index individual 32-bit FPRs: even
 * is the low half of a double, odd is the high half. Legacy slots widen
 * each half to 64 bits; n32 slots contain complete 64-bit registers. */
static void
save_fp(uint8_t *frame, bool legacy, bool n32, const mcontext_t *mc)
{
	unsigned base = legacy ? 272 : n32 ? 336 : 184;
	const uint8_t *fp = (const void *)&mc->__fpregs;
	for (unsigned i = 0; i < 32; i++) {
		if (n32)
			put64(frame, base + i * 8, get64(fp, i * 8));
		else if (legacy)
			put64(frame, base + i * 8, get32(fp, i * 4));
		else
			put32(frame, base + i * 4, get32(fp, i * 4));
	}
	put32(frame, legacy ? 532 : n32 ? 592 : 312,
	    n32 ? get64(fp, 256) : get32(fp, 128));
}

static void
restore_fp(const uint8_t *frame, bool legacy, bool n32, mcontext_t *mc)
{
	unsigned base = legacy ? 272 : n32 ? 336 : 184;
	uint8_t *fp = (void *)&mc->__fpregs;
	for (unsigned i = 0; i < 32; i++) {
		if (n32)
			put64(fp, i * 8, get64(frame, base + i * 8));
		else
			put32(fp, i * 4, legacy ?
			    get64(frame, base + i * 8) : get32(frame, base + i * 4));
	}
	uint32_t csr = get32(frame, legacy ? 532 : n32 ? 592 : 312);
	if (n32)
		put64(fp, 256, csr);
	else
		put32(fp, 128, csr);
}

void
irix_sendsig(const ksiginfo_t *ksi, const sigset_t *mask)
{
	struct lwp *l = curlwp;
	struct proc *p = l->l_proc;
	struct irix_process *ip = p->p_emuldata;
	struct trapframe *tf = l->l_md.md_utf;
	const unsigned signo = foreign_signal(ksi->ksi_signo);
	const bool n32 = n32_process(p);
	uint8_t frame[1024] = { 0 };
	mcontext_t mc;
	unsigned flags = 0;
	int onstack, error;

	KASSERT(mutex_owned(p->p_lock));
	if (signo == 0)
		sigexit(l, SIGILL);
	const bool legacy = !(ip->signal[signo].flags & 8);
	uint32_t handler = (uintptr_t)SIGACTION(p, ksi->ksi_signo).sa_handler;
	uint32_t trampoline = ip->signal[signo].trampoline;
	void *destination = getframe(l, ksi->ksi_signo, &onstack,
	    sizeof(frame), n32 ? 16 : 8);
	uint32_t address = (uintptr_t)destination;
	if ((uintptr_t)destination >= 0x80000000U ||
	    sizeof(frame) > 0x80000000U - address)
		sigexit(l, SIGILL);
	if (legacy) {
		put32(frame, 0, UINT32_MAX);
		put32(frame, 540, l->l_sigstk.ss_flags & SS_ONSTACK ? 1 : 0);
		put32(frame, 584, mask_to_word(mask));
	} else {
		put32(frame, 0, 7);
		put32(frame, 8, mask_to_word(mask));
		put32(frame, 24, (uintptr_t)l->l_sigstk.ss_sp);
		put32(frame, 28, l->l_sigstk.ss_size);
		put32(frame, 32, l->l_sigstk.ss_flags & SS_ONSTACK ? 1 : 2);
		unsigned si = n32 ? 800 : 512;
		put32(frame, si, signo);
		if (ksi->ksi_code == SI_USER) {
			put32(frame, si + 12, ksi->ksi_pid);
			put32(frame, si + 16, ksi->ksi_uid);
		} else if (signo == 18) {
			put32(frame, si + 4, ksi->ksi_code);
			put32(frame, si + 12, ksi->ksi_pid);
			put32(frame, si + 20, ksi->ksi_code == CLD_EXITED ?
			    ksi->ksi_status : foreign_signal(ksi->ksi_status));
		} else if (ksi->ksi_code > 0 && ksi->ksi_code != SI_NOINFO) {
			put32(frame, si + 4, ksi->ksi_code);
			put32(frame, si + 12, (uintptr_t)ksi->ksi_addr);
		}
	}
	sendsig_reset(l, ksi->ksi_signo);
	mutex_exit(p->p_lock);
	memset(&mc, 0, sizeof(mc));
	cpu_getmcontext(l, &mc, &flags);
	if (legacy) {
		put64(frame, 8, mc.__gregs[_REG_EPC]);
		for (unsigned i = 1; i < 32; i++)
			put64(frame, 16 + 8 * i, mc.__gregs[i]);
		put64(frame, 544, mc.__gregs[_REG_MDHI]);
		put64(frame, 552, mc.__gregs[_REG_MDLO]);
		put64(frame, 560, mc.__gregs[_REG_CAUSE]);
		put32(frame, 528, (flags & _UC_FPU) != 0);
	} else {
		for (unsigned i = 1; i < (n32 ? 37 : 36); i++) {
			if (n32) put64(frame, 40 + 8 * i, mc.__gregs[i]);
			else put32(frame, 40 + 4 * i, mc.__gregs[i]);
		}
		if (flags & _UC_FPU) put32(frame, 0, 15);
	}
	if (flags & _UC_FPU)
		save_fp(frame, legacy, n32, &mc);
	error = irix_prda_maskout(l);
	if (error == 0)
		error = copyout(frame, destination, sizeof(frame));
	mutex_enter(p->p_lock);
	if (error != 0)
		sigexit(l, SIGILL);
	tf->tf_regs[_R_A0] = (int32_t)(signo | (legacy ? 0 : 0x80000000U));
	tf->tf_regs[_R_A1] = legacy ? 0 : address + (n32 ? 800 : 512);
	tf->tf_regs[_R_A2] = address;
	tf->tf_regs[_R_A3] = (int32_t)handler;
	tf->tf_regs[_R_PC] = (int32_t)trampoline;
	tf->tf_regs[_R_T9] = (int32_t)trampoline;
	tf->tf_regs[_R_SP] = address;
	if (onstack)
		l->l_sigstk.ss_flags |= SS_ONSTACK;
}

int
irix_sigreturn(struct lwp *l, const void *args, register_t *rval)
{
	const struct irix_sigreturn_args *uap = args;
	struct proc *p = l->l_proc;
	bool legacy = SCARG(uap, sc) != 0;
	bool n32 = n32_process(p);
	uint32_t address = legacy ? SCARG(uap, sc) : SCARG(uap, uc);
	uint8_t frame[856];
	mcontext_t mc;
	sigset_t mask;
	unsigned flags = 0, wireflags, maskoffset;
	int error;

	/* No register or mask changes precede complete copyin and validation. */
	if (address == 0 || address >= 0x80000000U)
		return EFAULT;
	error = copyin(ptr32(address), frame, legacy ? 856 : n32 ? 800 : 512);
	if (error != 0)
		return error;
	memset(&mc, 0, sizeof(mc));
	wireflags = legacy ? 7 | (get32(frame, 528) ? 8 : 0) : get32(frame, 0);
	if (wireflags & ~15U)
		return EINVAL;
	if (wireflags & 4) {
		flags |= _UC_CPU;
		if (legacy) {
			for (unsigned i = 1; i < 32; i++)
				mc.__gregs[i] = get64(frame, 16 + 8 * i);
			mc.__gregs[_REG_EPC] = get64(frame, 8);
			mc.__gregs[_REG_MDHI] = get64(frame, 544);
			mc.__gregs[_REG_MDLO] = get64(frame, 552);
			mc.__gregs[_REG_CAUSE] = get64(frame, 560);
		} else {
			for (unsigned i = 1; i < (n32 ? 37 : 36); i++)
				mc.__gregs[i] = n32 ? (int64_t)get64(frame, 40 + 8 * i) :
				    (int32_t)get32(frame, 40 + 4 * i);
		}
		/* cpu_setmcontext deliberately ignores SR. Also bound PC/SP to
		 * this personality's user address space before any state change. */
		if ((uint64_t)mc.__gregs[_REG_EPC] >= 0x80000000U ||
		    (mc.__gregs[_REG_EPC] & 3) ||
		    (uint64_t)mc.__gregs[_REG_SP] >= 0x80000000U)
			return EINVAL;
	}
	if (wireflags & 8) {
		restore_fp(frame, legacy, n32, &mc);
		flags |= _UC_FPU;
	}
	if (wireflags & 2) {
		uint32_t stackflags = get32(frame, legacy ? 540 : 32);
		if (stackflags & ~3U)
			return EINVAL;
		flags |= stackflags & 1 ? _UC_SETSTACK : _UC_CLRSTACK;
	}
	maskoffset = legacy ? 584 : 8;
	if ((wireflags & 1) && ((get32(frame, maskoffset) & 0x80000000U) ||
	    get32(frame, maskoffset + 4) || get32(frame, maskoffset + 8) ||
	    get32(frame, maskoffset + 12)))
		return EOPNOTSUPP;
	error = cpu_setmcontext(l, &mc, flags);
	if (error != 0)
		return error;
	/* The native helper discards lazy-FPU validity before copying the
	 * replacement PCB. Mark that replacement valid, so the next COP1
	 * trap loads it instead of initializing a zero first-use context. */
	if (flags & _UC_FPU)
		pcu_discard(&mips_fpu_ops, l, true);
	if (wireflags & 1) {
		mutex_enter(p->p_lock);
		mask = l->l_sigmask;
		word_to_mask(get32(frame, maskoffset), &mask);
		(void)sigprocmask1(l, SIG_SETMASK, &mask, NULL);
		mutex_exit(p->p_lock);
	}
	if (irix_prda_maskout(l) != 0) {
		mutex_enter(p->p_lock);
		sigexit(l, SIGSEGV);
	}
	return EJUSTRETURN;
}
