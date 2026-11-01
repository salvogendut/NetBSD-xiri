# $NetBSD$
# SPDX-License-Identifier: GPL-2.0-only
.include <bsd.own.mk>

PROG=	mount_xfs
BINDIR=	/usr/sbin
MAN=	mount_xfs.8
SRCS=	mount_xfs.c xfs_sb.c xfs_probe.c xfs_cksum.c xfs_io.c xfs_ag.c \
	xfs_read_log.c xfs_read_btree.c xfs_read_inode.c xfs_inode_stat.c xfs_read_attr.c xfs_read_attr_external.c xfs_read_acl.c \
	xfs_read_file.c xfs_read_symlink.c xfs_read_directory.c xfs_read_dir_index.c
.PATH:	${.CURDIR}/../netbsd
# Keep the pinned Linux definitions pristine; check our own code with WARNS.
CPPFLAGS+= -I${.CURDIR}/../include -isystem ${.CURDIR}/../dist -Wno-system-headers
DPADD+=	${LIBPUFFS} ${LIBUTIL}
LDADD+=	-lpuffs -lutil
WARNS=	5

.include <bsd.prog.mk>
