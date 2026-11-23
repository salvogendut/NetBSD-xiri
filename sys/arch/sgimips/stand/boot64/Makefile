#	$NetBSD: Makefile,v 1.15 2024/06/29 08:06:56 rin Exp $

PROG=	ip30boot
NOMAN=	# defined

SRCS=	start64.S boot.c bootinfo.c conf.c devopen.c putchar.c getchar.c \
	disk.c arcbios_calls.S


FILES+=		ip28boot
CLEANFILES+=	boot.map ip28boot ip28boot.map

.include <bsd.init.mk>

# $S must correspond to the top of the 'sys' tree
S=	${.CURDIR}/../../../..

BINMODE?=	444

realall: ${PROG} ip28boot

.PATH:		${.CURDIR}/../common
.PATH.S:	${S}/dev/arcbios
AFLAGS+=	-D_LOCORE -D_KERNEL -mno-abicalls -mabi=64 -D_LP64 -DARCBIOS64
CPPFLAGS+=	-nostdinc -D_STANDALONE -DNO_ABICALLS -I${.OBJDIR} -I${S}
# compiler flags for smallest code size
CFLAGS=		-ffreestanding -Os -Wall -Werror -mno-abicalls -msoft-float -G 1024 -mabi=64 -D_LP64
LDBUG=		-T $S/arch/mips/conf/stand.ldscript
NETBSD_VERS!=	${HOST_SH} ${.CURDIR}/../../../../conf/osrelease.sh
CPPFLAGS+=	-DNETBSD_VERS='"${NETBSD_VERS}"'

CPPFLAGS+=	-Dsgimips -DARCBIOS64

LDFLAGS+=       -m elf64btsmip
LINKFORMAT+=    -m elf64btsmip

.include "${S}/dev/arcbios/Makefile.inc"

# IP28's firmware reserves the first three pages at physical 0x20000000.
# Keep its bootstrap below the kernel address documented in README.IPn.
# Octane desktop kernels start at 0x20010000. Place their bootstrap in the
# free memory above the PROM reservation at 0x20f00000..0x20ffffff.

LOAD_ADDRESS_IP30?=	0xa800000021000000
LOAD_ADDRESS_IP28?=	0xa800000020003000

### find out what to use for libkern
KERN_AS=	library
KERNMISCMAKEFLAGS+=	LD="${LD} -m elf64btsmip" RANLIB="true"
.include "${S}/lib/libkern/Makefile.inc"
LIBKERN=	${KERNLIB}

### find out what to use for libz
Z_AS=		library
ZMISCMAKEFLAGS+=	LD="${LD} -m elf64btsmip" RANLIB="true"
.include "${S}/lib/libz/Makefile.inc"
LIBZ=		${ZLIB}

### find out what to use for libsa
SA_AS=		library
SAMISCMAKEFLAGS+=	SA_USE_LOADFILE=yes SA_USE_CREAD=yes \
			SA_INCLUDE_NET=no \
			LD="${LD} -m elf64btsmip" RANLIB="true"
.include "${S}/lib/libsa/Makefile.inc"
LIBSA=		${SALIB}


LIBS=		${LIBSA} ${LIBZ} ${LIBSA} ${LIBKERN}

ip30boot: ${OBJS} ${LIBS}
	${_MKTARGET_LINK}
	${LD} -N -m elf64btsmip -Map boot.map -x -Ttext ${LOAD_ADDRESS_IP30} \
		${LDBUG} -e start -o ${.TARGET} ${OBJS} ${LIBS}
	${SIZE} ${.TARGET}

cleandir distclean: .WAIT cleanlibdir
cleanlibdir:
	-rm -rf lib

.include "${S}/conf/newvers_stand.mk"

.include <bsd.klinks.mk>
.include <bsd.prog.mk>

# bsd.prog.mk supplies OBJS, including the generated version object.
ip28boot: ${OBJS} ${LIBS}
	${_MKTARGET_LINK}
	${LD} -N -m elf64btsmip -Map ${.TARGET}.map -x \
		-Ttext ${LOAD_ADDRESS_IP28} ${LDBUG} -e start \
		-o ${.TARGET} ${OBJS} ${LIBS}
	${SIZE} ${.TARGET}
