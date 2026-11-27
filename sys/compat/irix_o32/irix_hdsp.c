/* IRIX Audio Library master-device controls for the native Indy HAL2 mixer. */
#include <sys/param.h>
#include <sys/audioio.h>
#include <sgimips/hpc/haltworeg.h>
#include <sys/atomic.h>
#include <sys/kmem.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/stat.h>
#include <sys/systm.h>

#include <dev/audio/audio_if.h>
#include <compat/irix_o32/irix_hdsp.h>

#define HDSP_SET_PARAMS 24
#define HDSP_GET_PARAMS 25
#define HDSP_QUERY_VALUES 26
#define HDSP_GET_PARAM_INFO 27
#define HDSP_LOOKUP 28
#define AL_NULL_RESOURCE 2
#define AL_DEFAULT_INPUT 9
#define AL_RESOURCE AL_PARAM(1)
#define AL_INTERFACE AL_PARAM(2)
#define AL_CHANNELS AL_PARAM(4)
#define AL_MAX_SETSIZE AL_PARAM(8)
#define AL_DEVICES AL_PARAM(12)
#define AL_RATE AL_PARAM(13)
#define AL_MASTER_CLOCK AL_PARAM(17)
#define AL_TYPE AL_PARAM(18)
#define AL_NAME AL_PARAM(19)
#define AL_PARAMS AL_PARAM(20)
#define AL_GAIN_REF AL_PARAM(27)
#define AL_SUBSYSTEM AL_PARAM(30)
#define AL_LABEL AL_PARAM(31)
#define AL_SHORT_NAME AL_PARAM(36)
#define AL_INPUT_PRESENT AL_PARAM(40)
#define AL_VERSION AL_PARAM(43)
#define CRYSTAL_44100 13
#define CRYSTAL_48000 14
#define CRYSTAL_TYPE 0x2000000a
#define OUTPUT_TYPE 0x2000001c
#define GAIN_OFF INT64_MIN

/* A pointer or int occupies the first four bytes of an n32 ALvalue. */
union al_value {
	uint32_t word[2];
	int64_t fixed;
};

struct al_info {
	uint32_t resource, param;
	int32_t kind, element, capacity, columns;
	char name[32];
	union al_value initial, minimum, maximum, min_delta, max_delta;
	int32_t special, ops, spare[15];
};

struct al_query {
	union al_value dest;
	int32_t version, resource, param, count, spare[8];
};

struct al_pv {
	uint32_t param, pad;
	union al_value value;
	int16_t size, size2, used, used2;
};

static const struct {
	uint32_t param;
	const char *name;
	int kind, element, count, operations;
} controls[] = {
	{ AL_INTERFACE, "Physical Interface", 2, 0x11, 1, 5 },
	{ AL_CHANNELS, "Channels", 2, 0x12, 1, 1 },
	{ AL_RATE, "Sample Rate", 2, 0x21, 1, 3 },
	{ AL_MASTER_CLOCK, "Master Clock", 2, 0x11, 1, 7 },
	{ AL_TYPE, "Resource Type", 2, 0x12, 1, 1 },
	{ AL_NAME, "Full Resource Name", 4, 0x80, 32, 1 },
	{ AL_PARAMS, "Resource Parameters", 3, 0x12, 64, 4 },
	{ AL_GAIN, "Gain", 1, 0x21, 2, 11 },
	{ AL_GAIN_REF, "Gain Reference", 2, 0x12, 1, 1 },
	{ AL_MUTE, "Mute Control", 2, 0x12, 1, 15 },
	{ AL_LABEL, "Resource Label", 4, 0x80, 32, 1 },
	{ AL_SHORT_NAME, "Short Resource Name", 4, 0x80, 32, 1 },
};

/* The virtual output clock feeds NetBSD per-track sample-rate conversion. */
static unsigned playback_rate, record_rate;
static unsigned stereo_microphone;

unsigned
irix_hdsp_microphone_channels(void)
{
	return atomic_load_relaxed(&stereo_microphone) ? 2 : 1;
}

unsigned
irix_hdsp_rate(bool record)
{
	return record ? atomic_load_relaxed(&record_rate) : atomic_load_relaxed(&playback_rate);
}

static bool
is_output(uint32_t id)
{
	return id == ANALOG_OUT || id == AL_DEFAULT_OUTPUT;
}

static bool
is_input(uint32_t id)
{
	return id == ANALOG_IN || id == AL_DEFAULT_INPUT;
}

