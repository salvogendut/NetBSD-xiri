# Experimental Xiri o32/n32 personality

`options COMPAT_IRIX_O32` builds a deliberately narrow, opt-in personality on
big-endian o32 and LP64 MIPS kernels. It supports Xiri's synthetic static fixtures and a
bounded dynamic interpreter experiment, not restored `COMPAT_IRIX` or general
vendor-application compatibility.

The static probe accepts ELF32/MSB MIPS III `ET_EXEC`, System V OSABI, ABI version zero,
and flags `0x20000101` (o32), or `0x20000021` (n32 on LP64). It requires two or three program headers immediately
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
The selected o32/n32 ABI is committed in `setregs` after selection succeeds.

Direct IRIX trap numbers 1001 (`exit`), 1003 (`read`), 1004 (`write`), 1005
(`open`), 1006 (`close`), 1015 (`chmod`), 1016 (`chown`), 1017 (`break`),
1024 (`getuid`/`geteuid`), 1047 (`getgid`/`getegid`),
1056 (`sysmp`, MP_PGSIZE only), 1134 (`mmap`), 1135 (`munmap`), and 1136 (`mprotect`)
are implemented using current native operations and `sy_invoke`. Trap 1040
implements the bounded `syssgi` queries below. Trap 1000 dispatches one level of
`syscall(number, ...)` through the same checked allowlist. Other numbers and
nested indirect forms select a dedicated handler that returns IRIX
`ENOSYS=89` without posting `SIGSYS`. This is an explicit experiment policy.
The dispatcher never indexes a table with an unchecked user-supplied number.
It handles branch-delay return, kernel restart, and the normal `userret` path.

The six-word direct o32 mmap call takes its descriptor and signed 32-bit offset
from `sp+16` and `sp+20`. An indirect call uses a0 for the selector, a1..a3 for
the first three arguments, and `sp+16` onward for the remaining arguments.
Indirect mmap therefore obtains flags/fd/offset at sp+16/20/24. The dispatcher
checks the entire spill range for arithmetic overflow before `copyin` and
translates failures to foreign `EFAULT`. The wrapper expands the offset into
native `off_t` and initializes the native argument padding.

## Static interface subset

- Argument setup uses current `copyargs` and `ps_strings` handling, with a
  16-byte-aligned first argument string and an explicit static `AT_NULL` pair.
  The o32 stack remains aligned to its native eight-byte requirement. The
  separate dynamic auxv contract is described below. Native `setregs` supplies
  the entry PC, t9, and stack; this has only been observed with the selected libc.
- Open accepts IRIX access modes 0/1/2, NDELAY `0x4`, APPEND `0x8`, NONBLOCK
  `0x80`, CREAT `0x100`, TRUNC `0x200`, EXCL `0x400`, and NOCTTY `0x800`, translating them to
  native flags. Access mode 3 or other flag bits return `EINVAL`. Current NetBSD
  emulation-root lookup uses `/emul/irix`, with its normal native-path fallback.
  This is not a chroot. Native mode/umask and descriptor behavior are used;
  the bounded PTY adaptation is described below.
- Mmap supports positive-length private non-fixed mappings with address
  hints below VM_MAXUSER_ADDRESS, page-aligned nonnegative 32-bit file offsets, and basic R/W/X protection
  bits. `/dev/zero` supplies zero-filled memory through the normal host device
  mapping path. IRIX's `0x1000` means SGI_ANYADDR, so it is rejected and never
  passed through as native MAP_ANON. Native non-fixed placement can round,
  clamp, or fall back from a hint; it never replaces occupied mappings.
  Shared/fixed mappings, autogrow, IRIX reserved-range placement policy, and
  other IRIX flags are not provided.
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
below enters rld. The dispatch extension subsequently runs asa's filter.

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
the actual first load address. The original checkpoint used only linked
addresses; branch `xiri/irix-elfmap-relocation` adds collision relocation. It
never replaces existing mappings or inspects an ELF header in the file: the caller supplies
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
a general multithreaded mapping transaction.

Before insertion, the relocation extension checks each rounded load range
under the VM map read lock. An occupant solely in the inter-segment gap does
not force a move. On collision it searches whole-layout gaps from 0x04000000
upward, bounded by 0x40000000, preserving the first address's residue modulo
the largest segment alignment. Only the kernel-local header addresses change;
the returned first load address lets the runtime relocate its own objects.
This is placement of segments, not a kernel ELF symbol relocator. An exhausted
search returns ENOMEM. Other insertion failures keep their errors and roll
back owned mappings without retrying elsewhere.

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

