
/*-
 * Copyright (c) 2001 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason R. Thorpe and by Charles M. Hannum.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 1988 University of Utah.
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department and Ralph Campbell.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * from: Utah Hdr: trap.c 1.32 91/04/06
 *
 *	@(#)trap.c	8.5 (Berkeley) 1/11/94
 */

/*
 * Experimental Xiri-marked static o32 personality, not general IRIX support.
 * Static argument, file, and private mapping support. The ELF note and
 * constrained image layout are an explicit test contract, not IRIX branding.
 */
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/cpu.h>
#include <sys/exec.h>
#include <sys/exec_elf.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/signalvar.h>
#include <sys/syscallargs.h>
#include <sys/syscallvar.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <uvm/uvm_extern.h>
#include <mips/locore.h>
#include <mips/regnum.h>
#include <mips/userret.h>
#include <compat/irix_o32/irix_errno.h>

#if !defined(__mips_o32) || BYTE_ORDER != BIG_ENDIAN
#error COMPAT_IRIX_O32 currently requires a big-endian o32 kernel
#endif

static int compat_irix_o32_modcmd(modcmd_t, void *);
MODULE(MODULE_CLASS_EXEC, compat_irix_o32, "exec_elf32");

static int
irix_o32_nosys(struct lwp *l, const void *args, register_t *rval)
{

	/* Explicit experimental policy: return an error without posting SIGSYS. */
	return ENOSYS;
}

/* IRIX uses the historical SVR4 open flag assignments for this subset. */
static int
irix_o32_open(struct lwp *l, const void *args, register_t *rval)
{
	const struct sys_open_args * const uap = args;
	struct sys_open_args native = *uap;
	const uint32_t flags = SCARG(uap, flags);
	const uint32_t supported = 3 | 0x8 | 0x80 | 0x100 | 0x200 | 0x400;
	int converted;

	if ((flags & ~supported) != 0 || (flags & 3) == 3)
		return EINVAL;
	converted = (flags & 3) == 2 ? O_RDWR : (flags & 3) == 1 ? O_WRONLY : O_RDONLY;
	if (flags & 0x8)
		converted |= O_APPEND;
	if (flags & 0x80)
		converted |= O_NONBLOCK;
	if (flags & 0x100)
		converted |= O_CREAT;
	if (flags & 0x200)
		converted |= O_TRUNC;
	if (flags & 0x400)
		converted |= O_EXCL;
	SCARG(&native, flags) = converted;
	return sys_open(l, &native, rval);
}

struct irix_o32_mmap_args {
	syscallarg(void *) addr;
	syscallarg(uint32_t) len;
	syscallarg(int32_t) prot;
	syscallarg(int32_t) flags;
	syscallarg(int32_t) fd;
	syscallarg(int32_t) pos;
};

