// sion-audiod: the Sion's sound router.
//
// Games and the dashboard play into the ALSA loopback card (snd-aloop,
// AUDIODEV=plughw:CARD=Loopback,DEV=0, set by xbox-env). This daemon takes
// what they play from the loopback's other side, mixes the streams, and sends
// the result to
//
//   - the main output: the first USB sound card (e.g. a headset), else the
//     TV's HDMI (hdmi:CARD=PCH,DEV=0), as xbox-env used to pick once per
//     session, but now following plug and unplug as they happen;
//   - every MCT Trigger 6 adapter (j5create JUA365, ALSA id Trigger6*) as well.
//
// Each output has its own clock, so each one is kept ~20 ms behind by now and
// then dropping or repeating a single frame. Format: S16_LE, stereo, 48 kHz.
//
//   sion-audiod [-v]      config: /etc/default/sion-audio (MAIN=<alsa pcm>
//                         overrides the main output, MIRROR=0 skips adapters)
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define RATE		48000
#define CH		2
#define PERIOD		240		/* 5 ms */
#define NSUB		8		/* loopback subdevices captured */
#define MAXOUT		4
#define TARGET		960		/* frames kept queued in each output (20 ms) */
#define SLACK		240

static int verbose;
static volatile sig_atomic_t quit;

static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("sion-audiod: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}
#define LOG(...) logf_(__VA_ARGS__)
#define DBG(...) do { if (verbose) logf_(__VA_ARGS__); } while (0)

static int setup(snd_pcm_t *pcm, snd_pcm_uframes_t period, snd_pcm_uframes_t buffer, int capture)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	unsigned int rate = RATE;
	int err;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
	if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, CH)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0)
		return err;
	if (rate != RATE)
		return -EINVAL;
	snd_pcm_sw_params_current(pcm, sw);
	if (capture)
		snd_pcm_sw_params_set_start_threshold(pcm, sw, 1);
	else
		snd_pcm_sw_params_set_start_threshold(pcm, sw, TARGET);
	snd_pcm_sw_params_set_avail_min(pcm, sw, period);
	if ((err = snd_pcm_sw_params(pcm, sw)) < 0)
		return err;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Outputs                                                                   */

struct output {
	char name[96];
	snd_pcm_t *pcm;
	int main;
	snd_pcm_uframes_t buffer;
	unsigned long dropped, repeated, xruns;
};

static struct output outs[MAXOUT];
static int nouts;

static void out_close(struct output *o)
{
	if (o->pcm) {
		snd_pcm_close(o->pcm);
		LOG("stopped sending to %s (%lu dropped, %lu repeated frames, %lu underruns)",
		    o->name, o->dropped, o->repeated, o->xruns);
	}
	memset(o, 0, sizeof(*o));
}