At the mapping checkpoint, unmodified asa's libc maps the supplied rld at
`0x0fb60000` (RX) and `0x0fbd8000` (RW/BSS), and transfers control into it.
Rld calls the then-unsupported
indirect `syscall(1056, MP_PGSIZE)` (sysmp request 14). Its unchecked -1 page
size contaminates allocation arithmetic: after a successful initial /dev/zero
mmap, it requests a zero-length mapping with a non-null hint, which the current
mmap subset rejects. It prints `rld_sbrk: Can't map /dev/zero -- probably not
enough virtual memory` and exits 1. This diagnostic does not establish host
memory exhaustion. The extension below resolves that measured boundary.

## Indirect calls and asa execution

Branch `xiri/irix-o32-dispatch` extends mapping commit
`755afd768ae96bfde4a52705f1afe0fe1889d068`. Indirect selectors use the same
explicit switch as direct traps, without unchecked subtraction or indexing.
Unsupported selectors consume no arguments and never read the stack. Known
calls copy only the implemented argument words; saved trap number, original
argument registers, restart PC, and branch-delay handling remain intact.
This does not add `__syscall`, recursive dispatch, or 64-bit argument forms.

`sysmp(MP_PGSIZE)` (1056, command 14) returns PAGE_SIZE. The dispatcher marshals
its five o32 argument words, including checked stack spills, matching the
reference's bad-stack EFAULT. Only the command is interpreted; other commands
return ENOSYS. Native non-fixed mmap now accepts the supplied user-space hint
while preserving all previous flag, length, protection and offset checks.

With unchanged IRIX 6.5.22m asa/libc/rld, the page-size query now returns 4096
and rld maps 0x6000 bytes of /dev/zero using hint 0x0fbda000. Startup reaches
the filter, which reads stdin, writes the expected output and exits zero as
root and as an ordinary native user. No vendor file is patched. Optional
syssgi RXEV_GET/USE_FP_BCOPY/TOSSTSAVE, terminal ioctl and prctl calls remain
unsupported; the tested non-terminal filter path tolerates those errors.
This qualifies that application path, not general dynamic linking or terminal
semantics. The companion suite retains all mapping/query/static regressions,
adds paired direct/indirect probes and asa carriage-control inputs, and checks
ordinary/effective credentials through indirect identity calls. Static
regression version 4 updates the two former indirect/hint rejection cases;
earlier fixture contracts remain separately reproducible.

General static recognition, wider ELF layouts, further runtime syscalls,
n64, PRDA, process sharing, caught signals, and the original desktop remain
separate work. No vendor IRIX files are included.


## LP64 and n32 extension

Branch `xiri/irix-lp64` builds on the separately qualified LP64 firmware bridge
and native ABI baseline. The historical option and emulation name remain
`COMPAT_IRIX_O32` and `irix_o32`; the implementation now selects o32 or n32
per accepted ELF image. LP64 also requires the native `compat_netbsd32` module.
The existing 32-bit kernel route remains available.

All implemented foreign argument types are explicitly 32-bit. Pointers are
sign-extended from 32 bits, sizes are widened as unsigned values, and file
offsets retain the supported signed 32-bit form. o32 stack spills are copied
as 32-bit words before expansion into native register slots. n32 takes up to
eight arguments from registers, or seven after the indirect selector. No
current call needs a spilled n32 argument. Full-width saved registers survive
traps, and current 32-bit returns are sign-extended. This does not implement
IRIX calls with 64-bit arguments or returns.

LP64 uses netbsd32 address placement and argument copying, 32-bit argc/argv/
envp/auxv, and the native ABI-specific register/FPR setup. `ELFSIZE=32` is
explicit: exec_elf32 supplies a 20-byte elf_args even to an LP64 personality.
Compile-time assertions guard that layout and the eight-byte auxiliary pair.
Without that definition, native-width fields corrupt the dynamic auxiliary
vector. Original synthetic interpreters check every auxiliary entry, stack
layout, initialized data, and BSS before entering the main program.

