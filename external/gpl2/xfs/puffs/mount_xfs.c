/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
/* Experimental read-only PUFFS server. All XFS processing is in this process. */
#include <sys/types.h>
#include <sys/file.h>
#include <sys/extattr.h>
#include <sys/stat.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <puffs.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xfs_directory.h"
#include "xfs_log.h"
#include "xfs_attr.h"

#define NODE_BUCKETS 256
#define MOUNT_FLAGS (MNT_RDONLY | MNT_NODEV | MNT_NOSUID | MNT_NOATIME)

struct xfs_node {
	struct xfs_node *next;
	struct puffs_node *pn;
	struct xfs_inode_image inode;
};

struct xfs_server {
	int fd;
	struct stat original;
	struct xfs_reader reader;
	struct xfs_node *nodes[NODE_BUCKETS];
	uint8_t *scratch;
	size_t capacity;
	bool stale;
};

/* Advisory locking is supplemental: callers must supply a quiescent image.
 * Once a change is observed, reject further service, including cached inodes.
 */
static int
image_unchanged(struct xfs_server *fs)
{
	struct stat now;
	if (fs->stale)
		return ESTALE;
	if (fstat(fs->fd, &now) == -1)
		return errno;
	if (now.st_size != fs->original.st_size ||
	    now.st_mtim.tv_sec != fs->original.st_mtim.tv_sec ||
	    now.st_mtim.tv_nsec != fs->original.st_mtim.tv_nsec ||
	    now.st_ctim.tv_sec != fs->original.st_ctim.tv_sec ||
	    now.st_ctim.tv_nsec != fs->original.st_ctim.tv_nsec) {
		fs->stale = true;
		return ESTALE;
	}
	return 0;
}

static int
image_read(void *cookie, uint64_t offset, void *buffer, size_t length, size_t *done)
{
	struct xfs_server *fs = cookie;
	ssize_t n;
	int error = image_unchanged(fs);
	if (error != 0)
		return error;
	do {
		n = pread(fs->fd, buffer, length, (off_t)offset);
	} while (n == -1 && errno == EINTR);
	if (n == -1)
		return errno;
	*done = (size_t)n;
	return image_unchanged(fs);
}

static int
io_error(int error)
{
	/* A bad on-disk structure is an I/O error at the filesystem boundary. */
	return error == EINVAL || error == EMSGSIZE ? EIO : error;
}

static int
attributes(struct xfs_server *fs, struct xfs_node *node, struct vattr *va,
    const char **why)
{
	struct xfs_inode_stat st;
	int error = xfs_inode_getattr(&fs->reader, &node->inode, &st, why);
	if (error != 0)
		return error;
	error = xfs_attrs_check_readonly(&fs->reader, &node->inode,
	    fs->scratch, fs->capacity, why);
	if (error != 0)
		return error;
	if (!S_ISREG(node->inode.mode) && !S_ISDIR(node->inode.mode) && !S_ISLNK(node->inode.mode)) {
		*why = "only regular files, directories and symlinks are supported";
		return EOPNOTSUPP;
	}
	if (st.uid == UINT32_MAX || st.gid == UINT32_MAX || st.nlink == 0 ||
	    st.nlink > INT32_MAX || (st.flags & ~XFS_DIFLAG_ANY)) {
		*why = "invalid inode ownership, link count or flags";
		return EINVAL;
	}
	memset(va, 0, sizeof(*va));
	va->va_type = puffs_mode2vt(node->inode.mode);
	va->va_mode = node->inode.mode & 07777;
	va->va_nlink = st.nlink;
	va->va_uid = st.uid;
	va->va_gid = st.gid;
	va->va_fileid = node->inode.number;
	va->va_size = node->inode.size;
	va->va_blocksize = fs->reader.sb.sb_blocksize;
	va->va_bytes = st.allocated_bytes;
	va->va_gen = st.generation;
	va->va_filerev = st.change_count;
	va->va_rdev = PUFFS_VNOVAL;
#define SET_TIME(field, value) do { \
	va->field.tv_sec = (time_t)(value).seconds; \
	if ((int64_t)va->field.tv_sec != (value).seconds) { \
		*why = "inode timestamp exceeds host time_t"; \
		return EOVERFLOW; \
	} \
	va->field.tv_nsec = (value).nanoseconds; \
} while (0)
	SET_TIME(va_atime, st.atime);
	SET_TIME(va_mtime, st.mtime);
	SET_TIME(va_ctime, st.ctime);
	if (st.has_birthtime) {
		SET_TIME(va_birthtime, st.birthtime);
	} else {
		/* V1/v2 inodes have no birth time; never substitute ctime. */
		va->va_birthtime.tv_sec = PUFFS_VNOVAL;
		va->va_birthtime.tv_nsec = PUFFS_VNOVAL;
	}
