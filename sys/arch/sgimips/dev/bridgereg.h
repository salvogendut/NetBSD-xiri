/*	$NetBSD$	*/

/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by the Xiri project.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _SGIMIPS_BRIDGEREG_H_
#define	_SGIMIPS_BRIDGEREG_H_

#define	BRIDGE_WIDGET_ID	0x0004
#define	BRIDGE_PART		0xc002
#define	BRIDGE_MANUFACTURER	0x036

#define	BRIDGE_CONFIG0_BASE	0x020000
#define	BRIDGE_CONFIG0_SIZE	0x0008000
#define	BRIDGE_SLOT_STRIDE	0x0001000
#define	BRIDGE_FUNC_STRIDE	0x0000100

#define	BRIDGE_INTR_STATUS	0x0104
#define	BRIDGE_INTR_ERROR_MASK	0xffffff00U
#define	BRIDGE_INTR_ENABLE	0x010c
#define	BRIDGE_INTR_RESET	0x0114
#define	BRIDGE_INTR_MODE	0x011c
#define	BRIDGE_INTR_DEVICE	0x0124
#define	BRIDGE_INTR_ADDR0	0x0134

#define	BRIDGE_DEVICE0		0x0204
#define	BRIDGE_DEVICE_STRIDE	0x0008

#define	XIO_WIDGET0_BASE	0x10000000
#define	XBOW_ID			0x0004
#define	XBOW_LINK_BASE		0x0100
#define	XBOW_LINK_STRIDE	0x0040
#define	XBOW_LINK_STATUS	0x0014
#define	XBOW_LINK_ALIVE		0x80000000U

/* PCI ID register value: device in bits 31:16, vendor in bits 15:0. */
#define	BRIDGE_EXPECT_SCSI0	0x10201077U
#define	BRIDGE_EXPECT_SCSI1	0x10201077U
#define	BRIDGE_EXPECT_IOC3	0x000310a9U
#define	BRIDGE_EXPECT_RAD	0x000510a9U

#endif /* _SGIMIPS_BRIDGEREG_H_ */
