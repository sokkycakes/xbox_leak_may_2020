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
// App levels: every program that plays gets its own loopback subdevice, and
// the router knows which program it is (the subdevice's owner_pid and that
// program's command line): the Xbox dashboard, Frolick, an Xbox game (by the
// XBE's title ID), the DS runtime, a Windows game, the Windows games' Guide.
// Each one gets ONE fixed volume, so that it sits at the Guide's loudness
// (TARGET, -19 LUFS). It is not a compressor: the volume is measured once,
// over an app's first two minutes of sound (BS.1770 loudness), saved in
// /data/sion-audio/levels and then never changes. Until an app has been
// measured it plays at its kind's starting volume (Xbox apps +12 dB, DS -6).
//
//   sion-audiod [-v]      config: /etc/default/sion-audio (MAIN=<alsa pcm>
//                         overrides the main output, MIRROR=0 skips adapters,
//                         LEVELS=0 plays every app as it is, TARGET=-19,
//                         GAIN_DASH=+8 etc. fixes a kind's volume by hand)
//   /data/sion-audio/levels   one line per app: kind key dB learned|set name;
//                         "set" lines are never measured over, delete a line
//                         to measure that app again
//   /run/sion-audiod.status   what plays right now, with its level
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RATE		48000
#define CH		2
#define PERIOD		240		/* 5 ms */
#define NSUB		8		/* loopback subdevices captured */
#define MAXOUT		8
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
static int cfg_levels = 1;
static float cfg_target = -19.0f;

/* Kinds of app, with the volume each starts at before it has been measured.
   xbcompat plays Xbox sound 12 dB under full scale (the Xbox's 6 dB buffer
   headroom and 6 dB mix bin headroom), so its apps start at +12. The Guide
   inside an Xbox game is mixed into the game's own stream and gets the
   game's volume, so the kit turns the Guide's sound files down 12 dB on
   disk to match; kazeta-guide (the Windows games' Guide, its own stream)
   plays those same files and gets the 12 dB back here. */
struct kind { const char *name; float start_db; int learn; float set_db; int has_set; };
#define KIND(n, db, l) { .name = n, .start_db = db, .learn = l }
static struct kind kinds[] = {
	KIND("dash", 12, 1), KIND("frolick", 12, 1), KIND("game", 12, 1), KIND("ds", -6, 1),
	KIND("wine", 0, 1), KIND("guide", 12, 0), KIND("boot", 0, 0), KIND("other", 0, 0),
};
#define NKINDS (int)(sizeof(kinds) / sizeof(kinds[0]))

