/* Original IRIX AL shared stereo audio ports on a NetBSD audio track. */
#include <sys/param.h>
#include <sys/atomic.h>
#include <sys/audioio.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <uvm/uvm_extern.h>
#include <dev/audio/audio_if.h>
#include <compat/irix_o32/irix_hdsp.h>

/* One extra stereo frame distinguishes full from empty. All indices count
 * channel samples. The reader owns head and the writer owns tail. */
struct ring {
	uint32_t head, tail, request, fillpoint;
	int32_t pcm[];
};

struct port {
	kmutex_t lock;
	struct selinfo sel;
	struct ring *ring;
	struct uvm_object *object;
	size_t size;
	file_t *audio;
	lwp_t *worker;
	uint32_t cursor, slots;
	bool stereo, record, stop;
	int error;
};

/* The driver owns head during playback and tail during capture. Never use
 * a user-written cursor to address kernel memory. Keep one frame empty. */
static int
available(struct port *p, uint32_t *count)
{
	uint32_t peer = p->record ? atomic_load_acquire(&p->ring->head) :
	    atomic_load_acquire(&p->ring->tail);
	if (peer >= p->slots || (peer & 1))
		return EINVAL;
	*count = (peer + p->slots - p->cursor +
	    (p->record ? p->slots - 2 : 0)) % p->slots;
	return 0;
}

static void
transfer(void *arg)
{
	struct port *p = arg;
	int16_t samples[1024];
	for (;;) {
		uint32_t count;
		mutex_enter(&p->lock);
		if (p->stop) {
			mutex_exit(&p->lock);
			break;
		}
		p->error = available(p, &count);
		if (p->error) {
			selnotify(&p->sel, POLLERR, 0);
			mutex_exit(&p->lock);
			break;
		}
		uint32_t cursor = p->cursor;
		count = MIN(count, p->slots - cursor);
		count = MIN(count, __arraycount(samples));
		mutex_exit(&p->lock);
		if (!count) {
			kpause("hdspidle", false, 1, NULL);
			continue;
		}
		/* AL stores signed 24-bit PCM in the low 24 bits of each word. */
		if (!p->record)
			for (unsigned i = 0; i < count; i++)
				samples[i] = (uint32_t)p->ring->pcm[cursor + i] >> 8;
		struct iovec iov = { samples, count * sizeof(*samples) };
		struct uio uio = { .uio_iov = &iov, .uio_iovcnt = 1,
		    .uio_resid = iov.iov_len, .uio_rw = p->record ? UIO_READ : UIO_WRITE,
		    .uio_vmspace = vmspace_kernel() };
		off_t off = 0;
		int error = p->record ?
		    p->audio->f_ops->fo_read(p->audio, &off, &uio, p->audio->f_cred, 0) :
		    p->audio->f_ops->fo_write(p->audio, &off, &uio, p->audio->f_cred, 0);
		size_t bytes = count * sizeof(*samples) - uio.uio_resid;
		mutex_enter(&p->lock);
		if (bytes % (2 * sizeof(*samples)) || (error && error != EWOULDBLOCK)) {
			p->error = error ? error : EIO;
			selnotify(&p->sel, POLLERR, 0);
			mutex_exit(&p->lock);
			break;
		}
		if (bytes) {
			size_t n = bytes / sizeof(*samples);
			p->cursor = (cursor + n) % p->slots;
			if (p->record) {
				/* SGI's standard mono microphone feeds the left
				 * converter into both slots of the stereo port. */
				bool stereo = irix_hdsp_microphone_channels() == 2;
				for (size_t i = 0; i < n; i += 2) {
					p->ring->pcm[cursor + i] = (int32_t)samples[i] * 256;
					p->ring->pcm[cursor + i + 1] =
					    (int32_t)samples[i + (stereo ? 1 : 0)] * 256;
				}
				atomic_store_release(&p->ring->tail, p->cursor);
				selnotify(&p->sel, POLLIN | POLLRDNORM, 0);
			} else {
				atomic_store_release(&p->ring->head, p->cursor);
				selnotify(&p->sel, POLLOUT | POLLWRNORM, 0);
			}
		}
		mutex_exit(&p->lock);
		if (!bytes)
			kpause("hdspio", false, 1, NULL);
	}
	kthread_exit(0);
}

