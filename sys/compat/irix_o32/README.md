# Experimental Xiri o32 personality

`options COMPAT_IRIX_O32` builds a deliberately narrow, opt-in personality on
big-endian o32 MIPS kernels. It supports Xiri's synthetic static fixtures and a
bounded dynamic interpreter experiment, not restored `COMPAT_IRIX` or general
vendor-application compatibility.

The static probe accepts ELF32/MSB MIPS III `ET_EXEC`, System V OSABI, ABI version zero,
and flags `0x20000101`. It requires two or three program headers immediately
after the ELF header, in this order: read/execute text, optional read/write
data/BSS, and one note. Text starts at `0x00400000` and file offset zero, has
equal file/memory sizes of at most 1 MiB, and contains both the entry point and
note. The earlier text-only layout remains accepted. This static path has no
interpreter, dynamic segment, or relocation.

The optional data segment has exactly R/W flags, nonzero memory size up to
1 MiB, and file size no larger than memory size. A zero file size supplies pure
BSS. Virtual address and file offset must be page-aligned, with congruent
power-of-two ELF alignment from PAGE_SIZE through 1 MiB. Data must start after
the rounded text end, and end no higher than `0x01000000`. This small synthetic
image window is a static test constraint, not general IRIX address-space policy.
File-backed data must start after the rounded text file extent. All file
extents are bounded by the actual file and the native signed 32-bit offset
range before mapping; overlapping pages, wrapping ranges, and extra segment
types are rejected.

Current NetBSD ELF VM commands provide private file-backed data, zero-fill in
the partial final file page, and whole BSS pages. The companion suite checks
initialized bytes, BSS including a nonzero file sentinel after data, writable
data, immutable executable files, fresh contents after repeated execs, and
faults on text writes and unmapped pages. Its permission checks do not establish
hardware-enforced non-executable data. Partial segment pages retain the native
page-granularity mapping behavior.

The note's exact 24 bytes encode namesz 5, descsz 4, type `0x49524958`, name
`Xiri\0` with zero padding, and big-endian descriptor 1. This is an experimental
opt-in convention, not historical IRIX branding. Native ELF selection retains
its existing priority; the foreign probe does not change process ABI state.
The o32 ABI is committed in `setregs` after selection succeeds.

Direct IRIX trap numbers 1001 (`exit`), 1003 (`read`), 1004 (`write`), 1005
(`open`), 1006 (`close`), 1024 (`getuid`/`geteuid`), 1047 (`getgid`/`getegid`),
1134 (`mmap`), 1135 (`munmap`), and 1136 (`mprotect`)
are implemented using current native operations and `sy_invoke`. Trap 1040
implements the bounded `syssgi` queries below. Other numbers, including indirect
forms, select a dedicated handler that returns IRIX
`ENOSYS=89` without posting `SIGSYS`. This is an explicit experiment policy.
The dispatcher never indexes a table with an unchecked user-supplied number.
It handles branch-delay return, kernel restart, and the normal `userret` path.

The six-word o32 mmap call takes its descriptor and signed 32-bit offset from
`sp+16` and `sp+20`. The dispatcher checks address addition before `copyin` and
translates failures to foreign `EFAULT`. The wrapper expands the offset into
native `off_t` and initializes the native argument padding.

## Static interface subset

- Argument setup uses current `copyargs` and `ps_strings` handling, with a
  16-byte-aligned first argument string and an explicit static `AT_NULL` pair.
  The o32 stack remains aligned to its native eight-byte requirement. The
  separate dynamic auxv contract is described below. Native `setregs` supplies
  the entry PC, t9, and stack; this has only been observed with the selected libc.
- Open accepts IRIX access modes 0/1/2, APPEND `0x8`, NONBLOCK `0x80`, CREAT
  `0x100`, TRUNC `0x200`, and EXCL `0x400`, translating them individually to
  native flags. Access mode 3 or other flag bits return `EINVAL`. Current NetBSD
  emulation-root lookup uses `/emul/irix`, with its normal native-path fallback.
  This is not a chroot. No IRIX device adaptation is provided. Native mode/umask
  and descriptor behavior are used.