static void read_config(void)
{
	FILE *f = fopen("/etc/default/sion-audio", "r");
	char line[256];

	cfg_main[0] = 0;
	cfg_mirror = 1;
	cfg_levels = 1;
	cfg_target = -19.0f;
	for (int k = 0; k < NKINDS; k++)
		kinds[k].has_set = 0;
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
		else if (!strcmp(line, "LEVELS"))
			cfg_levels = atoi(v);
		else if (!strcmp(line, "TARGET"))
			cfg_target = strtof(v, NULL);
		else if (!strncmp(line, "GAIN_", 5))
			for (int k = 0; k < NKINDS; k++)
				if (!strcasecmp(line + 5, kinds[k].name)) {
					kinds[k].set_db = strtof(v, NULL);
					kinds[k].has_set = 1;
				}
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

/* Turns on every playback switch of a card (USB headsets can come up with
   one off; the Sion has no amixer). Once per card id while it's plugged in. */
static char unmuted[256];

static void unmute(const char *id)
{
	char name[80], tag[72];
	snd_ctl_t *ctl;
	snd_ctl_elem_list_t *list;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *val;

	snprintf(tag, sizeof(tag), "|%s|", id);
	if (strstr(unmuted, tag))
		return;
	snprintf(name, sizeof(name), "hw:CARD=%s", id);
	if (snd_ctl_open(&ctl, name, 0) < 0)
		return;
	if (strlen(unmuted) + strlen(tag) < sizeof(unmuted))
		strcat(unmuted, tag);
	snd_ctl_elem_list_alloca(&list);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&val);
	if (snd_ctl_elem_list(ctl, list) == 0) {
		unsigned int n = snd_ctl_elem_list_get_count(list);
		if (snd_ctl_elem_list_alloc_space(list, n) == 0 && snd_ctl_elem_list(ctl, list) == 0) {
			for (unsigned int i = 0; i < snd_ctl_elem_list_get_used(list); i++) {
				snd_ctl_elem_info_set_numid(info, snd_ctl_elem_list_get_numid(list, i));
				if (snd_ctl_elem_info(ctl, info) < 0)
					continue;
				const char *en = snd_ctl_elem_info_get_name(info);
				if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_BOOLEAN ||
				    !strstr(en, "Playback Switch") || !snd_ctl_elem_info_is_writable(info))
					continue;
				snd_ctl_elem_value_set_numid(val, snd_ctl_elem_list_get_numid(list, i));
				if (snd_ctl_elem_read(ctl, val) < 0)
					continue;
				int off = 0;
				for (unsigned int k = 0; k < snd_ctl_elem_info_get_count(info); k++)
					if (!snd_ctl_elem_value_get_boolean(val, k)) {
						snd_ctl_elem_value_set_boolean(val, k, 1);
						off = 1;
					}
				if (off && snd_ctl_elem_write(ctl, val) >= 0)
					LOG("%s: turned on %s %u", id, en, snd_ctl_elem_info_get_index(info));
			}
			snd_ctl_elem_list_free_space(list);
		}
	}
	snd_ctl_close(ctl);
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
		if (read_file(path, id, sizeof(id)))
			continue;
		/* every playback device of the card: headsets such as the Razer
		   Kraken have the ear cups on the second one (DEV=1) */
		for (int d = 0; d < 8 && n < MAXOUT - 2; d++) {
			snprintf(path, sizeof(path), "/proc/asound/card%d/pcm%dp", c, d);
			if (exists(path))
				snprintf(want[n++], 96, "plughw:CARD=%s,DEV=%d", id, d);
		}
		unmute(id);
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
	unmuted[0] = 0;		/* a card replugged comes up with its own switches */
	read_config();
	nw = wanted(want);
	for (int i = 0; i < MAXOUT; i++) {
		const char *w = i < nw ? want[i] : NULL;
		struct output *o = &outs[i];
		if (o->pcm && (!w || strcmp(o->name, w)))
			out_close(o);
		if (w && !o->pcm && strcmp(w, "-")) {
			if (out_open(o, w, strncmp(w, "hw:CARD=Trigger6", 16) != 0) < 0)
				snprintf(o->name, sizeof(o->name), "%s", w);	/* retry later */
		} else if (!w)
			memset(o, 0, sizeof(*o));
		else if (!strcmp(w, "-"))
			snprintf(o->name, sizeof(o->name), "-");
	}
	nouts = nw;
}

/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* App levels                                                                */

#define BLOCK		19200		/* 400 ms loudness blocks (BS.1770) */
#define LEARN_BLOCKS	300		/* two minutes of sound measure an app */
#define GATE_ABS	-55.0		/* quieter blocks are silence, not sound */
#define GAIN_MIN	-24.0f
#define GAIN_MAX	20.0f
#define DB_STEP		0.05f		/* per 5 ms chunk: a new volume ramps in */
#define LEVELS_DIR	"/data/sion-audio"
#define LEVELS_FILE	LEVELS_DIR "/levels"

struct level { char kind[12], key[48], name[48]; float db; int set; };
static struct level levels[256];
static int nlevels;

struct stream {
	int pid;			/* owner of the loopback subdevice, 0 = closed */
	unsigned long cmd;		/* its command line's hash: an exec is a new app */
	int kind;
	char key[48], name[48];
	float db, target_db;		/* volume now, and where it is going */
	int learning;
	double z[CH][2][2];		/* K-weighting filter state */
	double blk;
	int nblk;
	float blocks[LEARN_BLOCKS];	/* mean square of each sounding block */
	int nb;
	double log_e;			/* for the minute log: input energy */
	int log_n;
	time_t last_log;
};
static struct stream streams[NSUB];

