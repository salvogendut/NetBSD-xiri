/* $NetBSD$ */
/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 The NetBSD Foundation, Inc. */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <dev/wscons/wsconsio.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr, "usage: wsdecorctl [-q] [-f device] [theme.conf]\n");
	exit(1);
}

static uint32_t
number(const char *s, int base, uint32_t limit)
{
	char *end;
	unsigned long n;

	if (*s == '\0' || *s == '-' || *s == '+')
		errx(1, "invalid number: %s", s);
	errno = 0;
	n = strtoul(s, &end, base);
	if (errno || *end || n > limit)
		errx(1, "invalid number: %s", s);
	return n;
}

static uint32_t
read32(FILE *fp)
{
	unsigned char b[4];

	if (fread(b, 1, sizeof(b), fp) != sizeof(b))
		errx(1, "truncated WSD image");
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
	    (uint32_t)b[2] << 8 | b[3];
}

static uint8_t *
load_image(const char *path, struct wsdisplay_decor *t)
{
	FILE *fp;
	char magic[8];
	uint8_t *pixels;
	uint32_t runs, run, i;
	size_t size, pos = 0;
	int color;

	if ((fp = fopen(path, "rb")) == NULL)
		err(1, "%s", path);
	if (fread(magic, 1, sizeof(magic), fp) != sizeof(magic) ||
	    memcmp(magic, "WSDIMG1\n", sizeof(magic)) != 0)
		errx(1, "%s: not a WSD image", path);
	t->width = read32(fp);
	t->height = read32(fp);
	t->ncolors = read32(fp);
	runs = read32(fp);
	if (t->width == 0 || t->height == 0 ||
	    t->width > WSDISPLAY_DECOR_MAXSIZE ||
	    t->height > WSDISPLAY_DECOR_MAXSIZE || t->ncolors == 0 ||
	    t->ncolors > WSDISPLAY_DECOR_MAXCOLORS)
		errx(1, "%s: unsupported dimensions or palette", path);
	size = t->width * t->height;
	if (runs == 0 || runs > size ||
	    fread(t->palette, 3, t->ncolors, fp) != t->ncolors)
		errx(1, "%s: invalid WSD image", path);
	if ((pixels = malloc(size)) == NULL)
		err(1, "image allocation");
	for (i = 0; i < runs; i++) {
		run = read32(fp);
		color = fgetc(fp);
		if (run == 0 || run > size - pos || color < 0 ||
		    (unsigned)color >= t->ncolors)
			errx(1, "%s: invalid image run", path);
		memset(pixels + pos, color, run);
		pos += run;
	}
	if (pos != size || fgetc(fp) != EOF || ferror(fp))
		errx(1, "%s: incorrect image length", path);
	fclose(fp);
	return pixels;
}

