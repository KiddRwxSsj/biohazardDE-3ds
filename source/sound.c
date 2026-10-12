#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "s3e.h"

/*
 * s3eSound on one ndsp voice. The game mixes its own effects through per-channel
 * GEN_AUDIO callbacks, which the mixer thread calls; channels without one are
 * plain 16-bit PCM.
 */

#define NCHANNELS 16
#define OUTPUT_RATE 22050
#define BUF_SAMPLES 1024
#define NBUFS 4
#define DSP_VOICE 0

enum {
	CB_END_SAMPLE,
	CB_GEN_AUDIO,
	CB_GEN_AUDIO_STEREO,
	CB_STOP_AUDIO,
	CB_COUNT
};

enum {
	CH_STATUS,
	CH_RATE,
	CH_USERVAR,
	CH_VOLUME,
};

/* layout read out of the game's generator at image+0x22b3c */
typedef struct {
	int32_t channel;
	int16_t *target;
	uint32_t num_samples;
	int32_t mix;
	int16_t *orig_start;
	uint32_t orig_num_samples;
	int32_t orig_repeat;
	int32_t end_sample;
	int32_t pad[4];
} gen_audio_info;

typedef struct {
	int32_t channel;
	int32_t reps_remaining;
} end_sample_info;

typedef struct {
	int playing, paused;
	int16_t *start;
	uint32_t num;
	int32_t repeat;
	uint32_t loop_from;
	uint32_t pos;
	int32_t rate, volume, uservar;
	struct {
		s3e_callback cb;
		void *user;
	} cb[CB_COUNT];
} channel;

static channel chans[NCHANNELS];
static RecursiveLock lock;
static LightEvent wake;
static ndspWaveBuf bufs[NBUFS];
static int16_t *pcm;
static Thread mixer;
static volatile int running;
static int available, master = 100;
static volatile uint32_t loud_buffers, gen_calls;
static FILE *dump;
static uint32_t dump_left;

static void end_channel(int i)
{
	channel *c = &chans[i];
	c->playing = 0;
	if (c->cb[CB_END_SAMPLE].cb) {
		end_sample_info e = { i, 0 };
		c->cb[CB_END_SAMPLE].cb(&e, c->cb[CB_END_SAMPLE].user);
	}
}

static void mix_pcm(channel *c, int idx, int32_t *acc, int n)
{
	uint32_t step = ((uint32_t)(c->rate ? c->rate : OUTPUT_RATE) << 16) / OUTPUT_RATE;
	int vol = c->volume;

	for (int i = 0; i < n; i++) {
		uint32_t s = c->pos >> 16;
		if (s >= c->num) {
			if (c->repeat == 1) {
				end_channel(idx);
				return;
			}
			if (c->repeat > 1)
				c->repeat--;
			c->pos = (c->loop_from < c->num ? c->loop_from : 0) << 16;
			s = c->pos >> 16;
		}
		acc[i] += c->start[s] * vol >> 8;
		c->pos += step;
	}
}

static void fill(int16_t *out, int n)
{
	static int32_t acc[BUF_SAMPLES];
	static int16_t gen[BUF_SAMPLES];

	memset(acc, 0, n * sizeof(int32_t));

	RecursiveLock_Lock(&lock);
	for (int i = 0; i < NCHANNELS; i++) {
		channel *c = &chans[i];
		if (!c->playing || c->paused)
			continue;

		if (c->cb[CB_GEN_AUDIO].cb) {
			gen_audio_info info;
			memset(&info, 0, sizeof(info));
			memset(gen, 0, n * sizeof(int16_t));
			info.channel = i;
			info.target = gen;
			info.num_samples = n;
			info.mix = 1;
			info.orig_start = c->start;
			info.orig_num_samples = c->num;
			info.orig_repeat = c->repeat;
			c->cb[CB_GEN_AUDIO].cb(&info, c->cb[CB_GEN_AUDIO].user);
			gen_calls++;
			for (int k = 0; k < n; k++)
				acc[k] += gen[k];
			if (info.end_sample && c->playing)
				end_channel(i);
		} else if (c->start && c->num) {
			mix_pcm(c, i, acc, n);
		}
	}
	RecursiveLock_Unlock(&lock);

	int loud = 0;
	for (int k = 0; k < n; k++) {
		int32_t v = acc[k] * master / 100;
		loud |= v > 256 || v < -256;
		out[k] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
	}
	loud_buffers += loud;
	if (dump && dump_left) {
		fwrite(out, sizeof(int16_t), n, dump);
		dump_left = dump_left > (uint32_t)n ? dump_left - n : 0;
		if (!dump_left) {
			fclose(dump);
			dump = NULL;
		}
	}
}

