/* Build on NetBSD with -I../../../sys; exercises the actual file decoder. */
#define main wsdecorctl_main
#include "../../../usr.sbin/wsdecorctl/wsdecorctl.c"
#undef main

int
main(int argc, char **argv)
{
	struct wsdisplay_decor current = { .width = 1282, .height = 1024 };
	struct wsdisplay_decor theme;

	if (argc != 2)
		return 2;
	free(load_theme(argv[1], &theme, &current));
	return 0;
}
