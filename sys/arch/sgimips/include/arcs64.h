/* $NetBSD$ */
/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Xiri contributors.
 */

#ifndef _SGIMIPS_ARCS64_H_
#define _SGIMIPS_ARCS64_H_

#define SGIMIPS_ARCS64_BOOTSTACK_SIZE (16 * 1024)

/* Full-width targets for vectors copied into the low exception block. */
#ifdef _LOCORE
#define SGIMIPS_ARCS64_CPU_INFO_CELL 0x80000ff0
#define SGIMIPS_ARCS64_SLOWFAULT_CELL 0x80000ff8
#else
#define SGIMIPS_ARCS64_CPU_INFO_CELL \
    ((volatile uint64_t *)(uintptr_t)0xffffffff80000ff0ULL)
#define SGIMIPS_ARCS64_SLOWFAULT_CELL \
    ((volatile uint64_t *)(uintptr_t)0xffffffff80000ff8ULL)

extern char sgimips_arcs64_bootstack[];
void sgimips_arcs64_set_slowfault_cell(void);
#endif

#endif