static uint32_t
resource_type(uint32_t id)
{
	if (is_input(id))
		return 0x2000001b;
	if (id == MICROPHONE)
		return 0x2000000d;
	if (id == SPEAKER)
		return 0x20000011;
	return (id == CRYSTAL_44100 || id == CRYSTAL_48000) ? CRYSTAL_TYPE : OUTPUT_TYPE;
}

/* Express the eight-bit analog gain as 20*log10(level) in signed 32.32 dB.
 * Compute log2 by repeated squaring with 24 fractional bits. */
static int64_t
gain_db(unsigned level)
{
	uint32_t logarithm;
	uint64_t x;
	int n;

	if (level == 0)
		return GAIN_OFF;
	n = 31 - __builtin_clz(level);
	logarithm = (uint32_t)n << 24;
	x = (uint64_t)level << (31 - n);
	for (int bit = 23; bit >= 0; bit--) {
		x = (x * x) >> 31;
		if (x >= (UINT64_C(1) << 32)) {
			logarithm |= 1U << bit;
			x >>= 1;
		}
	}
	return ((uint64_t)logarithm * UINT64_C(25858279730)) >> 24;
}

static unsigned
gain_level(int64_t gain)
{
	unsigned low = 1, high = 255;

	if (gain == GAIN_OFF)
		return 0;
	if (gain <= 0)
		return 1;
	if (gain >= gain_db(255))
		return 255;
	while (high - low > 1) {
		unsigned mid = (high + low) / 2;
		if (gain_db(mid) <= gain)
			low = mid;
		else
			high = mid;
	}
	return gain - gain_db(low) <= gain_db(high) - gain ? low : high;
}

static bool
is_crystal(uint32_t id)
{
	return id == CRYSTAL_44100 || id == CRYSTAL_48000;
}

static int
hardware_clock(file_t *fp, bool record, uint32_t *resource, unsigned *rate)
{
	audio_info_t format;
	int error = (*fp->f_ops->fo_ioctl)(fp, AUDIO_GETFORMAT, &format);
	if (error)
		return error;
	*rate = irix_hdsp_rate(record);
	if (*rate == 0)
		*rate = record ? format.record.sample_rate : format.play.sample_rate;
	switch (*rate) {
	case 44100: *resource = CRYSTAL_44100; return 0;
	case 48000: *resource = CRYSTAL_48000; return 0;
	default: return ENODEV;
	}
}

static bool
has_control(uint32_t resource, uint32_t param)
{
	if (is_input(resource) || resource == MICROPHONE) {
		if (is_input(resource) && (param == AL_INTERFACE || param == AL_MASTER_CLOCK || param == AL_RATE))
			return true;
		return param == AL_TYPE || param == AL_NAME || param == AL_LABEL ||
		    param == AL_SHORT_NAME || param == AL_PARAMS || param == AL_CHANNELS;
	}
	if (is_crystal(resource))
		return param == AL_TYPE || param == AL_NAME || param == AL_LABEL ||
		    param == AL_SHORT_NAME || param == AL_PARAMS;
	if (resource == SPEAKER)
		return param != AL_INTERFACE && param != AL_MASTER_CLOCK && param != AL_RATE;
	return is_output(resource);
}