static int
port_mmap(file_t *fp, off_t *off, size_t len, int prot, int *flags,
    int *advice, struct uvm_object **object, int *maxprot)
{
	struct port *p = fp->f_data;
	vaddr_t addr = 0;
	int error;
	if (*off != 0 || !(*flags & MAP_SHARED) || (prot & VM_PROT_EXECUTE) ||
	    len == 0 || len > 2 * 1024 * 1024)
		return EINVAL;
	mutex_enter(&p->lock);
	if (p->object) {
		error = EBUSY;
		goto out;
	}
	size_t size = round_page(len);
	struct uvm_object *obj = uao_create(size, 0);
	error = uvm_map(kernel_map, &addr, size, obj, 0, 0,
	    UVM_MAPFLAG(UVM_PROT_RW, UVM_PROT_RW, UVM_INH_NONE, UVM_ADV_RANDOM, 0));
	if (error) {
		uao_detach(obj);
		goto out;
	}
	p->size = size;
	p->ring = (void *)addr;
	p->object = obj;
	uao_reference(obj);
	*object = obj;
	*maxprot = UVM_PROT_RW;
	*advice = UVM_ADV_RANDOM;
	*flags = MAP_SHARED;
out:
	mutex_exit(&p->lock);
	return error;
}

static int
port_close(file_t *fp)
{
	struct port *p = fp->f_data;
	if (p->worker) {
		mutex_enter(&p->lock);
		p->stop = true;
		mutex_exit(&p->lock);
		kthread_join(p->worker);
	}
	if (p->audio)
		closef(p->audio);
	if (p->object)
		uvm_unmap(kernel_map, (vaddr_t)p->ring, (vaddr_t)p->ring + p->size);
	seldestroy(&p->sel);
	mutex_destroy(&p->lock);
	kmem_free(p, sizeof(*p));
	irix_hdsp_port_release();
	return 0;
}

static int
port_poll(file_t *fp, int events)
{
	struct port *p = fp->f_data;
	int ready = 0;
	mutex_enter(&p->lock);
	if (p->error)
		ready = POLLERR;
	else if (p->slots) {
		uint32_t count;
		if (available(p, &count))
			ready = POLLERR;
		else if (p->slots - 2 - count >= MAX(2, atomic_load_relaxed(&p->ring->fillpoint)))
			ready = events & (p->record ? (POLLIN | POLLRDNORM) :
			    (POLLOUT | POLLWRNORM));
	}
	if (!ready)
		selrecord(curlwp, &p->sel);
	mutex_exit(&p->lock);
	return ready;
}

static const struct fileops port_ops = {
	.fo_name = "xiri-pcm", .fo_read = fbadop_read, .fo_write = fbadop_write,
	.fo_ioctl = fbadop_ioctl, .fo_fcntl = fnullop_fcntl, .fo_poll = port_poll,
	.fo_stat = fbadop_stat, .fo_close = port_close, .fo_mmap = port_mmap,
	.fo_kqfilter = fnullop_kqfilter, .fo_restart = fnullop_restart,
};

int
irix_hdsp_port_open(int flags)
{
	file_t *fp;
	int fd, error;
	if ((flags & (FREAD | FWRITE)) != (FREAD | FWRITE))
		return EACCES;
	error = fd_allocfile(&fp, &fd);
	if (error)
		return error;
	struct port *p = kmem_zalloc(sizeof(*p), KM_SLEEP);
	mutex_init(&p->lock, MUTEX_DEFAULT, IPL_NONE);
	selinit(&p->sel);
	return fd_clone(fp, fd, flags, &port_ops, p);
}