static void levels_load(void)
{
	FILE *f = fopen(LEVELS_FILE, "r");
	char line[256];

	nlevels = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f) && nlevels < 256) {
		struct level *l = &levels[nlevels];
		char how[16] = "";
		int n;
		if (line[0] == '#')
			continue;
		memset(l, 0, sizeof(*l));
		if (sscanf(line, "%11s %47s %f %15s %n", l->kind, l->key, &l->db, how, &n) < 3)
			continue;
		l->set = !strcmp(how, "set");
		if (strlen(line) > (size_t)n) {
			snprintf(l->name, sizeof(l->name), "%s", line + n);
			l->name[strcspn(l->name, "\r\n")] = 0;
		}
		nlevels++;
	}
	fclose(f);
}

static void levels_save(void)
{
	FILE *f;

	mkdir(LEVELS_DIR, 0755);
	if (!(f = fopen(LEVELS_FILE ".new", "w")))
		return;
	fprintf(f, "# sion-audiod app volumes: kind key dB learned|set name\n"
		   "# learned = measured once by the router; change it to set to keep\n"
		   "# your own number. Delete a line to measure that app again.\n");
	for (int i = 0; i < nlevels; i++)
		fprintf(f, "%s %s %+.1f %s %s\n", levels[i].kind, levels[i].key, levels[i].db,
			levels[i].set ? "set" : "learned", levels[i].name);
	fclose(f);
	rename(LEVELS_FILE ".new", LEVELS_FILE);
}

static struct level *level_find(const char *kind, const char *key)
{
	for (int i = 0; i < nlevels; i++)
		if (!strcmp(levels[i].kind, kind) && !strcmp(levels[i].key, key))
			return &levels[i];
	return NULL;
}

static void basename_of(const char *path, char *out, size_t len)
{
	const char *b = path;
	for (const char *p = path; *p; p++)
		if (*p == '/' || *p == '\\')
			b = p + 1;
	snprintf(out, len, "%s", b);
}

static void sanitize(char *s)
{
	for (; *s; s++)
		if (isspace((unsigned char)*s) || *s == '#')
			*s = '_';
}

static int ends_with_ci(const char *s, const char *tail)
{
	size_t a = strlen(s), b = strlen(tail);
	return a >= b && !strcasecmp(s + a - b, tail);
}

/* The title ID and name from an XBE's certificate. */
static int xbe_title(const char *path, char *key, size_t klen, char *name, size_t nlen)
{
	FILE *f = fopen(path, "rb");
	uint32_t hdr[0x120 / 4], cert[0xB0 / 4];
	uint16_t wname[40];
	int ok = 0;

	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && hdr[0] == 0x48454258 /* XBEH */) {
		uint32_t base = hdr[0x104 / 4], at = hdr[0x118 / 4];
		if (at > base && fseek(f, at - base, SEEK_SET) == 0 &&
		    fread(cert, 1, sizeof(cert), f) == sizeof(cert)) {
			snprintf(key, klen, "%08X", cert[2]);
			memcpy(wname, (char *)cert + 0xC, sizeof(wname));
			size_t j = 0;
			for (int i = 0; i < 40 && wname[i] && j + 1 < nlen; i++)
				name[j++] = wname[i] < 0x80 ? (char)wname[i] : '?';
			name[j] = 0;
			ok = 1;
		}
	}
	fclose(f);
	return ok;
}

static int kind_index(const char *name)
{
	for (int k = 0; k < NKINDS; k++)
		if (!strcmp(kinds[k].name, name))
			return k;
	return NKINDS - 1;
}