#undef SET_TIME
	if (st.flags & XFS_DIFLAG_IMMUTABLE)
		va->va_flags |= UF_IMMUTABLE;
	if (st.flags & XFS_DIFLAG_APPEND)
		va->va_flags |= UF_APPEND;
	if (st.flags & XFS_DIFLAG_NODUMP)
		va->va_flags |= UF_NODUMP;
	return 0;
}

/* Callbacks never yield and use synchronous pread: one event loop owns scratch. */
static int
get_node(struct puffs_usermount *pu, uint64_t ino, struct puffs_node **result,
    const char **why)
{
	struct xfs_server *fs = puffs_getspecific(pu);
	struct xfs_node *node;
	struct vattr va;
	unsigned int bucket = ino % NODE_BUCKETS;
	int error;

	for (node = fs->nodes[bucket]; node != NULL; node = node->next) {
		if (node->inode.number == ino) {
			*result = node->pn;
			return 0;
		}
	}
	node = calloc(1, sizeof(*node));
	if (node == NULL) {
		*why = "cannot allocate inode workspace";
		return ENOMEM;
	}
	error = xfs_read_inode(&fs->reader, ino, fs->scratch, fs->capacity, &node->inode, why);
	if (error == 0)
		error = attributes(fs, node, &va, why);
	if (error != 0) {
		free(node);
		return error;
	}
	node->pn = puffs_pn_new(pu, node);
	if (node->pn == NULL) {
		free(node);
		*why = "cannot allocate PUFFS node";
		return ENOMEM;
	}
	node->pn->pn_va = va;
	node->pn->pn_size = node->inode.size;
	node->next = fs->nodes[bucket];
	fs->nodes[bucket] = node;
	*result = node->pn;
	return 0;
}

static int
node_access(struct puffs_usermount *pu, puffs_cookie_t cookie, int mode,
    const struct puffs_cred *cred)
{
	struct puffs_node *pn = cookie;
	int error = image_unchanged(puffs_getspecific(pu));
	if (error != 0)
		return error;
	if (mode & PUFFS_VWRITE)
		return EROFS;
	return puffs_access(pn->pn_va.va_type, pn->pn_va.va_mode,
	    pn->pn_va.va_uid, pn->pn_va.va_gid, mode, cred);
}

static int
node_lookup(struct puffs_usermount *pu, puffs_cookie_t cookie,
    struct puffs_newinfo *newinfo, const struct puffs_cn *cn)
{
	struct xfs_server *fs = puffs_getspecific(pu);
	struct puffs_node *dir = cookie, *pn;
	struct xfs_node *node = dir->pn_data;
	uint64_t ino;
	const char *why;
	int error;

