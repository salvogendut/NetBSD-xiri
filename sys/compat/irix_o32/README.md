# Experimental Xiri o32 personality

`options COMPAT_IRIX_O32` builds a deliberately narrow, opt-in personality on
big-endian o32 MIPS kernels. It is intended for Xiri's synthetic static fixtures,
not as a restoration of `COMPAT_IRIX` or a claim of vendor-binary compatibility.

The loader accepts ELF32/MSB MIPS III `ET_EXEC`, System V OSABI, ABI version zero,
and flags `0x20000101`. It requires exactly two program headers immediately
after the ELF header: one read/execute load segment at `0x00400000`, followed by
one note. The load segment starts at file offset zero, has equal file/memory
sizes of at most 1 MiB, and contains both the entry point and note. Alignment
and file bounds are checked. There is no interpreter, dynamic segment, writable
data segment, BSS, or general-purpose IRIX stack/auxv implementation.

The note's exact 24 bytes encode namesz 5, descsz 4, type `0x49524958`, name
`Xiri\0` with zero padding, and big-endian descriptor 1. This is an experimental
opt-in convention, not historical IRIX branding. Native ELF selection retains
its existing priority; the foreign probe does not change process ABI state.
The o32 ABI is committed in `setregs` after selection succeeds.

Only direct IRIX trap numbers 1004 (`write`) and 1001 (`exit`) are implemented,
using current native syscall argument types and `sy_invoke`. Other numbers,
including indirect forms, select a dedicated handler that returns IRIX
`ENOSYS=89` without posting `SIGSYS`. This is an explicit experiment policy.
The dispatcher never indexes a table with an unchecked user-supplied number.
It handles branch-delay return, kernel restart, and the normal `userret` path.

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

General static ELF recognition, argument/auxv compatibility, dynamic loading,
IRIX libraries, n32/n64, PRDA, process sharing, caught signals, and the original
desktop remain separate work. No vendor IRIX files are included.
