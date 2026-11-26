/* Pollable, per-open IRIX Audio Library gain-change subscriptions. */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kmem.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <compat/irix_o32/irix_hdsp.h>

#define EVENT_NEXT	101
#define EVENT_INIT	110
#define EVENT_SELECT	112
#define EVENT_DESELECT	113
#define EVENT_FLUSH	116
#define EVENT_PENDING	118
#define GAIN_MASK	(UINT64_C(1) << (AL_GAIN & 63))

static const uint32_t resources[] = {
	AL_SYSTEM, INDY_SUBSYSTEM, ANALOG_OUT, AL_DEFAULT_OUTPUT, SPEAKER
};

struct hdsp_event {
	uint32_t source, resource, param, subsystem;
	uint64_t time, value;
	uint32_t data[8];
};

/* If a slow subscriber fills its ring, discard its oldest observation. */
#define QUEUE_SIZE 32
struct hdsp_queue {
	LIST_ENTRY(hdsp_queue) entries;
	struct selinfo sel;
	kmutex_t reader;
	struct hdsp_event events[QUEUE_SIZE];
	uint64_t head, tail;
	uint64_t masks[__arraycount(resources)];
};

static LIST_HEAD(, hdsp_queue) queues = LIST_HEAD_INITIALIZER(queues);
static kmutex_t queue_lock;
static bool stopping;

static int
queue_poll(file_t *fp, int events)
{
	struct hdsp_queue *q = fp->f_data;
	int ready = 0;

	mutex_enter(&queue_lock);
	if (q->head != q->tail)
		ready = events & (POLLIN | POLLRDNORM);
	else if (events & (POLLIN | POLLRDNORM))
		selrecord(curlwp, &q->sel);
	mutex_exit(&queue_lock);
	return ready;
}

static int
queue_close(file_t *fp)
{
	struct hdsp_queue *q = fp->f_data;

	mutex_enter(&queue_lock);
	LIST_REMOVE(q, entries);
	seldestroy(&q->sel);
	mutex_destroy(&q->reader);
	kmem_free(q, sizeof(*q));
	mutex_exit(&queue_lock);
	return 0;
}

static const struct fileops queue_ops = {
	.fo_name = "xirihdsp", .fo_read = fbadop_read, .fo_write = fbadop_write,
	.fo_ioctl = fbadop_ioctl, .fo_fcntl = fnullop_fcntl, .fo_poll = queue_poll,
	.fo_stat = fbadop_stat, .fo_close = queue_close,
	.fo_kqfilter = fnullop_kqfilter, .fo_restart = fnullop_restart,
};

static int
queue_open(dev_t dev, int flags, int fmt, struct lwp *l)
{
	struct hdsp_queue *q;
	file_t *fp;
	int fd, error;

	if (minor(dev) != 0)
		return ENXIO;
	error = fd_allocfile(&fp, &fd);
	if (error)
		return error;
	q = kmem_zalloc(sizeof(*q), KM_SLEEP);
	selinit(&q->sel);
	mutex_init(&q->reader, MUTEX_DEFAULT, IPL_NONE);
	mutex_enter(&queue_lock);
	if (stopping) {
		mutex_exit(&queue_lock);
		seldestroy(&q->sel);
		mutex_destroy(&q->reader);
		kmem_free(q, sizeof(*q));
		fd_abort(l->l_proc, fp, fd);
		return ENXIO;
	}
	LIST_INSERT_HEAD(&queues, q, entries);
	mutex_exit(&queue_lock);
	return fd_clone(fp, fd, flags, &queue_ops, q);
}

static const struct cdevsw queue_cdevsw = {
	.d_open = queue_open, .d_close = nullclose, .d_read = noread,
	.d_write = nowrite, .d_ioctl = noioctl, .d_stop = nostop, .d_tty = notty,
	.d_poll = nopoll, .d_mmap = nommap, .d_kqfilter = nokqfilter,
	.d_discard = nodiscard, .d_flag = D_OTHER | D_MPSAFE,
};

