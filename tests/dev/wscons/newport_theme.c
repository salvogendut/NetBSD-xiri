/* Runtime decoration ioctl regression; run as root on a test Newport console. */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <dev/wscons/wsconsio.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd;
static volatile sig_atomic_t resized;

static void
winch(int sig)
{
	(void)sig;
	resized++;
}

static void
reject(struct wsdisplay_decor *t, int expected, const char *name)
{
	struct wsdisplay_decor before, after;
	if (ioctl(fd, WSDISPLAYIO_GDECOR, &before) == -1)
		err(1, "get before");
	errno = 0;
	if (ioctl(fd, WSDISPLAYIO_SDECOR, t) != -1 || errno != expected)
		errx(1, "%s: expected errno %d, got %d", name, expected, errno);
	if (ioctl(fd, WSDISPLAYIO_GDECOR, &after) == -1 ||
	    memcmp(&before, &after, sizeof(before)))
		errx(1, "%s changed the theme", name);
	printf("PASS %s\n", name);
}

static void
sizecheck(const char *path, unsigned rows, unsigned cols)
{
	struct winsize ws;
	int tty = open(path, O_RDONLY | O_NOCTTY);
	if (tty == -1 || ioctl(tty, TIOCGWINSZ, &ws) == -1)
		err(1, "%s size", path);
	if (ws.ws_row != rows || ws.ws_col != cols)
		errx(1, "%s: size %u,%u expected %u,%u", path,
		    ws.ws_row, ws.ws_col, rows, cols);
	close(tty);
}

int
main(int argc, char **argv)
{
	struct wsdisplay_decor current, gradient, bad, image;
	uint8_t *pixels;
	struct winsize spoof = { .ws_row = 42, .ws_col = 88 };
	int mode, status, ro;
	pid_t child;

	if (argc != 1)
		errx(1, "usage: %s (create ttyE1 before running)", argv[0]);
	setvbuf(stdout, NULL, _IONBF, 0);
	fd = open("/dev/ttyE0", O_RDWR | O_NOCTTY);
	if (fd == -1 || ioctl(fd, WSDISPLAYIO_GDECOR, &current) == -1)
		err(1, "open/get console");
	memset(&gradient, 0, sizeof(gradient));
	gradient.version = WSDISPLAY_DECOR_VERSION;
	gradient.type = WSDISPLAY_DECOR_GRADIENT;
	gradient.w = current.width;
	gradient.h = current.height;
	gradient.bottom = 0x008060;
	bad = gradient; bad.version++;
	reject(&bad, EINVAL, "version");
	bad = gradient; bad.reserved[0] = 1;
	reject(&bad, EINVAL, "reserved");
	bad = gradient; bad.palette[223][0] = 1;
	reject(&bad, EINVAL, "reserved palette");
	bad = gradient; bad.type = UINT32_MAX;
	reject(&bad, EINVAL, "type");
	bad = gradient; bad.x = UINT32_MAX;
	reject(&bad, EINVAL, "origin overflow");
	bad = gradient; bad.w = UINT32_MAX;
	reject(&bad, EINVAL, "extent overflow");
	bad = gradient; bad.w = 1;
	reject(&bad, EINVAL, "too few columns");
	bad = gradient; bad.h = 1;
	reject(&bad, EINVAL, "too few rows");
	bad = gradient; bad.top = 0x1000000;
	reject(&bad, EINVAL, "RGB overflow");
	image = gradient;
	image.type = WSDISPLAY_DECOR_IMAGE;
	image.width = current.width;
	image.height = current.height;
	image.ncolors = 1;
	image.pixels = 1;
	reject(&image, EFAULT, "bad image pointer");
	pixels = calloc(image.width, image.height);
	if (pixels == NULL)
		err(1, "calloc");
	image.pixels = (uintptr_t)pixels;
	bad = image; bad.ncolors = 224;
	reject(&bad, EINVAL, "palette overflow");
	bad = image; bad.panel = 1;
	reject(&bad, EINVAL, "invalid panel index");
	pixels[0] = 1;
	reject(&image, EINVAL, "invalid pixel index");
	image.ncolors = 2;
	reject(&image, EINVAL, "nonuniform text panel");
	free(pixels);

	ro = open("/dev/ttyE0", O_RDONLY | O_NOCTTY);
	if (ro == -1 || ioctl(ro, WSDISPLAYIO_SDECOR, &gradient) != -1 ||
	    errno != EBADF)
		errx(1, "read-only setter accepted");
	close(ro);
	puts("PASS read-only descriptor");
	child = fork();
	if (child == -1)
		err(1, "fork");
	if (child == 0) {
		if (setuid(32767) == -1)
			_exit(2);
		_exit(ioctl(fd, WSDISPLAYIO_SDECOR, &gradient) == -1 &&
		    errno == EPERM ? 0 : 3);
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		errx(1, "unprivileged setter accepted: status %d", status);
	puts("PASS unprivileged setter");
	mode = WSDISPLAYIO_MODE_MAPPED;
	if (ioctl(fd, WSDISPLAYIO_SMODE, &mode) == -1)
		err(1, "mapped mode");
	reject(&gradient, EBUSY, "graphics mode");
	mode = WSDISPLAYIO_MODE_EMUL;
	if (ioctl(fd, WSDISPLAYIO_SMODE, &mode) == -1)
		err(1, "emulation mode");

	signal(SIGWINCH, winch);
	bad = gradient;
	bad.x = 288; bad.y = 176; bad.w = 704; bad.h = 672;
	if (ioctl(fd, WSDISPLAYIO_SDECOR, &gradient) == -1 ||
	    ioctl(fd, TIOCSWINSZ, &spoof) == -1)
		err(1, "prepare resize with an independently changed tty size");
	resized = 0;
	if (ioctl(fd, WSDISPLAYIO_SDECOR, &bad) == -1)
		err(1, "resize to inset");
	sizecheck("/dev/ttyE0", 42, 88);
	sizecheck("/dev/ttyE1", 42, 88);
	if (resized == 0)
		errx(1, "missing SIGWINCH");
	puts("PASS resize all screens and SIGWINCH despite pre-set tty size");
	if (ioctl(fd, WSDISPLAYIO_SDECOR, &gradient) == -1)
		err(1, "resize to full screen");
	sizecheck("/dev/ttyE0", current.height / 16, current.width / 8);
	sizecheck("/dev/ttyE1", current.height / 16, current.width / 8);
	puts("PASS restore full-screen gradient");
	close(fd);
	return 0;
}
