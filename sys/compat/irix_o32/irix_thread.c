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

/* IRIX n32 pthread kernel workers on uniprocessor MIPS. */
#include <sys/param.h>
#include <sys/condvar.h>
#include <sys/kauth.h>
#include <sys/ktrace.h>
#include <sys/kmem.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/syscallargs.h>
#include <sys/systm.h>
#include <uvm/uvm_extern.h>
#include <uvm/uvm_map.h>
#include <mips/locore.h>
#include <mips/regnum.h>
#include <compat/irix_o32/irix_sproc.h>

#define NS_FLAGS 0x020000ffU
#define PRDA_BYTES 4096
#define PRDA_TID_OFFSET 0xe40
#define IRIX_SCHED_RR 2
#define IRIX_RT_MAX 255

struct irix_worker {
	uint32_t prda[PRDA_BYTES / sizeof(uint32_t)];
	uint32_t id;
	unsigned int wakeups;
	kcondvar_t cv;
};

/* Called at IPL_SCHED. Only this process's user threads touch its window.
 * Switching through an unrelated process or the idle LWP needs no copy. */
static void
irix_thread_switchin(struct lwp *l)
{
	struct irix_process *ip = l->l_proc->p_emuldata;
	struct irix_worker *next = l->l_emuldata;
	void *window = (void *)ip->prda_window;

	if (ip->active_worker == next)
		return;
	if (ip->active_worker != NULL)
		memcpy(ip->active_worker->prda, window, PRDA_BYTES);
	memcpy(window, next->prda, PRDA_BYTES);
	ip->active_worker = next;
}

void
irix_thread_exit(struct lwp *l)
{
	struct irix_worker *t = l->l_emuldata;
	struct proc *p = l->l_proc;
	struct irix_process *ip = p->p_emuldata;

	if (t == NULL)
		return;
	mutex_enter(p->p_lock);
	kpreempt_disable();
	lwp_setswitchin(l, NULL);
	if (ip->active_worker == t)
		ip->active_worker = NULL;
	l->l_emuldata = NULL;
	kpreempt_enable();
	mutex_exit(p->p_lock);
	cv_destroy(&t->cv);
	kmem_free(t, sizeof(*t));
}

/* Exec and exit have drained peers before dropping the wired alias. */
void
irix_threads_release(struct proc *p)
{
	struct irix_process *ip = p->p_emuldata;

	if (ip->prda_window == 0)
		return;
	irix_thread_exit(curlwp);
	uvm_unmap(kernel_map, ip->prda_window, ip->prda_window + PAGE_SIZE);
	ip->prda_window = 0;
	rw_destroy(&ip->maplock);
}

static int
irix_thread_init(struct lwp *l)
{
	struct proc *p = l->l_proc;
	struct irix_process *ip = p->p_emuldata;
	struct vm_map *map = &p->p_vmspace->vm_map;
	struct vm_map_entry *e;
	struct irix_worker *t;
	vaddr_t kva;
	int error;

	if (ip->prda_window != 0)
		return 0;
	if (!irix_share_single(p) || ip->share != NULL)
		return EOPNOTSUPP;
	vm_map_lock_read(map);
	bool valid = uvm_map_lookup_entry(map, IRIX_PRDA_ADDRESS, &e) &&
	    e->end >= IRIX_PRDA_ADDRESS + PAGE_SIZE &&
	    e->protection == (VM_PROT_READ | VM_PROT_WRITE) &&
	    e->object.uvm_obj == NULL && e->aref.ar_amap != NULL &&
	    e->inheritance == MAP_INHERIT_COPY;
	vm_map_unlock_read(map);
	if (!valid)
		return EFAULT;
	/* An independently referenced, cache-colour matched alias. The
	 * scheduler must never fault in the backing page at IPL_SCHED. */
	error = uvm_map_extract(map, IRIX_PRDA_ADDRESS, PAGE_SIZE, kernel_map,
	    &kva, UVM_EXTRACT_CONTIG);
	if (error != 0)
		return error;
	error = uvm_map_pageable(kernel_map, kva, kva + PAGE_SIZE, false, 0);
	if (error != 0)
		goto unmap;
	t = kmem_zalloc(sizeof(*t), KM_SLEEP);
	cv_init(&t->cv, "irixth");
	/* Nothing can schedule a peer until the first lwp_start. */
	ip->prda_window = kva;
	ip->active_worker = t;
	l->l_emuldata = t;
	error = lwp_setswitchin(l, irix_thread_switchin);
	if (error == 0) {
		ip->heap_floor = round_page((vaddr_t)p->p_vmspace->vm_daddr +
		    ptoa(p->p_vmspace->vm_dsize));
		rw_init(&ip->maplock);
		return 0;
	}
	ip->prda_window = 0;
	ip->active_worker = NULL;
	l->l_emuldata = NULL;
	cv_destroy(&t->cv);
	kmem_free(t, sizeof(*t));
unmap:
	uvm_unmap(kernel_map, kva, kva + PAGE_SIZE);
	return error;
}

static void
irix_thread_return(void *arg)
{
	md_child_return(curlwp);
}

