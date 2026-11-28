/* IRIX pollable user semaphores. Each open creates a private descriptor. */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kauth.h>
#include <sys/kmem.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscallargs.h>
#include <sys/systm.h>
#include <sys/vmem.h>

#include <compat/irix_o32/irix_usema.h>

struct waiter {
	TAILQ_ENTRY(waiter) link;
	struct proc *p;
	bool ready;
};

struct usema {
	LIST_ENTRY(usema) link;
	TAILQ_HEAD(, waiter) waiters;
	struct selinfo sel;
	vmem_addr_t id;
	mode_t mode;
	unsigned queued;
	unsigned preposts;
};

static LIST_HEAD(, usema) semaphores = LIST_HEAD_INITIALIZER(semaphores);
static kmutex_t lock;
static vmem_t *ids;
static devmajor_t cmajor;
static bool stopping;

static struct waiter *
find_waiter(struct usema *s, struct proc *p)
{
	struct waiter *w;

	TAILQ_FOREACH(w, &s->waiters, link)
		if (w->p == p)
			break;
	return w;
}

/* Remove process identities before they can be reused after exit or exec. */
void
irix_usema_leave(struct proc *p)
{
	struct usema *s;
	struct waiter *w;

	mutex_enter(&lock);
	LIST_FOREACH(s, &semaphores, link) {
		if ((w = find_waiter(s, p)) != NULL) {
			if (!w->ready)
				s->queued--;
			TAILQ_REMOVE(&s->waiters, w, link);
			kmem_free(w, sizeof(*w));
		}
	}
	mutex_exit(&lock);
}

static int
usema_poll(file_t *fp, int events)
{
	struct usema *s = fp->f_data;
	struct waiter *w;
	int ready = 0;

	mutex_enter(&lock);
	w = find_waiter(s, curproc);
	if (w != NULL && w->ready)
		ready = events & (POLLIN | POLLRDNORM);
	else if (events & (POLLIN | POLLRDNORM))
		selrecord(curlwp, &s->sel);
	mutex_exit(&lock);
	return ready;
}

static int
usema_close(file_t *fp)
{
	struct usema *s = fp->f_data;
	struct waiter *w;

	mutex_enter(&lock);
	LIST_REMOVE(s, link);
	while ((w = TAILQ_FIRST(&s->waiters)) != NULL) {
		TAILQ_REMOVE(&s->waiters, w, link);
		kmem_free(w, sizeof(*w));
	}
	seldestroy(&s->sel);
	vmem_free(ids, s->id, 1);
	kmem_free(s, sizeof(*s));
	mutex_exit(&lock);
	return 0;
}

static int
usema_stat(file_t *fp, struct stat *st)
{
	struct usema *s = fp->f_data;

	memset(st, 0, sizeof(*st));
	mutex_enter(&lock);
	st->st_mode = S_IFCHR | s->mode;
	mutex_exit(&lock);
	st->st_uid = kauth_cred_geteuid(fp->f_cred);
	st->st_gid = kauth_cred_getegid(fp->f_cred);
	st->st_rdev = makedev(cmajor, s->id);
	st->st_ino = s->id;
	st->st_nlink = 1;
	return 0;
}

static const struct fileops usema_ops = {
	.fo_name = "xiriusema", .fo_read = fbadop_read, .fo_write = fbadop_write,
	.fo_ioctl = fbadop_ioctl, .fo_fcntl = fnullop_fcntl, .fo_poll = usema_poll,
	.fo_stat = usema_stat, .fo_close = usema_close,
	.fo_kqfilter = fnullop_kqfilter, .fo_restart = fnullop_restart,
};

/* The two IRIX integer arguments have the native syscall slot layout. */
int
irix_usema_fchmod(struct lwp *l, const void *args, register_t *rval)
{
	const struct sys_fchmod_args *uap = args;
	const int fd = SCARG(uap, fd);
	file_t *fp;
	int error = 0;

	if ((fp = fd_getfile(fd)) == NULL)
		return EBADF;
	if (fp->f_ops != &usema_ops) {
		/* Keep this reference while native fchmod obtains its own. */
		error = sys_fchmod(l, uap, rval);
	} else if (kauth_cred_geteuid(l->l_cred) != kauth_cred_geteuid(fp->f_cred)
	    && kauth_authorize_generic(l->l_cred, KAUTH_GENERIC_ISSUSER, NULL)) {
		error = EPERM;
	} else {
		struct usema *s = fp->f_data;
		mutex_enter(&lock);
		s->mode = SCARG(uap, mode) & ACCESSPERMS;
		mutex_exit(&lock);
		*rval = 0;
	}
	fd_putfile(fd);
	return error;
}