Dynamic n32 recognition requires MIPS III, flags 0x20000024 for the executable
and 0x20000022 for libc, and `/lib32/libc.so.1` or `/usr/lib32/libc.so.1`.
Executable/interpreter ABIs must match. The bounded two-load linked-address
policy is otherwise shared with o32. Every load must meet the running kernel's
page alignment; the qualified LP64 kernel uses 8192-byte pages. Four-KiB-only
layouts are rejected. This is not a general IRIX 4-KiB page emulation layer.

`break` (1017) uses native obreak with an explicit 32-bit user bound and a
lower bound at vm_daddr, preventing page-rounding overflow and vm_dsize
underflow. Native RLIMIT_DATA, collision checks, zero-filled private memory,
and page-granular shrinking apply. Successful returns are explicitly zero.
No IRIX executable-heap or shared-process semantics are promised. The Xiri
companion lab adds original growth/shrink, zero-fill, inaccessible released
page, collision and invalid-range fixtures for both ABIs and call forms.


### Experimental select (1100)

Both o32 and n32 decode five 32-bit arguments, including the o32 direct and
indirect stack spill positions. The timeout consists of two signed 32-bit
fields (seconds, microseconds). It is copied and validated first, is allowed
at an unaligned address, and is never written back. Negative seconds or
microseconds, or microseconds >= 1000000, return EINVAL.

The measured IRIX behavior treats negative nfds as zero. This subset caps
positive nfds at 1024 before computing mask sizes. Three aligned arrays of
32-bit mask words are copied into owned storage, then selcommon_kern uses the
native select scan/wait and notification machinery. The adapter counts the
union of the three ready sets, so a descriptor ready for both reading and
writing counts once. Bits above nfds in the final word are preserved. Results
are copied back in read/write/exception order; a fault in one set does not
prevent later sets from being updated before EFAULT is returned. Aliased user sets cannot affect the already computed distinct-fd count.

Two explicit scope differences remain: nfds > 1024 returns EINVAL, and a
selected invalid descriptor within the requested range returns EBADF even
when the original IRIX allocation-dependent scan truncates the range and
ignores that bit. No assumption about IRIX's internal fd-table capacity is
encoded in the native table allocator. Signal delivery and concurrent fd/set
mutation are outside this qualification. Xiri's irix-select lab contains the
paired guarded records, timeout and delayed TCP readiness probes, and native
and vendor controls. Toolchest still needs version-2 xstat for its menu check.


### Experimental SIOCNREAD (1054, 0x4004730a)

The ioctl adapter decodes three explicit 32-bit arguments. Its SIOCNREAD path
supports IPv4 TCP streams. It keeps a native file reference and uses
a kernel-owned integer with native FIONREAD under the socket lock. Exactly
four bytes are copied out after releasing the socket lock. Counts are snapshots;
the query never consumes queued data or changes nonblocking mode.

Paired original IRIX probes establish zero for empty/unconnected streams,
queued-byte counts before and after partial reads, and ECONNRESET for drained
EOF. Descriptor and EOF errors precede output-pointer validation. Unaligned
output is accepted; page-crossing faults retain the copied accessible prefix.
Regular files/directories reject this socket selector with ENOTTY; FIFO and
/dev/null reject it with EINVAL. No socket ioctl is forwarded to a device.

Except for the PTY request below, other selectors retain ENOSYS, including
TCGETA, altered size/group encodings,
and 0x4004667f (which original IRIX also accepts for bytes-readable). This is an
explicit bounded-policy difference, including invalid-fd unknown commands.
AF_UNIX, datagrams, listening sockets, urgent data, resets, concurrent changes,
large queue counts, inherited unsupported descriptor types and general ioctl
translation are outside this qualification. The companion irix-siocnread lab
holds the original synthetic fixtures and reviewed observations.

### Experimental PTY allocation

Open and F_SETFL accept IRIX O_NDELAY (4), canonicalized to native O_NONBLOCK.
F_GETFL reports 0x80, including when the original call used only the legacy
bit. This intentionally does not reproduce IRIX's distinct legacy empty-read
semantics. Open also translates O_NOCTTY (0x800). chmod (1015) and chown (1016)
decode explicit 32-bit arguments, then use native vnode/credential checks and
emulation-root pathname lookup; no privilege bypass is supplied. Native chown
rejects unprivileged file giveaways that the supplied IRIX installation
permits, leaving ownership unchanged on EPERM.

