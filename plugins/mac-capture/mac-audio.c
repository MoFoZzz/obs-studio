#include <AudioUnit/AudioUnit.h>
#include <CoreFoundation/CFString.h>
#include <CoreAudio/CoreAudio.h>
#include <unistd.h>
#include <errno.h>

#include <obs.h>
#include <util/threading.h>
#include <util/c99defs.h>
#include <util/dstr.h>

#include "mac-helpers.h"

#define SCOPE_OUTPUT kAudioUnitScope_Output
#define SCOPE_INPUT  kAudioUnitScope_Input
#define SCOPE_GLOBAL kAudioUnitScope_Global

#define BUS_OUTPUT 0
#define BUS_INPUT  1

#define MAX_DEVICES 20

#define set_property AudioUnitSetProperty
#define get_property AudioUnitGetProperty

struct coreaudio_data {
	char               *device_name;
	char               *device_uid;
	AudioUnit           unit;
	AudioDeviceID       device_id;
	AudioBufferList    *buf_list;
	bool                au_initialized;
	bool                active;

	uint32_t            sample_rate;
	enum audio_format   format;
	enum speaker_layout speakers;

	pthread_t           reconnect_thread;
	event_t             exit_event;
	volatile bool       reconnecting;

	obs_source_t        source;
};

static bool find_device_id_by_uid(const char *uid, AudioDeviceID *id)
{
	OSStatus    stat;
	UInt32      size = sizeof(AudioDeviceID);
	CFStringRef cf_uid = CFStringCreateWithCString(NULL, uid,
			kCFStringEncodingUTF8);
	CFStringRef qual = NULL;
	UInt32      qual_size = 0;
	bool        success;

	AudioObjectPropertyAddress addr = {
		.mScope   = kAudioObjectPropertyScopeGlobal,
		.mElement = kAudioObjectPropertyElementMaster
	};

	bool is_default = (strcmp(uid, "Default") == 0);

	if (is_default) {
		addr.mSelector = kAudioHardwarePropertyDefaultInputDevice;
	} else {
		addr.mSelector = kAudioHardwarePropertyTranslateUIDToDevice;
		qual      = cf_uid;
		qual_size = sizeof(CFStringRef);
	}

	stat = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
			qual_size, &qual, &size, id);
	success = (stat == noErr);

	CFRelease(cf_uid);
	return success;
}

static inline void ca_warn(struct coreaudio_data *ca, const char *func,
		const char *format, ...)
{
	va_list args;
	struct dstr str = {0};

	va_start(args, format);

	dstr_printf(&str, "[%s]:[device '%s'] ", func, ca->device_name);
	dstr_vcatf(&str, format, args);
	blog(LOG_WARNING, "%s", str.array);
	dstr_free(&str);

	va_end(args);
}

static inline bool ca_success(OSStatus stat, struct coreaudio_data *ca,
		const char *func, const char *action)
{
	if (stat != noErr) {
		blog(LOG_WARNING, "[%s]:[device '%s'] %s failed: %d",
				func, ca->device_name, action, (int)stat);
		return false;
	}

	return true;
}

enum coreaudio_io_type {
	IO_TYPE_INPUT,
	IO_TYPE_OUTPUT,
};

static inline bool enable_io(struct coreaudio_data *ca,
		enum coreaudio_io_type type, bool enable)
{
	UInt32 enable_int = enable;
	return set_property(ca->unit, kAudioOutputUnitProperty_EnableIO,
			(type == IO_TYPE_INPUT) ? SCOPE_INPUT : SCOPE_OUTPUT,
			(type == IO_TYPE_INPUT) ? BUS_INPUT   : BUS_OUTPUT,
			&enable_int, sizeof(enable_int));
}

static inline enum audio_format convert_ca_format(UInt32 format_flags,
		UInt32 bits)
{
	bool planar = (format_flags & kAudioFormatFlagIsNonInterleaved) != 0;

	if (format_flags & kAudioFormatFlagIsFloat)
		return planar ? AUDIO_FORMAT_FLOAT_PLANAR : AUDIO_FORMAT_FLOAT;

	if (!(format_flags & kAudioFormatFlagIsSignedInteger) && bits == 8)
		return planar ? AUDIO_FORMAT_U8BIT_PLANAR : AUDIO_FORMAT_U8BIT;

	/* not float?  not signed int?  no clue, fail */
	if ((format_flags & kAudioFormatFlagIsSignedInteger) == 0)
		return AUDIO_FORMAT_UNKNOWN;

	if (bits == 16)
		return planar ? AUDIO_FORMAT_16BIT_PLANAR : AUDIO_FORMAT_16BIT;
	else if (bits == 32)
		return planar ? AUDIO_FORMAT_32BIT_PLANAR : AUDIO_FORMAT_32BIT;

	return AUDIO_FORMAT_UNKNOWN;
}

