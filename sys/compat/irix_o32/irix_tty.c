/* Xiri terminal attributes: measured IRIX 6.5.22 wire interfaces, backed
 * by the native PTY line discipline. No foreign ioctl is forwarded as-is. */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <sys/vnode.h>
#include <miscfs/specfs/specdev.h>
#include <uvm/uvm_extern.h>
#include <compat/irix_o32/irix_tty.h>

struct tty_flag { uint32_t foreign; tcflag_t native; };
static const struct tty_flag input_flags[] = {
	{ 0x1, IGNBRK }, { 0x2, BRKINT }, { 0x4, IGNPAR },
	{ 0x8, PARMRK }, { 0x10, INPCK }, { 0x20, ISTRIP },
	{ 0x40, INLCR }, { 0x80, IGNCR }, { 0x100, ICRNL },
	{ 0x400, IXON }, { 0x800, IXANY }, { 0x1000, IXOFF },
	{ 0x2000, IMAXBEL }
};
static const struct tty_flag output_flags[] = {
	{ 0x1, OPOST }, { 0x4, ONLCR }, { 0x8, OCRNL },
	{ 0x10, ONOCR }, { 0x20, ONLRET }, { 0x1800, OXTABS }
};
static const struct tty_flag control_flags[] = {
	{ 0x40, CSTOPB }, { 0x80, CREAD }, { 0x100, PARENB },
	{ 0x200, PARODD }, { 0x400, HUPCL }, { 0x800, CLOCAL },
	{ 0x200000, CRTSCTS }
};
static const struct tty_flag local_flags[] = {
	{ 0x1, ISIG }, { 0x2, ICANON }, { 0x8, ECHO },
	{ 0x10, ECHOE }, { 0x20, ECHOK }, { 0x40, ECHONL },
	{ 0x80, NOFLSH }, { 0x100, IEXTEN }, { 0x200, ECHOCTL },
	{ 0x400, ECHOPRT }, { 0x800, ECHOKE }, { 0x2000, FLUSHO },
	{ 0x4000, PENDIN }, { 0x8000, TOSTOP }
};
static const int cc_map[16] = {
	VINTR, VQUIT, VERASE, VKILL, VEOF, VEOL, VEOL2, -1,
	VSTART, VSTOP, VSUSP, VDSUSP, VREPRINT, VDISCARD, VWERASE, VLNEXT
};
static const int speeds[16] = {
	0, 50, 75, 110, 134, 150, 200, 300,
	600, 1200, 1800, 2400, 4800, 9600, 19200, 38400
};

static uint32_t
flags_out(tcflag_t value, const struct tty_flag *map, size_t n)
{
	uint32_t result = 0;
	for (size_t i = 0; i < n; i++)
		if ((value & map[i].native) == map[i].native)
			result |= map[i].foreign;
	return result;
}

/* Preserve native flags which have no foreign representation. Reject
 * unsupported foreign modes rather than reporting settings we cannot apply. */
static int
flags_in(uint32_t value, tcflag_t *native, const struct tty_flag *map, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		*native &= ~map[i].native;
		if ((value & map[i].foreign) == map[i].foreign) {
			*native |= map[i].native;
			value &= ~map[i].foreign;
		}
	}
	return value == 0 ? 0 : EINVAL;
}
#define OUT(v, map) flags_out(v, map, __arraycount(map))
#define IN(v, p, map) flags_in(v, p, map, __arraycount(map))

static uint32_t
speed_code(int speed)
{
	for (unsigned i = 0; i < __arraycount(speeds); i++)
		if (speeds[i] == speed)
			return i;
	return 10; /* IRIX's legacy unrepresentable-speed code. */
}

/* System V open semantics, restricted to native PTY slave vnodes. The
 * native tty ioctl decides whether this session may acquire the device. */
