#ifndef _COMPAT_IRIX_O32_IRIX_HDSP_H_
#define _COMPAT_IRIX_O32_IRIX_HDSP_H_

#define AL_SYSTEM 11
#define AL_DEFAULT_OUTPUT 10
#define ANALOG_IN 5
#define ANALOG_OUT 6
#define MICROPHONE 19
#define SPEAKER 22
#define INDY_SUBSYSTEM 25
#define AL_PARAM(n) (0x10000000U | (n))
#define AL_GAIN AL_PARAM(25)
#define AL_MUTE AL_PARAM(29)

int irix_hdsp_ioctl(int, uint32_t, void *, register_t *);

int irix_hdsp_event_ioctl(int, uint32_t, void *, register_t *);
int irix_hdsp_events_init(void);
int irix_hdsp_events_quiesce(void);
void irix_hdsp_events_resume(void);
void irix_hdsp_events_fini(void);
void irix_hdsp_changed(uint32_t, uint32_t);
int irix_hdsp_port_open(int);
void irix_hdsp_port_release(void);
int irix_hdsp_pcm_ioctl(int, uint32_t, void *, register_t *);
unsigned irix_hdsp_rate(bool);
unsigned irix_hdsp_microphone_channels(void);

#endif
