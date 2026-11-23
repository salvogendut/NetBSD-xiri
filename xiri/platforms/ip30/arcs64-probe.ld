/* Octane RAM begins at physical 0x20000000; reserve its low firmware area. */
OUTPUT_ARCH(mips)
ENTRY(arcs64_probe)
PHDRS { text PT_LOAD FLAGS(5); }
SECTIONS
{
	. = 0xa800000020080000;
	.text : { *(.text.entry) *(.text .text.*) } :text
	.rodata : { *(.rodata .rodata.*) } :text
	/DISCARD/ : { *(.comment) *(.note*) *(.MIPS.abiflags) *(.MIPS.options) *(.reginfo) *(.eh_frame*) }
}