int
irix_hdsp_events_init(void)
{
	devmajor_t b = NODEVMAJOR, c = NODEVMAJOR;
	int error;

	mutex_init(&queue_lock, MUTEX_DEFAULT, IPL_NONE);
	/* Publish the exec handler before admitting the first open. */
	stopping = true;
	error = devsw_attach("xirihdsp", NULL, &b, &queue_cdevsw, &c);
	if (error)
		mutex_destroy(&queue_lock);
	return error;
}

int
irix_hdsp_events_quiesce(void)
{
	bool busy;

	mutex_enter(&queue_lock);
	stopping = LIST_EMPTY(&queues);
	busy = !stopping;
	mutex_exit(&queue_lock);
	return busy ? EBUSY : 0;
}

void
irix_hdsp_events_resume(void)
{
	mutex_enter(&queue_lock);
	stopping = false;
	mutex_exit(&queue_lock);
}

void
irix_hdsp_events_fini(void)
{
	devsw_detach(NULL, &queue_cdevsw);
	mutex_destroy(&queue_lock);
}

void
irix_hdsp_gain_changed(void)
{
	struct hdsp_queue *q;
	struct timespec now;

	nanouptime(&now);
	mutex_enter(&queue_lock);
	LIST_FOREACH(q, &queues, entries) {
		for (unsigned i = 0; i < __arraycount(resources); i++) {
			if (!(q->masks[i] & GAIN_MASK))
				continue;
			if (q->tail - q->head == QUEUE_SIZE)
				q->head++;
			q->events[q->tail++ % QUEUE_SIZE] = (struct hdsp_event){
			    .source = SPEAKER, .resource = resources[i],
			    .param = AL_GAIN, .subsystem = INDY_SUBSYSTEM,
			    .time = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec,
			    .data = {0, 2} };
			selnotify(&q->sel, POLLIN | POLLRDNORM, 0);
		}
	}
	mutex_exit(&queue_lock);
}

int
irix_hdsp_event_ioctl(int fd, uint32_t cmd, void *address, register_t *rval)
{
	struct { uint32_t resource, pad; uint64_t mask; } sub;
	struct hdsp_event event;
	struct hdsp_queue *q;
	uint64_t sequence;
	file_t *fp;
	int error = 0, pending;
	unsigned i;

	CTASSERT(sizeof(event) == 64);
	fp = fd_getfile(fd);
	if (fp == NULL)
		return EBADF;
	if (fp->f_ops != &queue_ops) {
		error = ENOSYS;
		goto out;
	}
	if (!(fp->f_flag & FREAD)) {
		error = EBADF;
		goto out;
	}
	q = fp->f_data;
	*rval = 0;
	mutex_enter(&q->reader);
	switch (cmd) {
	case EVENT_INIT:
	case EVENT_FLUSH:
		mutex_enter(&queue_lock);
		q->head = q->tail;
		if (cmd == EVENT_INIT)
			memset(q->masks, 0, sizeof(q->masks));
		mutex_exit(&queue_lock);
		break;
	case EVENT_SELECT:
	case EVENT_DESELECT:
		error = copyin(address, &sub, sizeof(sub));
		if (error)
			break;
		for (i = 0; i < __arraycount(resources); i++)
			if (resources[i] == sub.resource)
				break;
		if (i == __arraycount(resources)) {
			error = ENODEV;
			break;
		}
		mutex_enter(&queue_lock);
		if (cmd == EVENT_SELECT)
			q->masks[i] |= sub.mask;
		else
			q->masks[i] &= ~sub.mask;
		mutex_exit(&queue_lock);
		break;
	case EVENT_PENDING:
		mutex_enter(&queue_lock);
		pending = q->tail - q->head;
		mutex_exit(&queue_lock);
		error = copyout(&pending, address, sizeof(pending));
		break;
	case EVENT_NEXT:
		mutex_enter(&queue_lock);
		if (q->head == q->tail) {
			mutex_exit(&queue_lock);
			error = EAGAIN;
			break;
		}
		sequence = q->head;
		event = q->events[sequence % QUEUE_SIZE];
		mutex_exit(&queue_lock);
		error = copyout(&event, address, sizeof(event));
		mutex_enter(&queue_lock);
		if (!error && q->head == sequence)
			q->head++;
		mutex_exit(&queue_lock);
		break;
	default:
		error = ENOTTY;
	}
	mutex_exit(&q->reader);
out:
	fd_putfile(fd);
	return error;
}