static int out_open(struct output *o, const char *name, int main_out)
{
	snd_pcm_uframes_t period, buffer;
	int err;

	memset(o, 0, sizeof(*o));
	snprintf(o->name, sizeof(o->name), "%s", name);
	o->main = main_out;
	err = snd_pcm_open(&o->pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (err < 0) {
		DBG("can't open %s: %s", name, snd_strerror(err));
		o->pcm = NULL;
		return err;
	}
	err = setup(o->pcm, PERIOD, TARGET * 4, 0);
	if (err < 0) {
		LOG("can't set up %s: %s", name, snd_strerror(err));
		snd_pcm_close(o->pcm);
		o->pcm = NULL;
		return err;
	}
	snd_pcm_get_params(o->pcm, &buffer, &period);
	o->buffer = buffer;
	LOG("sending to %s (%s, period %lu, buffer %lu)", name, main_out ? "main" : "mirror",
	    (unsigned long)period, (unsigned long)buffer);
	return 0;
}

/* Writes one chunk, nudging the queue towards TARGET frames. */
static void out_write(struct output *o, const int16_t *buf, int frames)
{
	static int16_t tmp[(PERIOD + 1) * CH];
	snd_pcm_sframes_t delay = 0, n;
	int err;

	if (snd_pcm_state(o->pcm) == SND_PCM_STATE_XRUN) {
		o->xruns++;
		snd_pcm_prepare(o->pcm);
	}
	if (snd_pcm_state(o->pcm) == SND_PCM_STATE_RUNNING && snd_pcm_delay(o->pcm, &delay) == 0) {
		if (delay > TARGET + SLACK && frames > 1) {
			frames--;		/* output runs slow: drop a frame */
			o->dropped++;
		} else if (delay < TARGET - SLACK && frames <= PERIOD) {
			memcpy(tmp, buf, frames * CH * 2);
			memcpy(tmp + frames * CH, buf + (frames - 1) * CH, CH * 2);
			buf = tmp;
			frames++;		/* output runs fast: repeat one */
			o->repeated++;
		}
	} else if (snd_pcm_state(o->pcm) == SND_PCM_STATE_PREPARED) {
		/* Start with TARGET frames queued: silence, then the sound. */
		snd_pcm_sframes_t avail = snd_pcm_avail_update(o->pcm);
		snd_pcm_sframes_t queued = avail >= 0 ? (snd_pcm_sframes_t)o->buffer - avail : 0;
		static const int16_t zero[PERIOD * CH];
		while (queued + frames < TARGET) {
			int z = TARGET - queued - frames;
			if (z > PERIOD)
				z = PERIOD;
			if (snd_pcm_writei(o->pcm, zero, z) < 0)
				break;
			queued += z;
		}
	}
	n = snd_pcm_writei(o->pcm, buf, frames);
	if (n == -EAGAIN)
		o->dropped += frames;	/* full: the output stalled */
	else if (n == -EPIPE) {
		o->xruns++;
		snd_pcm_prepare(o->pcm);
	} else if (n < 0) {
		err = snd_pcm_recover(o->pcm, n, 1);
		if (err < 0) {
			LOG("%s failed: %s", o->name, snd_strerror(n));
			out_close(o);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Which outputs should exist                                                */

static char cfg_main[96];
static int cfg_mirror = 1;

static void read_config(void)
{
	FILE *f = fopen("/etc/default/sion-audio", "r");
	char line[256];

	cfg_main[0] = 0;
	cfg_mirror = 1;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *v;
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || !(v = strchr(line, '=')))
			continue;
		*v++ = 0;
		if (*v == '"' || *v == '\'') {
			v++;
			v[strcspn(v, "\"'")] = 0;
		}
		if (!strcmp(line, "MAIN"))
			snprintf(cfg_main, sizeof(cfg_main), "%s", v);
		else if (!strcmp(line, "MIRROR"))
			cfg_mirror = atoi(v);
	}
	fclose(f);
}

static int read_file(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, len - 1, f);
	buf[n] = 0;
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int exists(const char *path)
{
	return access(path, F_OK) == 0;
}

/* Fills want[] with the PCM names the outputs should have right now. */
static int wanted(char want[MAXOUT][96])
{
	char id[64], path[96];
	int n = 0;

	if (cfg_main[0])
		snprintf(want[n++], 96, "%s", cfg_main);
	for (int c = 0; c < 32 && !n; c++) {
		snprintf(path, sizeof(path), "/proc/asound/card%d/usbid", c);
		if (!exists(path))
			continue;
		snprintf(path, sizeof(path), "/proc/asound/card%d/pcm0p", c);
		if (!exists(path))
			continue;
		snprintf(path, sizeof(path), "/proc/asound/card%d/id", c);
		if (!read_file(path, id, sizeof(id)))
			snprintf(want[n++], 96, "plughw:CARD=%s,DEV=0", id);
	}
	if (!n && exists("/proc/asound/PCH"))
		snprintf(want[n++], 96, "plug:'hdmi:CARD=PCH,DEV=0'");
	if (!n)
		snprintf(want[n++], 96, "-");	/* nothing yet: keep the slot */
	if (cfg_mirror)
		for (int c = 0; c < 32 && n < MAXOUT; c++) {
			snprintf(path, sizeof(path), "/proc/asound/card%d/id", c);
			if (!read_file(path, id, sizeof(id)) && !strncmp(id, "Trigger6", 8))
				snprintf(want[n++], 96, "hw:CARD=%s,DEV=0", id);
		}
	return n;
}

static char last_cards[4096];

static void rescan(int force)
{
	char cards[4096] = "", want[MAXOUT][96];
	int nw;

	FILE *f = fopen("/proc/asound/cards", "r");
	if (f) {
		size_t r = fread(cards, 1, sizeof(cards) - 1, f);
		cards[r] = 0;
		fclose(f);
	}
	if (!force && !strcmp(cards, last_cards)) {
		int dead = 0;
		for (int i = 0; i < nouts; i++)
			dead |= !outs[i].pcm && strcmp(outs[i].name, "-");
		if (!dead)
			return;
	}
	strcpy(last_cards, cards);
	read_config();
	nw = wanted(want);
	for (int i = 0; i < MAXOUT; i++) {
		const char *w = i < nw ? want[i] : NULL;
		struct output *o = &outs[i];
		if (o->pcm && (!w || strcmp(o->name, w)))
			out_close(o);
		if (w && !o->pcm && strcmp(w, "-")) {
			if (out_open(o, w, i == 0) < 0)
				snprintf(o->name, sizeof(o->name), "%s", w);	/* retry later */
		} else if (!w)
			memset(o, 0, sizeof(*o));
		else if (!strcmp(w, "-"))
			snprintf(o->name, sizeof(o->name), "-");
	}
	nouts = nw;
}

/* ------------------------------------------------------------------------ */

static void on_signal(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
	snd_pcm_t *in[NSUB] = { 0 };
	int nin = 0, err;
	static int16_t chunk[PERIOD * CH], mix[PERIOD * CH];
	static int32_t acc[PERIOD * CH];
	time_t last_scan = 0;

	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-v"))
			verbose = 1;
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	setpriority(PRIO_PROCESS, 0, -5);

	/* The loopback's capture side, every subdevice: each program that plays
	 * gets its own subdevice and they are mixed here. */
	for (int s = 0; s < NSUB; s++) {
		char name[64];
		snprintf(name, sizeof(name), "hw:CARD=Loopback,DEV=1,SUBDEV=%d", s);
		if ((err = snd_pcm_open(&in[s], name, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
			if (s == 0) {
				LOG("no loopback card (%s): is snd-aloop loaded?", snd_strerror(err));
				return 1;
			}
			break;
		}
		if ((err = setup(in[s], PERIOD, PERIOD * 8, 1)) < 0) {
			LOG("loopback %d: %s", s, snd_strerror(err));
			snd_pcm_close(in[s]);
			in[s] = NULL;
			if (s == 0)
				return 1;
			break;
		}
		if (s)
			snd_pcm_nonblock(in[s], 1);
		snd_pcm_start(in[s]);
		nin = s + 1;
	}
	LOG("listening on %d loopback streams", nin);
	rescan(1);

	while (!quit) {
		/* Subdevice 0 sets the pace (blocking); the rest are read as far
		 * as they have data, all on the same loopback clock. */
		snd_pcm_sframes_t n = snd_pcm_readi(in[0], mix, PERIOD);
		if (n < 0) {
			snd_pcm_recover(in[0], n, 1);
			snd_pcm_start(in[0]);
			continue;
		}
		int frames = n;
		for (int k = 0; k < frames * CH; k++)
			acc[k] = mix[k];
		for (int s = 1; s < nin; s++) {
			snd_pcm_sframes_t m = snd_pcm_readi(in[s], chunk, frames);
			if (m == -EPIPE || m == -ESTRPIPE) {
				snd_pcm_recover(in[s], m, 1);
				snd_pcm_start(in[s]);
				continue;
			}
			for (int k = 0; k < m * CH; k++)
				acc[k] += chunk[k];
		}
		for (int k = 0; k < frames * CH; k++)
			mix[k] = acc[k] > 32767 ? 32767 : acc[k] < -32768 ? -32768 : acc[k];

		for (int i = 0; i < nouts; i++)
			if (outs[i].pcm)
				out_write(&outs[i], mix, frames);

		time_t now = time(NULL);
		if (now != last_scan) {
			last_scan = now;
			rescan(0);
		}
	}
	for (int i = 0; i < nouts; i++)
		out_close(&outs[i]);
	return 0;
}
