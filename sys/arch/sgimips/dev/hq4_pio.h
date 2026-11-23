#ifndef _IP30_HQ4_PIO_H_
#define _IP30_HQ4_PIO_H_

#define HQ4_WIDTH	1280
#define HQ4_HEIGHT	1024
#define HQ4_REG_SIZE	0x74000

struct hq4_attach_args {
	bus_space_tag_t st;
	bus_addr_t base;
};

bool hq4_fifo_empty(bus_space_tag_t, bus_space_handle_t);
void hq4_cmd(bus_space_tag_t, bus_space_handle_t, unsigned, uint32_t, bool);
void hq4_setup(bus_space_tag_t, bus_space_handle_t);
void hq4_rectangle(bus_space_tag_t, bus_space_handle_t, unsigned, unsigned,
    unsigned, unsigned, uint32_t);

#endif