	if (dir->pn_va.va_type != VDIR)
		return ENOTDIR;
	if (cn->pcn_nameiop != NAMEI_LOOKUP)
		return EROFS;
	error = node_access(pu, cookie, PUFFS_VEXEC, cn->pcn_cred);
	if (error != 0)
		return error;
	error = xfs_directory_lookup(&fs->reader, &node->inode, cn->pcn_name,
	    cn->pcn_namelen, fs->scratch, fs->capacity, &ino, &why);
	if (error == 0)
		error = get_node(pu, ino, &pn, &why);
	if (error != 0)
		return io_error(error);
	puffs_newinfo_setcookie(newinfo, pn);
	puffs_newinfo_setvtype(newinfo, pn->pn_va.va_type);
	puffs_newinfo_setsize(newinfo, pn->pn_va.va_size);
	puffs_newinfo_setrdev(newinfo, pn->pn_va.va_rdev);
	puffs_newinfo_setva(newinfo, &pn->pn_va);
	return 0;
}

static int
node_getattr(struct puffs_usermount *pu, puffs_cookie_t cookie,
    struct vattr *va, const struct puffs_cred *cred __unused)
{
	struct puffs_node *pn = cookie;
	int error = image_unchanged(puffs_getspecific(pu));
	if (error == 0)
		*va = pn->pn_va;
	return error;
}

static int
node_open(struct puffs_usermount *pu, puffs_cookie_t cookie, int flags,
    const struct puffs_cred *cred)
{
	int mode = 0;
	if (flags & (FWRITE | O_TRUNC))
		return EROFS;
	if (flags & FREAD)
		mode |= PUFFS_VREAD;
	if (flags & O_EXEC)
		mode |= PUFFS_VEXEC;
	return node_access(pu, cookie, mode, cred);
}

static int
attr_access(struct puffs_usermount *pu, puffs_cookie_t cookie, int ns,
    const struct puffs_cred *cred)
{
	int error = image_unchanged(puffs_getspecific(pu));
	if (error != 0)
		return error;
	if (ns == EXTATTR_NAMESPACE_SYSTEM)
		return puffs_cred_isjuggernaut(cred) ? 0 : EPERM;
	if (ns == EXTATTR_NAMESPACE_USER)
		return node_access(pu, cookie, PUFFS_VREAD, cred);
	return EINVAL;
}

struct attr_get {
	uint8_t flags;
	const char *name;
	size_t namelen, length, capacity, copied;
	uint8_t *data;
	bool found;
};

static int
find_attr(void *cookie, uint8_t flags, const uint8_t *name, size_t namelen,
    const uint8_t *value, size_t length)
{
	struct attr_get *get = cookie;
	if (flags == get->flags && namelen == get->namelen &&
	    memcmp(name, get->name, namelen) == 0) {
		get->found = true;
		get->length = length;
		get->copied = get->capacity < length ? get->capacity : length;
		if (get->copied != 0)
			memcpy(get->data, value, get->copied);
	}
	return 0;
}

static int
node_getextattr(struct puffs_usermount *pu, puffs_cookie_t cookie, int ns,
    const char *name, size_t *size, uint8_t *data, size_t *resid,
    const struct puffs_cred *cred)
{
	struct xfs_server *fs = puffs_getspecific(pu);
	struct puffs_node *pn = cookie;
	struct xfs_node *node = pn->pn_data;
	struct attr_get get = { .name = name, .namelen = strlen(name),
	    .data = data, .capacity = data != NULL && resid != NULL ? *resid : 0,
	    .flags = ns == EXTATTR_NAMESPACE_USER ? 0 : XFS_ATTR_ROOT };
	const char *why;
	int error = attr_access(pu, cookie, ns, cred);
	if (error != 0)
		return error;
	if (get.namelen == 0 || get.namelen > UINT8_MAX)
		return EINVAL;
	error = xfs_read_attrs(&fs->reader, &node->inode, fs->scratch, fs->capacity,
	    find_attr, &get, &why);
	if (error != 0)
		return io_error(error);
	if (!get.found)
		return ENODATA;
	if (size != NULL)
		*size = get.length;
	/* Native extattr_get_* allows a truncated value, like read(2).
	 * The callback copies while remote-value staging is still valid. */
	if (data != NULL && resid != NULL)
		*resid -= get.copied;
	return 0;
}

struct attr_list {
	uint8_t flags, *data;
	bool prefix;
	size_t remaining, total;
};