static int
start_track(struct port *p, unsigned rate)
{
	audio_info_t info;
	audio_device_t device;
	struct stat st;
	file_t *fp;
	int fd, error;

	/* Open through VFS in the client's context for normal native device
	 * permissions and audio(4) multiuser arbitration. Keep a file reference
	 * independent of the application's descriptor table for the worker. */
	int access = p->record ? O_RDONLY : O_WRONLY;
	int flag = p->record ? FREAD : FWRITE;
	error = fd_open("/dev/audio0", access | O_NONBLOCK | O_CLOEXEC | O_CLOFORK, 0, &fd);
	if (error)
		return error;
	fp = fd_getfile(fd);
	if (fp == NULL)
		return EBADF;
	if (fp->f_type != DTYPE_MISC || fp->f_ops->fo_name == NULL ||
	    strcmp(fp->f_ops->fo_name, "audio") != 0 ||
	    (fp->f_flag & (flag | FNONBLOCK)) != (flag | FNONBLOCK) ||
	    fp->f_ops->fo_stat(fp, &st) || minor(st.st_dev) != AUDIO_DEVICE) {
		(void)fd_close(fd);
		return ENODEV;
	}
	mutex_enter(&fp->f_lock);
	fp->f_count++;
	mutex_exit(&fp->f_lock);
	(void)fd_close(fd);
	error = fp->f_ops->fo_ioctl(fp, AUDIO_GETDEV, &device);
	if (!error && strcmp(device.name, "HAL2") != 0)
		error = ENODEV;
	if (!error && rate == 0) {
		error = fp->f_ops->fo_ioctl(fp, AUDIO_GETFORMAT, &info);
		if (!error)
			rate = p->record ? info.record.sample_rate : info.play.sample_rate;
	}
	if (!error) {
		AUDIO_INITINFO(&info);
		audio_prinfo_t *track = p->record ? &info.record : &info.play;
		track->sample_rate = rate;
		track->channels = 2;
		track->precision = 16;
		track->encoding = AUDIO_ENCODING_SLINEAR_BE;
		error = fp->f_ops->fo_ioctl(fp, AUDIO_SETINFO, &info);
	}
	if (!error) {
		p->audio = fp;
		error = kthread_create(PRI_NONE, KTHREAD_MPSAFE | KTHREAD_MUSTJOIN,
		    NULL, transfer, p, &p->worker, "xiripcm");
	}
	if (error) {
		p->audio = NULL;
		closef(fp);
	}
	return error;
}

int
irix_hdsp_pcm_ioctl(int fd, uint32_t cmd, void *arg, register_t *rval)
{
	struct { uint32_t locations; uint8_t direction, pad[3]; } acquire;
	char info[24];
	file_t *fp = fd_getfile(fd);
	int error = 0;
	if (fp == NULL)
		return EBADF;
	if (fp->f_ops != &port_ops) {
		fd_putfile(fd);
		return ENOSYS;
	}
	struct port *p = fp->f_data;
	mutex_enter(&p->lock);
	*rval = 0;
	switch (cmd) {
	case 29: /* SET_RB_FORMAT */
		if ((uintptr_t)arg != 0)
			error = ENXIO;
		break;
	case 10: /* SET_RB_TYPE */
		if ((uintptr_t)arg != 2)
			error = ENXIO;
		else
			p->stereo = true;
		break;
	case 8: /* SET_RB_INFO, descriptive name and AL version */
		error = copyin(arg, info, sizeof(info));
		break;
	case 3: /* ACQUIRE_AUDIO_RB */
		error = copyin(arg, &acquire, sizeof(acquire));
		if (error)
			break;
		if (p->worker || !p->object || !p->stereo ||
		    acquire.direction > 1 || acquire.locations < 8 ||
		    (acquire.locations & 1) || acquire.locations > p->size / 4) {
			error = EINVAL;
			break;
		}
		p->slots = acquire.locations - 4;
		p->record = acquire.direction == 0;
		error = start_track(p, irix_hdsp_rate(p->record));
		if (error)
			p->slots = 0;
		break;
	default:
		error = ENOTTY;
	}
	mutex_exit(&p->lock);
	fd_putfile(fd);
	return error;
}
