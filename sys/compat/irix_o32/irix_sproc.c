/*-
 * Copyright (c) 2026 Salvatore Bognanni
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Two-member, single-LWP share groups, measured against IRIX 6.5.22m.
 * No FORK_SHAREVM: PRDA and preexisting LOCAL mappings remain private.
 * Group lock order is group -> UVM map; no process lookup under that lock.
 * Each member holds its old vmspace until the exec/exit emulation hook, so
 * another member never operates on a freed map or the new exec image.
 */
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/condvar.h>
#include <sys/filedesc.h>
#include <sys/kmem.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/syscallargs.h>
#include <sys/systm.h>
#include <uvm/uvm_extern.h>
#include <uvm/uvm_map.h>
#include <uvm/uvm.h>
#include <mips/regnum.h>
#include <mips/locore.h>
#include <compat/irix_o32/irix_sproc.h>

#define PR_SFDS 0x2U
#define PR_SADDR 0x40U
#define PR_BLOCK 0x01000000U
#define PR_NOLIBC 0x02000000U
#define IRIX_VM_ENTRIES 1024
#define IRIX_STACK_MAX (64 * 1024 * 1024)
#ifdef _LP64
#define IRIX_VM_MAX VM_MAXUSER_ADDRESS32
#else
#define IRIX_VM_MAX VM_MAXUSER_ADDRESS
#endif

struct irix_share {
	kmutex_t lock;
	kcondvar_t cv;
	struct irix_process *member[2];
	unsigned count;
};
struct irix_inherit {
	vaddr_t start, end;
	vm_inherit_t inheritance;
};
struct irix_spawn {
	struct irix_inherit range[IRIX_VM_ENTRIES];
	unsigned count;
	uint32_t entry, argument, flags;
	vaddr_t stack_base, stack_top;
	vsize_t stack_size;
};

bool
irix_share_single(struct proc *p)
{
	struct irix_process *ip = p->p_emuldata;
	struct irix_share *g = ip->share;
	bool single;

	if (g != NULL)
		mutex_enter(&g->lock);
	single = p->p_nlwps == 1 && p->p_vmspace->vm_refcnt ==
	    (g == NULL ? 1U : 2U) && (g == NULL || g->count == 1);
	if (g != NULL)
		mutex_exit(&g->lock);
	return single;
}

/* Build an owned, nonoverlapping LOCAL interval set before changing a map.
 * mmap replacement and partial munmap split intervals; exhaustion fails
 * before a native call, never after replacing an existing mapping. */
int
irix_local_plan(const struct irix_localset *old, vaddr_t start, vaddr_t end,
    bool add, struct irix_localset *next)
{
	struct irix_range pieces[IRIX_LOCAL_MAX + 2];
	unsigned n = 0;

	for (unsigned i = 0; i < old->count; i++) {
		struct irix_range r = old->range[i];
		if (r.end <= start || r.start >= end)
			pieces[n++] = r;
		else {
			if (r.start < start)
				pieces[n++] = (struct irix_range){r.start, start};
			if (r.end > end)
				pieces[n++] = (struct irix_range){end, r.end};
		}
	}
	if (add)
		pieces[n++] = (struct irix_range){start, end};
	/* Sort and coalesce adjacent private ranges. */
	for (unsigned i = 1; i < n; i++) {
		struct irix_range r = pieces[i];
		unsigned j = i;
		while (j > 0 && pieces[j - 1].start > r.start) {
			pieces[j] = pieces[j - 1];
			j--;
		}
		pieces[j] = r;
	}
	memset(next, 0, sizeof(*next));
	for (unsigned i = 0; i < n; i++) {
		if (next->count && next->range[next->count - 1].end == pieces[i].start)
			next->range[next->count - 1].end = pieces[i].end;
		else {
			if (next->count == IRIX_LOCAL_MAX)
				return ENOMEM;
			next->range[next->count++] = pieces[i];
		}
	}
	return 0;
}

static void
irix_restore_inherit(struct vmspace *vm, const struct irix_spawn *s)
{
	for (unsigned i = 0; i < s->count; i++)
		(void)uvm_map_inherit(&vm->vm_map, s->range[i].start,
		    s->range[i].end, s->range[i].inheritance);
}

