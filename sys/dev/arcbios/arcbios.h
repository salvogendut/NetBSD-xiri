/*	$NetBSD: arcbios.h,v 1.13 2011/02/20 08:02:46 matt Exp $	*/

/*-
 * Copyright (c) 2001 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason R. Thorpe.
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

/*
 * The ARC BIOS (which is similar, but not 100% compatible with SGI ARCS)
 * specification can be found at:
 *
 *	http://www.microsoft.com/hwdev/download/respec/riscspec.zip
 */

#ifndef _ARCBIOS_H_
#define _ARCBIOS_H_

#define	ARCBIOS_STDIN		0
#define	ARCBIOS_STDOUT		1

#define	ARCBIOS_PAGESIZE	4096

#if defined(_KERNEL) && defined(sgimips) && !defined(_STANDALONE)
#include "opt_arcbios64.h"
#endif

/*
 * ARCS64 widens firmware longs and pointers; enums remain 32 bits.
 * Standalone genassym retains the ARC32 vector layout: its ARCS64 assembly
 * wrapper doubles the generated offsets when reading the firmware vector.
 */
#if defined(ARCBIOS64) && !defined(_STANDALONE)
typedef uint64_t arcbios_ulong_t;
typedef int64_t arcbios_long_t;
#else
typedef uint32_t arcbios_ulong_t;
typedef int32_t arcbios_long_t;
#endif

/* ARC BIOS status codes. */
#define	ARCBIOS_ESUCCESS	0	/* Success */
#define	ARCBIOS_E2BIG		1	/* argument list too long */
#define	ARCBIOS_EACCES		2	/* permission denied */
#define	ARCBIOS_EAGAIN		3	/* resource temporarily unavailable */
#define	ARCBIOS_EBADF		4	/* bad file number */
#define	ARCBIOS_EBUSY		5	/* device or resource busy */
#define	ARCBIOS_EFAULT		6	/* bad address */
#define	ARCBIOS_EINVAL		7	/* invalid argument */
#define	ARCBIOS_EIO		8	/* I/O error */
#define	ARCBIOS_EISDIR		9	/* is a directory */
#define	ARCBIOS_EMFILE		10	/* too many open files */
#define	ARCBIOS_EMLINK		11	/* too many links */
#define	ARCBIOS_ENAMETOOLONG	12	/* file name too long */
#define	ARCBIOS_ENODEV		13	/* no such device */
#define	ARCBIOS_ENOENT		14	/* no such file or directory */
#define	ARCBIOS_ENOEXEC		15	/* exec format error */
#define	ARCBIOS_ENOMEM		16	/* out of memory */
#define	ARCBIOS_ENOSPC		17	/* no space left on device */
#define	ARCBIOS_ENOTDIR		18	/* not a directory */
#define	ARCBIOS_ENOTTY		19	/* not a typewriter */
#define	ARCBIOS_ENXIO		20	/* media not loaded */
#define	ARCBIOS_EROFS		21	/* read-only file system */
#if defined(sgimips)
#define	ARCBIOS_EADDRNOTAVAIL	31	/* address not available */
#define	ARCBIOS_ETIMEDOUT	32	/* operation timed out */
#define	ARCBIOS_ECONNABORTED	33	/* connection aborted */
#define	ARCBIOS_ENOCONNECT	34	/* not connected */
#endif /* sgimips */

/*
 * 4.2.2: System Parameter Block
 */
struct arcbios_spb {
	arcbios_ulong_t	SPBSignature;
	arcbios_ulong_t	SPBLength;
	uint16_t	Version;
	uint16_t	Revision;
	arcbios_long_t	RestartBlock;
	arcbios_long_t	DebugBlock;
	arcbios_long_t	GEVector;
	arcbios_long_t	UTLBMissVector;
	arcbios_ulong_t	FirmwareVectorLength;
	arcbios_long_t	FirmwareVector;
	arcbios_ulong_t	PrivateVectorLength;
	arcbios_long_t	PrivateVector;
	arcbios_ulong_t	AdapterCount;
	arcbios_ulong_t	AdapterType;
	arcbios_ulong_t	AdapterVectorLength;
	arcbios_long_t	AdapterVector;
};

#define	ARCBIOS_SPB_SIGNATURE	0x53435241	/* A R C S */
#define	ARCBIOS_SPB_SIGNATURE_1	0x41524353	/* S C R A */

/*
 * 4.2.5: System Configuration Data
 */
