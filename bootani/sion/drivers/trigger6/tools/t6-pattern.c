// Minimal KMS test pattern: kmstest <card> <connector index> [<w> <h> <clock kHz> <htotal> <vtotal>] [seconds]
// Without a mode it uses the connector's preferred (first) mode; with one it
// sets that custom progressive mode. Draws colour bars, a grey ramp and a checkerboard.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static void pattern(uint32_t *p, int w, int h, int pitch)
{
	static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t c;
			if (y < h / 2)
				c = bars[x * 8 / w];
			else if (y < h * 3 / 4)
				c = (x * 255 / (w - 1)) * 0x010101;
			else
				c = ((x / 16 + y / 16) & 1) ? 0xffffff : 0x202020;
			p[y * (pitch / 4) + x] = c;
		}
}

int main(int argc, char **argv)
{
	int fd = open(argc > 1 ? argv[1] : "/dev/dri/card0", O_RDWR);
	int ci = argc > 2 ? atoi(argv[2]) : 0;
	int custom = argc > 7;
	int w = custom ? atoi(argv[3]) : 0, h = custom ? atoi(argv[4]) : 0;
	int clk = custom ? atoi(argv[5]) : 0, ht = custom ? atoi(argv[6]) : 0, vt = custom ? atoi(argv[7]) : 0;
	int secs = custom ? (argc > 8 ? atoi(argv[8]) : 10) : (argc > 3 ? atoi(argv[3]) : 10);
	struct drm_mode_modeinfo modes[256];
	struct drm_mode_card_res res = {};
	uint32_t crtcs[8], conns[8], encs[8], fbs[8];
	struct drm_mode_get_connector conn = {};
	struct drm_mode_create_dumb cd = {};
	struct drm_mode_map_dumb md = {};
	struct drm_mode_fb_cmd fb = {};
	struct drm_mode_crtc crtc = {};
	struct drm_mode_fb_dirty_cmd dirty = {};
	uint32_t *pix;

	if (fd < 0) { perror("open"); return 1; }
	ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("res"); return 1; }
	res.crtc_id_ptr = (uintptr_t)crtcs; res.connector_id_ptr = (uintptr_t)conns;
	res.encoder_id_ptr = (uintptr_t)encs; res.fb_id_ptr = (uintptr_t)fbs;
	res.count_fbs = 0;
	ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
	printf("kmstest: %u crtcs %u connectors\n", res.count_crtcs, res.count_connectors);
	if (ci < 0 || ci >= (int)res.count_connectors) { fprintf(stderr, "no connector %d\n", ci); return 1; }
	conn.connector_id = conns[ci];
	ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
	if (conn.count_modes > 256) conn.count_modes = 256;
	conn.modes_ptr = (uintptr_t)modes;
	conn.count_props = conn.count_encoders = 0;
	ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
	printf("kmstest: connector %u status %u (1 = connected), %u modes\n", conn.connector_id, conn.connection, conn.count_modes);
	for (unsigned i = 0; i < conn.count_modes; i++)
		printf("kmstest:   %s %u kHz %u Hz\n", modes[i].name, modes[i].clock, modes[i].vrefresh);
	if (!custom) {
		if (!conn.count_modes) { fprintf(stderr, "no modes\n"); return 1; }
		w = modes[0].hdisplay; h = modes[0].vdisplay;
	}

	cd.width = w; cd.height = h; cd.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("dumb"); return 1; }
	md.handle = cd.handle;
	ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
	pix = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	pattern(pix, w, h, cd.pitch);
	fb.width = w; fb.height = h; fb.pitch = cd.pitch; fb.bpp = 32; fb.depth = 24; fb.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb)) { perror("addfb"); return 1; }

	crtc.crtc_id = crtcs[ci];
	crtc.fb_id = fb.fb_id;
	crtc.set_connectors_ptr = (uintptr_t)&conns[ci];
	crtc.count_connectors = 1;
	crtc.mode_valid = 1;
	if (!custom) {
		crtc.mode = modes[0];
		goto set;
	}
	crtc.mode.clock = clk;
	crtc.mode.hdisplay = w; crtc.mode.hsync_start = w + (ht - w) / 4; crtc.mode.hsync_end = crtc.mode.hsync_start + (ht - w) / 4; crtc.mode.htotal = ht;
	crtc.mode.vdisplay = h; crtc.mode.vsync_start = h + 9; crtc.mode.vsync_end = h + 15; crtc.mode.vtotal = vt;
	crtc.mode.vrefresh = 60;
	crtc.mode.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
	snprintf(crtc.mode.name, sizeof(crtc.mode.name), "%dx%d", w, h);
set:
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc)) { perror("setcrtc"); return 1; }
	printf("kmstest: mode set %s %u kHz on crtc %u\n", crtc.mode.name, crtc.mode.clock, crtc.crtc_id);
	dirty.fb_id = fb.fb_id;
	ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
	sleep(secs);
	return 0;
}