static int
param_info(uint32_t resource, uint32_t param, struct al_info *info, bool capture)
{
	memset(info, 0, sizeof(*info));
	info->resource = resource;
	info->param = param;
	info->columns = 1;
	if (resource == AL_SYSTEM) {
		const char *name;
		info->kind = 2; /* scalar */
		info->element = 0x11; /* resource */
		info->capacity = 1;
		info->ops = 1; /* get */
		switch (param) {
		case AL_DEVICES:
			name = "System Devices";
			info->kind = 3;
			info->capacity = 64;
			info->ops = 4;
			break;
		case AL_DEFAULT_INPUT:
		case AL_DEFAULT_OUTPUT:
			name = param == AL_DEFAULT_INPUT ? "Default Input" : "Default Output";
			info->initial.word[0] = param == AL_DEFAULT_INPUT ?
			    (capture ? ANALOG_IN : AL_NULL_RESOURCE) : ANALOG_OUT;
			info->ops = 5;	/* get, query choices */
			break;
		case AL_MAX_SETSIZE:
		case AL_VERSION:
			name = param == AL_VERSION ? "AL Version" : "Maximum Setsize";
			info->initial.word[0] = param == AL_VERSION ? 6 : 64;
			info->element = 0x10;
			break;
		default:
			return ENOENT;
		}
		strlcpy(info->name, name, sizeof(info->name));
		return 0;
	}
	if (!has_control(resource, AL_PARAMS) ||
	    (!capture && (is_input(resource) || resource == MICROPHONE)))
		return ENODEV;
	for (size_t i = 0; i < __arraycount(controls); i++) {
		if (controls[i].param != param || !has_control(resource, param))
			continue;
		info->kind = controls[i].kind;
		info->element = controls[i].element;
		info->capacity = controls[i].count;
		info->ops = controls[i].operations;
		strlcpy(info->name, controls[i].name, sizeof(info->name));
		switch (param) {
		case AL_GAIN:
			info->initial.fixed = gain_db(64);
			info->maximum.fixed = gain_db(255);
			info->min_delta.fixed = gain_db(255) - gain_db(254);
			info->max_delta.fixed = gain_db(2);
			info->special = 2; /* negative infinity represents silence */
			break;
		case AL_CHANNELS:
			info->initial.word[0] = resource == MICROPHONE ? 1 : 2;
			break;
		case AL_TYPE:
			info->initial.word[0] = resource_type(resource);
			break;
		case AL_MASTER_CLOCK: info->initial.word[0] = CRYSTAL_48000; break;
		case AL_RATE:
			info->initial.fixed = info->maximum.fixed = INT64_C(48000) << 32;
			info->minimum.fixed = INT64_C(44100) << 32;
			break;
		case AL_INTERFACE: info->initial.word[0] = is_input(resource) ? MICROPHONE : SPEAKER; break;
		case AL_GAIN_REF: info->initial.word[1] = 1; break;
		}
		return 0;
	}
	return ENOENT;
}

static int
query_values(struct al_query *q, register_t *rval, bool capture)
{
	union al_value values[__arraycount(controls)] = { 0 };
	size_t count;

	if (q->version != 7 || q->count < 0)
		return EINVAL;
	if (!capture && (is_input(q->resource) || q->resource == MICROPHONE))
		return ENODEV;
	if (q->resource == AL_SYSTEM && q->param == AL_DEVICES) {
		count = 0;
		if (capture)
			values[count++].word[0] = ANALOG_IN;
		values[count++].word[0] = ANALOG_OUT;
	} else if (q->resource == AL_SYSTEM && q->param == AL_DEFAULT_OUTPUT) {
		values[0].word[0] = ANALOG_OUT;
		count = 1;
	} else if (q->resource == AL_SYSTEM && q->param == AL_DEFAULT_INPUT) {
		values[0].word[0] = ANALOG_IN;
		count = capture;
	} else if ((is_input(q->resource) || is_output(q->resource)) && (q->param == AL_INTERFACE ||
	    q->param == AL_MASTER_CLOCK)) {
		if (q->param == AL_MASTER_CLOCK) {
			values[0].word[0] = CRYSTAL_44100;
			values[1].word[0] = CRYSTAL_48000;
			count = 2;
		} else {
			values[0].word[0] = is_input(q->resource) ? MICROPHONE : SPEAKER;
			count = 1;
		}
	} else if (q->param == AL_MUTE &&
	    (is_output(q->resource) || q->resource == SPEAKER)) {
		values[1].word[0] = 1;
		count = 2;
	} else if (has_control(q->resource, AL_PARAMS) && q->param == AL_PARAMS) {
		count = 0;
		for (size_t i = 0; i < __arraycount(controls); i++) {
			if (has_control(q->resource, controls[i].param))
				values[count++].word[0] = controls[i].param;
		}
	} else {
		return ENOENT;
	}
	*rval = count;
	count = MIN(count, (size_t)q->count);
	if (count == 0)
		return 0;
	return copyout(values, (void *)(intptr_t)(int32_t)q->dest.word[0],
	    count * sizeof(values[0]));
}