struct arcbios_component {
	uint32_t	Class;
	uint32_t	Type;
	uint32_t	Flags;
	uint16_t	Version;
	uint16_t	Revision;
	arcbios_ulong_t	Key;
	arcbios_ulong_t	AffinityMask;
	arcbios_ulong_t	ConfigurationDataSize;
	arcbios_ulong_t	IdentifierLength;
	arcbios_long_t	Identifier;
};

/*
 * SGI ARCS likes to be `special', so it moved some of the class/type
 * numbers around from the ARC standard definitions.
 */
#if defined(sgimips)
/* Component Class */
#define	COMPONENT_CLASS_SystemClass		0
#define	COMPONENT_CLASS_ProcessorClass		1
#define	COMPONENT_CLASS_CacheClass		2
#define	COMPONENT_CLASS_MemoryClass		3
#define	COMPONENT_CLASS_AdapterClass		4
#define	COMPONENT_CLASS_ControllerClass		5
#define	COMPONENT_CLASS_PeripheralClass		6
#else
/* Component Class */
#define	COMPONENT_CLASS_SystemClass		0
#define	COMPONENT_CLASS_ProcessorClass		1
#define	COMPONENT_CLASS_CacheClass		2
#define	COMPONENT_CLASS_AdapterClass		3
#define	COMPONENT_CLASS_ControllerClass		4
#define	COMPONENT_CLASS_PeripheralClass		5
#define	COMPONENT_CLASS_MemoryClass		6
#endif

/* Component Types */
#if defined(sgimips)
/* System Class */
#define	COMPONENT_TYPE_ARC			0

/* Processor Class */
#define	COMPONENT_TYPE_CPU			1
#define	COMPONENT_TYPE_FPU			2

/* Cache Class */
#define	COMPONENT_TYPE_PrimaryICache		3
#define	COMPONENT_TYPE_PrimaryDCache		4
#define	COMPONENT_TYPE_SecondaryICache		5
#define	COMPONENT_TYPE_SecondaryDCache		6
#define	COMPONENT_TYPE_SecondaryCache		7

/* Memory Class */
#define	COMPONENT_TYPE_MemoryUnit		8

/* Adapter Class */
#define	COMPONENT_TYPE_EISAAdapter		9
#define	COMPONENT_TYPE_TCAdapter		10
#define	COMPONENT_TYPE_SCSIAdapter		11
#define	COMPONENT_TYPE_DTIAdapter		12
#define	COMPONENT_TYPE_MultiFunctionAdapter	13

/* Controller Class */
#define	COMPONENT_TYPE_DiskController		14
#define	COMPONENT_TYPE_TapeController		15
#define	COMPONENT_TYPE_CDROMController		16
#define	COMPONENT_TYPE_WORMController		17
#define	COMPONENT_TYPE_SerialController		18
#define	COMPONENT_TYPE_NetworkController	19
#define	COMPONENT_TYPE_DisplayController	20
#define	COMPONENT_TYPE_ParallelController	21
#define	COMPONENT_TYPE_PointerController	22
#define	COMPONENT_TYPE_KeyboardController	23
#define	COMPONENT_TYPE_AudioController		24
#define	COMPONENT_TYPE_OtherController		25

/* Peripheral Class */
#define	COMPONENT_TYPE_DiskPeripheral		26
#define	COMPONENT_TYPE_FloppyDiskPeripheral	27
#define	COMPONENT_TYPE_TapePeripheral		28
#define	COMPONENT_TYPE_ModemPeripheral		29
#define	COMPONENT_TYPE_MonitorPeripheral	30
#define	COMPONENT_TYPE_PrinterPeripheral	31
#define	COMPONENT_TYPE_PointerPeripheral	32
#define	COMPONENT_TYPE_KeyboardPeripheral	33
#define	COMPONENT_TYPE_TerminalPeripheral	34
#define	COMPONENT_TYPE_LinePeripheral		35
#define	COMPONENT_TYPE_NetworkPeripheral	36
#define	COMPONENT_TYPE_OtherPeripheral		37
#else /* not sgimips */
/* System Class */
#define	COMPONENT_TYPE_ARC			0

/* Processor Class */
#define	COMPONENT_TYPE_CPU			1
#define	COMPONENT_TYPE_FPU			2

/* Cache Class */
#define	COMPONENT_TYPE_PrimaryICache		3
#define	COMPONENT_TYPE_PrimaryDCache		4
#define	COMPONENT_TYPE_SecondaryICache		5
#define	COMPONENT_TYPE_SecondaryDCache		6
#define	COMPONENT_TYPE_SecondaryCache		7