void
irix_tty_open(struct lwp *l, int fd, int flags)
{
	extern const struct cdevsw pts_cdevsw;
	struct proc *p = l->l_proc;
	file_t *fp;
	bool acquire;

	if (flags & O_NOCTTY)
		return;
	mutex_enter(&proc_lock);
	acquire = SESS_LEADER(p) && !(p->p_lflag & PL_CONTROLT);
	mutex_exit(&proc_lock);
	if (!acquire)
		return;
	fp = fd_getfile(fd);
	if (fp != NULL) {
		if (fp->f_type == DTYPE_VNODE && fp->f_vnode->v_type == VCHR &&
		    cdevsw_lookup(fp->f_vnode->v_rdev) == &pts_cdevsw)
			(void)(*fp->f_ops->fo_ioctl)(fp, TIOCSCTTY, NULL);
		fd_putfile(fd);
	}
}

int
irix_tty_ioctl(struct lwp *l, int fd, uint32_t command, uint32_t address,
    register_t *rval)
{
	extern const struct cdevsw ptc_cdevsw, pts_cdevsw;
	const struct cdevsw *device;
	struct termios native;
	uint32_t packet[12] = { 0 };
	uint8_t *chars;
	unsigned operation, offset, bytes;
	bool modern = false, line = false, group = false;
	file_t *fp;
	int error;
	u_long request = 0;

	/* Both ABIs use four 32-bit flags and 23 byte-sized controls.
	 * termio adds c_line; modern interfaces also add two 32-bit speeds. */
	switch (command) {
	case 0x7416: /* terminal session ID */
		group = true; operation = 0; request = TIOCGSID; break;
	case 0x40047477U:
		group = true; operation = 0; request = TIOCGPGRP; break;
	case 0x80047476U:
		group = true; operation = 1; request = TIOCSPGRP; break;
	case 0x7414: case 0x7415: /* Not IRIX's foreground-group selectors. */
		group = true; operation = 0; break;
	case 0x5401: case 0x5402: case 0x5403: case 0x5404:
		modern = false; line = true; operation = command - 0x5401; break;
	case 0x540d: case 0x540e: case 0x540f: case 0x5410:
		modern = false; line = false; operation = command - 0x540d; break;
	case 0x54c9: case 0x54ca: case 0x54cb: case 0x54cc:
		modern = true; line = true; operation = command - 0x54c9; break;
	case 0x54d5: case 0x54d6: case 0x54d7: case 0x54d8:
		modern = true; line = false; operation = command - 0x54d5; break;
	default:
		return ENOSYS;
	}
	bytes = modern ? 48 : 40;
	offset = (modern ? 24 : 16) + (line ? 1 : 0);
	chars = (uint8_t *)packet + offset;
	fp = fd_getfile(fd);
	if (fp == NULL)
		return EBADF;
	if ((fp->f_flag & (FREAD | FWRITE)) == 0) {
		error = EBADF;
		goto out;
	}
	if (fp->f_type != DTYPE_VNODE || fp->f_vnode->v_type != VCHR) {
		error = fp->f_type == DTYPE_VNODE &&
		    (fp->f_vnode->v_type == VREG || fp->f_vnode->v_type == VDIR) ?
		    ENOTTY : EINVAL;
		goto out;
	}
	device = cdevsw_lookup(fp->f_vnode->v_rdev);
	if (device != &ptc_cdevsw && device != &pts_cdevsw) {
		error = EINVAL;
		goto out;
	}
	if (group) {
		int32_t id;
		CTASSERT(sizeof(id) == sizeof(pid_t));
		if (request == 0) {
			error = EINVAL;
			goto out;
		}
		if (device == &ptc_cdevsw) {
			error = ENOTTY;
			goto out;
		}
		if (operation != 0) {
			if (address > 0x80000000U - sizeof(id)) {
				error = EFAULT;
				goto out;
			}
			error = copyin((void *)(uintptr_t)address, &id, sizeof(id));
			if (error != 0)
				goto out;
		}
		error = (*fp->f_ops->fo_ioctl)(fp, request, &id);
		if (error == 0 && operation == 0) {
			if (address > 0x80000000U - sizeof(id))
				error = EFAULT;
			else
				error = copyout(&id, (void *)(uintptr_t)address, sizeof(id));
		}
		goto out;
	}
	if (address >= 0x80000000U || bytes > 0x80000000U - address) {
		error = EFAULT;
		goto out;
	}
	if (operation != 0) {
		error = copyin((void *)(uintptr_t)address, packet, bytes);
		if (error != 0)
			goto out;
	}
	error = (*fp->f_ops->fo_ioctl)(fp, TIOCGETA, &native);
	if (error != 0)
		goto out;
	if (operation == 0) {
		packet[0] = OUT(native.c_iflag, input_flags);
		packet[1] = OUT(native.c_oflag, output_flags);
		packet[2] = OUT(native.c_cflag, control_flags) |
		    ((native.c_cflag & CSIZE) >> 4);
		packet[3] = OUT(native.c_lflag, local_flags);
		if (modern) {
			packet[4] = native.c_ospeed;
			packet[5] = native.c_ospeed;
		} else {
			uint32_t speed = speed_code(native.c_ospeed);
			packet[2] |= speed | (speed << 16);
		}
		if (line)
			((uint8_t *)packet)[offset - 1] = 1;
		for (unsigned i = 0; i < __arraycount(cc_map); i++) {
			int slot = cc_map[i];
			if (slot < 0)
				continue;
			if (!(native.c_lflag & ICANON) && (i == 4 || i == 5)) {
				chars[i] = native.c_cc[i == 4 ? VMIN : VTIME];
				continue;
			}
			chars[i] = native.c_cc[slot] == _POSIX_VDISABLE ?
			    0 : native.c_cc[slot];
		}
		/* Modern termio leaves the accessible prefix unchanged on a
		 * fault. The other measured getters permit a partial copy. */
		if (modern && line) {
			struct vm_map *map = &l->l_proc->p_vmspace->vm_map;
			bool writable;
			vm_map_lock_read(map);
			writable = uvm_map_checkprot(map, address, address + bytes,
			    VM_PROT_WRITE);
			vm_map_unlock_read(map);
			if (!writable) {
				error = EFAULT;
				goto out;
			}
		}
		error = copyout(packet, (void *)(uintptr_t)address, bytes);
		goto out;
	}
	/* Ignore the obsolete c_line byte, as the measured PTY driver does.
	 * Reserved character slots have no native terminal function. */
	if (chars[7] != 0) {
		error = EINVAL;
		goto out;
	}
	for (unsigned i = 16; i < 23; i++) {
		if (chars[i] != 0) {
			error = EINVAL;
			goto out;
		}
	}
	if (IN(packet[0], &native.c_iflag, input_flags) ||
	    IN(packet[1], &native.c_oflag, output_flags) ||
	    IN(packet[2] & ~0xf003fU, &native.c_cflag, control_flags) ||
	    IN(packet[3], &native.c_lflag, local_flags)) {
		error = EINVAL;
		goto out;
	}
	native.c_cflag = (native.c_cflag & ~CSIZE) | ((packet[2] & 0x30) << 4);
	if (modern && packet[4] > 500000) {
		error = EINVAL;
		goto out;
	}
	native.c_ospeed = modern ? (int)packet[4] : speeds[packet[2] & 15];
	native.c_ispeed = native.c_ospeed;
	for (unsigned i = 0; i < __arraycount(cc_map); i++) {
		int slot = cc_map[i];
		if (slot < 0)
			continue;
		if (!(native.c_lflag & ICANON) && (i == 4 || i == 5)) {
			native.c_cc[i == 4 ? VMIN : VTIME] = chars[i];
			continue;
		}
		native.c_cc[slot] = chars[i] == 0 ? _POSIX_VDISABLE : chars[i];
	}
	request = operation == 1 ? TIOCSETA : operation == 2 ? TIOCSETAW : TIOCSETAF;
	error = (*fp->f_ops->fo_ioctl)(fp, request, &native);
out:
	fd_putfile(fd);
	if (error == EPASSTHROUGH)
		error = ENOTTY;
	if (error == 0)
		*rval = 0;
	return error;
}
