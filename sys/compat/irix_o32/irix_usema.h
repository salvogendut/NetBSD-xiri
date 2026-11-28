#ifndef _COMPAT_IRIX_O32_IRIX_USEMA_H_
#define _COMPAT_IRIX_O32_IRIX_USEMA_H_

#define IRIX_UIOCABLOCKQ	0x75730c
#define IRIX_UIOCAUNBLOCKQ	0x75730d
#define IRIX_UIOCIDADDR		0x75730e
#define IRIX_UIOCGETCOUNT	0x757310

int irix_usema_init(void);
int irix_usema_quiesce(void);
void irix_usema_resume(void);
void irix_usema_fini(void);
void irix_usema_leave(struct proc *);
int irix_usema_ioctl(int, uint32_t, void *, register_t *);
int irix_usema_fchmod(struct lwp *, const void *, register_t *);

#endif
