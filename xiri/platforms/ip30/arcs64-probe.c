/*
 * Copyright (c) 2026 Xiri contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Standalone Octane firmware diagnostic, not a NetBSD kernel or bootloader.
 * The ARCS64 interface uses 64-bit LONGs, pointers and vector entries.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;

struct arcs64_spb {
	u64 signature, length;
	u16 version, revision;
	u64 restart, debug, exception, utlb;
	u64 vector_length;
	const u64 *vector;
};

struct arcs64_mem {
	u32 type;
	u64 base_page, page_count;
};

_Static_assert(sizeof(u64) == 8, "requires N64");
_Static_assert(__builtin_offsetof(struct arcs64_spb, vector) == 64,
    "ARCS64 firmware vector offset");
_Static_assert(__builtin_offsetof(struct arcs64_mem, base_page) == 8,
    "ARCS64 memory descriptor alignment");

/* ARCS function numbers, shared with the 32-bit interface. */
#define ARC_GET_MEMORY_DESCRIPTOR 18
#define ARC_WRITE 27
#define ARC_GET_ENVIRONMENT_VARIABLE 30
#define SPB ((const volatile struct arcs64_spb *)0xffffffffa0001000UL)

typedef long (*arc_write_fn)(u64, const void *, u64, u64 *);
typedef const struct arcs64_mem *(*arc_mem_fn)(const struct arcs64_mem *);
typedef const char *(*arc_env_fn)(const char *);

static void
putstr(const char *s)
{
	u64 n = 0, done;
	arc_write_fn write = (arc_write_fn)SPB->vector[ARC_WRITE];

	while (s[n] != '\0')
		n++;
	while (n != 0) {
		done = 0;
		if (write(1, s, n, &done) != 0 || done == 0 || done > n)
			return;
		s += done;
		n -= done;
	}
}

static void
hex(u64 value)
{
	char buf[19];
	const char *digits = "0123456789abcdef";
	unsigned i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 16; i++)
		buf[2 + i] = digits[(value >> (60 - i * 4)) & 15];
	buf[18] = '\0';
	putstr(buf);
}

static void
field(const char *name, u64 value)
{
	putstr(name);
	hex(value);
	putstr("\r\n");
}

long __attribute__((section(".text.entry")))
arcs64_probe(long argc, const char *const *argv)
{
	const struct arcs64_mem *mem = (void *)0;
	arc_mem_fn getmem;
	arc_env_fn getenv;
	const char *console;
	u64 prid, count = 0, pages = 0;
	long i;

	/* Returning is safer than calling an unverified firmware vector. */
	if (SPB->signature != 0x53435241 || SPB->length < sizeof(*SPB) ||
	    SPB->vector == (void *)0 || SPB->vector_length < 31 * sizeof(u64))
		return 1;

	putstr("XIRI_IP30_ARCS64_BEGIN\r\n");
	field("spb.length=", SPB->length);
	field("spb.version=", SPB->version);
	field("spb.revision=", SPB->revision);
	field("spb.vector=", (u64)SPB->vector);
	__asm volatile("mfc0 %0, $15" : "=r"(prid));
	field("cpu.prid=", prid);
	field("argc=", (u64)argc);
	if (argc >= 0 && argc <= 32 && argv != (void *)0) {
		for (i = 0; i < argc; i++) {
			putstr("argv=");
			if (argv[i] != (void *)0)
				putstr(argv[i]);
			putstr("\r\n");
		}
	}
	getenv = (arc_env_fn)SPB->vector[ARC_GET_ENVIRONMENT_VARIABLE];
	console = getenv("console");
	putstr("console=");
	if (console != (void *)0)
		putstr(console);
	putstr("\r\n");
	getmem = (arc_mem_fn)SPB->vector[ARC_GET_MEMORY_DESCRIPTOR];
	while ((mem = getmem(mem)) != (void *)0) {
		if (++count > 128) {
			putstr("XIRI_IP30_ARCS64_DESCRIPTOR_LIMIT\r\n");
			return 2;
		}
		putstr("memory type=");
		hex(mem->type);
		putstr(" base_page=");
		hex(mem->base_page);
		putstr(" pages=");
		hex(mem->page_count);
		putstr("\r\n");
		pages += mem->page_count;
	}
	field("memory.total_pages=", pages);
	putstr("XIRI_IP30_ARCS64_OK\r\n");
	return 0;
}