static void
append_attr_bytes(struct attr_list *list, const uint8_t *data, size_t length)
{
	size_t copy = list->remaining < length ? list->remaining : length;
	if (copy != 0) {
		memcpy(list->data, data, copy);
		list->data += copy;
		list->remaining -= copy;
	}
	list->total += length;
}

static int
list_attr(void *cookie, uint8_t flags, const uint8_t *name, size_t namelen,
    const uint8_t *value __unused, size_t length __unused)
{
	struct attr_list *list = cookie;
	uint8_t marker = list->prefix ? (uint8_t)namelen : 0;
	if (flags != list->flags)
		return 0;
	if (list->prefix)
		append_attr_bytes(list, &marker, 1);
	append_attr_bytes(list, name, namelen);
	if (!list->prefix)
		append_attr_bytes(list, &marker, 1);
	return 0;
}

static int
node_listextattr(struct puffs_usermount *pu, puffs_cookie_t cookie, int ns,
    size_t *size, uint8_t *data, size_t *resid, int flags,
    const struct puffs_cred *cred)
{
	struct xfs_server *fs = puffs_getspecific(pu);
	struct puffs_node *pn = cookie;
	struct xfs_node *node = pn->pn_data;
	struct attr_list list = { .data = data,
	    .remaining = data != NULL && resid != NULL ? *resid : 0,
	    .prefix = (flags & PUFFS_EXTATTR_LIST_LENPREFIX) != 0,
	    .flags = ns == EXTATTR_NAMESPACE_USER ? 0 : XFS_ATTR_ROOT };
	const char *why;
	int error = attr_access(pu, cookie, ns, cred);
	if (error != 0)
		return error;
	if (flags & ~PUFFS_EXTATTR_LIST_LENPREFIX)
		return EINVAL;
	error = xfs_read_attrs(&fs->reader, &node->inode, fs->scratch, fs->capacity,
	    list_attr, &list, &why);
	if (error != 0)
		return io_error(error);
	if (size != NULL)
		*size = list.total;
	if (resid != NULL)
		*resid = list.remaining;
	return 0;
}

static int
node_read(struct puffs_usermount *pu, puffs_cookie_t cookie, uint8_t *data,
    off_t offset, size_t *resid, const struct puffs_cred *cred __unused,
    int flags __unused)
{
	struct xfs_server *fs = puffs_getspecific(pu);
	struct puffs_node *pn = cookie;
	struct xfs_node *node = pn->pn_data;
	const char *why;
	size_t done;
	int error;
	if (offset < 0)
		return EINVAL;
	if (pn->pn_va.va_type != VREG)
		return pn->pn_va.va_type == VDIR ? EISDIR : EINVAL;
	error = image_unchanged(fs);
	if (error == 0) {
		error = xfs_read_file(&fs->reader, &node->inode, offset, data,
		    *resid, &done, fs->scratch, fs->capacity, &why);
		if (error == 0)
			*resid -= done;
	}
	return io_error(error);
}

static int
node_readlink(struct puffs_usermount *pu, puffs_cookie_t cookie,
    const struct puffs_cred *cred __unused, char *data, size_t *length)
{
	struct xfs_server *fs = puffs_getspecific(pu);
	struct puffs_node *pn = cookie;
	struct xfs_node *node = pn->pn_data;
	const char *why;
	size_t done;
	int error;
	if (pn->pn_va.va_type != VLNK)
		return EINVAL;
	error = image_unchanged(fs);
	if (error == 0) {
		error = xfs_read_file(&fs->reader, &node->inode, 0, data,
		    *length, &done, fs->scratch, fs->capacity, &why);
		if (error == 0)
			*length = done;
	}
	return io_error(error);
}

struct directory_read {
	struct dirent *dent;
	off_t index, start, next;
	size_t remaining, cookie_capacity, count;
	off_t *cookies;
	bool full;
};

