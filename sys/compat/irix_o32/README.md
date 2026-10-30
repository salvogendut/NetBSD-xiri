# Experimental Xiri o32 personality

`options COMPAT_IRIX_O32` builds a deliberately narrow, opt-in personality on
big-endian o32 MIPS kernels. It is intended for Xiri's synthetic static fixtures,
not as a restoration of `COMPAT_IRIX` or a claim of vendor-binary compatibility.

The loader accepts ELF32/MSB MIPS III `ET_EXEC`, System V OSABI, ABI version zero,
and flags `0x20000101`. It requires two or three program headers immediately
after the ELF header, in this order: read/execute text, optional read/write
data/BSS, and one note. Text starts at `0x00400000` and file offset zero, has
equal file/memory sizes of at most 1 MiB, and contains both the entry point and
note. The earlier text-only layout remains accepted. There is no interpreter,
dynamic segment, relocation, or dynamic auxiliary-vector implementation.

The optional data segment has exactly R/W flags, nonzero memory size up to
1 MiB, and file size no larger than memory size. A zero file size supplies pure
BSS. Virtual address and file offset must be page-aligned, with congruent
power-of-two ELF alignment from PAGE_SIZE through 1 MiB. Data must start after
the rounded text end, and end no higher than `0x01000000`. This small synthetic
image window is a test constraint, not general IRIX address-space policy.
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
(`open`), 1006 (`close`), 1134 (`mmap`), 1135 (`munmap`), and 1136 (`mprotect`)
are implemented using current native operations and `sy_invoke`. Other numbers,
including indirect forms, select a dedicated handler that returns IRIX
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
  The o32 stack remains aligned to its native eight-byte requirement. There is
  no interpreter auxv; startup register conventions beyond the earlier native
  `setregs` behavior remain unqualified against vendor startup code.
- Open accepts IRIX access modes 0/1/2, APPEND `0x8`, NONBLOCK `0x80`, CREAT
  `0x100`, TRUNC `0x200`, and EXCL `0x400`, translating them individually to
  native flags. Access mode 3 or other flag bits return `EINVAL`. Paths currently
  use the host namespace directly; no emulation-root or IRIX device adaptation
  is provided. Native mode/umask and descriptor behavior are used.
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

General static ELF recognition, full auxv compatibility, dynamic loading,
IRIX libraries, n32/n64, PRDA, process sharing, caught signals, and the original
desktop remain separate work. No vendor IRIX files are included.