static inline enum speaker_layout convert_ca_speaker_layout(UInt32 channels)
{
	/* directly map channel count to enum values */
	if (channels >= 1 && channels <= 8 && channels != 7)
		return (enum speaker_layout)channels;

	return SPEAKERS_UNKNOWN;
}

static bool coreaudio_init_format(struct coreaudio_data *ca)
{
	AudioStreamBasicDescription desc;
	OSStatus stat;
	UInt32 size = sizeof(desc);

	stat = get_property(ca->unit, kAudioUnitProperty_StreamFormat,
			SCOPE_INPUT, BUS_INPUT, &desc, &size);
	if (!ca_success(stat, ca, "coreaudio_init_format", "get input format"))
		return false;

	stat = set_property(ca->unit, kAudioUnitProperty_StreamFormat,
			SCOPE_OUTPUT, BUS_INPUT, &desc, size);
	if (!ca_success(stat, ca, "coreaudio_init_format", "set output format"))
		return false;

	if (desc.mFormatID != kAudioFormatLinearPCM) {
		ca_warn(ca, "coreaudio_init_format", "format is not PCM");
		return false;
	}

	ca->format = convert_ca_format(desc.mFormatFlags, desc.mBitsPerChannel);
	if (ca->format == AUDIO_FORMAT_UNKNOWN) {
		ca_warn(ca, "coreaudio_init_format", "unknown format flags: "
				"%u, bits: %u",
				(unsigned int)desc.mFormatFlags,
				(unsigned int)desc.mBitsPerChannel);
		return false;
	}

	ca->sample_rate = (uint32_t)desc.mSampleRate;
	ca->speakers = convert_ca_speaker_layout(desc.mChannelsPerFrame);

	if (ca->speakers == SPEAKERS_UNKNOWN) {
		ca_warn(ca, "coreaudio_init_format", "unknown speaker layout: "
				"%u channels",
				(unsigned int)desc.mChannelsPerFrame);
		return false;
	}

	return true;
}

static bool coreaudio_init_buffer(struct coreaudio_data *ca)
{
	UInt32 buf_size = 0;
	UInt32 size     = 0;
	UInt32 frames   = 0;
	OSStatus stat;

	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyStreamConfiguration,
		kAudioDevicePropertyScopeInput,
		kAudioObjectPropertyElementMaster
	};

	stat = AudioObjectGetPropertyDataSize(ca->device_id, &addr, 0, NULL,
			&buf_size);
	if (!ca_success(stat, ca, "coreaudio_init_buffer", "get list size"))
		return false;

	size = sizeof(frames);
	stat = get_property(ca->unit, kAudioDevicePropertyBufferFrameSize,
			SCOPE_GLOBAL, 0, &frames, &size);
	if (!ca_success(stat, ca, "coreaudio_init_buffer", "get frame size"))
		return false;

	/* ---------------------- */

	ca->buf_list = bmalloc(buf_size);

	stat = AudioObjectGetPropertyData(ca->device_id, &addr, 0, NULL,
			&buf_size, ca->buf_list);
	if (!ca_success(stat, ca, "coreaudio_init_buffer", "allocate")) {
		bfree(ca->buf_list);
		ca->buf_list = NULL;
		return false;
	}

	for (UInt32 i = 0; i < ca->buf_list->mNumberBuffers; i++) {
		size = ca->buf_list->mBuffers[i].mDataByteSize;
		ca->buf_list->mBuffers[i].mData = bmalloc(size);
	}
	
	return true;
}

static void buf_list_free(AudioBufferList *buf_list)
{
	if (buf_list) {
		for (UInt32 i = 0; i < buf_list->mNumberBuffers; i++)
			bfree(buf_list->mBuffers[i].mData);

		bfree(buf_list);
	}
}

static OSStatus input_callback(
		void *data,
		AudioUnitRenderActionFlags *action_flags,
		const AudioTimeStamp *ts_data,
		UInt32 bus_num,
		UInt32 frames,
		AudioBufferList *ignored_buffers)
{
	struct coreaudio_data *ca = data;
	OSStatus stat;
	struct source_audio audio;

	stat = AudioUnitRender(ca->unit, action_flags, ts_data, bus_num, frames,
			ca->buf_list);
	if (!ca_success(stat, ca, "input_callback", "audio retrieval"))
		return noErr;

	for (UInt32 i = 0; i < ca->buf_list->mNumberBuffers; i++)
		audio.data[i] = ca->buf_list->mBuffers[i].mData;

	audio.frames          = frames;
	audio.speakers        = ca->speakers;
	audio.format          = ca->format;
	audio.samples_per_sec = ca->sample_rate;
	audio.timestamp       = ts_data->mHostTime;

	obs_source_output_audio(ca->source, &audio);

	UNUSED_PARAMETER(ignored_buffers);
	return noErr;
}

