// Emulated MCT Trigger 6 (JUA365) on raw-gadget + dummy_hcd, for testing the
// trigger6 DRM driver without the hardware. Control answers are replayed from
// a real JUA365 USB capture; frames are checked and written to a disk.
//
// usage: t6emu <out0: none|hd|uhd> <out1: none|hd|uhd> [/dev/vda]
// commands on /tmp/t6ctl (fifo): "plug <n> <hd|uhd>", "unplug <n>"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>
#include "t6data.h"

static int fd, disk = -1, ep_bulk_out = -1, ep_int_in = -1;
static const unsigned char *edid[2];
static int edid_len[2];
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int pending_flags[64], npending;
static unsigned int intr_counter;

static const struct resp *find(unsigned char req, unsigned short val, unsigned short idx)
{
	for (unsigned i = 0; i < sizeof(resps) / sizeof(resps[0]); i++)
		if (resps[i].req == req && resps[i].val == val && resps[i].idx == idx)
			return &resps[i];
	return NULL;
}

static void pick_edid(int out, const char *what)
{
	const struct resp *r;

	edid[out] = NULL;
	edid_len[out] = 0;
	if (!strcmp(what, "hd")) {
		r = find(0x80, 0, 1);
		edid[out] = r->data;
		edid_len[out] = 128;
	} else if (!strcmp(what, "uhd")) {
		static unsigned char two[256];
		memcpy(two, find(0x80, 0, 0)->data, 128);
		memcpy(two + 128, find(0x80, 0x80, 0)->data, 128);
		edid[out] = two;
		edid_len[out] = 256;
	}
	printf("t6emu: output %d -> %s\n", out, what);
}

static void queue_intr(int flags)
{
	pthread_mutex_lock(&lk);
	if (npending < 64)
		pending_flags[npending++] = flags;
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&lk);
}

static void *intr_thread(void *arg)
{
	struct { struct usb_raw_ep_io io; unsigned char d[64]; } p;
	(void)arg;
	for (;;) {
		int f;
		pthread_mutex_lock(&lk);
		while (!npending)
			pthread_cond_wait(&cv, &lk);
		f = pending_flags[0];
		memmove(pending_flags, pending_flags + 1, --npending * sizeof(int));
		pthread_mutex_unlock(&lk);
		memset(&p, 0, sizeof(p));
		p.io.ep = ep_int_in;
		p.io.length = 64;
		p.d[0] = 4;
		p.d[12] = intr_counter++;
		p.d[19] = f;
		if (ioctl(fd, USB_RAW_IOCTL_EP_WRITE, &p) < 0)
			perror("intr write");
	}
	return NULL;
}

struct sel { uint32_t session, total, dest, len, offset, more, r[2]; };
struct hdr { uint32_t type, len, seq, format, geometry, addr0, addr1, src, enc, r[3]; };

static void *bulk_thread(void *arg)
{
	static struct { struct usb_raw_ep_io io; unsigned char d[0x19000]; } p;
	static unsigned char frame[16 << 20];
	size_t have = 0;
	int frames = 0;
	(void)arg;
	for (;;) {
		struct sel s;
		int n;
		p.io.ep = ep_bulk_out;
		p.io.flags = 0;
		p.io.length = 32;
		n = ioctl(fd, USB_RAW_IOCTL_EP_READ, &p);
		if (n != 32) {
			fprintf(stderr, "t6emu: select read %d (%s)\n", n, strerror(errno));
			if (n < 0) { usleep(100000); }
			continue;
		}
		memcpy(&s, p.d, 32);
		p.io.length = s.len;
		n = ioctl(fd, USB_RAW_IOCTL_EP_READ, &p);
		if (n != (int)s.len) {
			fprintf(stderr, "t6emu: BAD data read %d want %u\n", n, s.len);
			continue;
		}
		if (s.session != 0 || s.offset != have || s.total > sizeof(frame) ||
		    s.offset + s.len > s.total || (s.more != (s.offset + s.len < s.total))) {
			printf("t6emu: BAD select: session %u total %u dest %08x len %u off %u more %u (have %zu)\n",
			       s.session, s.total, s.dest, s.len, s.offset, s.more, have);
			have = 0;
			continue;
		}
		memcpy(frame + s.offset, p.d, s.len);
		have = s.offset + s.len;
		if (have == s.total) {
			struct hdr h;
			int out, w, ok = 1;
			memcpy(&h, frame, sizeof(h));
			out = h.type - 3;
			w = h.geometry & 0xffff;
			if (out < 0 || out > 1) ok = 0;
			if (h.len + 0x30 != s.total) ok = 0;
			if (h.addr0 != s.dest + 0x30 || h.src != h.addr0) ok = 0;
			if (h.format == 6 && (h.enc != 6 || h.addr1 != h.addr0 + w * (h.len / (w * 3 / 2)))) ok = 0;
			printf("t6emu: frame %d out %d seq %u fmt %u enc %u geom %08x y %08x uv %08x len %u -> %s\n",
			       ++frames, out, h.seq, h.format, h.enc, h.geometry, h.addr0, h.addr1, h.len,
			       ok ? "OK" : "BAD");
			fflush(stdout);
			if (disk >= 0 && ok && h.len <= (4 << 20)) {
				static int per[2];
				off_t base = ((off_t)out * 16 + (per[out]++ % 16)) * ((4 << 20) + 512);
				uint32_t meta[4] = { 0x36543654, (uint32_t)w, h.len / (w * 3 / 2), h.seq };
				if (pwrite(disk, meta, 16, base) != 16 ||
				    pwrite(disk, frame + 0x30, h.len, base + 512) != (ssize_t)h.len)
					perror("disk");
				fsync(disk);
			}
			have = 0;
			queue_intr(4);
		}
	}
	return NULL;
}