- Mmap supports positive-length private non-fixed mappings with a null address
  hint, page-aligned nonnegative 32-bit file offsets, and basic R/W/X protection
  bits. `/dev/zero` supplies zero-filled memory through the normal host device
  mapping path. IRIX's `0x1000` means SGI_ANYADDR, so it is rejected and never
  passed through as native MAP_ANON. Shared/fixed mappings, autogrow, address
  hints, reserved-range placement policy, and other IRIX flags are not provided.
- Munmap/mprotect require page-aligned addresses and positive signed 32-bit
  lengths; mprotect accepts only basic R/W/X bits. Native VM operations supply
  the actual protections, copy-on-write, bounds checks, and cleanup. This subset
  does not implement IRIX process-sharing or PRDA semantics.

These definitions follow the pre-removal `syscalls.master`, `irix_types.h`,
`irix_exec_elf32.c`, `irix_mman.{c,h}`, `irix_fcntl.c`, and `svr4_fcntl.h` at
`5aca2679d78276864d2980a08314887b2058fb2c`. Unsupported combinations fail explicitly;
this is not a claim that all original IRIX errors or edge cases match.

`irix_errno.h` retains the historical NetBSD definitions and license from
revision `5aca2679d78276864d2980a08314887b2058fb2c`. The translation table uses
symbolic native indices instead of the old positional mapping. Host errors
without a mapping return IRIX `EIO`, never zero. This does not qualify the
semantics of other IRIX calls. Default native signal termination is retained;
there is no interface for installing foreign handlers. A defensive sendsig
hook terminates rather than constructing an unqualified foreign signal frame.
Core dumps, if enabled, use NetBSD's ELF core implementation.

