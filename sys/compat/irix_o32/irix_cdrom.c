/* SGI devscsi flat-buffer interface for read-only optical media commands. */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/disk.h>
#include <sys/buf.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/ktrace.h>
#include <sys/proc.h>
#include <sys/scsiio.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <miscfs/specfs/specdev.h>

#include <dev/dkvar.h>
#include <dev/scsipi/scsipiconf.h>
#include <dev/scsipi/cdvar.h>
#include <compat/irix_o32/irix_cdrom.h>

/* The desktop media service manages cd0. Report the actual SCSI address,
 * even with an empty tray, and leave other bus types out of this inventory. */
bool
irix_cdrom_inventory(int32_t *bus, int32_t *target, int32_t *state)
{
	device_t dev = device_find_by_driver_unit_acquire("cd", 0);
	bool found = false;

	if (dev == NULL)
		return false;
	if (device_is_a(device_parent(dev), "scsibus")) {
		struct cd_softc *cd = device_private(dev);
		struct scsipi_periph *p = cd->sc_periph;
		if (p != NULL && p->periph_lun == 0) {
			*bus = device_unit(device_parent(dev));
			*target = p->periph_target;
			*state = (p->periph_version & 7) |
			    ((p->periph_flags & PERIPH_REMOVABLE) ? 0x80 : 0);
			found = true;
		}
	}
	device_release(dev);
	return found;
}

struct irix_dsreq {
	uint32_t flags, timeout, private, command;
	uint8_t cmdlen, pad1[3];
	uint32_t data, datalen, sense;
	uint8_t senselen, pad2[3];
	uint32_t iov;
	uint16_t iovlen, pad3;
	uint32_t link;
	uint16_t synch;
	uint8_t revision, ret, status, message, cmdsent, pad4;
	uint32_t datasent;
	uint8_t sensesent, pad5[3];
};
CTASSERT(sizeof(struct irix_dsreq) == 64);
CTASSERT(offsetof(struct irix_dsreq, ret) == 51);
CTASSERT(offsetof(struct irix_dsreq, datasent) == 56);

#define IRIX_DSRQ_SENSE 0x02U
#define IRIX_DSRQ_READ  0x100U
#define IRIX_DSRQ_WRITE 0x200U

/* Keep command direction independent of caller-supplied SGI flags. */
static size_t
cd_read_cmdlen(uint8_t op)
{
	switch (op) {
	case 0x00: /* TEST UNIT READY */
	case 0x03: /* REQUEST SENSE */
	case 0x12: /* INQUIRY */
	case 0x1a: /* MODE SENSE(6) */
	case 0x0b: /* SEEK(6) */
		return 6;
	case 0x25: /* READ CAPACITY */
	case 0x28: /* READ(10) */
	case 0x2b: /* SEEK(10) */
	case 0x42: /* READ SUB-CHANNEL */
	case 0x43: /* READ TOC */
	case 0x5a: /* MODE SENSE(10) */
		return 10;
	case 0xbe: /* READ CD */
		return 12;
	default:
		return 0;
	}
}

int
irix_cdrom_enter(int fd, void *arg)
{
	struct irix_dsreq ds;
	scsireq_t req = { 0 };
	file_t *fp;
	int error;

	if ((fp = fd_getfile(fd)) == NULL)
		return EBADF;
	if (fp->f_type != DTYPE_VNODE || fp->f_vnode->v_type != VCHR ||
	    major(fp->f_vnode->v_rdev) != devsw_name2chr("cd", NULL, 0)) {
		error = ENOTTY;
		goto out;
	}
	if ((fp->f_flag & FREAD) == 0) {
		error = EBADF;
		goto out;
	}
	if ((error = copyin(arg, &ds, sizeof(ds))) != 0)
		goto out;
	if (ds.cmdlen == 0 || ds.cmdlen > sizeof(req.cmd) || ds.datalen > MAXPHYS ||
	    ds.iovlen != 0 || ds.link != 0 ||
	    (ds.flags & ~(IRIX_DSRQ_SENSE | IRIX_DSRQ_READ | IRIX_DSRQ_WRITE))) {
		error = EINVAL;
		goto out;
	}
	if ((error = copyin((void *)(uintptr_t)ds.command, req.cmd, ds.cmdlen)) != 0)
		goto out;
	ktrkuser("irix_cd_cdb", req.cmd, ds.cmdlen);
	if (ds.cmdlen != cd_read_cmdlen(req.cmd[0]) ||
	    (ds.datalen && (ds.flags & (IRIX_DSRQ_READ | IRIX_DSRQ_WRITE)) != IRIX_DSRQ_READ)) {
		error = EOPNOTSUPP;
		goto out;
	}
	req.flags = SCCMD_READ;
	req.cmdlen = ds.cmdlen;
	req.datalen = ds.datalen;
	req.databuf = (void *)(uintptr_t)ds.data;
	req.timeout = ds.timeout ? uimin(ds.timeout, 60000) : 1000;
	if (ds.flags & IRIX_DSRQ_SENSE)
		req.senselen = uimin(ds.senselen, sizeof(req.sense));
	error = (*fp->f_ops->fo_ioctl)(fp, SCIOCCOMMAND, &req);
	if (error)
		goto out;
	ds.status = req.status;
	ds.message = 0;
	ds.cmdsent = ds.cmdlen;
	ds.datasent = req.datalen_used;
	ds.sensesent = uimin(req.senselen, req.senselen_used);
	switch (req.retsts) {
	case SCCMD_OK: ds.ret = ds.datasent < ds.datalen ? 0x35 : 0; break;
	case SCCMD_SENSE: ds.ret = ds.sensesent ? 0x33 : 0x32; break;
	case SCCMD_TIMEOUT: ds.ret = 0x31; break;
	case SCCMD_BUSY: ds.ret = 0x43; break;
	default: ds.ret = 0x37; break;
	}
	if (ds.sensesent) {
		error = copyout(req.sense, (void *)(uintptr_t)ds.sense, ds.sensesent);
		if (error)
			goto out;
	}
	error = copyout(&ds, arg, sizeof(ds));
out:
	fd_putfile(fd);
	return error;
}
