
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
 * Experimental o32 personality, not general IRIX support. Xiri-marked static
 * fixtures and a bounded vendor interpreter path have separate ELF contracts.
 * File and private mapping support remains deliberately small.
 */
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/cpu.h>
#include <sys/exec.h>
#include <sys/exec_elf.h>
#include <sys/fcntl.h>
#include <sys/ktrace.h>
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

struct irix_o32_syssgi_args {
	syscallarg(int32_t) request;
	syscallarg(uint32_t) arg1;
	syscallarg(uint32_t) arg2;
	syscallarg(uint32_t) arg3;
};

static int
irix_o32_syssgi(struct lwp *l, const void *args, register_t *rval)
{
	const struct irix_o32_syssgi_args * const uap = args;

	switch (SCARG(uap, request)) {
	case 22: /* SGI_SYSCONF */
		switch (SCARG(uap, arg1)) {
		case 11: /* _SC_PAGESIZE */
			*rval = PAGE_SIZE;
			return 0;
		case 29: /* _SC_CAP: CAP_SYS_DISABLED, traditional uid permissions */
			*rval = 0;
			return 0;
		default:
			return EINVAL;
		}
	case 85: { /* SGI_PROC_ATTR_GET */
		/* IRIX 6.5.22m proc_attr_get bounds copyinstr to 80 bytes. */
		char name[80];
		/* Effective, permitted, inheritable 64-bit IRIX capability masks.
		 * This personality has no IRIX capabilities. NetBSD credentials
		 * and authorization still govern every native operation. */
		const uint64_t caps[3] = { 0, 0, 0 };
		int error;

		CTASSERT(sizeof(caps) == 24);
		if (SCARG(uap, arg1) == 0 || SCARG(uap, arg2) == 0)
			return EINVAL;
		error = copyinstr((const void *)(uintptr_t)SCARG(uap, arg1),
		    name, sizeof(name), NULL);
		if (error != 0)
			return error;
		if (strcmp(name, "SGI_CAP_PROCESS") != 0)
			return ENOATTR;
		error = copyout(caps, (void *)(uintptr_t)SCARG(uap, arg2), sizeof(caps));
		if (error != 0)
			return error;
		*rval = 0;
		return 0;
	}
	default:
		/* Other syssgi operations, including setters and ELFMAP, remain
		 * unsupported; a successful capability query grants no privilege. */
		return ENOSYS;
	}
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
	[24] = { .sy_call = (sy_call_t *)sys_getuid_with_euid },
	[40] = {
		.sy_narg = 4,
		.sy_argsize = sizeof(struct irix_o32_syssgi_args),
		.sy_flags = SYCALL_ARG_PTR,
		.sy_call = irix_o32_syssgi,
	},
	[47] = { .sy_call = (sy_call_t *)sys_getgid_with_egid },
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
	[5] = "open", [6] = "close", [24] = "getuid", [40] = "syssgi",
	[47] = "getgid", [134] = "mmap", [135] = "munmap",
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
	/* Preserve the actual trap number, including unsupported calls which
	 * use table slot zero. Only emit records when explicitly traced. */
	if (KTRPOINT(p, KTR_USER)) {
		char record[160];
		int len = snprintf(record, sizeof(record),
		    "syscall=%u pc=%#x a0=%#x a1=%#x a2=%#x a3=%#x",
		    number, (uint32_t)pc, (uint32_t)reg->r_regs[_R_A0],
		    (uint32_t)reg->r_regs[_R_A1], (uint32_t)reg->r_regs[_R_A2],
		    (uint32_t)reg->r_regs[_R_A3]);
		ktr_kuser("irix_o32", record, len);
	}
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
	case 1024: code = 24; break;
	case 1040: code = 40; break;
	case 1047: code = 47; break;
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
	.e_path = "/emul/irix",
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
	Aux32Info aux[7];
	struct elf_args * const ap = pack->ep_emul_arg;
	size_t count = 0;
	int error;

	/* IRIX's six dynamic entries, or just AT_NULL for the static fixtures.
	 * Use current copyargs/ps_strings and emul-argument ownership rules. */
	if (ap != NULL) {
		aux[count++] = (Aux32Info){ AT_PHDR, ap->arg_phaddr };
		aux[count++] = (Aux32Info){ AT_PHENT, ap->arg_phentsize };
		aux[count++] = (Aux32Info){ AT_PHNUM, ap->arg_phnum };
		aux[count++] = (Aux32Info){ AT_ENTRY, ap->arg_entry };
		aux[count++] = (Aux32Info){ AT_BASE, ap->arg_interp };
		aux[count++] = (Aux32Info){ AT_PAGESZ, PAGE_SIZE };
	}
	aux[count++] = (Aux32Info){ AT_NULL, 0 };
	const size_t size = count * sizeof(aux[0]);
	local_exec.es_arglen = roundup(vectors_end + size, 16) - vectors_end;
	KASSERT(local_exec.es_arglen <= pack->ep_esch->es_arglen);
	local_pack.ep_esch = &local_exec;
	if ((error = copyargs(l, &local_pack, arginfo, stackp, argp)) != 0)
		return error;
	if ((error = copyout(aux, *stackp, size)) != 0)
		return error;
	*stackp += size;
	exec_free_emul_arg(pack);
	return 0;
}