The source baseline is `a852b4a03800c32a127640b784fcc9fc94159d12`. Kernel
configurations, assembly fixtures, direct-exec runner, native regression checks,
and CI/ordinary IRIS comparison live in the companion
[Xiri repository](https://github.com/salvogendut/xiri), under `lab/irix-o32` and
`scripts/probe-irix-experiment.py`. Build the unmodified baseline first and keep
its kernel and tool/object paths separate from the qualified NetBSD 11.0 lab.

The data/BSS extension is developed on `xiri/irix-o32-segments` from the
argument/file/memory implementation, `c9ab9ccc062650870e17006571c93e665e2133d9`.
Its companion harness uses `--suite segments` and a version 3 fixture manifest.
The earlier version 1/minimal and version 2/basics suites remain available.

## Bounded interpreter experiment

Branch `xiri/irix-o32-runtime` extends the segment commit
`6a162a938061d16a60842fd9bb31d69e546d66dc`. A distinct dynamic probe accepts
ELF32/MSB/System V MIPS II or III executables with exact flags `0x10000005`
or `0x20000005` and an exact `/lib/libc.so.1` or `/usr/lib/libc.so.1` interpreter
name. These combined traits identify the intended IRIX subset; they do not
authenticate a file's origin. n32, arbitrary interpreter paths, and static
unbranded files remain rejected.

Both executable and interpreter must have two ordered, page-separated loads:
RX text at file offset zero and RW data/BSS. Each is bounded to 16 MiB within
`0x00400000..0x40000000`, with page-aligned address/offset, power-of-two alignment
from PAGE_SIZE to 1 MiB, congruent address/offset, valid file extents, and an
aligned entry inside file-backed text. Headers immediately follow the ELF
header, with at most 16 entries. PHDR, INTERP, and DYNAMIC metadata must point
into corresponding file-backed bytes; duplicates and unsupported segment types
are rejected. MIPS OPTIONS, REGINFO, and RTPROC metadata are permitted but are
not interpreted by the kernel. RTPROC need not be mapped.

The interpreter additionally requires ET_DYN and exact flags `0x10000003` or
`0x20000003`; it cannot have another interpreter or overlap the executable with
its reserved span. Validation uses the interpreter vnode, marks it as text,
and follows current exec ownership/cleanup. The native mapper still performs
its permission and mount checks. Self-interpreter vnode recursion is rejected.
`ELF32_LINK_ADDR` preserves libc's linked address and bootstrap GOT. Native VM
commands supply private data and BSS zero-fill. No relocator is implemented in
the kernel.

Dynamic copyargs supplies PHDR, PHENT, PHNUM, ENTRY, BASE, PAGESZ, and NULL, using
the current `elf_args` ownership API. Static images still receive only NULL.
Argument strings remain 16-byte aligned and the o32 stack eight-byte aligned.
The companion synthetic interpreter checks every auxiliary value, initialized
data/BSS in both mappings, repeated exec, and transfer to the main entry.

An explicitly enabled `ktrace -t u` records actual trap numbers, PCs and the
first four argument registers in `irix_o32` user records. This preserves the
number of unsupported calls routed through table slot zero; no new syscall
semantics are added. Untraced execution emits no records.

The initial runtime branch lets IRIX 6.5.22m asa and matching libc pass exec and
enter libc's bootstrap, stopping at `syssgi(SGI_SYSCONF, _SC_CAP)`. The query
extension moved the observed boundary to `SGI_ELFMAP`. The mapping extension
below now enters rld; asa's filter has not executed.

## Runtime query subset

Branch `xiri/irix-o32-queries` extends runtime commit
`64915ea0900cd81e3bc2577504c709fb33051854`. Its trap 1040 query subset is:

- `SGI_SYSCONF` (22), `_SC_PAGESIZE` (11): returns the native page size.
- `SGI_SYSCONF`, `_SC_CAP` (29): returns `CAP_SYS_DISABLED` (0), the IRIX-defined
  traditional superuser model. No IRIX capability enforcement is implemented;
  every operation retains current NetBSD credentials and authorization checks.
  Other sysconf names return EINVAL, rather than invented values.
- `SGI_PROC_ATTR_GET` (85), `SGI_CAP_PROCESS`: copies three zero 64-bit masks,
  exactly 24 bytes, describing effective/permitted/inheritable IRIX capability
  sets. It grants no privileges. Null name or output pointers return EINVAL;
  non-null bad pointers use checked copyinstr/copyout and return EFAULT.
  Names are bounded to 80 bytes including NUL, matching the supplied IRIX kernel.
  Overlong names return ENAMETOOLONG; other names return ENOATTR (IRIX 1009).

Other syssgi requests, including setters, retain the explicit ENOSYS policy;
ELFMAP is the separate bounded addition below. Foreign credential-changing
syscalls also remain unsupported. The supplied IRIX kernel instead
returns EINVAL for an unknown request. That difference is intentional and
covered by the paired probe. The first four argument registers suffice for
these implemented operations; later stack arguments are not fetched.

Trap 1024 returns native real uid in v0 and effective uid in v1; 1047 does the
same for gids. The native `sys_getuid_with_euid`/`sys_getgid_with_egid` operations
provide both values. The companion native parent tests ordinary and differing
real/effective credentials without adding foreign identity-changing syscalls.

Original IRIX 6.5.22m reports capability mode 1 on the supplied installation,
but zero process capability masks for both root and uid/gid 60001. Its query
layout, guarded copyout, errno values, name-length boundary, and identity
returns are measured by the same original static probe used on Xiri. The mode
0 advertisement on Xiri is a deliberate description of its smaller model,
supported by the supplied capability header and historical NetBSD definition;
it is not a claim to reproduce the reference's enabled capability subsystem.

At the query checkpoint, unmodified asa/libc pass the startup queries, open
`/lib/rld` through the emulation root, read its 512-byte ELF header area, and request
`syssgi(SGI_ELFMAP, fd, phdrs, 2)`. That checkpoint leaves the operation
unsupported, producing libc's mapping diagnostic and exit 239. The extension
below advances this boundary without patching vendor files.

## Bounded SGI_ELFMAP

Branch `xiri/irix-o32-elfmap` extends query commit
`dc08d65a8e7b3e6cdbd7ca978a200b6f50a42ad5`. Request 68 accepts exactly two
ELF32 program headers, copied once into a fixed-size kernel array. It returns
the first linked virtual address. It does not relocate or replace existing
mappings and does not inspect an ELF header in the file: the caller supplies
the load descriptions, as in the original interface.

The subset requires ordered PT_LOAD headers, RX text at file offset zero
without BSS, then RW data with optional BSS. Each segment is at most 16 MiB;
the total virtual span is at most 32 MiB within `0x00400000..0x40000000`.
Addresses/offsets are page-aligned; ELF alignment is a power of two from one
page through 1 MiB with congruent address/offset. File/virtual ranges cannot
overlap, wrap, or exceed the file or signed 32-bit file-offset range. Physical
addresses are ignored. Gaps remain unmapped. Other layouts return ENOEXEC;
other counts return EINVAL, and invalid header pointers return EFAULT.

The descriptor must be readable (otherwise EBADF) and refer to a regular
vnode (otherwise EINVAL). Current credentials must pass VEXEC, the mount must
allow execution, and vn_marktext must reject active writers. File size is
rechecked after marking text. File-backed mappings use the native VOP_MMAP
and exec VM helpers; PaX protection checks remain active. An address-space
limit check precedes mapping. Shared vmspaces, multiple LWPs, and future-wired
maps are explicitly unsupported; this personality has no calls creating them.

Full file pages use private paged-vnode mappings. A partial RW page is first
allocated as zero-filled memory, then read with exactly p_filesz's remaining
bytes; nonzero bytes following p_filesz cannot enter BSS. Whole BSS pages use
the native zero-fill helper. At most four VM commands are needed. Each fixed
insertion refuses overlap without removing an existing entry. The rollback
journal includes a partial page before its read can fail, and unmaps only
ranges successfully created by this request. The single, unshared vmspace
requirement prevents another user thread from replacing journaled mappings.
This provides all-or-nothing completion for the supported process model, not
a general multithreaded mapping transaction. Collisions return ENOMEM rather
than the original system's relocation behavior.

The companion tests exercise executable code, initialized bytes, partial and
whole BSS, pure BSS, private writes and fresh contents after unmap/remap, text
and gap faults, two rollback stages with preserved old data and a successful
retry, descriptor kinds/access modes, non-executable files/mounts, and malformed
bounds/alignment/counts. Original IRIX observations are pinned separately;
its wider layout acceptance and error policy are not advertised as implemented.
In particular, the reference accepts the tested non-executable file, read/write
descriptor, and /dev/zero descriptor. This subset deliberately retains native
execution/text-write checks and rejects device mappings. Its ENOEXEC/EINVAL
results for unsupported layouts/descriptors need not match IRIX's errors.

Unmodified asa's libc now maps the supplied rld at `0x0fb60000` (RX) and
`0x0fbd8000` (RW/BSS), and transfers control into it. Rld calls the unsupported
indirect `syscall(1056, MP_PGSIZE)` (sysmp request 14). Its unchecked -1 page
size contaminates allocation arithmetic: after a successful initial /dev/zero
mmap, it requests a zero-length mapping with a non-null hint, which the current
mmap subset rejects. It prints `rld_sbrk: Can't map /dev/zero -- probably not
enough virtual memory` and exits 1. This diagnostic does not establish host
memory exhaustion. The next work is indirect dispatch and the real sysmp page
size query, followed by measured mmap-hint semantics. Asa's filter and vendor
relocations have not been qualified.

General static recognition, wider ELF layouts, further runtime syscalls,
n32/n64, PRDA, process sharing, caught signals, and the original desktop remain
separate work. No vendor IRIX files are included.