static int
emit_entry(void *cookie, uint64_t ino, uint8_t type, const uint8_t *name, size_t length)
{
	static const uint8_t types[] = {
		DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK, DT_WHT
	};
	struct directory_read *read = cookie;
	char terminated[256];
	size_t record_size;
	if (read->index == INT64_MAX)
		return EOVERFLOW;
	if (read->index++ < read->start || read->full)
		return 0;
	if (length == 0 || length >= sizeof(terminated) || type >= __arraycount(types))
		return EIO;
	memcpy(terminated, name, length);
	terminated[length] = '\0';
	record_size = _DIRENT_RECLEN(read->dent, length);
	if ((read->cookies != NULL && read->count == read->cookie_capacity) ||
	    record_size > read->remaining) {
		read->full = true;
		return 0; /* Still verify the rest of the directory before publishing. */
	}
	memset(read->dent, 0, record_size);
	if (!puffs_nextdent(&read->dent, terminated, ino, types[type], &read->remaining))
		return EIO;
	read->next = read->index;
	if (read->cookies != NULL)
		read->cookies[read->count] = read->next;
	read->count++;
	return 0;
}

static int
node_readdir(struct puffs_usermount *pu, puffs_cookie_t cookie, struct dirent *dent,
    off_t *offset, size_t *resid, const struct puffs_cred *cred __unused,
    int *eof, off_t *cookies, size_t *ncookies)
{
	struct xfs_server *fs = puffs_getspecific(pu);
	struct puffs_node *pn = cookie;
	struct xfs_node *node = pn->pn_data;
	struct directory_read read = { .dent = dent, .start = *offset, .next = *offset,
	    .remaining = *resid, .cookies = cookies, .cookie_capacity = *ncookies };
	const char *why;
	int error;
	if (*offset < 0)
		return EINVAL;
	if (pn->pn_va.va_type != VDIR)
		return ENOTDIR;
	error = image_unchanged(fs);
	if (error == 0)
		error = xfs_read_directory(&fs->reader, &node->inode, fs->scratch,
		    fs->capacity, emit_entry, &read, &why);
	if (error != 0)
		return io_error(error);
	if (read.start > read.index || (read.full && read.count == 0))
		return EINVAL;
	*offset = read.next;
	*resid = read.remaining;
	*ncookies = cookies != NULL ? read.count : 0;
	*eof = !read.full;
	return 0;
}

static void
free_node(struct xfs_server *fs, struct xfs_node *node)
{
	struct xfs_node **link = &fs->nodes[node->inode.number % NODE_BUCKETS];
	while (*link != node)
		link = &(*link)->next;
	*link = node->next;
	puffs_pn_put(node->pn);
	free(node);
}

static int
node_reclaim(struct puffs_usermount *pu, puffs_cookie_t cookie)
{
	struct puffs_node *pn = cookie;
	/* libpuffs handles lookup/reclaim races before calling this hook. */
	if (pn != puffs_getroot(pu))
		free_node(puffs_getspecific(pu), pn->pn_data);
	return 0;
}

static int
fs_statvfs(struct puffs_usermount *pu, struct puffs_statvfs *st)
{
	struct xfs_server *fs = puffs_getspecific(pu);
	const struct xfs_sb *sb = &fs->reader.sb;
	int error = image_unchanged(fs);
	if (error != 0)
		return error;
	if (sb->sb_logblocks > sb->sb_dblocks ||
	    sb->sb_fdblocks > sb->sb_dblocks - sb->sb_logblocks ||
	    sb->sb_ifree > sb->sb_icount)
		return EIO;
	memset(st, 0, sizeof(*st));
	st->f_flag = MOUNT_FLAGS;
	st->f_bsize = st->f_frsize = st->f_iosize = sb->sb_blocksize;
	st->f_blocks = sb->sb_dblocks - sb->sb_logblocks;
	st->f_bfree = sb->sb_fdblocks;
	st->f_files = sb->sb_icount;
	st->f_ffree = sb->sb_ifree;
	/* Read-only: no blocks or inode slots available for new allocations. */
	st->f_namemax = 255;
	return 0;
}

