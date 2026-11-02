/* Manual hardware/emulator regression exerciser; see README.newport-decor. */
#include <sys/ioctl.h>
#include <dev/wscons/wsconsio.h>

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	struct termios saved, raw;
	struct winsize size;
	FILE *report = NULL;
	char command;
	int i, mode, fd;

	if (argc > 2)
		errx(1, "usage: newport_decor [progress-file-or-serial-device]");
	if (argc == 2) {
		fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK,
		    0600);
		if (fd == -1 || (report = fdopen(fd, "w")) == NULL)
			err(1, "progress output");
		if (isatty(fd)) {
			if (tcgetattr(fd, &raw) == -1)
				err(1, "serial attributes");
			cfmakeraw(&raw);
			raw.c_cflag |= CLOCAL;
			(void)cfsetspeed(&raw, B9600);
			if (tcsetattr(fd, TCSANOW, &raw) == -1)
				err(1, "serial attributes");
		}
		setvbuf(report, NULL, _IONBF, 0);
	}

	if (tcgetattr(STDIN_FILENO, &saved) == -1 ||
	    ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == -1)
		err(1, "console terminal");
	if (size.ws_row == 0 || size.ws_col == 0)
		errx(1, "console size is unknown");
	raw = saved;
	cfmakeraw(&raw);
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1)
		err(1, "tcsetattr");
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("\033[0m\033[?25l\033[2J\033[H");
	if (report != NULL)
		fprintf(report, "READY rows=%u cols=%u\n", size.ws_row,
		    size.ws_col);
	while (read(STDIN_FILENO, &command, 1) == 1 && command != 'q') {
		switch (command) {
		case 'b':	/* Full clear, including restoration of margins. */
			printf("\033[0m\033[2J\033[H");
			break;
		case 's':	/* Scroll blank text; the backdrop must not move. */
			printf("\033[%u;1H", size.ws_row);
			for (i = 0; i < size.ws_row + 3; i++)
				printf("\r\n");
			break;
		case 'r':	/* Scroll down using reverse index. */
			printf("\033[H");
			for (i = 0; i < size.ws_row + 3; i++)
				printf("\033M");
			break;
		case 'i':	/* Move columns on every gradient band. */
			for (i = 1; i <= size.ws_row; i++)
				printf("\033[%d;1H\033[7@\033[3P", i);
			break;
		case 'e':	/* Coloured fills, line erase, then row erase. */
			for (i = 1; i <= size.ws_row; i++)
				printf("\033[%d;1H\033[41m\033[2K"
				    "\033[0m\033[2K", i);
			printf("\033[H\033[J");
			break;
		case 'c':	/* Cursor inversion must be exactly reversible. */
			for (i = 1; i <= size.ws_row; i++)
				printf("\033[%d;%uH\033[?25h\033[?25l",
				    i, size.ws_col * 3 / 4);
			break;
		case 't':	/* Text, underline, reverse, and opaque backgrounds. */
			printf("\033[HNewport live console decoration\r\n"
			    "\033[4mUnderline\033[0m "
			    "\033[7mReverse\033[0m "
			    "\033[37;41mRed background\033[0m\r\n");
			for (i = 4; i < size.ws_row; i += 4)
				printf("\033[%d;%uHText over the fixed gradient",
				    i, size.ws_col / 2);
			break;
		case 'm':	/* Text and backdrop must survive a mode round trip. */
			mode = WSDISPLAYIO_MODE_MAPPED;
			if (ioctl(STDIN_FILENO, WSDISPLAYIO_SMODE, &mode) == -1)
				goto fail;
			mode = WSDISPLAYIO_MODE_EMUL;
			if (ioctl(STDIN_FILENO, WSDISPLAYIO_SMODE, &mode) == -1)
				goto fail;
			break;
		}
		(void)tcdrain(STDOUT_FILENO);
		if (report != NULL)
			fprintf(report, "DONE %c\n", command);
	}
	printf("\033[0m\033[?25h\033[H");
	if (tcsetattr(STDIN_FILENO, TCSANOW, &saved) == -1)
		err(1, "restore terminal");
	return 0;
fail:
	(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	err(1, "display mode");
}
