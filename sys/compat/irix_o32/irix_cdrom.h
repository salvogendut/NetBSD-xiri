#ifndef _COMPAT_IRIX_O32_CDROM_H_
#define _COMPAT_IRIX_O32_CDROM_H_

/* SGI 32-bit devscsi optical requests. */
#define IRIX_DS_ENTER 0xc0404465U
int irix_cdrom_enter(int, void *);
bool irix_cdrom_inventory(int32_t *, int32_t *, int32_t *);

#endif