static int
fs_sync(struct puffs_usermount *pu, int waitfor __unused,
    const struct puffs_cred *cred __unused)
{
	return image_unchanged(puffs_getspecific(pu));
}

static int
fs_unmount(struct puffs_usermount *pu __unused, int flags __unused)
{
	/* No writeback and no private open references to drain. */
	return 0;
}

static int
readonly_create(struct puffs_usermount *pu __unused, puffs_cookie_t cookie __unused,
    struct puffs_newinfo *newinfo __unused, const struct puffs_cn *cn __unused,
    const struct vattr *va __unused)
{ return EROFS; }

static int
readonly_remove(struct puffs_usermount *pu __unused, puffs_cookie_t dir __unused,
    puffs_cookie_t node __unused, const struct puffs_cn *cn __unused)
{ return EROFS; }

static int
readonly_rename(struct puffs_usermount *pu __unused, puffs_cookie_t srcdir __unused,
    puffs_cookie_t src __unused, const struct puffs_cn *srccn __unused,
    puffs_cookie_t dstdir __unused, puffs_cookie_t dst __unused,
    const struct puffs_cn *dstcn __unused)
{ return EROFS; }

static int
readonly_symlink(struct puffs_usermount *pu __unused, puffs_cookie_t dir __unused,
    struct puffs_newinfo *ni __unused, const struct puffs_cn *cn __unused,
    const struct vattr *va __unused, const char *target __unused)
{ return EROFS; }

static int
readonly_setattr(struct puffs_usermount *pu __unused, puffs_cookie_t cookie __unused,
    const struct vattr *va __unused, const struct puffs_cred *cred __unused)
{ return EROFS; }

static int
readonly_write(struct puffs_usermount *pu __unused, puffs_cookie_t cookie __unused,
    uint8_t *data __unused, off_t offset __unused, size_t *resid __unused,
    const struct puffs_cred *cred __unused, int flags __unused)
{ return EROFS; }

static int
readonly_space(struct puffs_usermount *pu __unused, puffs_cookie_t cookie __unused,
    off_t offset __unused, off_t length __unused)
{ return EROFS; }

static int
readonly_setextattr(struct puffs_usermount *pu __unused, puffs_cookie_t cookie __unused,
    int ns __unused, const char *name __unused, uint8_t *data __unused,
    size_t *resid __unused, const struct puffs_cred *cred __unused)
{ return EROFS; }

static int
readonly_delextattr(struct puffs_usermount *pu __unused, puffs_cookie_t cookie __unused,
    int ns __unused, const char *name __unused, const struct puffs_cred *cred __unused)
{ return EROFS; }