static int
irix_prepare_inherit(struct irix_process *ip, struct irix_spawn *s)
{
	struct vm_map *map = &ip->proc->p_vmspace->vm_map;
	struct vm_map_entry *e;
	int error = 0;

	vm_map_lock_read(map);
	for (e = map->header.next; e != &map->header; e = e->next) {
		if (s->count == IRIX_VM_ENTRIES || e->wired_count != 0 ||
		    UVM_ET_ISSUBMAP(e) || (e->inheritance != MAP_INHERIT_COPY &&
		    e->inheritance != MAP_INHERIT_SHARE)) {
			error = EOPNOTSUPP;
			break;
		}
		s->range[s->count++] = (struct irix_inherit){e->start, e->end, e->inheritance};
	}
	vm_map_unlock_read(map);
	if (error)
		return error;
	/* Native fork materializes and shares each non-local amap. Restore the
	 * original inheritance in BOTH maps before either returns to userland;
	 * a later ordinary fork must retain its normal COPY/SHARE behavior. */
	for (unsigned i = 0; i < s->count; i++)
		(void)uvm_map_inherit(map, s->range[i].start, s->range[i].end, MAP_INHERIT_SHARE);
	(void)uvm_map_inherit(map, IRIX_PRDA_ADDRESS,
	    IRIX_PRDA_ADDRESS + PAGE_SIZE, MAP_INHERIT_COPY);
	for (unsigned i = 0; i < ip->local.count; i++)
		(void)uvm_map_inherit(map, ip->local.range[i].start,
		    ip->local.range[i].end, MAP_INHERIT_COPY);
	return 0;
}

void
irix_share_fork(struct proc *child, struct lwp *parent)
{
	struct irix_process *old = parent->l_proc->p_emuldata;
	struct irix_process *ip = child->p_emuldata;
	struct irix_spawn *s = old->spawning;

	ip->local = old->local;
	ip->proc = child;
	if (s == NULL)
		return;
	struct irix_share *g = old->share;
	KASSERT(mutex_owned(&g->lock));
	KASSERT(g->count == 1);
	if (s->flags & PR_SADDR)
		irix_restore_inherit(child->p_vmspace, s);
	ip->share = g;
	ip->vm = child->p_vmspace;
	uvmspace_addref(ip->vm);
	ip->sharemask = s->flags & (PR_SADDR | PR_SFDS);
	ip->entry = s->entry;
	ip->argument = s->argument;
	ip->stack_base = s->stack_base;
	ip->stack_size = s->stack_size;
	ip->stack_top = s->stack_top;
	g->member[g->member[0] == NULL ? 0 : 1] = ip;
	g->count++;
}

void
irix_share_child(struct lwp *l)
{
	struct irix_process *ip = l->l_proc->p_emuldata;
	struct irix_share *g = ip->share;

	if (ip->entry == 0)
		return;
	/* Parent owns this lock until its inheritance has been restored. */
	mutex_enter(&g->lock);
	struct trapframe *tf = l->l_md.md_utf;
	tf->tf_regs[_R_PC] = ip->entry;
	tf->tf_regs[_R_SP] = ip->stack_top - 64;
	tf->tf_regs[_R_RA] = 0;
	tf->tf_regs[_R_A0] = ip->argument;
	tf->tf_regs[_R_A1] = tf->tf_regs[_R_A2] = tf->tf_regs[_R_A3] = 0;
	ip->entry = 0;
	mutex_exit(&g->lock);
}

void
irix_share_leave(struct proc *p)
{
	struct irix_process *ip = p->p_emuldata;
	struct irix_share *g = ip->share;
	struct vmspace *vm;
	bool last;

	if (g == NULL)
		return;
	mutex_enter(&g->lock);
	if (ip->unblock != NULL) {
		ip->unblock->blocked = false;
		cv_broadcast(&g->cv);
	}
	for (unsigned i = 0; i < 2; i++) {
		struct irix_process *peer = g->member[i];
		if (peer == ip) {
			g->member[i] = NULL;
			continue;
		}
		if (peer == NULL)
			continue;
		if (peer->unblock == ip)
			peer->unblock = NULL;
		/* Only automatically allocated child stacks are group-owned.
		 * Explicit user stacks and the founder's stack are never removed. */
		if (ip->stack_size)
			uvm_unmap(&peer->vm->vm_map, ip->stack_base,
			    ip->stack_base + ip->stack_size);
	}
	last = --g->count == 0;
	vm = ip->vm;
	ip->share = NULL;
	ip->vm = NULL;
	mutex_exit(&g->lock);
	uvmspace_free(vm);
	if (last) {
		cv_destroy(&g->cv);
		mutex_destroy(&g->lock);
		kmem_free(g, sizeof(*g));
	}
}

/* Only the calling member and its current peer can be targets. This is a
 * bounded policy, not a general pid-based procblk implementation. Pointers
 * are group-owned and cleared on departure, so PID reuse cannot redirect an
 * armed unblock to an unrelated process. */