/* Resource paths are relative to the supplied scope, including that scope. */
static int
lookup_resource(void *address, bool capture)
{
	struct {
		char name[80];
		uint32_t path[8], depth;
	} req;
	const uint32_t speaker[] = { AL_SYSTEM, INDY_SUBSYSTEM, ANALOG_OUT, SPEAKER };
	const uint32_t microphone[] = { AL_SYSTEM, INDY_SUBSYSTEM, ANALOG_IN, MICROPHONE };
	const uint32_t *path = speaker;
	unsigned first = 0, last;
	int error;

	error = copyin(address, &req, sizeof(req));
	if (error)
		return error;
	if (memchr(req.name, 0, sizeof(req.name)) == NULL)
		return EINVAL;
	if (strcmp(req.name, "A2.Speaker") == 0 || strcmp(req.name, "Speaker") == 0 ||
	    strcmp(req.name, "Headphone/Speakers") == 0)
		last = 3;
	else if (strcmp(req.name, "A2.AnalogOut") == 0 || strcmp(req.name, "AnalogOut") == 0 ||
	    strcmp(req.name, "Analog Out") == 0)
		last = 2;
	else if (capture && (strcmp(req.name, "A2.AnalogIn") == 0 ||
	    strcmp(req.name, "AnalogIn") == 0 || strcmp(req.name, "Analog In") == 0)) {
		path = microphone;
		last = 2;
	} else if (capture && (strcmp(req.name, "A2.Microphone") == 0 ||
	    strcmp(req.name, "Microphone") == 0)) {
		path = microphone;
		last = 3;
	} else if (strcmp(req.name, "A2") == 0)
		last = 1;
	else
		last = 0;
	while (first < __arraycount(speaker) && path[first] != req.path[0])
		first++;
	req.depth = last != 0 && first <= last ? last - first + 1 : 0;
	memcpy(req.path, path + first, req.depth * sizeof(speaker[0]));
	return copyout(&req, address, sizeof(req));
}