Only actual native PTY character devices with minor below 400 receive IRIX
master/slave majors 14/15 in st_rdev. Filesystem IDs and other devices retain
their prior encoding, and native ownership/mode stay authoritative. The
companion lab provisions a native ptm clone node, ptyfs with group 0/mode 0600,
a nullfs view inside /emul/irix/dev/pts, and the first 400 IRIX q/r aliases.
Native clone allocation grants the slave to the real UID. This disposable
configuration is not a production session/device-namespace installer.

ioctl I_STR (0x5308) accepts only the four-word request {0x5002, 0, 0, 0}
emitted by original libc's UNLKPT helper, on an actual native PTY master.
Descriptor validation precedes the 16-byte copyin: an absent descriptor gives
EBADF, another descriptor type ENOTTY, and bad request storage EFAULT. Other
inner commands or fields give ENOSYS. No foreign ioctl encoding is forwarded
to a device. NetBSD has no corresponding slave-lock state; this validated
unlock is a no-op and never changes ownership or permissions.

Original libc can allocate a master and open its slave under root and an
ordinary UID. Its next terminal-attribute ioctl (0x54d5) remains ENOSYS;
original xwsh reached unsupported fork at that PTY checkpoint; the process
subset below advances it through child slave opening to unsupported execve. General
STREAMS, higher PTY banks, concurrent lifecycle, IRIX slave-lock isolation,
termios, signals, shell execution and interactive winterm are not qualified.


### Experimental process subset (1002, 1020, 1164, 1170)

The process checkpoint adds native fork and the getpid/getppid register pair,
explicit 16-byte signal-mask marshalling, and positive-P_PID exit waits.
Traditional IRIX signals 1–31 map to native numbers; native-only inherited
mask bits survive replacement. Unsupported checkpoint/realtime bits are
ignored/reported zero. Input copy precedes operation validation; old-mask
copyout follows mutation. NULL input ignores the operation code. How 256
uses the mapped low-word subset.

Wait accepts WEXITED with optional WNOHANG/WNOWAIT. Unsupported selectors,
options and non-NULL rusage fail before consuming a child. NULL info is EINVAL;
unmapped ranges fault before waiting, whereas read-only mapped output can
fault after the native wait consumes the child. Concurrent map changes can
still cause a later copyout fault. The owned 128-byte info buffer has zero
padding/accounting. Reaping an ordinary exited child returns its encoded
status in the second register; no-event/nowait preserves that register.

The paired tests cover fork register returns, PID/identity relationships,
COW data, shared file offsets, actual KERN_PROC2 signal masks, positive-child
exit/nohang/nowait and copy faults under both ABIs. Signal delivery/handlers,
signal-caused child deaths, accounting, stopped/continued children, other wait
selectors, exec/session/descriptor setup and interactive terminal behavior
remain unqualified. See xiri's lab/irix-process and 2026-11-01 process report.


### Experimental shell handoff (1041, 1059, 1063, syssgi 20/109)

The shell branch adds native descriptor duplication and session creation.
SGI_FDHI reads the native descriptor high-water mark under fd_lock and returns
one beyond the highest open descriptor, including holes below that descriptor.
The only admitted ulimit selector is 4, returning the native soft descriptor
limit clamped to INT32_MAX; unsupported selectors return EINVAL.

Exec fetches each argument/environment element as a signed four-byte pointer
for both o32 and n32. Misaligned vectors fault with EFAULT, as measured on
IRIX 6.5.22m. Native execve1 owns pathname lookup, permissions, argument limits,
loading, signal-state reset and the final personality transition. NULL argv
retains native EINVAL; the reference /bin/sh terminates with status 139 for
that input. NULL environment is admitted. This is not a general qualification
of foreign-to-foreign or script exec, native n64 targets, set-ID transitions,
concurrent argument mutation, or caught-signal delivery.

Xiri's lab/irix-shell uses an explicit native helper after the handoff to
reopen the verified native PTY before acquiring its controlling terminal,
set its foreground group and 80x24 size, and exec
native /bin/sh. Foreign termios, window-size changes and signal handlers are
still separate work. Test cleanup reserves an otherwise unused UID in a fresh
guest and drops all privilege before launching descendants, allowing it to
clean up session children that outlive the original winterm launcher.

The native reopen avoids an observed layer_revoke/vrelel panic when shell
teardown revokes a controlling terminal through the nullfs compatibility view.
This is an explicit native-session integration policy; general layered-tty
revoke behavior remains open. Original xwsh and the unchanged winterm launcher
now execute native /bin/sh and accept XTest keyboard commands in the Xiri lab.
See xiri's 2026-11-01 shell report for the accepted runs and limitations.