int
irix_nsproc(struct lwp *l, const void *args, register_t *retval)
{
	const struct irix_nsproc_args *uap = args;
	struct proc *p = l->l_proc;
	struct irix_process *ip = p->p_emuldata;
	struct { uint32_t entry, arg, flags, sp, len; } desc;
	struct { int32_t priority, policy; } sched;
	struct irix_worker *t;
	struct lwp *child;
	vaddr_t uarea;
	uint32_t id, pc = SCARG(uap, entry);
	int error;
	pri_t priority = PRI_NONE;
	bool initialized = ip->prda_window == 0;

	if (p->p_md.md_abi != _MIPS_BSD_API_N32 || !ip->threads ||
	    SCARG(uap, flags) != NS_FLAGS)
		return EOPNOTSUPP;
	if (SCARG(uap, desc) > INT32_MAX || SCARG(uap, sched) > INT32_MAX)
		return EFAULT;
	if ((error = copyin((void *)(uintptr_t)SCARG(uap, desc), &desc,
	    sizeof(desc))) != 0)
		return error;
	if (pc == 0 || pc >= INT32_MAX || (pc & 3) != 0 ||
	    desc.sp < 80 || desc.sp > INT32_MAX || desc.flags != NS_FLAGS)
		return EINVAL;
	if (desc.len != 0)
		return EOPNOTSUPP;
	if (SCARG(uap, sched) != 0) {
		if (kauth_cred_geteuid(l->l_cred) != 0)
			return EPERM;
		if ((error = copyin((void *)(uintptr_t)SCARG(uap, sched),
		    &sched, sizeof(sched))) != 0)
			return error;
		if (KTRPOINT(p, KTR_USER)) {
			char record[64];
			int n = snprintf(record, sizeof(record),
			    "policy=%d priority=%d", sched.policy, sched.priority);
			ktr_kuser("irix_nsproc", record, n);
		}
		/* Vendor libpthread requests round-robin for system scope. */
		if (sched.policy != IRIX_SCHED_RR)
			return EOPNOTSUPP;
		if (sched.priority < 0 || sched.priority > IRIX_RT_MAX)
			return EINVAL;
		priority = PRI_USER_RT + sched.priority * SCHED_PRI_MAX / IRIX_RT_MAX;
		error = kauth_authorize_process(l->l_cred,
		    KAUTH_PROCESS_SCHEDULER_SETPARAM, p, l, KAUTH_ARG(SCHED_RR),
		    KAUTH_ARG(priority));
		if (error != 0)
			return error;
	}
	if ((error = irix_thread_init(l)) != 0)
		return error;
	t = kmem_zalloc(sizeof(*t), KM_SLEEP);
	cv_init(&t->cv, "irixth");
	uarea = uvm_uarea_alloc();
	if (uarea == 0) {
		error = ENOMEM;
		goto free;
	}
	mutex_enter(p->p_lock);
	if (ip->thread_serial == INT32_MAX) {
		mutex_exit(p->p_lock);
		error = EAGAIN;
		goto free_uarea;
	}
	id = ++ip->thread_serial;
	mutex_exit(p->p_lock);
	error = lwp_create(l, p, uarea, LWP_DETACHED, NULL, 0, irix_thread_return,
	    NULL, &child, l->l_class, &l->l_sigmask, &l->l_sigstk);
	if (error != 0)
		goto free_uarea;
	/* The caller's view is installed whenever this LWP executes. */
	memcpy(t->prda, (void *)ip->prda_window, PRDA_BYTES);
	t->id = id;
	t->prda[PRDA_TID_OFFSET / sizeof(uint32_t)] = id;
	struct trapframe *tf = child->l_md.md_utf;
	tf->tf_regs[_R_PC] = pc;
	tf->tf_regs[_R_SP] = (desc.sp & ~15U) - 64;
	tf->tf_regs[_R_RA] = 0;
	tf->tf_regs[_R_A0] = (int32_t)desc.arg;
	tf->tf_regs[_R_A1] = tf->tf_regs[_R_A2] = 0;
	mutex_enter(p->p_lock);
	child->l_emuldata = t;
	if (priority != PRI_NONE) {
		lwp_lock(child);
		child->l_class = SCHED_RR;
		lwp_changepri(child, priority);
		lwp_unlock(child);
	}
	mutex_exit(p->p_lock);
	error = lwp_setswitchin(child, irix_thread_switchin);
	KASSERT(error == 0); /* same kernel as the registered parent */
	lwp_start(child, 0);
	retval[0] = id;
	return 0;
free_uarea:
	uvm_uarea_free(uarea);
free:
	if (initialized)
		irix_threads_release(p);
	cv_destroy(&t->cv);
	kmem_free(t, sizeof(*t));
	return error;
}

int
irix_thread_ctl(struct lwp *l, uint32_t cmd, uint32_t id, register_t *retval)
{
	struct proc *p = l->l_proc;
	struct irix_worker *t = l->l_emuldata;
	struct lwp *peer;
	int error = 0;

	if (t == NULL)
		return ENOSYS;
	*retval = 0;
	if (cmd == 1)
		lwp_exit(l);
	mutex_enter(p->p_lock);
	switch (cmd) {
	case 2: /* PR_THREAD_BLOCK */
		while (t->wakeups == 0 && error == 0)
			error = cv_wait_sig(&t->cv, p->p_lock);
		if (error == 0)
			t->wakeups--;
		break;
	case 3: /* PR_THREAD_UNBLOCK */
		LIST_FOREACH(peer, &p->p_lwps, l_sibling) {
			t = peer->l_emuldata;
			if (t != NULL && t->id == id)
				break;
		}
		if (peer == NULL)
			error = ESRCH;
		else if (t->wakeups == UINT_MAX)
			error = EOVERFLOW;
		else {
			t->wakeups++;
			cv_signal(&t->cv);
		}
		break;
	default:
		error = ENOSYS;
	}
	mutex_exit(p->p_lock);
	return error;
}