/* What program is this: kind, key (the levels file's), and a readable name. */
static void classify(int pid, struct stream *st)
{
	char path[64], buf[4096], arg0[256] = "", low[512];
	const char *kind = "other", *xbe = NULL, *exe = NULL, *nds = NULL;
	size_t n = 0;
	FILE *f;

	st->key[0] = st->name[0] = 0;
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	if ((f = fopen(path, "r"))) {
		n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
	}
	buf[n] = 0;
	for (size_t i = 0; i < n; i += strlen(buf + i) + 1) {
		const char *a = buf + i;
		if (!i)
			basename_of(a, arg0, sizeof(arg0));
		if (ends_with_ci(a, ".xbe"))
			xbe = a;
		else if (ends_with_ci(a, ".exe"))
			exe = a;
		else if (ends_with_ci(a, ".nds") || ends_with_ci(a, ".srl"))
			nds = a;
	}
	if (!strncmp(arg0, "xbcompat", 8)) {
		snprintf(low, sizeof(low), "%s", xbe ? xbe : "");
		for (char *p = low; *p; p++)
			*p = tolower((unsigned char)*p);
		if (strstr(low, "xboxdash") || strstr(low, "xonlinedash"))
			kind = "dash";
		else if (strstr(low, "frolick"))
			kind = "frolick";
		else if (strstr(low, "bootscreen") || strstr(low, "bootani"))
			kind = "boot";
		else
			kind = "game";
		if (strcmp(kind, "game"))
			snprintf(st->key, sizeof(st->key), "%s", kind);
		else if (!xbe || !xbe_title(xbe, st->key, sizeof(st->key), st->name, sizeof(st->name))) {
			char b[256];
			basename_of(xbe ? xbe : "xbcompat", b, sizeof(b));
			snprintf(st->key, sizeof(st->key), "%s", b);
		}
		if (xbe && !st->name[0]) {
			char b[256];
			basename_of(xbe, b, sizeof(b));
			snprintf(st->name, sizeof(st->name), "%s", b);
		}
	} else if (!strncmp(arg0, "mirrorrun", 9) || !strncmp(arg0, "dscart", 6) || nds) {
		kind = "ds";
		basename_of(nds ? nds : "ds", st->key, sizeof(st->key));
	} else if (!strcmp(arg0, "kazeta-guide")) {
		kind = "guide";
		snprintf(st->key, sizeof(st->key), "guide");
	} else if (exe || strstr(arg0, "wine")) {
		kind = "wine";
		basename_of(exe ? exe : arg0, st->key, sizeof(st->key));
	} else
		snprintf(st->key, sizeof(st->key), "%s", arg0[0] ? arg0 : "unknown");
	sanitize(st->key);
	sanitize(st->name);
	if (!st->name[0])
		snprintf(st->name, sizeof(st->name), "%s", arg0);
	st->kind = kind_index(kind);
}

/* The volume an app plays at, and whether it still needs measuring. */
static float volume_for(struct stream *st, int *learn)
{
	struct kind *k = &kinds[st->kind];
	struct level *l;

	*learn = 0;
	if (!cfg_levels)
		return 0;
	if (k->has_set)
		return k->set_db;
	if ((l = level_find(k->name, st->key)))
		return l->db;
	*learn = k->learn;
	return k->start_db;
}

static unsigned long cmdline_hash(int pid)
{
	char path[64], buf[4096];
	unsigned long h = 5381;
	size_t n = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	if ((f = fopen(path, "r"))) {
		n = fread(buf, 1, sizeof(buf), f);
		fclose(f);
	}
	for (size_t i = 0; i < n; i++)
		h = h * 33 + (unsigned char)buf[i];
	return h;
}

static int owner_pid(int sub)
{
	char path[96], buf[512];
	FILE *f;
	size_t n;
	char *p;

	snprintf(path, sizeof(path), "/proc/asound/Loopback/pcm0p/sub%d/status", sub);
	if (!(f = fopen(path, "r")))
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	if (!(p = strstr(buf, "owner_pid")) || !(p = strchr(p, ':')))
		return 0;
	return atoi(p + 1);
}