int
irix_share_prctl(struct lwp *l, uint32_t option, uint32_t pid, register_t *rval)
{
	struct irix_process *ip = l->l_proc->p_emuldata;
	struct irix_share *g = ip->share;
	struct irix_process *target = NULL;
	int error = 0;

	if (option != 2 && option != 6 && option != 13 && option != 14)
		return ENOSYS;
	if (g == NULL) {
		if (option == 14 || (option == 2 && (pid == 0 || pid == l->l_proc->p_pid))) {
			*rval = 0;
			return 0;
		}
		return option == 13 ? EINVAL : ESRCH;
	}
	mutex_enter(&g->lock);
	if (pid == 0 || pid == l->l_proc->p_pid)
		target = ip;
	for (unsigned i = 0; i < 2; i++)
		if (g->member[i] && g->member[i]->proc->p_pid == pid)
			target = g->member[i];
	if (option == 14)
		*rval = g->count;
	else if (target == NULL)
		error = ESRCH;
	else if (option == 2)
		*rval = target->blocked;
	else if (option == 13)
		*rval = 1 | (ip->sharemask & target->sharemask);
	else if (target == ip || ip->unblock != NULL)
		error = EINVAL;
	else {
		ip->unblock = target;
		*rval = 0;
	}
	mutex_exit(&g->lock);
	return error;
}

int
irix_procblk(struct lwp *l, const void *args, register_t *rval)
{
	const struct irix_procblk_args *uap = args;
	struct irix_process *ip = l->l_proc->p_emuldata;
	struct irix_share *g = ip->share;
	int error = ESRCH;

	if (SCARG(uap, command) != 1)
		return EOPNOTSUPP;
	if (g == NULL)
		return ESRCH;
	mutex_enter(&g->lock);
	for (unsigned i = 0; i < 2; i++) {
		struct irix_process *target = g->member[i];
		if (target && target->proc->p_pid == SCARG(uap, pid)) {
			/* Do not pretend to implement procblk's general counting
			 * semaphore: only release a pending PR_BLOCK wait. */
			if (!target->blocked)
				error = EOPNOTSUPP;
			else {
				target->blocked = false;
				cv_broadcast(&g->cv);
				*rval = 0;
				error = 0;
			}
			break;
		}
	}
	mutex_exit(&g->lock);
	return error;
}

int
irix_share_mmap(struct lwp *l, struct sys_mmap_args *args, register_t *rval)
{
	struct irix_process *ip = l->l_proc->p_emuldata;
	struct irix_share *g = ip->share;
	struct irix_process *peer = NULL;
	vaddr_t address;
	vsize_t length = round_page(SCARG(args, len));
	int error;

	if (g == NULL)
		return sys_mmap(l, args, rval);
	mutex_enter(&g->lock);
	if (g->count > 1 && (SCARG(args, flags) & MAP_FIXED)) {
		error = EOPNOTSUPP;
		goto out;
	}
	for (unsigned i = 0; i < 2; i++)
		if (g->member[i] && g->member[i] != ip &&
		    (g->member[i]->sharemask & ip->sharemask & PR_SADDR))
			peer = g->member[i];
	if (peer && (length > peer->proc->p_rlimit[RLIMIT_AS].rlim_cur ||
	    peer->vm->vm_map.size > peer->proc->p_rlimit[RLIMIT_AS].rlim_cur - length)) {
		error = ENOMEM;
		goto out;
	}
	error = sys_mmap(l, args, rval);
	if (error || peer == NULL)
		goto out;
	address = *rval;
	vaddr_t alias = address;
	/* A non-replacing exact reservation: never overwrite a peer's LOCAL
	 * mapping. Failed extraction and source allocation are both rolled
	 * back below. No other member can mutate these maps here. */
	if (!uvm_map_reserve(&peer->vm->vm_map, length, address, 0, &alias,
	    UVM_FLAG_FIXED))
		error = ENOMEM;
	else {
		error = uvm_map_extract(&ip->vm->vm_map, address, length,
		    &peer->vm->vm_map, &alias, UVM_EXTRACT_RESERVED | UVM_EXTRACT_CONTIG);
		if (error)
			uvm_unmap(&peer->vm->vm_map, address, address + length);
	}
	if (error)
		uvm_unmap(&ip->vm->vm_map, address, address + length);
out:
	mutex_exit(&g->lock);
	return error;
}