static void coreaudio_stop(struct coreaudio_data *ca);
static bool coreaudio_init(struct coreaudio_data *ca);
static void coreaudio_uninit(struct coreaudio_data *ca);

#define RETRY_TIME 3000

static void *reconnect_thread(void *param)
{
	struct coreaudio_data *ca = param;

	ca->reconnecting = true;

	while (event_timedwait(ca->exit_event, RETRY_TIME) == ETIMEDOUT) {
		if (coreaudio_init(ca))
			break;
	}

	blog(LOG_DEBUG, "coreaudio: exit the reconnect thread");
	ca->reconnecting = false;
	return NULL;
}

static void coreaudio_begin_reconnect(struct coreaudio_data *ca)
{
	int ret;

	if (ca->reconnecting)
		return;

	ret = pthread_create(&ca->reconnect_thread, NULL, reconnect_thread, ca);
	if (ret != 0)
		blog(LOG_WARNING, "[coreaudio_begin_reconnect] failed to "
		                  "create thread, error code: %d", ret);
}

static OSStatus disconnection_callback(
		AudioObjectID id,
		UInt32 num_addresses,
		const AudioObjectPropertyAddress addresses[],
		void *data)
{
	struct coreaudio_data *ca = data;
	UInt32 alive = 0;
	UInt32 size  = sizeof(alive);
	OSStatus stat;

	stat = AudioObjectGetPropertyData(id, addresses, 0, NULL, &size,
			&alive);
	if (ca_success(stat, ca, "disconnection_callback", "get property")) {
		if (!alive) {
			coreaudio_stop(ca);
			coreaudio_uninit(ca);

			blog(LOG_INFO, "coreaudio: device '%s' disconnected.  "
			               "attempting to reconnect",
			               ca->device_name);

			coreaudio_begin_reconnect(ca);
		}
	}

	UNUSED_PARAMETER(num_addresses);
	return noErr;
}

static const AudioObjectPropertyAddress alive_addr = {
	kAudioDevicePropertyDeviceIsAlive,
	kAudioObjectPropertyScopeGlobal,
	kAudioObjectPropertyElementMaster
};

static bool coreaudio_init_hooks(struct coreaudio_data *ca)
{
	OSStatus stat;
	AURenderCallbackStruct callback_info = {
		.inputProc       = input_callback,
		.inputProcRefCon = ca
	};

	stat = AudioObjectAddPropertyListener(ca->device_id, &alive_addr,
			disconnection_callback, ca);
	if (!ca_success(stat, ca, "coreaudio_init_hooks",
				"set disconnect callback"))
		return false;

	stat = set_property(ca->unit, kAudioOutputUnitProperty_SetInputCallback,
			SCOPE_GLOBAL, 0, &callback_info, sizeof(callback_info));
	if (!ca_success(stat, ca, "coreaudio_init_hooks", "set input callback"))
		return false;

	return true;
}

static void coreaudio_remove_hooks(struct coreaudio_data *ca)
{
	AURenderCallbackStruct callback_info = {
		.inputProc       = NULL,
		.inputProcRefCon = NULL
	};

	AudioObjectRemovePropertyListener(ca->device_id, &alive_addr,
			disconnection_callback, ca);

	set_property(ca->unit, kAudioOutputUnitProperty_SetInputCallback,
			SCOPE_GLOBAL, 0, &callback_info, sizeof(callback_info));
}

static bool coreaudio_get_device_name(struct coreaudio_data *ca)
{
	CFStringRef cf_name = NULL;
	UInt32 size = sizeof(CFStringRef);
	char name[1024];

	const AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyDeviceNameCFString,
		kAudioObjectPropertyScopeInput,
		kAudioObjectPropertyElementMaster
	};

	OSStatus stat = AudioObjectGetPropertyData(ca->device_id, &addr,
			0, NULL, &size, &cf_name);
	if (stat != noErr) {
		blog(LOG_WARNING, "[coreaudio_get_device_name] failed to "
		                  "get name: %d", (int)stat);
		return false;
	}

	if (!cf_to_cstr(cf_name, name, 1024)) {
		blog(LOG_WARNING, "[coreaudio_get_device_name] failed to "
		                  "convert name to cstr for some reason");
		return false;
	}

	bfree(ca->device_name);
	ca->device_name = bstrdup(name);

	CFRelease(cf_name);
	return true;
}

static bool coreaudio_start(struct coreaudio_data *ca)
{
	OSStatus stat;

	if (ca->active)
		return true;

	stat = AudioOutputUnitStart(ca->unit);
	return ca_success(stat, ca, "coreaudio_start", "start audio");
}

static void coreaudio_stop(struct coreaudio_data *ca)
{
	OSStatus stat;

	if (!ca->active)
		return;

	ca->active = false;

	stat = AudioOutputUnitStop(ca->unit);
	ca_success(stat, ca, "coreaudio_stop", "stop audio");
}

