#ifndef _IRIX_O32_TTY_H_
#define _IRIX_O32_TTY_H_
struct lwp;
void irix_tty_open(struct lwp *, int, int);
int irix_tty_ioctl(struct lwp *, int, uint32_t, uint32_t, register_t *);
#endif