static int
param_value(file_t *fp, uint32_t resource, struct al_pv *pv, bool set, bool capture)
{
	int64_t gain[2];
	mixer_ctrl_t control = { .dev = HALTWO_MASTER_VOL, .type = AUDIO_MIXER_VALUE };
	void *ptr = (void *)(intptr_t)(int32_t)pv->value.word[0];
	const char *s;
	uint32_t crystal;
	unsigned rate;
	int error;

	pv->used2 = 0;
	if (set && (is_output(resource) || is_input(resource)) && (pv->param == AL_RATE ||
	    pv->param == AL_MASTER_CLOCK)) {
		if (pv->param == AL_RATE) {
			if (pv->value.fixed == (INT64_C(44100) << 32))
				rate = 44100;
			else if (pv->value.fixed == (INT64_C(48000) << 32))
				rate = 48000;
			else {
				pv->used = -1;
				return 0;
			}
		} else if (pv->value.word[0] == CRYSTAL_44100)
			rate = 44100;
		else if (pv->value.word[0] == CRYSTAL_48000)
			rate = 48000;
		else {
			pv->used = -1;
			return 0;
		}
		if (is_input(resource))
			atomic_store_relaxed(&record_rate, rate);
		else
			atomic_store_relaxed(&playback_rate, rate);
		pv->used = 1;
		return 0;
	}
	if (set && resource == MICROPHONE && pv->param == AL_CHANNELS) {
		unsigned channels = pv->value.word[0];
		if (channels != 1 && channels != 2) {
			pv->used = -1;
			return 0;
		}
		atomic_store_relaxed(&stereo_microphone, channels == 2);
		pv->used = 1;
		return 0;
	}
	if (set && ((pv->param != AL_GAIN && pv->param != AL_MUTE) ||
	    !has_control(resource, pv->param))) {
		pv->used = -1;
		return 0;
	}
	if (resource == AL_SYSTEM) {
		pv->value.fixed = 0;
		pv->used = 1;
		switch (pv->param) {
		case AL_DEFAULT_OUTPUT: pv->value.word[0] = ANALOG_OUT; break;
		case AL_DEFAULT_INPUT: pv->value.word[0] = capture ? ANALOG_IN : AL_NULL_RESOURCE; break;
		case AL_MAX_SETSIZE: pv->value.word[0] = 64; break;
		case AL_VERSION: pv->value.word[0] = 6; break;
		default: pv->used = -1;
		}
		return 0;
	}
	if (resource == INDY_SUBSYSTEM) {
		pv->used = 1;
		if (pv->param == AL_TYPE) {
			pv->value.word[0] = 0x2000001d;
			return 0;
		}
		if (pv->param != AL_NAME && pv->param != AL_LABEL && pv->param != AL_SHORT_NAME) {
			pv->used = -1;
			return 0;
		}
		pv->used = sizeof("A2");
		if (pv->size <= 0)
			return 0;
		return copyout("A2", ptr, MIN(pv->size, pv->used));
	}
	/* Sound Editor asks for this together with AL_RATE. On an Indy the
	 * internally clocked analog input reports present even during silence. */
	if (!set && is_input(resource) && pv->param == AL_INPUT_PRESENT) {
		pv->value.fixed = 0;
		pv->value.word[0] = capture;
		pv->used = 1;
		return 0;
	}
	if (pv->param != AL_SUBSYSTEM && !has_control(resource, pv->param)) {
		pv->used = -1;
		return 0;
	}
	switch (pv->param) {
	case AL_RATE:
	case AL_MASTER_CLOCK:
		error = hardware_clock(fp, is_input(resource), &crystal, &rate);
		if (error)
			return error;
		if (pv->param == AL_RATE)
			pv->value.fixed = (int64_t)rate << 32;
		else
			pv->value.word[0] = crystal;
		pv->used = 1;
		return 0;
	case AL_SUBSYSTEM:
		pv->value.word[0] = INDY_SUBSYSTEM;
		pv->used = 1;
		return 0;
	case AL_INTERFACE:
		pv->value.word[0] = is_input(resource) ? MICROPHONE : SPEAKER;
		pv->used = resource == SPEAKER ? -1 : 1;
		return 0;
	case AL_TYPE:
		pv->value.word[0] = resource_type(resource);
		pv->used = 1;
		return 0;
	case AL_CHANNELS:
		pv->value.word[0] = resource == MICROPHONE ?
		    irix_hdsp_microphone_channels() : 2;
		pv->used = 1;
		return 0;
	case AL_GAIN_REF:
		pv->value.fixed = 1;
		pv->used = 1;
		return 0;
	case AL_NAME:
	case AL_LABEL:
	case AL_SHORT_NAME:
		if (resource == CRYSTAL_44100)
			s = pv->param == AL_NAME ? "A2.Crystal441Mclk" :
			    pv->param == AL_LABEL ? "44100 Hz Internal Clock" : "Crystal441Mclk";
		else if (resource == CRYSTAL_48000)
			s = pv->param == AL_NAME ? "A2.Crystal48Mclk" :
			    pv->param == AL_LABEL ? "48 kHz Internal Clock" : "Crystal48Mclk";
		else if (is_input(resource))
			s = pv->param == AL_NAME ? "A2.AnalogIn" :
			    pv->param == AL_LABEL ? "Analog In" : "AnalogIn";
		else if (resource == MICROPHONE)
			s = pv->param == AL_NAME ? "A2.Microphone" : "Microphone";
		else if (resource == SPEAKER)
			s = pv->param == AL_NAME ? "A2.Speaker" :
			    pv->param == AL_LABEL ? "Headphone/Speakers" : "Speaker";
		else
			s = pv->param == AL_NAME ? "A2.AnalogOut" :
			    pv->param == AL_LABEL ? "Analog Out" : "AnalogOut";
		pv->used = strlen(s) + 1;
		if (pv->size <= 0)
			return 0;
		return copyout(s, ptr, MIN(pv->size, pv->used));
	case AL_MUTE:
		control.dev = HALTWO_MASTER_MUTE;
		control.type = AUDIO_MIXER_ENUM;
		if (set && pv->value.word[0] > 1) {
			pv->used = -1;
			return 0;
		}
		error = (*fp->f_ops->fo_ioctl)(fp, AUDIO_MIXER_READ, &control);
		if (error)
			return error;
		if (set && (uint32_t)control.un.ord != pv->value.word[0]) {
			control.un.ord = pv->value.word[0];
			error = (*fp->f_ops->fo_ioctl)(fp, AUDIO_MIXER_WRITE, &control);
			if (error)
				return error;
		}
		pv->used = 1;
		pv->value.fixed = 0;
		pv->value.word[0] = control.un.ord;
		return 0;
	case AL_GAIN:
		if (pv->size < 0 || (set && pv->size != 2)) {
			pv->used = -1;
			return 0;
		}
		control.un.value.num_channels = 2;
		if (set) {
			error = copyin(ptr, gain, sizeof(gain));
			if (error)
				return error;
			for (unsigned i = 0; i < 2; i++)
				control.un.value.level[i] = gain_level(gain[i]);
		}
		error = (*fp->f_ops->fo_ioctl)(fp, set ? AUDIO_MIXER_WRITE :
		    AUDIO_MIXER_READ, &control);
		if (error)
			return error;
		pv->used = 2;
		if (set)
			irix_hdsp_changed(AL_GAIN, 0);
		if (set || pv->size == 0)
			return 0;
		for (unsigned i = 0; i < 2; i++)
			gain[i] = gain_db(control.un.value.level[i]);
		return copyout(gain, ptr, MIN(pv->size, 2) * sizeof(gain[0]));
	default:
		pv->used = -1;
		return 0;
	}
}