static void
usage(void)
{
	fprintf(stderr, "usage: mount_xfs [-s] [-o ro] image mountpoint\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct xfs_server *fs;
	struct puffs_usermount *pu;
	struct puffs_ops *ops;
	struct puffs_node *root;
	struct xfs_io io;
	const char *why = NULL;
	char *image;
	bool foreground = false;
	int ch, error;
	unsigned int i;

	setprogname(argv[0]);
	while ((ch = getopt(argc, argv, "so:")) != -1) {
		switch (ch) {
		case 's': foreground = true; break;
		case 'o':
			if (strcmp(optarg, "ro") != 0)
				errx(2, "only -o ro is supported");
			break;
		default: usage();
		}
	}
	if (argc - optind != 2)
		usage();
	image = realpath(argv[optind], NULL);
	if (image == NULL)
		err(1, "image path");
	fs = calloc(1, sizeof(*fs));
	if (fs == NULL)
		err(1, "server allocation");
	fs->fd = open(image, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fs->fd == -1 || fstat(fs->fd, &fs->original) == -1)
		err(1, "open image");
	if (!S_ISREG(fs->original.st_mode) || fs->original.st_size < 0)
		errx(1, "input must be a quiescent regular image file");
	if (flock(fs->fd, LOCK_SH | LOCK_NB) == -1)
		err(1, "lock image");
	fs->capacity = XFS_ATTR_WORKSPACE_SIZE;
	fs->scratch = malloc(fs->capacity);
	if (fs->scratch == NULL)
		err(1, "reader workspace");
	io = (struct xfs_io){ fs, (uint64_t)fs->original.st_size, image_read };
	error = xfs_reader_open(&io, fs->scratch, fs->capacity, &fs->reader, &why);
	if (error == 0)
		error = xfs_require_clean_log(&fs->reader, fs->scratch, fs->capacity, &why);
	if (error != 0)
		errx(1, "%s: %s", why, strerror(error));
	if (xfs_directory_scratch_size(&fs->reader) > fs->capacity)
		errx(1, "unsupported directory workspace size");
	if (fs->reader.sb.sb_features_ro_compat & XFS_SB_FEAT_RO_COMPAT_UNKNOWN)
		errx(1, "unknown read-only-compatible filesystem feature");
	ops = calloc(1, sizeof(*ops));
	if (ops == NULL)
		err(1, "operations allocation");
	ops->puffs_fs_unmount = fs_unmount;
	ops->puffs_fs_sync = fs_sync;
	ops->puffs_fs_statvfs = fs_statvfs;
	ops->puffs_node_lookup = node_lookup;
	ops->puffs_node_getattr = node_getattr;
	ops->puffs_node_access = node_access;
	ops->puffs_node_open = node_open;
	ops->puffs_node_read = node_read;
	ops->puffs_node_readlink = node_readlink;
	ops->puffs_node_readdir = node_readdir;
	ops->puffs_node_reclaim = node_reclaim;
	ops->puffs_node_getextattr = node_getextattr;
	ops->puffs_node_listextattr = node_listextattr;
	ops->puffs_node_create = readonly_create;
	ops->puffs_node_mknod = readonly_create;
	ops->puffs_node_mkdir = readonly_create;
	ops->puffs_node_remove = readonly_remove;
	ops->puffs_node_rmdir = readonly_remove;
	ops->puffs_node_link = readonly_remove;
	ops->puffs_node_rename = readonly_rename;
	ops->puffs_node_symlink = readonly_symlink;
	ops->puffs_node_setattr = readonly_setattr;
	ops->puffs_node_write = readonly_write;
	ops->puffs_node_fallocate = readonly_space;
	ops->puffs_node_fdiscard = readonly_space;
	ops->puffs_node_setextattr = readonly_setextattr;
	ops->puffs_node_deleteextattr = readonly_delextattr;
	pu = puffs_init(ops, image, "xfs", fs, PUFFS_KFLAG_NOCACHE);
	if (pu == NULL)
		err(1, "puffs_init");
	error = get_node(pu, fs->reader.sb.sb_rootino, &root, &why);
	if (error != 0)
		errx(1, "root inode: %s: %s", why, strerror(error));
	if (root->pn_va.va_type != VDIR)
		errx(1, "root inode is not a directory");
	puffs_setroot(pu, root);
	puffs_setrootinfo(pu, VDIR, root->pn_va.va_size, 0);
	if (puffs_unmountonsignal(SIGINT, true) == -1 ||
	    puffs_unmountonsignal(SIGTERM, true) == -1)
		err(1, "signal handling");
	if (!foreground && puffs_daemon(pu, 1, 0) == -1)
		err(1, "puffs_daemon");
	if (puffs_mount(pu, argv[optind + 1], MOUNT_FLAGS, root) == -1)
		err(1, "puffs_mount");
	error = puffs_mainloop(pu);
	if (error == -1)
		warn("puffs_mainloop");
	for (i = 0; i < NODE_BUCKETS; i++)
		while (fs->nodes[i] != NULL)
			free_node(fs, fs->nodes[i]);
	close(fs->fd);
	free(fs->scratch);
	free(fs);
	free(image);
	return error == -1 ? 1 : 0;
}