void sound_stats(uint32_t *loud, uint32_t *gens)
{
	*loud = loud_buffers;
	*gens = gen_calls;
	loud_buffers = gen_calls = 0;
}

static void frame_cb(void *arg)
{
	LightEvent_Signal(&wake);
}

static void mixer_thread(void *arg)
{
	while (running) {
		for (int b = 0; b < NBUFS && running; b++) {
			if (bufs[b].status != NDSP_WBUF_DONE && bufs[b].status != NDSP_WBUF_FREE)
				continue;
			fill(bufs[b].data_pcm16, BUF_SAMPLES);
			DSP_FlushDataCache(bufs[b].data_pcm16, BUF_SAMPLES * sizeof(int16_t));
			ndspChnWaveBufAdd(DSP_VOICE, &bufs[b]);
		}
		LightEvent_Wait(&wake);
	}
}

void sound_init(void)
{
	RecursiveLock_Init(&lock);
	LightEvent_Init(&wake, RESET_ONESHOT);

	/* [PORT] DumpAudio=seconds writes the mix as raw 16-bit mono to audio.raw, for checking by ear */
	int secs = port_option("DumpAudio", 0);
	if (secs > 0 && (dump = fopen(DATA_DIR "/audio.raw", "wb")))
		dump_left = secs * OUTPUT_RATE;

	Result rc = ndspInit();
	if (R_FAILED(rc)) {
		port_log("no sound: ndspInit failed (%08lx), is sdmc:/3ds/dspfirm.cdc there?\n", rc);
		return;
	}

	pcm = linearAlloc(NBUFS * BUF_SAMPLES * sizeof(int16_t));
	if (!pcm) {
		ndspExit();
		return;
	}
	memset(pcm, 0, NBUFS * BUF_SAMPLES * sizeof(int16_t));

	ndspSetOutputMode(NDSP_OUTPUT_MONO);
	ndspChnReset(DSP_VOICE);
	ndspChnSetInterp(DSP_VOICE, NDSP_INTERP_LINEAR);
	ndspChnSetRate(DSP_VOICE, OUTPUT_RATE);
	ndspChnSetFormat(DSP_VOICE, NDSP_FORMAT_MONO_PCM16);
	for (int b = 0; b < NBUFS; b++) {
		memset(&bufs[b], 0, sizeof(bufs[b]));
		bufs[b].data_pcm16 = pcm + b * BUF_SAMPLES;
		bufs[b].nsamples = BUF_SAMPLES;
	}
	ndspSetCallback(frame_cb, NULL);

	/* the old 3DS lends apps a slice of the system core, mixing there keeps it off the game's back */
	int core = R_SUCCEEDED(APT_SetAppCpuTimeLimit(30)) ? 1 : 0;
	running = 1;
	mixer = threadCreate(mixer_thread, NULL, 32 * 1024, 0x18, core, false);
	if (!mixer && core) {
		core = 0;
		mixer = threadCreate(mixer_thread, NULL, 32 * 1024, 0x18, 0, false);
	}
	if (!mixer) {
		running = 0;
		ndspExit();
		port_log("no sound: mixer thread failed\n");
		return;
	}
	available = 1;
	port_log("sound mixer on core %d\n", core);
}

void sound_exit(void)
{
	if (!available)
		return;
	/*
	 * No more game callbacks, it's freeing its sound data. A thread still alive on
	 * the system core hangs the console on "closing software", hence the timeout.
	 */
	RecursiveLock_Lock(&lock);
	running = 0;
	memset(chans, 0, sizeof(chans));
	RecursiveLock_Unlock(&lock);
	LightEvent_Signal(&wake);
	if (R_SUCCEEDED(threadJoin(mixer, 500000000ULL)))
		threadFree(mixer);
	else
		port_log("mixer thread did not stop\n");
	ndspExit();
	APT_SetAppCpuTimeLimit(0);
	linearFree(pcm);
	available = 0;
}