static int
params(file_t *fp, void *address, bool set, register_t *rval, bool capture)
{
	struct al_pv *pv;
	uint32_t count;
	size_t nbytes;
	int error;

	error = copyin(address, &count, sizeof(count));
	if (error)
		return error;
	/* libaudio sends a count record, a resource selector, then up to 62 PVs. */
	if (count < 2 || count > 64)
		return EINVAL;
	nbytes = count * sizeof(*pv);
	pv = kmem_alloc(nbytes, KM_SLEEP);
	error = copyin(address, pv, nbytes);
	if (error == 0 && (pv[0].param != count || pv[1].param != AL_RESOURCE))
		error = EINVAL;
	if (error == 0 && !is_output(pv[1].value.word[0]) &&
	    !(capture && (is_input(pv[1].value.word[0]) || pv[1].value.word[0] == MICROPHONE)) &&
	    pv[1].value.word[0] != AL_SYSTEM && pv[1].value.word[0] != SPEAKER &&
	    pv[1].value.word[0] != INDY_SUBSYSTEM && !is_crystal(pv[1].value.word[0]))
		error = ENODEV;
	*rval = 0;
	for (uint32_t i = 2; !error && i < count; i++) {
		error = param_value(fp, pv[1].value.word[0], &pv[i], set, capture);
		if (pv[i].used >= 0)
			(*rval)++;
	}
	if (!error)
		error = copyout(pv, address, nbytes);
	kmem_free(pv, nbytes);
	return error;
}

int
irix_hdsp_ioctl(int fd, uint32_t cmd, void *address, register_t *rval)
{
	audio_device_t device;
	struct al_info info;
	struct al_query query;
	struct stat st;
	uint32_t ids[2];
	int error;
	file_t *fp;

	CTASSERT(sizeof(struct al_info) == 168);
	CTASSERT(sizeof(struct al_pv) == 24);
	CTASSERT(sizeof(struct al_query) == 56);
	fp = fd_getfile(fd);
	if (fp == NULL)
		return EBADF;
	if (fp->f_type != DTYPE_MISC || fp->f_ops->fo_name == NULL ||
	    strcmp(fp->f_ops->fo_name, "audio") != 0) {
		error = ENOSYS;
		goto out;
	}
	error = (*fp->f_ops->fo_stat)(fp, &st);
	if (error)
		goto out;
	if (minor(st.st_dev) != AUDIOCTL_DEVICE) {
		error = ENOSYS;
		goto out;
	}
	if (!(fp->f_flag & (cmd == HDSP_SET_PARAMS ? FWRITE : FREAD))) {
		error = EBADF;
		goto out;
	}
	error = (*fp->f_ops->fo_ioctl)(fp, AUDIO_GETDEV, &device);
	if (error)
		goto out;
	if (strcmp(device.name, "HAL2") != 0) {
		error = ENODEV;
		goto out;
	}
	int props;
	error = fp->f_ops->fo_ioctl(fp, AUDIO_GETPROPS, &props);
	if (error)
		goto out;
	bool capture = (props & AUDIO_PROP_CAPTURE) != 0;
	*rval = 0;
	switch (cmd) {
	case 2: { /* FIND_MINOR: each open of this node clones a private port. */
		int32_t minor = 2;
		error = copyout(&minor, address, sizeof(minor));
		break;
	}
	case HDSP_SET_PARAMS:
	case HDSP_GET_PARAMS:
		error = params(fp, address, cmd == HDSP_SET_PARAMS, rval, capture);
		break;
	case HDSP_QUERY_VALUES:
		error = copyin(address, &query, sizeof(query));
		if (!error)
			error = query_values(&query, rval, capture);
		break;
	case HDSP_LOOKUP:
		error = lookup_resource(address, capture);
		break;
	case HDSP_GET_PARAM_INFO:
		error = copyin(address, ids, sizeof(ids));
		if (!error)
			error = param_info(ids[0], ids[1], &info, capture);
		if (!error)
			error = copyout(&info, address, sizeof(info));
		break;
	default:
		error = ENOTTY;
	}
out:
	fd_putfile(fd);
	return error;
}