/* Validate the bounded vendor layout before the native ELF mapper sees it.
 * This is a separate contract from the unchanged Xiri static-note probe. */
static int
irix_o32_runtime_image(struct lwp *l, struct vnode *vp, off_t filesize,
    const Elf32_Ehdr *eh, bool interpreter, Elf32_Phdr loads[2])
{
	Elf32_Phdr ph[16];
	unsigned nload = 0, ninterp = 0, ndynamic = 0, nphdr = 0;
	const uint32_t flags = interpreter ? 3 : 5;
	size_t phsize;
	int error;

	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
	    eh->e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh->e_ident[EI_DATA] != ELFDATA2MSB ||
	    eh->e_ident[EI_VERSION] != EV_CURRENT ||
	    eh->e_ident[EI_OSABI] != ELFOSABI_SYSV ||
	    eh->e_ident[EI_ABIVERSION] != 0 || eh->e_version != EV_CURRENT ||
	    eh->e_machine != EM_MIPS ||
	    eh->e_type != (interpreter ? ET_DYN : ET_EXEC) ||
	    (eh->e_flags != (EF_MIPS_ARCH_2 | flags) &&
	     eh->e_flags != (EF_MIPS_ARCH_3 | flags)) ||
	    mips_options.mips_cpu_arch < CPU_ARCH_MIPS3 ||
	    eh->e_ehsize != sizeof(*eh) || eh->e_phoff != sizeof(*eh) ||
	    eh->e_phentsize != sizeof(ph[0]) ||
	    eh->e_phnum == 0 || eh->e_phnum > __arraycount(ph))
		return ENOEXEC;
	phsize = eh->e_phnum * sizeof(ph[0]);
	if (filesize < 0 || sizeof(*eh) + phsize > (uint64_t)filesize)
		return ENOEXEC;
	if ((error = exec_read(l, vp, eh->e_phoff, ph, phsize,
	    IO_NODELOCKED)) != 0)
		return error;
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		const Elf32_Phdr *p = &ph[i];

		if ((uint64_t)p->p_offset + p->p_filesz > (uint64_t)filesize)
			return ENOEXEC;
		switch (p->p_type) {
		case PT_LOAD:
			if (nload == 2 ||
			    p->p_flags != (nload == 0 ? PF_R | PF_X : PF_R | PF_W) ||
			    p->p_memsz == 0 || p->p_memsz > 16 * 1024 * 1024 ||
			    p->p_filesz > p->p_memsz ||
			    p->p_vaddr < 0x00400000 ||
			    p->p_vaddr > 0x40000000 - p->p_memsz ||
			    p->p_offset > INT32_MAX - p->p_filesz ||
			    (p->p_vaddr & PAGE_MASK) != 0 ||
			    (p->p_offset & PAGE_MASK) != 0 ||
			    p->p_align < PAGE_SIZE || p->p_align > 1024 * 1024 ||
			    !powerof2(p->p_align) ||
			    p->p_vaddr % p->p_align != p->p_offset % p->p_align)
				return ENOEXEC;
			loads[nload++] = *p;
			break;
		case PT_INTERP:
			if (interpreter || ++ninterp != 1)
				return ENOEXEC;
			break;
		case PT_DYNAMIC:
			if (++ndynamic != 1 || p->p_filesz == 0)
				return ENOEXEC;
			break;
		case PT_PHDR:
			if (++nphdr != 1 || p->p_offset != eh->e_phoff ||
			    p->p_filesz != phsize || p->p_memsz != phsize)
				return ENOEXEC;
			break;
		case 0x70000000: /* PT_MIPS_REGINFO */
		case 0x70000001: /* PT_MIPS_RTPROC: may be outside mapped segments */
		case 0x70000002: /* PT_MIPS_OPTIONS */
			break;
		default:
			return ENOEXEC;
		}
	}
	if (nload != 2 || ndynamic != 1 || ninterp != (interpreter ? 0 : 1) ||
	    loads[0].p_offset != 0 || loads[0].p_filesz < sizeof(*eh) + phsize ||
	    loads[0].p_memsz != loads[0].p_filesz ||
	    loads[1].p_vaddr < round_page(loads[0].p_vaddr + loads[0].p_memsz) ||
	    (loads[1].p_filesz != 0 &&
	     loads[1].p_offset < round_page(loads[0].p_filesz)) ||
	    eh->e_entry < loads[0].p_vaddr ||
	    eh->e_entry >= loads[0].p_vaddr + loads[0].p_filesz ||
	    (eh->e_entry & 3) != 0)
		return ENOEXEC;
	/* Metadata passed to userland must point into file-backed image bytes. */
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		const Elf32_Phdr *p = &ph[i];
		bool contained = false;

		if (p->p_type != PT_PHDR && p->p_type != PT_INTERP &&
		    p->p_type != PT_DYNAMIC)
			continue;
		for (unsigned j = 0; j < 2; j++) {
			if (p->p_offset >= loads[j].p_offset &&
			    (uint64_t)p->p_offset + p->p_filesz <=
			    (uint64_t)loads[j].p_offset + loads[j].p_filesz &&
			    (uint64_t)p->p_vaddr == (uint64_t)loads[j].p_vaddr +
			    p->p_offset - loads[j].p_offset)
				contained = true;
		}
		if (!contained)
			return ENOEXEC;
	}
	return 0;
}

