/* $NetBSD$ */
/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Xiri contributors.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <dev/arcbios/arcbios.h>
#include <machine/arcs64.h>

__CTASSERT(offsetof(struct arcbios_spb, FirmwareVector) == 64);
__CTASSERT(offsetof(struct arcbios_component, Identifier) == 48);
__CTASSERT(offsetof(struct arcbios_mem, BasePage) == 8);
__CTASSERT(sizeof(struct arcbios_mem) == 24);
__CTASSERT(offsetof(struct arcbios_fv, Write) == 27 * 8);

char sgimips_arcs64_bootstack[SGIMIPS_ARCS64_BOOTSTACK_SIZE]
    __attribute__((__aligned__(16)));