static bool coreaudio_init_unit(struct coreaudio_data *ca)
{
	AudioComponentDescription desc = {
		.componentType    = kAudioUnitType_Output,
		.componentSubType = kAudioUnitSubType_HALOutput
	};

	AudioComponent component = AudioComponentFindNext(NULL, &desc);
	if (!component) {
		ca_warn(ca, "coreaudio_init_unit", "find component failed");
		return false;
	}

	OSStatus stat = AudioComponentInstanceNew(component, &ca->unit);
	if (!ca_success(stat, ca, "coreaudio_init_unit", "instance unit"))
		return false;

	ca->au_initialized = true;
	return true;
}

static bool coreaudio_init(struct coreaudio_data *ca)
{
	OSStatus stat;

	if (ca->au_initialized)
		return true;

	if (!find_device_id_by_uid(ca->device_uid, &ca->device_id))
		return false;
	if (!coreaudio_get_device_name(ca))
		return false;
	if (!coreaudio_init_unit(ca))
		return false;

	stat = enable_io(ca, IO_TYPE_INPUT, true);
	if (!ca_success(stat, ca, "coreaudio_init", "enable input io"))
		goto fail;

	stat = enable_io(ca, IO_TYPE_OUTPUT, false);
	if (!ca_success(stat, ca, "coreaudio_init", "disable output io"))
		goto fail;

	stat = set_property(ca->unit, kAudioOutputUnitProperty_CurrentDevice,
			SCOPE_GLOBAL, 0, &ca->device_id, sizeof(ca->device_id));
	if (!ca_success(stat, ca, "coreaudio_init", "set current device"))
		goto fail;

	if (!coreaudio_init_format(ca))
		goto fail;
	if (!coreaudio_init_buffer(ca))
		goto fail;
	if (!coreaudio_init_hooks(ca))
		goto fail;

	stat = AudioUnitInitialize(ca->unit);
	if (!ca_success(stat, ca, "coreaudio_initialize", "initialize"))
		goto fail;

	if (coreaudio_start(ca))
		goto fail;

	blog(LOG_INFO, "coreaudio: device '%s' initialized", ca->device_name);
	return ca->au_initialized;

fail:
	coreaudio_uninit(ca);
	return false;
}

static void coreaudio_try_init(struct coreaudio_data *ca)
{
	if (!coreaudio_init(ca)) {
		blog(LOG_INFO, "coreaudio: failed to find device "
		               "uid: %s, waiting for connection",
		               ca->device_uid);

		coreaudio_begin_reconnect(ca);
	}
}

static void coreaudio_uninit(struct coreaudio_data *ca)
{
	if (!ca->au_initialized)
		return;

	if (ca->unit) {
		coreaudio_stop(ca);

		OSStatus stat = AudioUnitUninitialize(ca->unit);
		ca_success(stat, ca, "coreaudio_uninit", "uninitialize");

		coreaudio_remove_hooks(ca);

		stat = AudioComponentInstanceDispose(ca->unit);
		ca_success(stat, ca, "coreaudio_uninit", "dispose");

		ca->unit = NULL;
	}

	ca->au_initialized = false;

	buf_list_free(ca->buf_list);
	ca->buf_list = NULL;
}

/* ------------------------------------------------------------------------- */

static const char *coreaudio_getname(const char *locale)
{
	/* TODO: Locale */
	UNUSED_PARAMETER(locale);
	return "CoreAudio Input";
}

static void coreaudio_destroy(void *data)
{
	struct coreaudio_data *ca = data;

	if (ca) {
		if (ca->reconnecting) {
			event_signal(ca->exit_event);
			pthread_join(ca->reconnect_thread, NULL);
		}

		coreaudio_uninit(ca);

		if (ca->unit)
			AudioComponentInstanceDispose(ca->unit);

		event_destroy(ca->exit_event);
		bfree(ca->device_name);
		bfree(ca->device_uid);
		bfree(ca);
	}
}

static void *coreaudio_create(obs_data_t settings, obs_source_t source)
{
	struct coreaudio_data *ca = bzalloc(sizeof(struct coreaudio_data));

	obs_data_set_default_string(settings, "device_id", "Default");

	if (event_init(&ca->exit_event, EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_WARNING, "[coreaudio_create] failed to create "
		                  "semephore: %d", errno);
		bfree(ca);
		return NULL;
	}

	ca->device_uid = bstrdup(obs_data_getstring(settings, "device_id"));
	ca->source = source;

	coreaudio_try_init(ca);
	return ca;
}

struct obs_source_info coreaudio_info = {
	.id           = "coreaudio_capture",
	.type         = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.getname      = coreaudio_getname,
	.create       = coreaudio_create,
	.destroy      = coreaudio_destroy,
};