static int
irix_o32_runtime_probe(struct lwp *l, struct exec_package *epp,
    const Elf32_Ehdr *eh, char *interp, vaddr_t *pos)
{
	Elf32_Phdr main_loads[2], interp_loads[2];
	Elf32_Ehdr ieh;
	struct vattr va;
	struct vnode *vp;
	int error;

	if (strcmp(interp, "/lib/libc.so.1") != 0 &&
	    strcmp(interp, "/usr/lib/libc.so.1") != 0)
		return ENOEXEC;
	if ((error = irix_o32_runtime_image(l, epp->ep_vp,
	    epp->ep_vap->va_size, eh, false, main_loads)) != 0)
		return error;
	if ((error = emul_find_interp(l, epp, interp)) != 0)
		return error;
	vp = epp->ep_interp;
	/* Do not recursively lock an executable used as its own interpreter. */
	if (vp == epp->ep_vp)
		return ENOEXEC;
	vn_lock(vp, LK_SHARED | LK_RETRY);
	if (vp->v_type != VREG)
		error = EACCES;
	else if ((error = vn_marktext(vp)) == 0 &&
	    (error = VOP_GETATTR(vp, &va, l->l_cred)) == 0 &&
	    (error = exec_read(l, vp, 0, &ieh, sizeof(ieh), IO_NODELOCKED)) == 0)
		error = irix_o32_runtime_image(l, vp, va.va_size, &ieh,
		    true, interp_loads);
	VOP_UNLOCK(vp);
	if (error != 0)
		return error;
	/* The native interpreter mapper reserves gaps as well as segments. */
	const vaddr_t lo = interp_loads[0].p_vaddr;
	const vaddr_t hi = round_page(interp_loads[1].p_vaddr + interp_loads[1].p_memsz);
	for (unsigned i = 0; i < 2; i++) {
		if (main_loads[i].p_vaddr < hi &&
		    round_page(main_loads[i].p_vaddr + main_loads[i].p_memsz) > lo)
			return ENOEXEC;
	}
	/* IRIX libc's bootstrap GOT is already linked at this address. */
	*pos = ELF32_LINK_ADDR;
	return 0;
}