static int
irix_spawn(struct lwp *l, uint32_t entry, uint32_t flags, uint32_t argument,
    uint32_t sp, uint32_t len, void (*childfn)(void *), register_t *rval)
{
	struct proc *p = l->l_proc;
	struct irix_process *ip = p->p_emuldata;
	struct vm_map *map = &p->p_vmspace->vm_map;
	struct irix_spawn *s;
	struct irix_share *g;
	bool newgroup = false, valid;
	int error;

	if ((flags & PR_NOLIBC) == 0 ||
	    (flags & ~(PR_NOLIBC | PR_BLOCK | PR_SADDR | PR_SFDS)) != 0)
		return EOPNOTSUPP;
	if (!irix_share_single(p) || ip->threads || ip->stack_top != 0 ||
	    (p->p_slflag & PSL_TRACED) || (map->flags & VM_MAP_WIREFUTURE))
		return EOPNOTSUPP;
	if (entry == 0 || entry >= IRIX_VM_MAX || (entry & 3) ||
	    argument >= IRIX_VM_MAX || sp >= IRIX_VM_MAX)
		return EFAULT;
	if (sp == 0 && (len < 8192 || len > IRIX_STACK_MAX))
		return EINVAL;
	if (sp != 0 && (sp < 8192 || len < 8192 || len > IRIX_STACK_MAX))
		return EINVAL;
	if ((flags & PR_SFDS) && l->l_fd->fd_foclose)
		return EINVAL;
	vm_map_lock_read(map);
	valid = uvm_map_checkprot(map, entry, entry + 4, VM_PROT_EXECUTE) &&
	    uvm_map_checkprot(map, IRIX_PRDA_ADDRESS, IRIX_PRDA_ADDRESS + PAGE_SIZE,
	    VM_PROT_READ | VM_PROT_WRITE);
	if (sp)
		valid = valid && uvm_map_checkprot(map, (sp & ~15U) - 8192,
		    sp & ~15U, VM_PROT_READ | VM_PROT_WRITE);
	vm_map_unlock_read(map);
	if (!valid)
		return EFAULT;
	s = kmem_zalloc(sizeof(*s), KM_SLEEP);
	s->entry = entry;
	s->argument = argument;
	s->flags = flags;
	s->stack_top = sp & ~15U;
	if (sp == 0) {
		struct sys_mmap_args mm = { 0 };
		register_t address[2];
		SCARG(&mm, len) = round_page(len);
		SCARG(&mm, prot) = PROT_READ | PROT_WRITE;
		SCARG(&mm, flags) = MAP_ANON | MAP_PRIVATE;
		SCARG(&mm, fd) = -1;
		error = sys_mmap(l, &mm, address);
		if (error)
			goto free_spawn;
		s->stack_base = address[0];
		s->stack_size = round_page(len);
		s->stack_top = s->stack_base + s->stack_size;
	}
	ip->proc = p;
	if (flags & PR_SADDR) {
		error = irix_prepare_inherit(ip, s);
		if (error)
			goto unmap_stack;
	}
	g = ip->share;
	if (g == NULL) {
		g = kmem_zalloc(sizeof(*g), KM_SLEEP);
		mutex_init(&g->lock, MUTEX_DEFAULT, IPL_NONE);
		cv_init(&g->cv, "irsproc");
		g->member[0] = ip;
		g->count = 1;
		ip->share = g;
		ip->sharemask = PR_SADDR | PR_SFDS;
		ip->vm = p->p_vmspace;
		uvmspace_addref(ip->vm);
		newgroup = true;
	}
	mutex_enter(&g->lock);
	ip->spawning = s;
	ip->blocked = (flags & PR_BLOCK) != 0;
	error = fork1(l, flags & PR_SFDS ? FORK_SHAREFILES : 0, SIGCHLD,
	    NULL, 0, childfn, NULL, rval);
	ip->spawning = NULL;
	if (flags & PR_SADDR)
		irix_restore_inherit(p->p_vmspace, s);
	if (error)
		ip->blocked = false;
	while (ip->blocked) {
		if (cv_wait_sig(&g->cv, &g->lock) != 0) {
			/* A child already exists: do not restart creation. Native
			 * userret delivers the pending terminating signal. */
			ip->blocked = false;
			break;
		}
	}
	mutex_exit(&g->lock);
	if (!error) {
		kmem_free(s, sizeof(*s));
		return 0;
	}
	if (newgroup)
		irix_share_leave(p);
unmap_stack:
	if (s->stack_size)
		uvm_unmap(map, s->stack_base, s->stack_base + s->stack_size);
free_spawn:
	kmem_free(s, sizeof(*s));
	return error;
}

/* Existing personality callback updates the private PRDA and performs the
 * native child return; irix_share_child adjusts its entry first. */

int
irix_sproc(struct lwp *l, const void *args, register_t *rval)
{
	const struct irix_sproc_args *uap = args;
	rlim_t size = l->l_proc->p_rlimit[RLIMIT_STACK].rlim_cur;

	if (size > IRIX_STACK_MAX)
		return EOPNOTSUPP;
	return irix_spawn(l, SCARG(uap, entry), SCARG(uap, flags), SCARG(uap, arg),
	    0, size, irix_o32_child_return, rval);
}

int
irix_sprocsp(struct lwp *l, const void *args, register_t *rval)
{
	const struct irix_sprocsp_args *uap = args;

	return irix_spawn(l, SCARG(uap, entry), SCARG(uap, flags), SCARG(uap, arg),
	    SCARG(uap, sp), SCARG(uap, len), irix_o32_child_return, rval);
}