static channel *chan(int32_t i)
{
	return i >= 0 && i < NCHANNELS ? &chans[i] : NULL;
}

int32_t s3eSoundGetInt(int32_t prop)
{
	switch (prop) {
	case 0: return master;
	case 1:
	case 2: return OUTPUT_RATE;
	case 3: return NCHANNELS;
	case 5: return available;
	}
	return 0;
}

int32_t s3eSoundSetInt(int32_t prop, int32_t value)
{
	if (prop == 0)
		master = value < 0 ? 0 : value;
	return S3E_RESULT_SUCCESS;
}

int32_t s3eSoundGetFreeChannel(void)
{
	int32_t r = -1;

	RecursiveLock_Lock(&lock);
	for (int i = 0; i < NCHANNELS && r < 0; i++)
		if (!chans[i].playing)
			r = i;
	RecursiveLock_Unlock(&lock);
	return r;
}

int32_t s3eSoundChannelRegister(int32_t ch, int32_t ev, s3e_callback cb, void *user)
{
	channel *c = chan(ch);
	if (!c || ev < 0 || ev >= CB_COUNT)
		return S3E_RESULT_ERROR;
	RecursiveLock_Lock(&lock);
	c->cb[ev].cb = cb;
	c->cb[ev].user = user;
	RecursiveLock_Unlock(&lock);
	return S3E_RESULT_SUCCESS;
}

int32_t s3eSoundChannelUnRegister(int32_t ch, int32_t ev)
{
	channel *c = chan(ch);
	if (!c || ev < 0 || ev >= CB_COUNT)
		return S3E_RESULT_ERROR;
	RecursiveLock_Lock(&lock);
	c->cb[ev].cb = NULL;
	RecursiveLock_Unlock(&lock);
	return S3E_RESULT_SUCCESS;
}

int32_t s3eSoundChannelPlay(int32_t ch, int16_t *start, uint32_t num, int32_t repeat, int32_t loop_from)
{
	channel *c = chan(ch);
	if (!c)
		return S3E_RESULT_ERROR;
	RecursiveLock_Lock(&lock);
	c->start = start;
	c->num = num;
	c->repeat = repeat;
	c->loop_from = loop_from;
	c->pos = 0;
	c->paused = 0;
	if (!c->volume)
		c->volume = 256;
	c->playing = 1;
	RecursiveLock_Unlock(&lock);
	return S3E_RESULT_SUCCESS;
}

int32_t s3eSoundChannelStop(int32_t ch)
{
	channel *c = chan(ch);
	if (!c)
		return S3E_RESULT_ERROR;
	RecursiveLock_Lock(&lock);
	c->playing = 0;
	c->paused = 0;
	RecursiveLock_Unlock(&lock);
	return S3E_RESULT_SUCCESS;
}

int32_t s3eSoundChannelPause(int32_t ch)
{
	channel *c = chan(ch);
	if (c)
		c->paused = 1;
	return c ? S3E_RESULT_SUCCESS : S3E_RESULT_ERROR;
}

int32_t s3eSoundChannelResume(int32_t ch)
{
	channel *c = chan(ch);
	if (c)
		c->paused = 0;
	return c ? S3E_RESULT_SUCCESS : S3E_RESULT_ERROR;
}

int32_t s3eSoundChannelGetInt(int32_t ch, int32_t prop)
{
	channel *c = chan(ch);
	if (!c)
		return 0;
	switch (prop) {
	case CH_STATUS: return c->playing;
	case CH_RATE: return c->rate;
	case CH_USERVAR: return c->uservar;
	case CH_VOLUME: return c->volume;
	}
	return 0;
}

int32_t s3eSoundChannelSetInt(int32_t ch, int32_t prop, int32_t value)
{
	channel *c = chan(ch);
	if (!c)
		return S3E_RESULT_ERROR;
	switch (prop) {
	case CH_RATE: c->rate = value; break;
	case CH_USERVAR: c->uservar = value; break;
	case CH_VOLUME: c->volume = value; break;
	}
	return S3E_RESULT_SUCCESS;
}
