/* $NetBSD: mips_fputrap.c,v 1.13 2025/05/03 02:00:46 riastradh Exp $ */

/*
 * Copyright (c) 2004
 *	Matthias Drochner. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef _KERNEL_OPT
#include "opt_cputype.h"
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/siginfo.h>

#include <mips/cpuregs.h>
#include <mips/regnum.h>
#include <mips/locore.h>
#include <mips/pcb.h>

#if defined(FPEMUL) || !defined(NOFPU)
void mips_fpuexcept(struct lwp *, uint32_t);
void mips_fpuillinst(struct lwp *, uint32_t);
static int fpustat2sicode(uint32_t);

void
mips_fpuexcept(struct lwp *l, uint32_t fpustat)
{
	ksiginfo_t ksi;

#ifdef FPEMUL_DEBUG
	printf("%s(%#x,%#"PRIxREGISTER")\n",
	   __func__, fpustat, l->l_md.md_utf->tf_regs[_R_PC]);
#endif

	KSI_INIT_TRAP(&ksi);
	ksi.ksi_signo = SIGFPE;
	ksi.ksi_code = fpustat2sicode(fpustat);
	ksi.ksi_trap = fpustat;
	(*l->l_proc->p_emul->e_trapsignal)(l, &ksi);
}

void
mips_fpuillinst(struct lwp *l, uint32_t opcode)
{
	ksiginfo_t ksi;

#ifdef FPEMUL_DEBUG
	printf("%s(%#x,%#"PRIxREGISTER")\n",
	   __func__, opcode, l->l_md.md_utf->tf_regs[_R_PC]);
#endif

	KSI_INIT_TRAP(&ksi);
	ksi.ksi_signo = SIGILL;
	ksi.ksi_code = ILL_ILLOPC;
	ksi.ksi_trap = opcode;
	ksi.ksi_addr = (void *)(uintptr_t)l->l_md.md_utf->tf_regs[_R_PC];
	(*l->l_proc->p_emul->e_trapsignal)(l, &ksi);
}

#if !defined(FPEMUL) && (defined(__mips_n32) || defined(__mips_n64))
/*
 * R4x00/R5000 request software completion of CVT.D.L for large inputs.
 * Work in the saved FPU context so both paired and 64-bit FPRs, including
 * odd-numbered wide registers, use the same layout as context switching.
 */
void mips_cvt_d_l(uint32_t, struct trapframe *, uint32_t);

void
mips_cvt_d_l(uint32_t insn, struct trapframe *tf, uint32_t csr)
{
	struct lwp *l = curlwp;
	struct pcb *pcb = lwp_getpcb(l);
	struct fpreg *fp = &pcb->pcb_fpregs;
	struct fpreg_oabi *pairs = (struct fpreg_oabi *)(void *)fp;
	const unsigned fs = (insn >> 11) & 31, fd = (insn >> 6) & 31;
	const bool wide = (tf->tf_regs[_R_SR] & MIPS3_SR_FR) != 0;
	uint64_t src, mag, bits = 0;
	bool inexact = false;

	if (!wide && ((fs | fd) & 1)) {
		mips_fpuillinst(l, insn);
		return;
	}
	fpu_save(l);
	fpu_discard(l);
	if (wide)
		src = fp->r_regs[fs];
	else
		src = (uint32_t)pairs->r_regs[fs] |
		    (uint64_t)(uint32_t)pairs->r_regs[fs + 1] << 32;

	const bool neg = (src >> 63) != 0;
	mag = neg ? -src : src;	/* unsigned, including INT64_MIN */
	if (mag != 0) {
		unsigned exp = 63 - __builtin_clzll(mag);
		uint64_t frac;

		if (exp <= 52) {
			frac = mag << (52 - exp);
		} else {
			unsigned shift = exp - 52;
			uint64_t half = UINT64_C(1) << (shift - 1);
			uint64_t rem = mag & ((half << 1) - 1);
			frac = mag >> shift;
			inexact = rem != 0;
			switch (csr & MIPS_FCSR_RM) {
			case MIPS_FCSR_RM_RN:
				frac += rem > half || (rem == half && (frac & 1));
				break;
			case MIPS_FCSR_RM_RP:
				frac += inexact && !neg;
				break;
			case MIPS_FCSR_RM_RM:
				frac += inexact && neg;
				break;
			}
		}
		/* Adding the significand carries a rounded 2.0 into exp. */
		bits = (src & (UINT64_C(1) << 63)) |
		    (((uint64_t)(exp + 1022) << 52) + frac);
	}

	csr &= ~MIPS_FCSR_CAUSE;
	if (inexact) {
		csr |= MIPS_FCSR_CAUSE_I;
		if ((csr & MIPS_FCSR_ENABLES_I) == 0)
			csr |= MIPS_FCSR_FLAGS_I;
	}
	if (wide)
		fp->r_regs[32] = csr;
	else
		pairs->r_regs[32] = csr;
	if (inexact && (csr & MIPS_FCSR_ENABLES_I)) {
		mips_fpuexcept(l, csr);
		return;
	}
	if (wide) {
		fp->r_regs[fd] = bits;
	} else {
		pairs->r_regs[fd] = (uint32_t)bits;
		pairs->r_regs[fd + 1] = (uint32_t)(bits >> 32);
	}
	vaddr_t pc = tf->tf_regs[_R_PC];
	tf->tf_regs[_R_PC] = (tf->tf_regs[_R_CAUSE] & MIPS_CR_BR_DELAY) ?
	    mips_emul_branch(tf, pc, csr, false) : pc + 4;
}
#endif

static const struct {
	unsigned int bit;
	int code;
} fpecodes[] = {
	{ MIPS_FCSR_CAUSE_I, FPE_FLTRES },
	{ MIPS_FCSR_CAUSE_U, FPE_FLTUND },
	{ MIPS_FCSR_CAUSE_O, FPE_FLTOVF },
	{ MIPS_FCSR_CAUSE_Z, FPE_FLTDIV },
	{ MIPS_FCSR_CAUSE_V, FPE_FLTINV },
	{ MIPS_FCSR_CAUSE_E, FPE_FLTINV }
};

static int
fpustat2sicode(uint32_t fpustat)
{
	for (size_t i = 0; i < __arraycount(fpecodes); i++) {
		if (fpustat & fpecodes[i].bit)
			return fpecodes[i].code;
	}

	return FPE_FLTINV;
}
#endif /* FPEMUL || !NOFPU */
