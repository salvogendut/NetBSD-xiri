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

#ifndef _SGIMIPS_HEARTREG_H_
#define	_SGIMIPS_HEARTREG_H_

#define	HEART_PIU_SIZE		0x00070000

#define	HEART_MODE		0x00000000
#define	HEART_STATUS		0x00000080
#define	HEART_IMR0		0x00010000
#define	HEART_IMR1		0x00010008
#define	HEART_IMR2		0x00010010
#define	HEART_IMR3		0x00010018
#define	HEART_ISR_SET		0x00010020
#define	HEART_ISR_CLEAR		0x00010028
#define	HEART_ISR		0x00010030
#define	HEART_IMSR		0x00010038
#define	HEART_COUNT		0x00020000
#define	HEART_COMPARE		0x00030000

#define	HEART_COUNT_FREQ	12500000ULL
#define	HEART_COUNT_MASK	0x000fffffffffffffULL

#endif /* _SGIMIPS_HEARTREG_H_ */