### Experimental 4 KiB ELF data boundaries

The `xiri/irix-elfmap-subpage` branch admits a data PT_LOAD with a 4 KiB
boundary when its file and virtual offsets agree within a native page. This
covers original 4Dwm's libimdUtil and libXpm on the 8 KiB LP64 kernel. Code
segments retain the previous native-page restrictions. Code and data virtual
pages must be disjoint; overlapping logical file segments remain rejected.

Only an owned header copy is normalized. For nonempty data, mapping starts
at the preceding native file-page boundary and includes those prefix bytes.
For pure BSS, the prefix is zero-filled. A shared backing file page can appear
in separate RX and private RW mappings; native COW keeps writes to the data
alias from changing code or the file. Accounting, collision selection and
rollback use the full normalized native-page ranges. The returned code base
and common relocation displacement retain their previous contract, and the
caller's original headers are unchanged.

The paired synthetic fixtures in xiri's lab/irix-elfmap-subpage distinguish
IRIX's 4 KiB page behavior from native padding and rejection policy. They
retain all prior relocation inputs and add data/prefix/BSS, file-page COW,
collision, protection, limit and invalid-layout checks. This is a bounded
extension, not arbitrary ELF segment layout support.

### Experimental private fixed/local mappings (1134)

The mmap wrapper now accepts IRIX PRIVATE with optional FIXED and LOCAL
(0x2, 0x10 and 0x80), converting only the supported flags. Fixed ranges must
fit the foreign user address space and native page alignment. Native mmap
owns descriptor access checks, protections, page rounding, COW and replacement.
Shared mappings, AUTOGROW, AUTORESRV, ANYADDR and unknown bits remain EINVAL;
0x1000 must never be confused with native MAP_ANON. File offsets remain signed
32-bit values aligned to the native page size.

LOCAL isolates mappings from sproc share groups on IRIX. This implementation
accepts it only with one LWP and an unshared vmspace, and still cannot create
such a group. Ordinary private-map semantics apply within that supported scope.

Original libGL explicitly maps the page containing address 0x1000 for dispatch
data. With 8 KiB pages it requests address zero and length 4096; data at 0x1050
is covered by native rounding. After accepting a validated IRIX ELF image, its
exec probe supplies VM_MIN_ADDRESS instead of the native page-zero guard.
Only the foreign exec package changes: native exec retains exec_vm_minaddr,
and vm.user_va0_disable is not changed globally. Xiri's lab/irix-mmap-local
checks native zero-map rejection both directly and after o32/n32-to-native exec,
under both test identities, before and after the foreign mmap matrix.

The paired original fixtures measure fixed placement, complete rounded-page
payloads, replacement and adjacent mappings, invalid-descriptor preservation,
private file COW, read-only faults and explicit unsupported-input policy.
See the companion lab for reference records, exact kernel pins and results.

### Experimental measured PRDA and fork lifecycle

The validated foreign exec maps a private zero-filled native page at 0x200000.
An explicit uint32 prefix at offset 0xe00 supplies PID, two zero words, measured
initial FP flags (o32 0, n32 4), and native CPU PRId. Xiri's paired IRIX 6.5.22m
records establish these values; the configured R4400 reports 0x440. The older
historical initialization of PRId as PID is not used.

Ordinary fork inherits the page through COW. A child-return callback updates
only the child's PID before native child_return, preserving user data and the
parent's page. Fork rejects absent/read-only PID storage with EFAULT before
child creation; a later copyout failure terminates the child with SIGSEGV.
This damaged-PRDA behavior is native policy. Exec resets the area, and native
exec removes it. Native page-zero guards remain unchanged.

The lab/irix-prda fixtures in Xiri qualify initial state, private user writes,
fork parent/child identity and COW, exec reset, and traditional signal masks.
The LP64 native page's extra half is verified zero. Native controls check PRDA
absence directly and after foreign-to-native exec. All 80 executions/180
records, 20 damaged-state cases and 94 other controls pass across LP64 CI,
LP64 ordinary and o32 CI. Share groups, threads, scheduling hints, user-managed
PRDA signal holds and general FP control remain outside this subset.