static void *ctl_thread(void *arg)
{
	char line[128];
	(void)arg;
	mkfifo("/tmp/t6ctl", 0666);
	for (;;) {
		FILE *f = fopen("/tmp/t6ctl", "r");
		while (f && fgets(line, sizeof(line), f)) {
			int n; char what[16] = "none";
			if (sscanf(line, "plug %d %15s", &n, what) >= 2 && n >= 0 && n < 2) {
				pick_edid(n, what);
				queue_intr(1 << n);
			} else if (sscanf(line, "unplug %d", &n) == 1 && n >= 0 && n < 2) {
				pick_edid(n, "none");
				queue_intr(1 << n);
			}
		}
		if (f) fclose(f);
	}
	return NULL;
}

static const struct usb_device_descriptor dev_desc = {
	.bLength = USB_DT_DEVICE_SIZE, .bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200, .bDeviceClass = 0xff, .bMaxPacketSize0 = 64,
	.idVendor = 0x0711, .idProduct = 0x5601, .bcdDevice = 0x1010,
	.iManufacturer = 1, .iProduct = 2, .bNumConfigurations = 1,
};

static unsigned char cfg[9 + 9 + 7 * 3];
static struct usb_endpoint_descriptor eps[3] = {
	{ USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, 0x81, USB_ENDPOINT_XFER_BULK, 512, 0 },
	{ USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, 0x02, USB_ENDPOINT_XFER_BULK, 512, 0 },
	{ USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, 0x85, USB_ENDPOINT_XFER_INT, 64, 5 },
};

static void build_cfg(void)
{
	struct usb_config_descriptor c = { 9, USB_DT_CONFIG, sizeof(cfg), 1, 1, 0, 0x80, 200 };
	struct usb_interface_descriptor i = { 9, USB_DT_INTERFACE, 0, 0, 3, 0xff, 0, 0, 0 };
	memcpy(cfg, &c, 9);
	memcpy(cfg + 9, &i, 9);
	for (int k = 0; k < 3; k++)
		memcpy(cfg + 18 + 7 * k, &eps[k], 7);
}

static void ep0_write(const void *d, int len)
{
	struct { struct usb_raw_ep_io io; unsigned char d[4096]; } p = {};
	p.io.ep = 0;
	p.io.length = len;
	memcpy(p.d, d, len);
	if (ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, &p) < 0)
		perror("ep0 write");
}

static int ep0_read(void *d, int len)
{
	struct { struct usb_raw_ep_io io; unsigned char d[4096]; } p = {};
	int n;
	p.io.ep = 0;
	p.io.length = len;
	n = ioctl(fd, USB_RAW_IOCTL_EP0_READ, &p);
	if (n > 0 && d)
		memcpy(d, p.d, n);
	return n;
}

static void stall(void) { ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0); }