static uint8_t *
load_theme(const char *path, struct wsdisplay_decor *t,
    const struct wsdisplay_decor *current)
{
	FILE *fp;
	char line[PATH_MAX + 128], image[PATH_MAX] = "", resolved[PATH_MAX];
	char *v[6], *p, *save;
	const char *slash;
	uint8_t *pixels = NULL;
	unsigned seen = 0, bit, lineno = 0;
	int n, solid = 0;
	uint32_t x, y;
	size_t dirlen;

	enum { TYPE=1, TOP=2, BOTTOM=4, COLOR=8, IMAGE=16, VIEWPORT=32 };
	memset(t, 0, sizeof(*t));
	t->version = WSDISPLAY_DECOR_VERSION;
	t->w = current->width;
	t->h = current->height;
	if ((fp = fopen(path, "r")) == NULL)
		err(1, "%s", path);
	while (fgets(line, sizeof(line), fp) != NULL) {
		lineno++;
		if (!strchr(line, '\n') && !feof(fp))
			errx(1, "%s:%u: line too long", path, lineno);
		if ((p = strchr(line, '#')) != NULL)
			*p = '\0';
		n = 0;
		for (p = strtok_r(line, " \t\r\n", &save); p != NULL;
		    p = strtok_r(NULL, " \t\r\n", &save)) {
			if (n == 6)
				errx(1, "%s:%u: too many fields", path, lineno);
			v[n++] = p;
		}
		if (n == 0)
			continue;
		bit = 0;
		if (n == 2 && !strcmp(v[0], "type")) {
			bit = TYPE;
			if (!strcmp(v[1], "image"))
				t->type = WSDISPLAY_DECOR_IMAGE;
			else if (!strcmp(v[1], "gradient"))
				t->type = WSDISPLAY_DECOR_GRADIENT;
			else if (!strcmp(v[1], "solid")) {
				t->type = WSDISPLAY_DECOR_GRADIENT;
				solid = 1;
			} else
				errx(1, "%s:%u: unknown theme type", path, lineno);
		} else if (n == 2 && !strcmp(v[0], "top")) {
			bit = TOP;
			t->top = number(v[1], 16, 0xffffff);
		} else if (n == 2 && !strcmp(v[0], "bottom")) {
			bit = BOTTOM;
			t->bottom = number(v[1], 16, 0xffffff);
		} else if (n == 2 && !strcmp(v[0], "color")) {
			bit = COLOR;
			t->top = t->bottom = number(v[1], 16, 0xffffff);
		} else if (n == 2 && !strcmp(v[0], "image")) {
			bit = IMAGE;
			if (strlen(v[1]) >= sizeof(image))
				errx(1, "image path too long");
			strcpy(image, v[1]);
		} else if (n == 5 && !strcmp(v[0], "viewport")) {
			bit = VIEWPORT;
			t->x = number(v[1], 10, WSDISPLAY_DECOR_MAXSIZE);
			t->y = number(v[2], 10, WSDISPLAY_DECOR_MAXSIZE);
			t->w = number(v[3], 10, WSDISPLAY_DECOR_MAXSIZE);
			t->h = number(v[4], 10, WSDISPLAY_DECOR_MAXSIZE);
		}
		if (bit == 0 || (seen & bit))
			errx(1, "%s:%u: unknown, duplicate or malformed directive",
			    path, lineno);
		seen |= bit;
	}
	if (ferror(fp))
		err(1, "%s", path);
	fclose(fp);
	if (!(seen & TYPE) ||
	    (t->type == WSDISPLAY_DECOR_IMAGE ?
	    seen != (TYPE | IMAGE | VIEWPORT) :
	    (seen & ~VIEWPORT) != (solid ? TYPE | COLOR : TYPE | TOP | BOTTOM)))
		errx(1, "%s: missing or conflicting theme directives", path);
	if (t->x >= current->width || t->y >= current->height ||
	    t->w == 0 || t->h == 0 || t->w > current->width - t->x ||
	    t->h > current->height - t->y)
		errx(1, "%s: viewport does not fit %ux%u display", path,
		    current->width, current->height);
	if (t->type == WSDISPLAY_DECOR_IMAGE) {
		if (image[0] == '/') {
			strcpy(resolved, image);
		} else {
			slash = strrchr(path, '/');
			dirlen = slash == NULL ? 0 : (size_t)(slash - path + 1);
			if (dirlen + strlen(image) >= sizeof(resolved))
				errx(1, "image path too long");
			memcpy(resolved, path, dirlen);
			strcpy(resolved + dirlen, image);
		}
		pixels = load_image(resolved, t);
		if (t->width < current->width || t->width > current->width + 2 ||
		    t->height != current->height)
			errx(1, "%s: image does not match %ux%u display", path,
			    current->width, current->height);
		t->panel = pixels[t->y * t->width + t->x];
		for (y = t->y; y < t->y + t->h; y++)
			for (x = t->x; x < t->x + t->w; x++)
				if (pixels[y * t->width + x] != t->panel)
					errx(1, "%s: text rectangle is not uniform", path);
		t->pixels = (uintptr_t)pixels;
	}
	return pixels;
}

int
main(int argc, char **argv)
{
	const char *device = "/dev/ttyE0";
	struct wsdisplay_decor current, theme;
	uint8_t *pixels;
	int ch, fd, quiet = 0;

	while ((ch = getopt(argc, argv, "f:q")) != -1) {
		switch (ch) {
		case 'f': device = optarg; break;
		case 'q': quiet = 1; break;
		default: usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage();
	fd = open(device, (argc ? O_RDWR : O_RDONLY) | O_NOCTTY);
	if (fd == -1) {
		if (quiet && (errno == ENOENT || errno == ENXIO || errno == ENODEV))
			return 0;
		err(1, "%s", device);
	}
	memset(&current, 0, sizeof(current));
	if (ioctl(fd, WSDISPLAYIO_GDECOR, &current) == -1) {
		if (quiet && (errno == ENOTTY || errno == EOPNOTSUPP ||
		    errno == ENODEV || errno == ENXIO))
			return 0;
		err(1, "%s: get decoration", device);
	}
	if (current.version != WSDISPLAY_DECOR_VERSION)
		errx(1, "unsupported decoration protocol %u", current.version);
	if (argc == 0) {
		printf("%s: %ux%u, %s, viewport %u %u %u %u\n", device,
		    current.width, current.height,
		    current.type == WSDISPLAY_DECOR_IMAGE ? "image" : "gradient",
		    current.x, current.y, current.w, current.h);
	} else {
		pixels = load_theme(argv[0], &theme, &current);
		if (ioctl(fd, WSDISPLAYIO_SDECOR, &theme) == -1)
			err(1, "%s: set decoration", device);
		free(pixels);
	}
	close(fd);
	return 0;
}