/* Adapter Class */
#define	COMPONENT_TYPE_EISAAdapter		8
#define	COMPONENT_TYPE_TCAdapter		9
#define	COMPONENT_TYPE_SCSIAdapter		10
#define	COMPONENT_TYPE_DTIAdapter		11
#define	COMPONENT_TYPE_MultiFunctionAdapter	12

/* Controller Class */
#define	COMPONENT_TYPE_DiskController		13
#define	COMPONENT_TYPE_TapeController		14
#define	COMPONENT_TYPE_CDROMController		15
#define	COMPONENT_TYPE_WORMController		16
#define	COMPONENT_TYPE_SerialController		17
#define	COMPONENT_TYPE_NetworkController	18
#define	COMPONENT_TYPE_DisplayController	19
#define	COMPONENT_TYPE_ParallelController	20
#define	COMPONENT_TYPE_PointerController	21
#define	COMPONENT_TYPE_KeyboardController	22
#define	COMPONENT_TYPE_AudioController		23
#define	COMPONENT_TYPE_OtherController		24

/* Peripheral Class */
#define	COMPONENT_TYPE_DiskPeripheral		25
#define	COMPONENT_TYPE_FloppyDiskPeripheral	26
#define	COMPONENT_TYPE_TapePeripheral		27
#define	COMPONENT_TYPE_ModemPeripheral		28
#define	COMPONENT_TYPE_MonitorPeripheral	29
#define	COMPONENT_TYPE_PrinterPeripheral	30
#define	COMPONENT_TYPE_PointerPeripheral	31
#define	COMPONENT_TYPE_KeyboardPeripheral	32
#define	COMPONENT_TYPE_TerminalPeripheral	33
#define	COMPONENT_TYPE_OtherPeripheral		34
#define	COMPONENT_TYPE_LinePeripheral		35
#define	COMPONENT_TYPE_NetworkPeripheral	36

/* Memory Class */
#define	COMPONENT_TYPE_MemoryUnit		37
#endif

/* Component flags */
#define	COMPONENT_FLAG_Failed			1
#define	COMPONENT_FLAG_ReadOnly			2
#define	COMPONENT_FLAG_Removable		4
#define	COMPONENT_FLAG_ConsoleIn		8
#define	COMPONENT_FLAG_ConsoleOut		16
#define	COMPONENT_FLAG_Input			32
#define	COMPONENT_FLAG_Output			64

/* Key for Cache: */
#define	COMPONENT_KEY_Cache_CacheSize(x)				\
	(ARCBIOS_PAGESIZE << ((x) & 0xffff))
#define	COMPONENT_KEY_Cache_LineSize(x)					\
	(1U << (((x) >> 16) & 0xff))
#define	COMPONENT_KEY_Cache_RefillSize(x)				\
	(((x) >> 24) & 0xff)

/*
 * ARC system ID
 */
#define	ARCBIOS_SYSID_FIELDLEN		8
struct arcbios_sysid {
	char		VendorId[ARCBIOS_SYSID_FIELDLEN];
	char		ProductId[ARCBIOS_SYSID_FIELDLEN];
};

/*
 * ARC memory descriptor
 */
struct arcbios_mem {
	uint32_t	Type;
	arcbios_ulong_t	BasePage;
	arcbios_ulong_t	PageCount;
};

#if defined(sgimips)
#define	ARCBIOS_MEM_ExceptionBlock		0
#define	ARCBIOS_MEM_SystemParameterBlock	1
#define	ARCBIOS_MEM_FreeContiguous		2
#define	ARCBIOS_MEM_FreeMemory			3
#define	ARCBIOS_MEM_BadMemory			4
#define	ARCBIOS_MEM_LoadedProgram		5
#define	ARCBIOS_MEM_FirmwareTemporary		6
#define	ARCBIOS_MEM_FirmwarePermanent		7
#elif defined(arc)
#define	ARCBIOS_MEM_ExceptionBlock		0
#define	ARCBIOS_MEM_SystemParameterBlock	1
#define	ARCBIOS_MEM_FreeMemory			2
#define	ARCBIOS_MEM_BadMemory			3
#define	ARCBIOS_MEM_LoadedProgram		4
#define	ARCBIOS_MEM_FirmwareTemporary		5
#define	ARCBIOS_MEM_FirmwarePermanent		6
#define	ARCBIOS_MEM_FreeContiguous		7
#endif

/*
 * ARC display status
 */
struct arcbios_dsp_stat {
	uint16_t	CursorXPosition;
	uint16_t	CursorYPosition;
	uint16_t	CursorMaxXPosition;
	uint16_t	CursorMaxYPosition;
	uint8_t		ForegroundColor;
	uint8_t		BackgroundColor;
	uint8_t		HighIntensity;
	uint8_t		Underscored;
	uint8_t		ReverseVideo;
};