static int
irix_o32_probe(struct lwp *l, struct exec_package *epp, void *header,
    char *interp, vaddr_t *pos)
{
	const Elf32_Ehdr * const eh = header;
	Elf32_Phdr ph[3];
	const Elf32_Phdr * const load = &ph[0];
	const Elf32_Phdr *note;
	size_t phsize;
	/* namesz=5, descsz=4, type=IRIX, name=Xiri\0, version=1; big endian. */
	static const uint8_t marker[24] = {
		0, 0, 0, 5, 0, 0, 0, 4, 0x49, 0x52, 0x49, 0x58,
		'X', 'i', 'r', 'i', 0, 0, 0, 0, 0, 0, 0, 1
	};
	uint8_t actual[sizeof(marker)];
	int error;

	if (interp != NULL)
		return irix_o32_runtime_probe(l, epp, eh, interp, pos);

	if (eh->e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh->e_ident[EI_DATA] != ELFDATA2MSB ||
	    eh->e_ident[EI_VERSION] != EV_CURRENT ||
	    eh->e_ident[EI_OSABI] != ELFOSABI_SYSV ||
	    eh->e_ident[EI_ABIVERSION] != 0 || eh->e_version != EV_CURRENT ||
	    eh->e_machine != EM_MIPS || eh->e_type != ET_EXEC || interp != NULL ||
	    eh->e_flags != 0x20000101 ||
	    mips_options.mips_cpu_arch < CPU_ARCH_MIPS3 ||
	    eh->e_ehsize != sizeof(*eh) || eh->e_phoff != sizeof(*eh) ||
	    eh->e_phentsize != sizeof(ph[0]) ||
	    (eh->e_phnum != 2 && eh->e_phnum != 3))
		return ENOEXEC;
	phsize = eh->e_phnum * sizeof(ph[0]);
	note = &ph[eh->e_phnum - 1];
	if ((error = exec_read(l, epp->ep_vp, eh->e_phoff, ph, phsize,
	    IO_NODELOCKED)) != 0)
		return error;

	/* Fixed RX text, optional separate RW data/BSS, and the exact note. */
	if (load->p_type != PT_LOAD || note->p_type != PT_NOTE ||
	    load->p_offset != 0 || load->p_vaddr != 0x00400000 ||
	    load->p_flags != (PF_R | PF_X) ||
	    load->p_filesz < sizeof(*eh) + phsize ||
	    load->p_filesz != load->p_memsz || load->p_filesz > 1024 * 1024 ||
	    load->p_filesz > epp->ep_vap->va_size ||
	    load->p_align < PAGE_SIZE || load->p_align > 1024 * 1024 ||
	    !powerof2(load->p_align) || (load->p_vaddr % load->p_align) != 0 ||
	    note->p_offset < sizeof(*eh) + phsize ||
	    note->p_filesz != sizeof(marker) ||
	    note->p_offset > load->p_filesz - sizeof(marker) ||
	    eh->e_entry < load->p_vaddr ||
	    eh->e_entry >= load->p_vaddr + load->p_filesz ||
	    (eh->e_entry & 3) != 0)
		return ENOEXEC;
	if (eh->e_phnum == 3) {
		const Elf32_Phdr * const data = &ph[1];

		/* This static subset uses page-separated segments in a bounded
		 * image window. Check subtraction bounds before any native mapper
		 * arithmetic, including its signed file offset and page rounding. */
		if (data->p_type != PT_LOAD || data->p_flags != (PF_R | PF_W) ||
		    data->p_memsz == 0 || data->p_memsz > 1024 * 1024 ||
		    data->p_filesz > data->p_memsz ||
		    data->p_align < PAGE_SIZE || data->p_align > 1024 * 1024 ||
		    !powerof2(data->p_align) ||
		    (data->p_vaddr & PAGE_MASK) != 0 ||
		    (data->p_offset & PAGE_MASK) != 0 ||
		    data->p_vaddr % data->p_align != data->p_offset % data->p_align ||
		    data->p_vaddr < round_page(load->p_vaddr + load->p_memsz) ||
		    data->p_vaddr > 0x01000000 - data->p_memsz ||
		    data->p_offset > INT32_MAX - data->p_filesz ||
		    (uint64_t)data->p_offset + data->p_filesz > epp->ep_vap->va_size ||
		    (data->p_filesz != 0 && data->p_offset < round_page(load->p_filesz)))
			return ENOEXEC;
	}
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
		.es_arglen = 7 * sizeof(Aux32Info) + 15,
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