static void string_desc(int idx, int len)
{
	unsigned char b[64];
	const char *s = idx == 1 ? "MCT Corp." : "T6 USB Station";
	int n = 2;
	if (idx == 0) { b[0] = 4; b[1] = 3; b[2] = 9; b[3] = 4; ep0_write(b, len < 4 ? len : 4); return; }
	for (; *s; s++) { b[n++] = *s; b[n++] = 0; }
	b[0] = n; b[1] = 3;
	ep0_write(b, len < n ? len : n);
}

static void control(struct usb_ctrlrequest *c)
{
	int len = c->wLength, val = c->wValue, idx = c->wIndex;
	unsigned char data[4096];

	if ((c->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
		switch (c->bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			switch (val >> 8) {
			case USB_DT_DEVICE: ep0_write(&dev_desc, len < 18 ? len : 18); return;
			case USB_DT_CONFIG: ep0_write(cfg, len < (int)sizeof(cfg) ? len : (int)sizeof(cfg)); return;
			case USB_DT_STRING: string_desc(val & 0xff, len); return;
			}
			stall();
			return;
		case USB_REQ_SET_CONFIGURATION:
			for (int k = 0; k < 3; k++) {
				int h = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, &eps[k]);
				if (h < 0) { perror("ep enable"); exit(1); }
				if (k == 1) ep_bulk_out = h;
				if (k == 2) ep_int_in = h;
			}
			ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, 100);
			ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0);
			ep0_read(NULL, 0);
			{
				pthread_t a, b;
				pthread_create(&a, NULL, bulk_thread, NULL);
				pthread_create(&b, NULL, intr_thread, NULL);
			}
			return;
		case USB_REQ_SET_INTERFACE:
			ep0_read(NULL, 0);
			return;
		}
		stall();
		return;
	}

	if (c->bRequestType == 0xc0) {
		const struct resp *r;
		if (c->bRequest == 0x87) {
			unsigned char st = val < 2 && edid[val];
			ep0_write(&st, 1);
			return;
		}
		if (c->bRequest == 0x80) {
			if (idx < 2 && edid[idx] && val + len <= edid_len[idx]) {
				ep0_write(edid[idx] + val, len);
				return;
			}
			printf("t6emu: EDID read out %d off %d: none\n", idx, val);
			stall();
			return;
		}
		r = find(c->bRequest, val, idx);
		if (r) {
			ep0_write(r->data, len < r->len ? len : r->len);
			return;
		}
		printf("t6emu: IN  %02x v=%04x i=%04x l=%d -> stall\n", c->bRequest, val, idx, len);
		stall();
		return;
	}
	if (c->bRequestType == 0x40) {
		int n = ep0_read(data, len);
		printf("t6emu: OUT %02x v=%04x i=%04x l=%d", c->bRequest, val, idx, len);
		for (int k = 0; k < n && k < 40; k++)
			printf("%s%02x", k ? "" : " ", data[k]);
		printf("\n");
		fflush(stdout);
		return;
	}
	stall();
}

int main(int argc, char **argv)
{
	struct usb_raw_init init = {};
	struct { struct usb_raw_event ev; unsigned char d[64]; } e;
	pthread_t ct;

	setvbuf(stdout, NULL, _IOLBF, 0);
	pick_edid(0, argc > 1 ? argv[1] : "hd");
	pick_edid(1, argc > 2 ? argv[2] : "none");
	if (argc > 3)
		disk = open(argv[3], O_RDWR);
	build_cfg();
	fd = open("/dev/raw-gadget", O_RDWR);
	if (fd < 0) { perror("raw-gadget"); return 1; }
	strcpy((char *)init.driver_name, "dummy_udc");
	strcpy((char *)init.device_name, "dummy_udc.0");
	init.speed = USB_SPEED_HIGH;
	if (ioctl(fd, USB_RAW_IOCTL_INIT, &init) < 0) { perror("init"); return 1; }
	if (ioctl(fd, USB_RAW_IOCTL_RUN, 0) < 0) { perror("run"); return 1; }
	pthread_create(&ct, NULL, ctl_thread, NULL);
	for (;;) {
		e.ev.type = 0;
		e.ev.length = sizeof(e.d);
		if (ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, &e) < 0) { perror("fetch"); return 1; }
		if (e.ev.type == USB_RAW_EVENT_CONTROL)
			control((struct usb_ctrlrequest *)e.d);
	}
}