/*
 * ARC firmware vector
 */
struct arcbios_fv {
	arcbios_long_t	Load;
	arcbios_long_t	Invoke;
	arcbios_long_t	Execute;
	arcbios_long_t	Halt;
	arcbios_long_t	PowerDown;
	arcbios_long_t	Restart;
	arcbios_long_t	Reboot;
	arcbios_long_t	EnterInteractiveMode;
	arcbios_long_t	ReturnFromMain;		/* not on sgimips */
	arcbios_long_t	GetPeer;
	arcbios_long_t	GetChild;
	arcbios_long_t	GetParent;
	arcbios_long_t	GetConfigurationData;
	arcbios_long_t	AddChild;
	arcbios_long_t	DeleteComponent;
	arcbios_long_t	GetComponent;
	arcbios_long_t	SaveConfiguration;
	arcbios_long_t	GetSystemId;
	arcbios_long_t	GetMemoryDescriptor;
	arcbios_long_t	Signal;			/* not on sgimips */
	arcbios_long_t	GetTime;
	arcbios_long_t	GetRelativeTime;
	arcbios_long_t	GetDirectoryEntry;
	arcbios_long_t	Open;
	arcbios_long_t	Close;
	arcbios_long_t	Read;
	arcbios_long_t	GetReadStatus;
	arcbios_long_t	Write;
	arcbios_long_t	Seek;
	arcbios_long_t	Mount;
	arcbios_long_t	GetEnvironmentVariable;
	arcbios_long_t	SetEnvironmentVariable;
	arcbios_long_t	GetFileInformation;
	arcbios_long_t	SetFileInformation;
	arcbios_long_t	FlushAllCaches;
	arcbios_long_t	TestUnicode;		/* not on sgimips */
	arcbios_long_t	GetDisplayStatus;	/* not on sgimips */
};

#if defined(_KERNEL) || defined(_STANDALONE)
/*
 * ARC firmware vector calls
 */
long	arcbios_Load(char *, u_long, u_long, u_long *);
long	arcbios_Invoke(u_long, u_long, u_long, char **, char **);
long	arcbios_Execute(char *, u_long, char **, char **);
void	arcbios_Halt(void) __dead;
void	arcbios_PowerDown(void) __dead;
void	arcbios_Restart(void) __dead;
void	arcbios_Reboot(void) __dead;
void	arcbios_EnterInteractiveMode(void) __dead;
void	arcbios_ReturnFromMain(void) __dead;		/* not on sgimips */
void *	arcbios_GetPeer(void *);
void *	arcbios_GetChild(void *);
void *	arcbios_GetParent(void *);
long	arcbios_GetConfigurationData(void *, void *);
void *	arcbios_AddChild(void *, void *);
long	arcbios_DeleteComponent(void *);
void *	arcbios_GetComponent(char *);
long	arcbios_SaveConfiguration(void);
void *	arcbios_GetSystemId(void);
void *	arcbios_GetMemoryDescriptor(void *);
void	arcbios_Signal(u_long, void *);			/* not on sgimips */
void *	arcbios_GetTime(void);
u_long	arcbios_GetRelativeTime(void);

long	arcbios_GetDirectoryEntry(u_long, void *, u_long, u_long *);
long	arcbios_Open(const char *, u_long, u_long *);
long	arcbios_Close(u_long);
long	arcbios_Read(u_long, void *, u_long, u_long *);
long	arcbios_GetReadStatus(u_long);
long	arcbios_Write(u_long, void *, u_long, u_long *);
#ifdef ARCBIOS64
/* ARCS64 stores each 32-bit half of a seek position in a firmware long. */
struct arcbios_large_integer64 {
	int64_t high;
	uint64_t low;
};
long	arcbios_Seek(u_long, struct arcbios_large_integer64 *, u_long);
#else
long	arcbios_Seek(u_long, int64_t *, u_long);
#endif
long	arcbios_Mount(char *, u_long);
const char *
	arcbios_GetEnvironmentVariable(const char *);
long	arcbios_SetEnvironmentVariable(const char *, const char *);
long	arcbios_GetFileInformation(u_long, void *);
long	arcbios_SetFileInformation(u_long, u_long, u_long);
void	arcbios_FlushAllCaches(void);
paddr_t	arcbios_TestUnicode(u_long, uint16_t);		/* not on sgimips */
void *	arcbios_GetDisplayStatus(u_long);		/* not on sgimips */

#endif /* _KERNEL || _STANDALONE */

#endif /* _ARCBIOS_H_ */