On this PRDA kernel, Xiri's original 4Dwm now passes reparenting, pointer focus,
key delivery and dragging on native Xvfb in CI and ordinary IRIS execution.
Combined sessions run Toolchest and a native shell in original winterm/xwsh.
The original resources must be readable, and SG_manageSession/SG_autoSave
are explicitly disabled. Saved-state file operations, the unexpected Trusted
Path panel, startup repeatability and complete desktop services remain open.
See Xiri's 2026-11-01 4Dwm report, including retained failed startup attempts.

### Measured resource-limit queries and executable data alignment

Trap 1148 (`getrlimit`) and 1075 (`getrlimit64`) translate IRIX selectors
0 through 8 to native CPU, FSIZE, DATA, STACK, CORE, NOFILE, AS, RSS and NTHR
limits. The selector is validated before user storage is touched. Invalid
selectors return EINVAL; invalid or read-only output storage returns EFAULT.
Unaligned writable storage is accepted. The native process limits are read
under the process lock and copied through explicit fixed-width arrays.

The original IRIX measurements establish an ABI-dependent layout: ordinary
o32 getrlimit returns two 32-bit fields, while n32 getrlimit and either ABI's
getrlimit64 return two 64-bit fields. Infinity is 0x7fffffff or
0x7fffffffffffffff respectively. Finite o32 values above INT32_MAX remain
unsigned values; those above UINT32_MAX saturate to UINT32_MAX, distinct from
infinity. Soft and hard fields are converted independently. Fork and exec use
the existing native limit inheritance; no new emulation-private limit state
is introduced.

PTHREAD reports native NTHR policy, not a fixed successful placeholder. Its
native accounting rules are not a claim of full IRIX thread equivalence.
Foreign setrlimit calls, pthread creation and synchronization remain outside
this query subset. Native controls set distinct soft/hard descriptor, thread
and large file-size limits before foreign exec to verify actual translation.
See Xiri's lab/irix-rlimit and paired original/NetBSD records.

The bounded runtime executable validator additionally accepts a declared
4 KiB alignment for its RW data segment, as used by original bgicons. Both
virtual address and file offset must still satisfy the native page mask.
Text and interpreter minimum alignment, size/protection/overlap checks,
metadata validation and native VM mapping remain unchanged. Synthetic cases
exercise initialized data and BSS, half-page rejection, invalid alignments,
permissions and the previous loader matrix. This is separate from SGI_ELFMAP's
existing subpage library support.

The paired qualification passes 1,168 original-IRIX records, 4,760 NetBSD
records across LP64 CI/ordinary and o32 CI, 280 loader executions and 24
native/vendor controls. The application follow-up now executes bgicons and
successfully queries PTHREAD in all four file/icon launches. The next observed
failure is mmap with flags 0x102 (PRIVATE|AUTORESRV), returning EINVAL before
libpthread's table-allocation panic. AUTORESRV and general pthread behavior
were still unsupported at that checkpoint; see Xiri's 2026-11-01 resource-limit
and desktop-startup-fixes reports for the exact pins, trace evidence and
interaction limitations.

### Private AUTORESRV mappings

The private mmap subset accepts IRIX MAP_AUTORESRV (0x100), optionally with
FIXED and LOCAL. It requests deferred swap reservation. UVM already creates
private mappings with demand allocation/COW and does not reserve swap for the
whole mapping at mmap time, so no extra native flag is needed. Native
MAP_NORESERVE is unimplemented; passing it would not supply an additional
reservation policy. This adapter continues to translate the accepted flags
explicitly rather than passing their foreign bit values through.

Descriptor access modes, protections, native page alignment, address ranges,
resource limits and fixed replacement remain native mmap responsibilities.
LOCAL still requires one LWP and an unshared vmspace. Shared mappings,
AUTOGROW, ANYADDR and unknown bits remain unsupported. This does not reproduce
IRIX logical-swap accounting, swap-exhaustion SIGBUS behavior or mlock effects;
native memory-pressure policy applies.

Paired fixtures exercise the original pthread table request (1.375 MiB,
READ|WRITE, PRIVATE|AUTORESRV, O_RDWR /dev/zero), regular-file privacy, fixed
replacement, descriptor lifetime/access errors, protection faults and changes,
and private fork inheritance. The prior fixed/local cases and native page-zero
controls remain part of the new matrix. Qualification passes 464 original-IRIX
records, 1,160 native mapping cases, 30 native/vendor/cleanup controls and 32
native page-zero controls. See Xiri's lab/irix-mmap-autoresrv.