static int
irix_o32_mmap(struct lwp *l, const void *args, register_t *rval)
{
	const struct irix_o32_mmap_args * const uap = args;
	struct sys_mmap_args native = { 0 };

	/* Private non-fixed mappings only. In IRIX, 0x1000 is NOT MAP_ANON. */
	if (SCARG(uap, addr) != NULL || SCARG(uap, len) == 0 ||
	    (SCARG(uap, prot) & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0 ||
	    SCARG(uap, flags) != 0x2 || SCARG(uap, pos) < 0 ||
	    (SCARG(uap, pos) & PAGE_MASK) != 0)
		return EINVAL;
	SCARG(&native, addr) = NULL;
	SCARG(&native, len) = SCARG(uap, len);
	SCARG(&native, prot) = SCARG(uap, prot);
	SCARG(&native, flags) = MAP_PRIVATE;
	SCARG(&native, fd) = SCARG(uap, fd);
	SCARG(&native, pos) = SCARG(uap, pos); /* signed 32-bit to native off_t */
	return sys_mmap(l, &native, rval);
}

static int
irix_o32_munmap(struct lwp *l, const void *args, register_t *rval)
{
	const struct sys_munmap_args * const uap = args;

	if ((int32_t)SCARG(uap, len) <= 0 ||
	    ((vaddr_t)SCARG(uap, addr) & PAGE_MASK) != 0)
		return EINVAL;
	return sys_munmap(l, uap, rval);
}

static int
irix_o32_mprotect(struct lwp *l, const void *args, register_t *rval)
{
	const struct sys_mprotect_args * const uap = args;

	if ((int32_t)SCARG(uap, len) <= 0 ||
	    ((vaddr_t)SCARG(uap, addr) & PAGE_MASK) != 0 ||
	    (SCARG(uap, prot) & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0)
		return EINVAL;
	return sys_mprotect(l, uap, rval);
}

static struct sysent irix_o32_sysent[] = {
	[0] = { .sy_call = irix_o32_nosys },
	[1] = {
		.sy_narg = 1,
		.sy_argsize = sizeof(struct sys_exit_args),
		.sy_call = (sy_call_t *)sys_exit,
	},
	[2] = { .sy_call = irix_o32_nosys },
	[3] = {
		.sy_narg = 3,
		.sy_argsize = sizeof(struct sys_read_args),
		.sy_flags = SYCALL_ARG_PTR | SYCALL_RET_WIDE,
		.sy_call = (sy_call_t *)sys_read,
	},
	[4] = {
		.sy_narg = 3,
		.sy_argsize = sizeof(struct sys_write_args),
		.sy_flags = SYCALL_ARG_PTR | SYCALL_RET_WIDE,
		.sy_call = (sy_call_t *)sys_write,
	},
	[5] = {
		.sy_narg = 3,
		.sy_argsize = sizeof(struct sys_open_args),
		.sy_flags = SYCALL_ARG_PTR,
		.sy_call = irix_o32_open,
	},
	[6] = {
		.sy_narg = 1,
		.sy_argsize = sizeof(struct sys_close_args),
		.sy_call = (sy_call_t *)sys_close,
	},
	[134] = {
		.sy_narg = 6,
		.sy_argsize = sizeof(struct irix_o32_mmap_args),
		.sy_flags = SYCALL_ARG_PTR,
		.sy_call = irix_o32_mmap,
	},
	[135] = {
		.sy_narg = 2,
		.sy_argsize = sizeof(struct sys_munmap_args),
		.sy_flags = SYCALL_ARG_PTR,
		.sy_call = irix_o32_munmap,
	},
	[136] = {
		.sy_narg = 3,
		.sy_argsize = sizeof(struct sys_mprotect_args),
		.sy_flags = SYCALL_ARG_PTR,
		.sy_call = irix_o32_mprotect,
	},
};

static const uint32_t irix_o32_nomodbits[howmany(__arraycount(irix_o32_sysent), 32)] = { 0 };
static const char * const irix_o32_syscallnames[__arraycount(irix_o32_sysent)] = {
	[0] = "unsupported", [1] = "exit", [3] = "read", [4] = "write",
	[5] = "open", [6] = "close", [134] = "mmap", [135] = "munmap",
	[136] = "mprotect",
};

/* Symbolic native indices avoid importing the obsolete positional table. */
#include <compat/irix_o32/irix_errno_map.h>

static void
irix_o32_syscall(struct lwp *l, uint32_t status, uint32_t cause, vaddr_t pc)
{
	struct proc * const p = l->l_proc;
	struct trapframe * const tf = l->l_md.md_utf;
	struct reg * const reg = &tf->tf_registers;
	const mips_reg_t saved_v0 = reg->r_regs[_R_V0];
	const uint32_t number = (uint32_t)saved_v0;
	register_t args[6] = { 0 };
	int code, error;

	CTASSERT(sizeof(register_t) == 4);
	CTASSERT(sizeof(struct irix_o32_mmap_args) == sizeof(args));
	KASSERT(p->p_md.md_abi == _MIPS_BSD_API_O32);
	curcpu()->ci_data.cpu_nsyscall++;
	reg->r_regs[_R_PC] = (cause & MIPS_CR_BR_DELAY) ?
	    mips_emul_branch(tf, pc, 0, false) : pc + sizeof(uint32_t);

	/* Never subtract or use an unchecked user value as an array index. */
	switch (number) {
	case 1001: code = 1; break;
	case 1003: code = 3; break;
	case 1004: code = 4; break;
	case 1005: code = 5; break;
	case 1006: code = 6; break;
	case 1134: code = 134; break;
	case 1135: code = 135; break;
	case 1136: code = 136; break;
	default: code = 0; break;
	}
	const size_t nargs = irix_o32_sysent[code].sy_narg;
	KASSERT(nargs <= __arraycount(args));
	for (size_t i = 0; i < MIN(nargs, 4); i++)
		args[i] = reg->r_regs[_R_A0 + i];
	if (nargs > 4) {
		const uint32_t usp = reg->r_regs[_R_SP];
		if (usp > UINT32_MAX - 16) {
			error = EFAULT;
			goto done;
		}
		error = copyin((const void *)(uintptr_t)(usp + 16), args + 4,
		    (nargs - 4) * sizeof(args[0]));
		if (error != 0)
			goto done;
	}
	error = sy_invoke(&irix_o32_sysent[code], l, args,
	    &reg->r_regs[_R_V0], code);
done:
	switch (error) {
	case 0:
		reg->r_regs[_R_A3] = 0;
		break;
	case ERESTART:
		reg->r_regs[_R_V0] = saved_v0;
		reg->r_regs[_R_PC] = pc;
		break;
	case EJUSTRETURN:
		break;
	default:
		/* Unmapped host errors must not appear as successful returns. */
		reg->r_regs[_R_V0] = error > 0 &&
		    (unsigned)error < __arraycount(irix_o32_errno) &&
		    irix_o32_errno[error] != 0 ? irix_o32_errno[error] : IRIX_EIO;
		reg->r_regs[_R_A3] = 1;
		break;
	}
	KASSERT(l->l_blcnt == 0);
	KASSERT(curcpu()->ci_biglock_count == 0);
	userret(l);
}

static void
irix_o32_syscall_intern(struct proc *p)
{

	p->p_md.md_syscall = irix_o32_syscall;
}

static void
irix_o32_setregs(struct lwp *l, struct exec_package *epp, vaddr_t stack)
{

	/* Commit ABI state only after executable selection has succeeded. */
	l->l_proc->p_md.md_abi = _MIPS_BSD_API_O32;
	setregs(l, epp, stack);
}

static void
irix_o32_sendsig(const ksiginfo_t *ksi, const sigset_t *mask)
{

	/* No syscall installs foreign handlers; never build a native signal frame. */
	sigexit(curlwp, SIGILL);
}

static struct emul emul_irix_o32 = {
	.e_name = "irix_o32",
	.e_nosys = 0,
	.e_nsysent = __arraycount(irix_o32_sysent),
	.e_sysent = irix_o32_sysent,
	.e_nomodbits = irix_o32_nomodbits,
	.e_syscallnames = irix_o32_syscallnames,
	.e_sendsig = irix_o32_sendsig,
	.e_trapsignal = trapsignal,
	.e_setregs = irix_o32_setregs,
	.e_syscall_intern = irix_o32_syscall_intern,
	.e_vm_default_addr = uvm_default_mapaddr,
};

static int
irix_o32_copyargs(struct lwp *l, struct exec_package *pack,
    struct ps_strings *arginfo, char **stackp, void *argp)
{
	struct exec_package local_pack = *pack;
	struct execsw local_exec = *pack->ep_esch;
	const uintptr_t vectors_end = (uintptr_t)*stackp +
	    (arginfo->ps_nargvstr + arginfo->ps_nenvstr + 3) * sizeof(uint32_t);
	const Aux32Info terminator = { .a_type = AT_NULL, .a_v = 0 };
	int error;

	/* Keep current copying/ps_strings logic; reserve only static AT_NULL and
	 * align the first argument string as the historical IRIX loader did. */
	local_exec.es_arglen = roundup(vectors_end + sizeof(terminator), 16) - vectors_end;
	KASSERT(local_exec.es_arglen <= pack->ep_esch->es_arglen);
	local_pack.ep_esch = &local_exec;
	if ((error = copyargs(l, &local_pack, arginfo, stackp, argp)) != 0)
		return error;
	if ((error = copyout(&terminator, *stackp, sizeof(terminator))) != 0)
		return error;
	*stackp += sizeof(terminator);
	return 0;
}

static int
irix_o32_probe(struct lwp *l, struct exec_package *epp, void *header,
    char *interp, vaddr_t *pos)
{
	const Elf32_Ehdr * const eh = header;
	Elf32_Phdr ph[2];
	const Elf32_Phdr * const load = &ph[0], * const note = &ph[1];
	/* namesz=5, descsz=4, type=IRIX, name=Xiri\0, version=1; big endian. */
	static const uint8_t marker[24] = {
		0, 0, 0, 5, 0, 0, 0, 4, 0x49, 0x52, 0x49, 0x58,
		'X', 'i', 'r', 'i', 0, 0, 0, 0, 0, 0, 0, 1
	};
	uint8_t actual[sizeof(marker)];
	int error;

	if (eh->e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh->e_ident[EI_DATA] != ELFDATA2MSB ||
	    eh->e_ident[EI_VERSION] != EV_CURRENT ||
	    eh->e_ident[EI_OSABI] != ELFOSABI_SYSV ||
	    eh->e_ident[EI_ABIVERSION] != 0 || eh->e_version != EV_CURRENT ||
	    eh->e_machine != EM_MIPS || eh->e_type != ET_EXEC || interp != NULL ||
	    eh->e_flags != 0x20000101 ||
	    mips_options.mips_cpu_arch < CPU_ARCH_MIPS3 ||
	    eh->e_ehsize != sizeof(*eh) || eh->e_phoff != sizeof(*eh) ||
	    eh->e_phentsize != sizeof(ph[0]) || eh->e_phnum != __arraycount(ph))
		return ENOEXEC;
	if ((error = exec_read(l, epp->ep_vp, eh->e_phoff, ph, sizeof(ph),
	    IO_NODELOCKED)) != 0)
		return error;

	/* One fixed, read/execute segment and one exact note; no dynamic loader. */
	if (load->p_type != PT_LOAD || note->p_type != PT_NOTE ||
	    load->p_offset != 0 || load->p_vaddr != 0x00400000 ||
	    load->p_flags != (PF_R | PF_X) ||
	    load->p_filesz < sizeof(*eh) + sizeof(ph) ||
	    load->p_filesz != load->p_memsz || load->p_filesz > 1024 * 1024 ||
	    load->p_filesz > epp->ep_vap->va_size ||
	    load->p_align < PAGE_SIZE || load->p_align > 1024 * 1024 ||
	    !powerof2(load->p_align) || (load->p_vaddr % load->p_align) != 0 ||
	    note->p_offset < sizeof(*eh) + sizeof(ph) ||
	    note->p_filesz != sizeof(marker) ||
	    note->p_offset > load->p_filesz - sizeof(marker) ||
	    eh->e_entry < load->p_vaddr ||
	    eh->e_entry >= load->p_vaddr + load->p_filesz ||
	    (eh->e_entry & 3) != 0)
		return ENOEXEC;
	if ((error = exec_read(l, epp->ep_vp, note->p_offset, actual,
	    sizeof(actual), IO_NODELOCKED)) != 0)
		return error;
	if (memcmp(actual, marker, sizeof(marker)) != 0)
		return ENOEXEC;
	return 0;
}

static struct execsw irix_o32_execsw[] = {
	{
		.es_hdrsz = sizeof(Elf32_Ehdr),
		.es_makecmds = exec_elf32_makecmds,
		.u = { .elf_probe_func = irix_o32_probe },
		.es_emul = &emul_irix_o32,
		.es_prio = EXECSW_PRIO_ANY,
		.es_arglen = sizeof(Aux32Info) + 15,
		.es_copyargs = irix_o32_copyargs,
		.es_coredump = coredump_elf32,
		.es_setup_stack = exec_setup_stack,
	},
};

static int
compat_irix_o32_modcmd(modcmd_t cmd, void *arg)
{

	switch (cmd) {
	case MODULE_CMD_INIT:
		return exec_add(irix_o32_execsw, __arraycount(irix_o32_execsw));
	case MODULE_CMD_FINI:
		return exec_remove(irix_o32_execsw, __arraycount(irix_o32_execsw));
	default:
		return ENOTTY;
	}
}