static int
usema_open(dev_t dev, int flags, int fmt, struct lwp *l)
{
	struct usema *s;
	file_t *fp;
	int fd, error;

	if (minor(dev) != 0)
		return ENXIO;
	if ((error = fd_allocfile(&fp, &fd)) != 0)
		return error;
	s = kmem_zalloc(sizeof(*s), KM_SLEEP);
	TAILQ_INIT(&s->waiters);
	selinit(&s->sel);
	s->mode = 0600;
	mutex_enter(&lock);
	error = stopping ? ENXIO :
	    vmem_alloc(ids, 1, VM_NOSLEEP | VM_INSTANTFIT, &s->id);
	if (error == 0)
		LIST_INSERT_HEAD(&semaphores, s, link);
	mutex_exit(&lock);
	if (error) {
		seldestroy(&s->sel);
		kmem_free(s, sizeof(*s));
		fd_abort(l->l_proc, fp, fd);
		return error;
	}
	return fd_clone(fp, fd, flags, &usema_ops, s);
}

static const struct cdevsw usema_cdevsw = {
	.d_open = usema_open, .d_close = nullclose, .d_read = noread,
	.d_write = nowrite, .d_ioctl = noioctl, .d_stop = nostop, .d_tty = notty,
	.d_poll = nopoll, .d_mmap = nommap, .d_kqfilter = nokqfilter,
	.d_discard = nodiscard, .d_flag = D_OTHER | D_MPSAFE,
};

int
irix_usema_init(void)
{
	devmajor_t b = NODEVMAJOR;
	int error;

	cmajor = NODEVMAJOR;
	mutex_init(&lock, MUTEX_DEFAULT, IPL_NONE);
	stopping = true;
	ids = vmem_create("xiriusema", 1, 0x3ffff, 1, NULL, NULL, NULL, 0,
	    VM_SLEEP, IPL_NONE);
	error = devsw_attach("xiriusema", NULL, &b, &usema_cdevsw, &cmajor);
	if (error) {
		vmem_destroy(ids);
		mutex_destroy(&lock);
	}
	return error;
}

int
irix_usema_quiesce(void)
{
	mutex_enter(&lock);
	bool busy = !LIST_EMPTY(&semaphores);
	stopping = !busy;
	mutex_exit(&lock);
	return busy ? EBUSY : 0;
}

void
irix_usema_resume(void)
{
	mutex_enter(&lock);
	stopping = false;
	mutex_exit(&lock);
}

void
irix_usema_fini(void)
{
	devsw_detach(NULL, &usema_cdevsw);
	vmem_destroy(ids);
	mutex_destroy(&lock);
}

int
irix_usema_ioctl(int fd, uint32_t cmd, void *arg, register_t *rval)
{
	file_t *fp = fd_getfile(fd);
	struct usema *s;
	struct waiter *w, *spare = NULL;
	int error = 0;

	if (fp == NULL)
		return EBADF;
	if (fp->f_ops != &usema_ops) {
		fd_putfile(fd);
		return ENOTTY;
	}
	s = fp->f_data;
	*rval = 0;
	if (cmd == IRIX_UIOCIDADDR) {
		/* A clone and its inherited descriptors identify the same queue.
		 * The owner-field address is used by IRIX for separately attached
		 * clients; this driver only supports descriptor inheritance. */
		uint32_t id[2];
		error = copyin(arg, id, sizeof(id));
		fd_putfile(fd);
		return error;
	}
	if (cmd == IRIX_UIOCABLOCKQ)
		spare = kmem_alloc(sizeof(*spare), KM_SLEEP);
	mutex_enter(&lock);
	switch (cmd) {
	case IRIX_UIOCABLOCKQ:
		if (s->preposts != 0) {
			s->preposts--;
			error = EAGAIN;
			break;
		}
		w = find_waiter(s, curproc);
		if (w == NULL) {
			w = spare;
			spare = NULL;
			w->p = curproc;
		} else if (!w->ready) {
			error = ERANGE;
			break;
		} else
			TAILQ_REMOVE(&s->waiters, w, link);
		w->ready = false;
		TAILQ_INSERT_TAIL(&s->waiters, w, link);
		s->queued++;
		break;
	case IRIX_UIOCAUNBLOCKQ:
		TAILQ_FOREACH(w, &s->waiters, link)
			if (!w->ready)
				break;
		if (w == NULL) {
			if (s->preposts == INT_MAX)
				error = EOVERFLOW;
			else {
				s->preposts++;
				error = EAGAIN;
			}
			break;
		}
		w->ready = true;
		s->queued--;
		selnotify(&s->sel, POLLIN | POLLRDNORM, 0);
		break;
	case IRIX_UIOCGETCOUNT:
		*rval = (register_t)s->preposts - (register_t)s->queued;
		break;
	default:
		error = ENOTTY;
	}
	mutex_exit(&lock);
	if (spare)
		kmem_free(spare, sizeof(*spare));
	fd_putfile(fd);
	return error;
}