Original bgicons, fm and Icon Catalog now pass table allocation and its
following fixed mapping. Their next observed failure is trap 1130,
prctl(PR_INIT_THREADS=20), returning IRIX ENOSYS before libpthread's
INIT_THREADS panic. General pthread initialization and creation remain open.


### Measured first-pthread initialization

PR_INIT_THREADS (prctl option20) now initializes the measured PRDA signal hold
for one native LWP and an unshared vmspace. It copies the effective traditional
signal mask to the second word at 0x200e70 and ORs bit0 at 0x200e6c, preserving
other PRDA data and flags. Per-process emulation data activates the hold;
forging or clearing its user validity bit does not change that kernel state.
Repeated initialization succeeds without resetting the hold. Fork inherits the
effective mask and user bytes but starts the child uninitialized with its
validity bit clear and its own PID. Exec resets the foreign state; native exec
removes the PRDA mapping and frees emulation data.

The adapter imports user holds before foreign syscalls, traps and pending
signal delivery, and exports the mask after supported sigprocmask changes.
The new optional emul.e_sigmask callback runs from lwp_userret in current-LWP
context, without process/LWP locks held. It handles direct PRDA writes followed
by asynchronous delivery without another syscall. Native-only mask bits are
preserved; native signal machinery keeps SIGKILL/SIGSTOP unmaskable. This
callback is another core dependency to resolve for the desired compat_linux-
style loadable module, alongside the existing select helper.

Code revision a9e715bb082a755881799234dfdb54abca5a04c1 passes 260 native
executions / 860 records against 104 original-IRIX executions / 344 records,
32 asynchronous-signal controls, 12 initialized foreign-to-native exec
controls, and the retained native/vendor/cleanup, page-zero and damaged-PRDA
checks. LP64 CI/ordinary and o32 CI kernels are covered. The additional native
exec checks verify inherited SIGUSR1/SIGINFO/SIGRTMIN holds, PRDA removal and
native fork/wait. See Xiri lab/irix-init-threads and its 2026-11-01 reports.

Traditional foreign signals1..31 are the supported mapping. Thread creation,
realtime delivery, foreign handlers and general pthread semantics remain open.
PR_GETNSHARE/PR_GETSHMASK remain ENOSYS. NetBSD's PRDA protection policy is
explicit: initialization on read-only/inaccessible memory returns EFAULT even
where IRIX succeeds; the kernel does not bypass UVM protections. An unreadable
active hold terminates with SIGSEGV.

Original bgicons, fm and Icon Catalog now pass initialization and reach
session-directory setup. mkdir returns ENOSYS; Icon Catalog displays a fatal
Desktop session-directory dialog. fm also encounters waitsys(options=3)
returning EINVAL. No additional image assets were staged. The CI desktop
retains 4Dwm interaction and typed winterm shell execution; an ordinary run
retains a failed terminal geometry check. These are bounded observations, not
full desktop or startup-repeatability qualification.


### Directory creation

Trap1080 (mkdir) now copies a bounded foreign pathname, removes trailing
slashes after checking the original length, and calls native do_sys_mkdir.
The measured IRIX existing-file-with-trailing-slash result is EEXIST. Native
CREATE lookup retains TRYEMULROOT parent selection/fallback, effective-credential
checks, umask, filesystem authorization and ownership policy. No new core hook
is introduced; the standard VFS helper performs creation.

Source 1690de11a095134b6eee4d7f85a8b046b1a86c47 passes 3,200 native records
against 1,280 original-IRIX records, with 1,540 new directories checked using
independent native before/after find/stat snapshots and 44 native/vendor
controls. The matrix covers o32/n32, direct/indirect dispatch, five real/effective
credential combinations and umasks022/077 on LP64 CI/ordinary and o32 CI kernels.
All three guests halt cleanly. See Xiri lab/irix-mkdir and its 2026-11-01 report.

Native FFS assigns the parent directory's group; the tested original IRIX
children receive effective GID. The qualifier retains 960 explicit group-policy
records. Neither tested creation path retains special mode bits, including
under the parent configured with setgid. All other recorded fields match the
reference. Existing paths retain their native type, mode and UID/GID, and the
native shadow remains untouched when the emulation parent is selected.

This qualifies mkdir only. ACLs, supplementary groups, quota/full-filesystem
behavior, concurrent namespace changes and physical hardware remain outside
this matrix. Directory removal/rename, wider wait semantics and the complete
file/icon desktop remain separate work.


