# Xiri IRIX o32/n32 personality for a big-endian N64 MIPS kernel.

.include "../Makefile.inc"

KMOD=	compat_irix_o32

.if ${MACHINE_ARCH} != "mips64eb"
.error "compat_irix_o32 modules require the big-endian N64 MIPS module loader"
.endif

CPPFLAGS+=	-DEXEC_ELF32 -DCOMPAT_NETBSD32
.PATH:	${S}/compat/irix_o32
SRCS=	irix_o32.c irix_signal.c irix_sproc.c irix_thread.c irix_tty.c irix_hdsp.c
SRCS+=	irix_hdsp_events.c irix_hdsp_pcm.c irix_cdrom.c irix_usema.c

.include <bsd.kmodule.mk>