static void stream_reset(struct stream *st, int pid)
{
	memset(st, 0, sizeof(*st));
	st->pid = pid;
	st->kind = NKINDS - 1;
	if (!pid)
		return;
	st->cmd = cmdline_hash(pid);
	classify(pid, st);
	st->db = st->target_db = volume_for(st, &st->learning);
	st->last_log = time(NULL);
	LOG("stream %d: %s %s (%s, pid %d) plays at %+.1f dB%s", (int)(st - streams),
	    kinds[st->kind].name, st->key, st->name, pid, st->db,
	    st->learning ? ", measuring it" : "");
}

/* Once a second: who owns each subdevice; settings may have changed too. */
static void streams_scan(int nin, int reload)
{
	if (reload) {
		read_config();
		levels_load();
	}
	for (int s = 0; s < nin; s++) {
		struct stream *st = &streams[s];
		int pid = owner_pid(s);
		if (pid != st->pid || (pid && cmdline_hash(pid) != st->cmd))
			stream_reset(st, pid);
		else if (reload && pid) {
			int learn;
			float db = volume_for(st, &learn);
			if (!learn)
				st->learning = 0;
			if (!st->learning)
				st->target_db = db;
		}
	}
}

static void stream_learned(struct stream *st)
{
	double sum = 0, gate, li;
	int n = 0;
	struct level *l;

	for (int i = 0; i < st->nb; i++)
		sum += st->blocks[i];
	gate = -0.691 + 10 * log10(sum / st->nb) - 10;	/* BS.1770 relative gate */
	sum = 0;
	for (int i = 0; i < st->nb; i++)
		if (-0.691 + 10 * log10(st->blocks[i]) > gate) {
			sum += st->blocks[i];
			n++;
		}
	li = -0.691 + 10 * log10(sum / (n ? n : 1));
	float db = cfg_target - (float)li;
	db = db < GAIN_MIN ? GAIN_MIN : db > GAIN_MAX ? GAIN_MAX : db;
	db = roundf(db * 2) / 2;
	st->learning = 0;
	st->target_db = db;
	if (!(l = level_find(kinds[st->kind].name, st->key)) && nlevels < 256) {
		l = &levels[nlevels++];
		memset(l, 0, sizeof(*l));
		snprintf(l->kind, sizeof(l->kind), "%s", kinds[st->kind].name);
		snprintf(l->key, sizeof(l->key), "%s", st->key);
		snprintf(l->name, sizeof(l->name), "%s", st->name);
	}
	if (l) {
		l->db = db;
		levels_save();
	}
	LOG("measured %s %s (%s): %.1f LUFS, its volume is now %+.1f dB", kinds[st->kind].name,
	    st->key, st->name, li, db);
}

/* K-weighting (BS.1770, 48 kHz): a high shelf, then a high pass. */
static const double KB[2][3] = { { 1.53512485958697, -2.69169618940638, 1.19839281085285 },
				 { 1.0, -2.0, 1.0 } };
static const double KA[2][2] = { { -1.69065929318241, 0.73248077421585 },
				 { -1.99004745483398, 0.99007225036621 } };

/* Measures what the app plays (before its volume) and mixes it into acc. */
static void stream_mix(struct stream *st, const int16_t *in, int frames, float *acc)
{
	float from = st->db, to = st->db;

	if (to < st->target_db)
		to = fminf(st->target_db, to + DB_STEP);
	else if (to > st->target_db)
		to = fmaxf(st->target_db, to - DB_STEP);
	st->db = to;
	float g0 = powf(10, from / 20), g1 = powf(10, to / 20);
	for (int i = 0; i < frames; i++) {
		float g = g0 + (g1 - g0) * (float)(i + 1) / (float)frames;
		for (int c = 0; c < CH; c++) {
			double x = in[i * CH + c] / 32768.0;
			acc[i * CH + c] += (float)x * g;
			if (!st->pid)
				continue;
			for (int k = 0; k < 2; k++) {	/* transposed direct form II */
				double *z = st->z[c][k];
				double y = KB[k][0] * x + z[0];
				z[0] = KB[k][1] * x - KA[k][0] * y + z[1];
				z[1] = KB[k][2] * x - KA[k][1] * y;
				x = y;
			}
			st->blk += x * x;
		}
		if (st->pid && ++st->nblk == BLOCK) {
			double ms = st->blk / BLOCK;
			if (-0.691 + 10 * log10(ms + 1e-20) > GATE_ABS) {
				st->log_e += ms;
				st->log_n++;
				if (st->learning && st->nb < LEARN_BLOCKS) {
					st->blocks[st->nb++] = (float)ms;
					if (st->nb == LEARN_BLOCKS)
						stream_learned(st);
				}
			}
			st->blk = 0;
			st->nblk = 0;
		}
	}
}