The original desktop retry now observes bgicons exit zero and Icon Catalog's
main window in both CI and ordinary modes. The original defaults name its
empty fallback page Blank; catalog page data is not staged, so emptiness alone
is not a compatibility defect. No additional image assets were added.
Generated state includes HOME/.desktop-xiri-indy and
HOME/.desktophost/xiri-indy/Icons, created under native ownership policy.

Both fm launches now exit 139 with SIGSEGV after pipe (1042) and sgikopt (1083)
return ENOSYS. This identifies the next investigation, not a proven sole crash
cause. Earlier wider-wait limitations remain unqualified. Measure original
pipe semantics, add native descriptor support and retry before assigning the
remaining fault to sgikopt or missing resources.

4Dwm input/drag checks pass in both modes; only CI also passes the native shell
handoff and typed-command marker. Ordinary mode retains TYPE_STATUS=3 with no
terminal handoff and an interrupted XWD capture (BadDrawable). Both guarded
sessions remove descendants and halt cleanly. See Xiri's
2026-11-01-irix-desktop-mkdir report and pixel-verified CI PNG. Full desktop,
repeatable startup, catalog actions and session saving remain unqualified.


### Native pipe dispatch

Trap1042 now dispatches directly to native sys_pipe with zero arguments.
The measured IRIX ABI returns the read/write endpoints in V0/V1, and the
original libc wrapper stores them in its caller's array. Native pipe1 owns
limits, rollback and file-descriptor lifetime; no new core hook is needed.

Source 039ac3ab33386392c178031ad4997b0cd86fb53c passes 220 native records
against 88 original-IRIX records, without error/descriptor normalization,
and 44 native/vendor controls. LP64 CI/ordinary and o32 CI builds cover both
foreign ABIs where supported, direct/indirect traps, UID0/UID1234, ignored
argument pointers, endpoint/descriptor flags, lowest-slot reuse, partial I/O,
nonblocking and reversed-direction errors, dup/EOF, inherited fork endpoints,
reaped exit status and EPIPE with SIGPIPE blocked. Zero/one/two-slot tests at
RLIMIT_NOFILE=32 verify atomic allocation failure and descriptor recovery.
All guests halt cleanly; every foreign execution proves process-group removal.

See Xiri lab/irix-pipe and the 2026-11-01-irix-pipe report. STREAMS interfaces,
poll translation, capacity/atomicity under contention, foreign signal handlers
and physical hardware remain outside this qualification. sgikopt behavior is
unchanged; desktop behavior is checked separately.


The pipe desktop retry renders FileManager in ordinary mode; fm -b remains
until guarded cleanup and the separate home launch exits zero. Repeated
select EINVAL remains, with usable browsing unqualified. Ordinary 4Dwm/shell
checks pass; the independent CI desktop capture times out and is retained as
an unclean diagnostic, not a seed. The isolated select tracer records missing
gettimeofday but does not reproduce EINVAL; its cause remains open.

### Original software installer and native homes

The supplied May 2004 freeware media contains four CD sets and 489 distinct
packages. The selected installation test is xmahjongg 3.6.1 through original
inst and its iconcatalogedit hooks for the Freeware page, followed by original
Software Manager. Runtime imports do not substitute for this workflow.

Original inst loads on the pipe kernel but faults before distribution reading.
A checked capture measures mmap(0, 0x100000, READ|WRITE, 0x141, 3, 0), after
opening /dev/zero read/write. Flags are SHARED|AUTOGROW|AUTORESRV, outside the
current private-mapping subset. After EINVAL, libinst iStringG::vprintf stores
through the failed pointer: EPC 0x043e18d4, delay-slot sb at 0x043e18d8,
register8 all ones, fault address 0xffffffff. Captured code matches the original
library. Measure shared/autogrow semantics before extending the adapter.
No vendor bytes/returns are modified and no installer success is claimed.

The native account helper now creates /usr/people/<username> homes with native
ksh and account skeletons. Native ksh writes and original asa reads the same
file; its ownership/content survive a cold boot. Original root sessions use
HOME=/; native administrator accounts are not relocated. Eight native
execution and two known-fault controls pass, and both installer test guests
halt cleanly. Graphical login persistence and catalog actions remain open.
See Xiri's 2026-11-01-irix-installer and irix-freeware reports.