/* A line a minute per app that played, and /run/sion-audiod.status. */
static void streams_report(int nin)
{
	time_t now = time(NULL);
	FILE *f = fopen("/run/sion-audiod.status.new", "w");

	for (int s = 0; s < nin; s++) {
		struct stream *st = &streams[s];
		double in = st->log_n ? -0.691 + 10 * log10(st->log_e / st->log_n) : -99;
		if (f && st->pid)
			fprintf(f, "%d %s %s pid=%d volume=%+.1f%s last-minute=%.1fLUFS (%s)\n", s,
				kinds[st->kind].name, st->key, st->pid, st->db,
				st->learning ? " measuring" : "", in, st->name);
		if (!st->pid || now - st->last_log < 60)
			continue;
		if (st->log_n)
			LOG("%s %s: %.1f LUFS in, volume %+.1f dB, %.1f LUFS out%s", kinds[st->kind].name,
			    st->key, in, st->db, in + st->db,
			    st->learning ? " (still measuring)" : "");
		st->log_e = 0;
		st->log_n = 0;
		st->last_log = now;
	}
	if (f) {
		fclose(f);
		rename("/run/sion-audiod.status.new", "/run/sion-audiod.status");
	}
}

/* Keeps the sum out of hard clipping: untouched up to -1 dBFS, a soft
   shoulder above (apps that were turned up can add up past full scale). */
static inline int16_t to_s16(float x)
{
	const float knee = 0.89f, room = 1.0f - 0.89f;
	float a = fabsf(x);
	if (a > knee)
		a = knee + room * tanhf((a - knee) / room);
	return (int16_t)lrintf(copysignf(a, x) * 32767.0f);
}

static void on_signal(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
	snd_pcm_t *in[NSUB] = { 0 };
	int nin = 0, err;
	static int16_t chunk[PERIOD * CH], mix[PERIOD * CH];
	static float acc[PERIOD * CH];
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

	streams_scan(nin, 1);

	while (!quit) {
		/* Subdevice 0 sets the pace (blocking); the rest are read as far
		 * as they have data, all on the same loopback clock. Each one is
		 * measured and turned to its app's volume as it is mixed. */
		snd_pcm_sframes_t n = snd_pcm_readi(in[0], chunk, PERIOD);
		if (n < 0) {
			snd_pcm_recover(in[0], n, 1);
			snd_pcm_start(in[0]);
			continue;
		}
		int frames = n;
		memset(acc, 0, sizeof(acc));
		stream_mix(&streams[0], chunk, frames, acc);
		for (int s = 1; s < nin; s++) {
			snd_pcm_sframes_t m = snd_pcm_readi(in[s], chunk, frames);
			if (m == -EPIPE || m == -ESTRPIPE) {
				snd_pcm_recover(in[s], m, 1);
				snd_pcm_start(in[s]);
				continue;
			}
			if (m > 0)
				stream_mix(&streams[s], chunk, m, acc);
		}
		for (int k = 0; k < frames * CH; k++)
			mix[k] = to_s16(acc[k]);

		for (int i = 0; i < nouts; i++)
			if (outs[i].pcm)
				out_write(&outs[i], mix, frames);

		time_t now = time(NULL);
		if (now != last_scan) {
			last_scan = now;
			rescan(0);
			streams_scan(nin, now % 5 == 0);
			streams_report(nin);
		}
	}
	for (int i = 0; i < nouts; i++)
		out_close(&outs[i]);
	return 0;
}
