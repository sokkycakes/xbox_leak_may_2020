// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for MCT Trigger 6 (T6-688L / T6-688SL) USB 3.0 display adapters,
 * such as the j5create JUA365 / JCA365 (USB to dual HDMI), JUA214, JUA254,
 * JUA311, JCD543 and StarTech USB32HD2 / USB32HDES (rev 2).
 *
 * Protocol from the public reverse-engineering notes and USB captures in
 * https://github.com/cyrozap/mct-usb-display-adapter-re (0BSD), decoded
 * further for this driver:
 *
 *  - The chip has up to two display pipes ("outputs" 0 and 1). On the JUA365
 *    output 0 is the chip's own HDMI (up to 4K30), output 1 goes through a
 *    Sil9136 HDMI transmitter (up to ~200 MHz pixel clock).
 *  - Mode setting is one 32-byte vendor request with the timings and the
 *    pixel PLL setting: VCO = refclk * (N + frac / 1000), pixel clock =
 *    VCO / {32, 8, 16, 4}[post-divider index].
 *  - Frames go over the bulk OUT endpoint into the adapter's video RAM. The
 *    Windows driver mostly sends JPEG files, but the firmware also takes raw
 *    NV12 (full-range BT.601) frames, which is what this driver sends, so no
 *    image encoder is needed in the kernel.
 *  - The interrupt endpoint reports hotplug changes and one "frame done" per
 *    frame.
 */

#include <linux/module.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/wait.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#define DRIVER_NAME	"trigger6"
#define DRIVER_DESC	"MCT Trigger 6 USB display adapters"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

/* Vendor requests (bmRequestType 0x40 out / 0xc0 in) */
#define T6_SET_OUTPUT_ENABLE	0x03	/* wValue output, wIndex 0/1 */
#define T6_SET_MODE		0x12	/* wValue output, 32 bytes */
#define T6_GAMMA_CTRL		0x1c	/* wValue (output << 8) | 0 begin, 2 commit */
#define T6_GAMMA_UPLOAD		0x1d	/* wValue output << 8, wIndex 0x100/0x200/0x400 */
#define T6_SET_UNK23		0x23	/* 40 bytes, sent once at init */
#define T6_SET_ACTIVE		0x24	/* 16 bytes */
#define T6_UNK31		0x31
#define T6_GET_EDID		0x80	/* wValue byte offset, wIndex output */
#define T6_GET_STATUS		0x87	/* wValue output, 1 byte */
#define T6_GET_VRAM_MB		0x88
#define T6_GET_MODES		0x89	/* wValue output, wIndex byte offset */
#define T6_GET_A1		0xa1
#define T6_GET_A2		0xa2
#define T6_GET_A3		0xa3
#define T6_GET_A4		0xa4
#define T6_GET_A5		0xa5
#define T6_GET_INFO		0xb0	/* wIndex field */
#define T6_GET_SESSION		0xb1
#define T6_GET_DISP		0xb3
#define T6_GET_B4		0xb4
#define T6_GET_CC		0xcc


#define T6_NUM_OUTPUTS		2
#define T6_NUM_SLOTS		3
#define T6_CHUNK		0x19000
#define T6_FRAME_HDR		0x30
#define T6_MAX_MODES		64

#define T6_FMT_NV12		6
#define T6_FMT_RGB888		9	/* seen in captures, unverified */

static int t6_rgb888;
module_param_named(rgb888, t6_rgb888, int, 0644);
MODULE_PARM_DESC(rgb888, "Send 24-bit RGB frames instead of NV12 (experimental, unverified)");

static int t6_ack_timeout_ms = 100;
module_param_named(ack_timeout_ms, t6_ack_timeout_ms, int, 0644);
MODULE_PARM_DESC(ack_timeout_ms, "How long to wait for the adapter's frame-done report (0 = don't wait)");

struct t6_mode {
	__le32 clock_khz;
	__le16 refresh;
	__le16 htotal;
	__le16 hdisplay;
	__le16 hsync_start;
	__le16 hsync_len;
	__le16 vtotal;
	__le16 vdisplay;
	__le16 vsync_start;
	__le16 vsync_len;
	__le16 pll_frac;
	__le16 pll_frac_den;
	__le16 pll_ctrl;	/* (post-divider index << 8) | N */
	u8 hsync_pos;
	u8 vsync_pos;
	u8 flag2;
	u8 flag3;
} __packed;
static_assert(sizeof(struct t6_mode) == 32);

struct t6_select {
	__le32 session;		/* 0 = video, 3 = audio */
	__le32 total;		/* whole transfer, frame header included */
	__le32 dest;		/* video RAM address of the frame header */
	__le32 len;		/* bytes in the packet after this one */
	__le32 offset;		/* of that packet within the transfer */
	__le32 more;		/* 1 while more packets follow */
	__le32 rsvd[2];
} __packed;
static_assert(sizeof(struct t6_select) == 32);

struct t6_frame_hdr {
	__le32 type;		/* 3 = output 0, 4 = output 1 (full frame) */
	__le32 len;		/* bytes after this header */
	__le32 seq;
	__le32 format;		/* 6 = NV12 buffer, 9 = RGB888 buffer */
	__le32 geometry;	/* width | stride << 16 (bytes for RGB) */
	__le32 addr0;		/* Y / RGB */
	__le32 addr1;		/* UV */
	__le32 src;		/* raw: same as addr0; JPEG: 0 */
	__le32 encoding;	/* 6 raw NV12, 9 raw RGB, 0xd JPEG */
	__le32 rsvd[3];
} __packed;
static_assert(sizeof(struct t6_frame_hdr) == T6_FRAME_HDR);

/* Video RAM layout used by the Windows driver on a 58 MB JUA365 */
static const u32 t6_vram_base[T6_NUM_OUTPUTS] = { 0x00000030, 0x02500430 };
static const u32 t6_vram_slot[T6_NUM_OUTPUTS] = { 0x00c55560, 0x004552c0 };
static const int t6_max_clock[T6_NUM_OUTPUTS] = { 300000, 200000 };

struct t6_device;

struct t6_output {
	struct t6_device *t6;
	unsigned int idx;

	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct t6_mode chip_modes[T6_MAX_MODES];
	unsigned int num_chip_modes;

	/* frame buffers on the host side: back <- plane updates, sending -> USB */
	spinlock_t lock;
	u8 *buf[3];
	u8 *back, *ready, *sending;
	size_t buf_cap;
	bool dirty;
	bool active;
	unsigned int width, height, stride;
	size_t payload;
	unsigned int slot;
	struct work_struct work;
};

struct t6_device {
	struct drm_device drm;
	struct usb_device *udev;
	struct usb_interface *intf;
	unsigned int bulk_out_pipe;
	unsigned int intr_in_pipe;

	struct mutex ctrl_lock;
	u8 *ctrl_buf;
	u8 *bulk_buf;

	struct urb *intr_urb;
	u8 *intr_buf;
	dma_addr_t intr_dma;
	atomic_t acks;
	wait_queue_head_t ack_wq;
	struct work_struct hpd_work;

	struct workqueue_struct *wq;
	u32 seq;
	unsigned int ref_khz;
	unsigned int vram_mb;
	unsigned int num_active;

	struct t6_output out[T6_NUM_OUTPUTS];
};

#define to_t6(d) container_of(d, struct t6_device, drm)

/* ------------------------------------------------------------------------ */
/* USB helpers                                                              */

static int t6_ctrl_in(struct t6_device *t6, u8 req, u16 val, u16 idx, void *data, u16 len)
{
	int ret;

	if (len > PAGE_SIZE)
		return -EINVAL;
	mutex_lock(&t6->ctrl_lock);
	ret = usb_control_msg(t6->udev, usb_rcvctrlpipe(t6->udev, 0), req,
			      USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			      val, idx, t6->ctrl_buf, len, 1000);
	if (ret > 0 && data)
		memcpy(data, t6->ctrl_buf, ret);
	mutex_unlock(&t6->ctrl_lock);
	return ret;
}

static int t6_ctrl_out(struct t6_device *t6, u8 req, u16 val, u16 idx, const void *data, u16 len)
{
	int ret;

	if (len > PAGE_SIZE)
		return -EINVAL;
	mutex_lock(&t6->ctrl_lock);
	if (len)
		memcpy(t6->ctrl_buf, data, len);
	ret = usb_control_msg(t6->udev, usb_sndctrlpipe(t6->udev, 0), req,
			      USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			      val, idx, len ? t6->ctrl_buf : NULL, len, 1000);
	mutex_unlock(&t6->ctrl_lock);
	if (ret < 0)
		drm_dbg_driver(&t6->drm, "request %02x %04x %04x failed: %d\n", req, val, idx, ret);
	return ret < 0 ? ret : 0;
}

static int t6_bulk(struct t6_device *t6, const void *data, size_t len)
{
	int actual, ret;

	if (data != t6->bulk_buf)
		memcpy(t6->bulk_buf, data, len);
	ret = usb_bulk_msg(t6->udev, t6->bulk_out_pipe,
			   t6->bulk_buf, len, &actual, 1000);
	if (!ret && actual != len)
		ret = -EIO;
	return ret;
}

static void t6_set_active(struct t6_device *t6, bool on)
{
	/* The Windows driver sends this around every burst of frames. */
	static const u8 off[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x25, 0, 0, 0, 0, 2, 0 };
	u8 buf[16];

	memcpy(buf, off, sizeof(buf));
	buf[0] = on;
	t6_ctrl_out(t6, T6_SET_ACTIVE, 0, 0, buf, sizeof(buf));
}

static void t6_load_gamma(struct t6_output *out)
{
	struct t6_device *t6 = out->t6;
	u8 *ramp;
	int i;

	ramp = kmalloc(256, GFP_KERNEL);
	if (!ramp)
		return;
	for (i = 0; i < 256; i++)
		ramp[i] = i;
	t6_ctrl_out(t6, T6_GAMMA_CTRL, out->idx << 8, 0, NULL, 0);
	for (i = 0; i < 3; i++)
		t6_ctrl_out(t6, T6_GAMMA_UPLOAD, out->idx << 8, 0x100 << i, ramp, 256);
	t6_ctrl_out(t6, T6_GAMMA_CTRL, (out->idx << 8) | 2, 0, NULL, 0);
	kfree(ramp);
}

/* ------------------------------------------------------------------------ */
/* Interrupt endpoint                                                       */

static void t6_intr_complete(struct urb *urb)
{
	struct t6_device *t6 = urb->context;
	u8 *b = t6->intr_buf;
	u32 type;

	switch (urb->status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
	case -EPROTO:
		return;
	default:
		goto resubmit;
	}

	if (urb->actual_length >= 36) {
		type = get_unaligned_le32(b);
		if (type == 0x04) {
			if (b[19] & 0x04) {
				atomic_inc(&t6->acks);
				wake_up(&t6->ack_wq);
			}
			if (b[19] & 0x03)
				schedule_work(&t6->hpd_work);
		} else if (type == 0x20) {
			schedule_work(&t6->hpd_work);
		}
	}

resubmit:
	usb_submit_urb(urb, GFP_ATOMIC);
}

static void t6_hpd_work(struct work_struct *work)
{
	struct t6_device *t6 = container_of(work, struct t6_device, hpd_work);

	drm_helper_hpd_irq_event(&t6->drm);
}

/* ------------------------------------------------------------------------ */
/* Frames                                                                   */

static void t6_xrgb_to_nv12(u8 *dst, unsigned int stride, unsigned int w, unsigned int h,
			    const u8 *src, unsigned int pitch)
{
	u8 *y = dst, *uv = dst + (size_t)stride * h;
	unsigned int r, c;

	for (r = 0; r < h; r += 2) {
		const u32 *s0 = (const u32 *)(src + (size_t)r * pitch);
		const u32 *s1 = (const u32 *)(src + (size_t)(r + 1 < h ? r + 1 : r) * pitch);
		u8 *y0 = y + (size_t)r * stride;
		u8 *y1 = y + (size_t)(r + 1 < h ? r + 1 : r) * stride;
		u8 *d = uv + (size_t)(r / 2) * stride;

		for (c = 0; c < w; c += 2) {
			u32 p[4] = { s0[c], s0[c + 1], s1[c], s1[c + 1] };
			int R = 0, G = 0, B = 0, k;

			for (k = 0; k < 4; k++) {
				int pr = (p[k] >> 16) & 0xff, pg = (p[k] >> 8) & 0xff, pb = p[k] & 0xff;

				R += pr;
				G += pg;
				B += pb;
				/* full-range BT.601 (JFIF), as the adapter's JPEG path */
				(k < 2 ? y0 : y1)[c + (k & 1)] = (77 * pr + 150 * pg + 29 * pb + 128) >> 8;
			}
			d[c] = clamp((((-43 * R - 85 * G + 128 * B) / 4 + 128) >> 8) + 128, 0, 255);
			d[c + 1] = clamp((((128 * R - 107 * G - 21 * B) / 4 + 128) >> 8) + 128, 0, 255);
		}
	}
}

static void t6_xrgb_to_rgb888(u8 *dst, unsigned int stride, unsigned int w, unsigned int h,
			      const u8 *src, unsigned int pitch)
{
	unsigned int r, c;

	for (r = 0; r < h; r++) {
		const u32 *s = (const u32 *)(src + (size_t)r * pitch);
		u8 *d = dst + (size_t)r * stride;

		for (c = 0; c < w; c++) {
			d[3 * c + 0] = s[c] & 0xff;
			d[3 * c + 1] = (s[c] >> 8) & 0xff;
			d[3 * c + 2] = (s[c] >> 16) & 0xff;
		}
	}
}

static void t6_send_frame(struct t6_output *out, const u8 *frame)
{
	struct t6_device *t6 = out->t6;
	struct t6_frame_hdr *hdr = (struct t6_frame_hdr *)frame;
	struct t6_select sel = {};
	bool rgb = out->stride != out->width;
	u32 addr = t6_vram_base[out->idx] + out->slot * t6_vram_slot[out->idx];
	size_t total = T6_FRAME_HDR + out->payload, off;
	int acks, ret;

	out->slot = (out->slot + 1) % T6_NUM_SLOTS;

	/* The header sits in front of the pixels, right below their address. */
	memset(hdr, 0, sizeof(*hdr));
	hdr->type = cpu_to_le32(3 + out->idx);
	hdr->len = cpu_to_le32(out->payload);
	hdr->seq = cpu_to_le32(++t6->seq);
	hdr->format = cpu_to_le32(rgb ? T6_FMT_RGB888 : T6_FMT_NV12);
	/* NV12: width | stride << 16 (equal here); RGB888: stride in bytes */
	hdr->geometry = cpu_to_le32(rgb ? out->stride : out->width | (out->stride << 16));
	hdr->addr0 = cpu_to_le32(addr);
	hdr->addr1 = cpu_to_le32(rgb ? addr : addr + out->stride * out->height);
	hdr->src = cpu_to_le32(addr);
	hdr->encoding = cpu_to_le32(rgb ? T6_FMT_RGB888 : T6_FMT_NV12);

	acks = atomic_read(&t6->acks);
	for (off = 0; off < total; off += T6_CHUNK) {
		size_t len = min_t(size_t, T6_CHUNK, total - off);

		sel.session = 0;
		sel.total = cpu_to_le32(total);
		sel.dest = cpu_to_le32(addr - T6_FRAME_HDR);
		sel.len = cpu_to_le32(len);
		sel.offset = cpu_to_le32(off);
		sel.more = cpu_to_le32(off + len < total);
		ret = t6_bulk(t6, &sel, sizeof(sel));
		if (!ret)
			ret = t6_bulk(t6, frame + off, len);
		if (ret) {
			drm_err_ratelimited(&t6->drm, "output %u: frame send failed: %d\n",
					    out->idx, ret);
			return;
		}
	}

	if (t6_ack_timeout_ms > 0 &&
	    !wait_event_timeout(t6->ack_wq, atomic_read(&t6->acks) != acks,
				msecs_to_jiffies(t6_ack_timeout_ms)))
		drm_dbg_driver(&t6->drm, "output %u: no frame-done report\n", out->idx);
}

static void t6_output_work(struct work_struct *work)
{
	struct t6_output *out = container_of(work, struct t6_output, work);
	struct t6_device *t6 = out->t6;
	unsigned long flags;
	int idx;

	spin_lock_irqsave(&out->lock, flags);
	if (!out->dirty || !out->active) {
		spin_unlock_irqrestore(&out->lock, flags);
		return;
	}
	swap(out->ready, out->sending);
	out->dirty = false;
	spin_unlock_irqrestore(&out->lock, flags);

	if (!drm_dev_enter(&t6->drm, &idx))
		return;
	t6_send_frame(out, out->sending);
	drm_dev_exit(idx);
}

static void t6_free_bufs(struct t6_output *out)
{
	int i;

	for (i = 0; i < 3; i++) {
		vfree(out->buf[i]);
		out->buf[i] = NULL;
	}
	out->back = out->ready = out->sending = NULL;
	out->buf_cap = 0;
}

static int t6_alloc_bufs(struct t6_output *out, size_t size)
{
	int i;

	if (out->buf_cap >= size)
		return 0;
	t6_free_bufs(out);
	for (i = 0; i < 3; i++) {
		out->buf[i] = vzalloc(size);
		if (!out->buf[i]) {
			t6_free_bufs(out);
			return -ENOMEM;
		}
	}
	out->back = out->buf[0];
	out->ready = out->buf[1];
	out->sending = out->buf[2];
	out->buf_cap = size;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Modes                                                                    */

static bool t6_mode_matches(const struct t6_mode *cm, const struct drm_display_mode *m)
{
	int clk = le32_to_cpu(cm->clock_khz);

	return le16_to_cpu(cm->hdisplay) == m->hdisplay &&
	       le16_to_cpu(cm->hsync_start) == m->hsync_start &&
	       le16_to_cpu(cm->hsync_len) == m->hsync_end - m->hsync_start &&
	       le16_to_cpu(cm->htotal) == m->htotal &&
	       le16_to_cpu(cm->vdisplay) == m->vdisplay &&
	       le16_to_cpu(cm->vsync_start) == m->vsync_start &&
	       le16_to_cpu(cm->vsync_len) == m->vsync_end - m->vsync_start &&
	       le16_to_cpu(cm->vtotal) == m->vtotal &&
	       abs(clk - m->clock) <= max(m->clock / 200, 10);
}

static int t6_build_mode(struct t6_output *out, const struct drm_display_mode *m,
			 struct t6_mode *tm)
{
	/*
	 * The chip's own mode table always takes the largest post-divider that
	 * keeps the VCO under 1600 MHz; its lowest VCO is 805.6 MHz (25.175 MHz
	 * pixel clock / 32), so nothing below 800 MHz is tried.
	 */
	static const struct { u8 idx; u8 div; } posts[] = { {0, 32}, {2, 16}, {1, 8}, {3, 4} };
	unsigned int i, ref = out->t6->ref_khz;
	u64 vco = 0;
	u32 n, frac;

	for (i = 0; i < out->num_chip_modes; i++) {
		if (t6_mode_matches(&out->chip_modes[i], m)) {
			*tm = out->chip_modes[i];
			return 0;
		}
	}

	memset(tm, 0, sizeof(*tm));
	for (i = 0; i < ARRAY_SIZE(posts); i++) {
		vco = (u64)m->clock * posts[i].div;
		if (vco < 1600000)
			break;
	}
	if (i == ARRAY_SIZE(posts) || vco < 800000)
		return -ERANGE;
	n = div_u64(vco, ref);
	frac = DIV_ROUND_CLOSEST_ULL((vco - (u64)n * ref) * 1000, ref);
	if (frac >= 1000) {
		n++;
		frac -= 1000;
	}

	tm->clock_khz = cpu_to_le32(m->clock);
	tm->refresh = cpu_to_le16(drm_mode_vrefresh(m));
	tm->htotal = cpu_to_le16(m->htotal);
	tm->hdisplay = cpu_to_le16(m->hdisplay);
	tm->hsync_start = cpu_to_le16(m->hsync_start);
	tm->hsync_len = cpu_to_le16(m->hsync_end - m->hsync_start);
	tm->vtotal = cpu_to_le16(m->vtotal);
	tm->vdisplay = cpu_to_le16(m->vdisplay);
	tm->vsync_start = cpu_to_le16(m->vsync_start);
	tm->vsync_len = cpu_to_le16(m->vsync_end - m->vsync_start);
	tm->pll_frac = cpu_to_le16(frac);
	tm->pll_frac_den = cpu_to_le16(1000);
	tm->pll_ctrl = cpu_to_le16((posts[i].idx << 8) | n);
	tm->hsync_pos = !!(m->flags & DRM_MODE_FLAG_PHSYNC);
	tm->vsync_pos = !!(m->flags & DRM_MODE_FLAG_PVSYNC);
	return 0;
}

static enum drm_mode_status t6_check_mode(struct t6_output *out, const struct drm_display_mode *m)
{
	struct t6_mode tm;
	size_t need;

	if (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_DBLCLK))
		return MODE_NO_INTERLACE;
	if (m->hdisplay % 16)
		return MODE_H_ILLEGAL;
	if (m->vdisplay % 2)
		return MODE_V_ILLEGAL;
	if (m->clock > t6_max_clock[out->idx])
		return MODE_CLOCK_HIGH;
	need = (size_t)m->hdisplay * m->vdisplay * 3 / (t6_rgb888 ? 1 : 2);
	if (need + T6_FRAME_HDR > t6_vram_slot[out->idx])
		return MODE_MEM;
	if (t6_build_mode(out, m, &tm))
		return MODE_CLOCK_RANGE;
	return MODE_OK;
}

static void t6_read_chip_modes(struct t6_output *out)
{
	struct t6_device *t6 = out->t6;
	unsigned int off, i, n = 0;
	u8 *buf;
	int ret;

	buf = kmalloc(512, GFP_KERNEL);
	if (!buf)
		return;
	for (off = 0; off < T6_MAX_MODES * 32 && n < T6_MAX_MODES; off += 512) {
		ret = t6_ctrl_in(t6, T6_GET_MODES, out->idx, off, buf, 512);
		if (ret < 32)
			break;
		for (i = 0; i + 32 <= ret && n < T6_MAX_MODES; i += 32) {
			struct t6_mode *cm = (struct t6_mode *)(buf + i);
			u32 clk = le32_to_cpu(cm->clock_khz);

			if (!clk || clk > 600000 || !cm->hdisplay || !cm->vdisplay)
				goto done;
			out->chip_modes[n++] = *cm;
		}
		if (ret < 512)
			break;
	}
done:
	out->num_chip_modes = n;
	kfree(buf);
	drm_dbg_driver(&t6->drm, "output %u: %u chip modes\n", out->idx, n);
}

/* ------------------------------------------------------------------------ */
/* KMS                                                                      */

static const u32 t6_formats[] = { DRM_FORMAT_XRGB8888 };
static const u64 t6_modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };

static int t6_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *cs = NULL;

	if (ps->crtc)
		cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	return drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING, false, false);
}

static void t6_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct t6_output *out = container_of(plane, struct t6_output, plane);
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *sps = to_drm_shadow_plane_state(ps);
	struct drm_framebuffer *fb = ps->fb;
	unsigned long flags;
	const u8 *src;
	unsigned int w, h;
	int idx;

	if (!fb || !ps->visible || !out->back)
		return;
	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		return;
	if (!drm_dev_enter(plane->dev, &idx))
		goto out_end;

	w = min_t(unsigned int, out->width, drm_rect_width(&ps->dst));
	h = min_t(unsigned int, out->height, drm_rect_height(&ps->dst));
	src = (const u8 *)sps->data[0].vaddr + fb->offsets[0] +
	      (size_t)(ps->src.y1 >> 16) * fb->pitches[0] + (ps->src.x1 >> 16) * 4;
	if (out->stride != out->width)
		t6_xrgb_to_rgb888(out->back + T6_FRAME_HDR, out->stride, w, h, src, fb->pitches[0]);
	else
		t6_xrgb_to_nv12(out->back + T6_FRAME_HDR, out->stride, w, h & ~1u, src, fb->pitches[0]);

	spin_lock_irqsave(&out->lock, flags);
	swap(out->back, out->ready);
	out->dirty = true;
	spin_unlock_irqrestore(&out->lock, flags);
	queue_work(out->t6->wq, &out->work);

	drm_dev_exit(idx);
out_end:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
}

static const struct drm_plane_helper_funcs t6_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = t6_plane_atomic_check,
	.atomic_update = t6_plane_atomic_update,
};

static const struct drm_plane_funcs t6_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static enum drm_mode_status t6_crtc_mode_valid(struct drm_crtc *crtc,
					       const struct drm_display_mode *mode)
{
	return t6_check_mode(container_of(crtc, struct t6_output, crtc), mode);
}

static void t6_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct t6_output *out = container_of(crtc, struct t6_output, crtc);
	struct t6_device *t6 = out->t6;
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_display_mode *m = &cs->adjusted_mode;
	struct t6_mode tm;
	unsigned long flags;
	int idx;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	out->width = m->hdisplay;
	out->height = m->vdisplay;
	out->stride = t6_rgb888 ? m->hdisplay * 3 : m->hdisplay;
	out->payload = t6_rgb888 ? (size_t)out->stride * out->height :
				   (size_t)out->stride * out->height * 3 / 2;
	if (t6_alloc_bufs(out, T6_FRAME_HDR + out->payload)) {
		drm_err(&t6->drm, "output %u: out of memory\n", out->idx);
		goto exit;
	}
	if (t6_build_mode(out, m, &tm))
		goto exit;

	drm_dbg_driver(&t6->drm, "output %u: " DRM_MODE_FMT ", pll %04x %u/1000\n", out->idx,
		       DRM_MODE_ARG(m), le16_to_cpu(tm.pll_ctrl), le16_to_cpu(tm.pll_frac));
	t6_ctrl_out(t6, T6_SET_MODE, out->idx, 0, &tm, sizeof(tm));
	t6_ctrl_out(t6, T6_SET_OUTPUT_ENABLE, out->idx, 1, NULL, 0);
	if (!t6->num_active++)
		t6_set_active(t6, true);
	t6_load_gamma(out);

	spin_lock_irqsave(&out->lock, flags);
	out->active = true;
	spin_unlock_irqrestore(&out->lock, flags);
exit:
	drm_dev_exit(idx);
}

static void t6_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct t6_output *out = container_of(crtc, struct t6_output, crtc);
	struct t6_device *t6 = out->t6;
	unsigned long flags;
	bool was;
	int idx;

	spin_lock_irqsave(&out->lock, flags);
	was = out->active;
	out->active = false;
	out->dirty = false;
	spin_unlock_irqrestore(&out->lock, flags);
	flush_work(&out->work);

	if (!drm_dev_enter(crtc->dev, &idx))
		return;
	t6_ctrl_out(t6, T6_SET_OUTPUT_ENABLE, out->idx, 0, NULL, 0);
	if (was && !--t6->num_active)
		t6_set_active(t6, false);
	drm_dev_exit(idx);
}

static const struct drm_crtc_helper_funcs t6_crtc_helper_funcs = {
	.mode_valid = t6_crtc_mode_valid,
	.atomic_check = drm_crtc_helper_atomic_check,
	.atomic_enable = t6_crtc_atomic_enable,
	.atomic_disable = t6_crtc_atomic_disable,
};

static const struct drm_crtc_funcs t6_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_encoder_funcs t6_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int t6_edid_block(void *data, u8 *buf, unsigned int block, size_t len)
{
	struct t6_output *out = data;
	int ret;

	if (len > 128 || block > 255)
		return -EINVAL;
	ret = t6_ctrl_in(out->t6, T6_GET_EDID, block * 128, out->idx, buf, len);
	return ret == len ? 0 : -EIO;
}

static int t6_connector_get_modes(struct drm_connector *connector)
{
	struct t6_output *out = container_of(connector, struct t6_output, connector);
	const struct drm_edid *edid;
	int count;

	edid = drm_edid_read_custom(connector, t6_edid_block, out);
	drm_edid_connector_update(connector, edid);
	count = drm_edid_connector_add_modes(connector);
	drm_edid_free(edid);
	if (!count) {
		count = drm_add_modes_noedid(connector, 1920, 1080);
		drm_set_preferred_mode(connector, 1280, 720);
	}
	return count;
}

static int t6_connector_detect(struct drm_connector *connector,
			       struct drm_modeset_acquire_ctx *ctx, bool force)
{
	struct t6_output *out = container_of(connector, struct t6_output, connector);
	u8 st = 0;
	int idx, ret;

	if (!drm_dev_enter(connector->dev, &idx))
		return connector_status_disconnected;
	ret = t6_ctrl_in(out->t6, T6_GET_STATUS, out->idx, 0, &st, 1);
	drm_dev_exit(idx);
	if (ret != 1)
		return connector_status_unknown;
	return st ? connector_status_connected : connector_status_disconnected;
}

static enum drm_mode_status t6_connector_mode_valid(struct drm_connector *connector,
						    const struct drm_display_mode *mode)
{
	return t6_check_mode(container_of(connector, struct t6_output, connector), mode);
}

static const struct drm_connector_helper_funcs t6_connector_helper_funcs = {
	.get_modes = t6_connector_get_modes,
	.detect_ctx = t6_connector_detect,
	.mode_valid = t6_connector_mode_valid,
};

static const struct drm_connector_funcs t6_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs t6_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs t6_mode_config_helpers = {
	/* modeset before planes, so a frame never goes out ahead of its mode */
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static int t6_output_init(struct t6_device *t6, unsigned int i)
{
	struct drm_device *drm = &t6->drm;
	struct t6_output *out = &t6->out[i];
	int ret;

	out->t6 = t6;
	out->idx = i;
	spin_lock_init(&out->lock);
	INIT_WORK(&out->work, t6_output_work);

	ret = drm_universal_plane_init(drm, &out->plane, BIT(i), &t6_plane_funcs,
				       t6_formats, ARRAY_SIZE(t6_formats), t6_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&out->plane, &t6_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&out->plane);

	ret = drm_crtc_init_with_planes(drm, &out->crtc, &out->plane, NULL, &t6_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&out->crtc, &t6_crtc_helper_funcs);

	ret = drm_encoder_init(drm, &out->encoder, &t6_encoder_funcs, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	out->encoder.possible_crtcs = drm_crtc_mask(&out->crtc);

	ret = drm_connector_init(drm, &out->connector, &t6_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;
	drm_connector_helper_add(&out->connector, &t6_connector_helper_funcs);
	out->connector.polled = DRM_CONNECTOR_POLL_HPD | DRM_CONNECTOR_POLL_CONNECT |
				 DRM_CONNECTOR_POLL_DISCONNECT;
	return drm_connector_attach_encoder(&out->connector, &out->encoder);
}

/* ------------------------------------------------------------------------ */
/* Device                                                                   */

static int t6_hw_init(struct t6_device *t6)
{
	/* What the Windows driver writes after reading request 0xa3 */
	static const u8 unk23[40] = {
		0x03, 0, 0, 0, 0, 0, 0, 0, 0x1f, 0, 0, 0, 0x1f, 0, 0, 0,
		0x0f, 0, 0, 0, 0x0f, 0, 0, 0, 0x0f, 0, 0, 0, 0x0f, 0, 0, 0,
		0x0f, 0, 0, 0, 0x0f, 0, 0, 0,
	};
	u8 *b;
	u32 boot = 0, image = 0, platform = 0;
	int ret;

	b = kzalloc(256, GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	/* Same order as the Windows driver; most answers are only logged. */
	if (t6_ctrl_in(t6, T6_GET_INFO, 0, 1, b, 4) == 4)
		boot = get_unaligned_le32(b);
	if (t6_ctrl_in(t6, T6_GET_INFO, 0, 2, b, 4) == 4)
		image = get_unaligned_le32(b);
	t6_ctrl_in(t6, T6_GET_INFO, 0, 4, b, 4);
	ret = t6_ctrl_in(t6, T6_GET_INFO, 0, 0, b, 4);
	if (ret != 4) {
		drm_err(&t6->drm, "adapter does not answer (%d)\n", ret);
		kfree(b);
		return ret < 0 ? ret : -EIO;
	}
	platform = get_unaligned_le32(b);
	t6->ref_khz = platform == 1 ? 40000 : 48000;
	t6_ctrl_in(t6, T6_GET_INFO, 0, 5, b, 8);
	t6_ctrl_in(t6, T6_GET_B4, 0, 0, b, 4);
	memset(b, 0, 17);
	t6_ctrl_in(t6, T6_GET_INFO, 0, 3, b, 16);
	drm_info(&t6->drm, "MCT Trigger 6 \"%.16s\", platform %u, boot %08x, image %08x\n",
		 b, platform, boot, image);

	t6_ctrl_in(t6, T6_GET_SESSION, 0, 0, b, 132);
	t6_ctrl_in(t6, T6_GET_CC, 1, 0, b, 104);
	t6_ctrl_in(t6, T6_GET_SESSION, 0, 0, b, 132);
	t6_ctrl_in(t6, T6_GET_SESSION, 0, 3, b, 132);
	t6_ctrl_in(t6, T6_GET_A1, 0, 0, b, 16);
	t6_ctrl_in(t6, T6_GET_A4, 0, 0, b, 16);
	t6_ctrl_in(t6, T6_GET_A1, 0, 1, b, 16);
	t6_ctrl_in(t6, T6_GET_A5, 0, 0, b, 32);
	t6_ctrl_in(t6, T6_GET_A2, 0, 0, b, 16);
	t6_ctrl_in(t6, T6_GET_A3, 0, 0, b, 40);
	t6_ctrl_out(t6, T6_SET_UNK23, 0, 0, unk23, sizeof(unk23));
	t6_ctrl_in(t6, T6_GET_DISP, 0, 0, b, 112);

	b[0] = 0;
	t6_ctrl_in(t6, T6_GET_VRAM_MB, 0, 0, b, 1);
	t6->vram_mb = b[0];
	if (t6->vram_mb && t6->vram_mb < 58)
		drm_warn(&t6->drm, "only %u MB of video RAM; the layout assumes 58\n", t6->vram_mb);

	t6_ctrl_out(t6, T6_GAMMA_CTRL, 0x0000, 0, NULL, 0);
	t6_ctrl_out(t6, T6_GAMMA_CTRL, 0x0100, 0, NULL, 0);
	t6_ctrl_out(t6, T6_UNK31, 0, 0, NULL, 0);
	kfree(b);
	return 0;
}

static void t6_release(struct drm_device *drm, void *arg)
{
	struct t6_device *t6 = to_t6(drm);
	int i;

	for (i = 0; i < T6_NUM_OUTPUTS; i++)
		t6_free_bufs(&t6->out[i]);
	if (t6->intr_urb) {
		usb_kill_urb(t6->intr_urb);
		usb_free_coherent(t6->udev, 64, t6->intr_buf, t6->intr_dma);
		usb_free_urb(t6->intr_urb);
	}
	if (t6->wq)
		destroy_workqueue(t6->wq);
	kfree(t6->bulk_buf);
	kfree(t6->ctrl_buf);
	usb_put_dev(t6->udev);
}

DEFINE_DRM_GEM_FOPS(t6_fops);

static const struct drm_driver t6_driver = {
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops = &t6_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
};

static int t6_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct t6_device *t6;
	struct drm_device *drm;
	struct usb_endpoint_descriptor *ep_out, *ep_int;
	struct device *dma_dev;
	unsigned int i;
	int ret;

	/* bulk OUT 0x02 and interrupt IN 0x83 on the adapters seen so far */
	ret = usb_find_common_endpoints(intf->cur_altsetting, NULL, &ep_out, &ep_int, NULL);
	if (ret)
		return -ENODEV;

	t6 = devm_drm_dev_alloc(&intf->dev, &t6_driver, struct t6_device, drm);
	if (IS_ERR(t6))
		return PTR_ERR(t6);
	drm = &t6->drm;
	t6->intf = intf;
	t6->udev = usb_get_dev(interface_to_usbdev(intf));
	t6->bulk_out_pipe = usb_sndbulkpipe(t6->udev, usb_endpoint_num(ep_out));
	t6->intr_in_pipe = usb_rcvintpipe(t6->udev, usb_endpoint_num(ep_int));
	mutex_init(&t6->ctrl_lock);
	init_waitqueue_head(&t6->ack_wq);
	INIT_WORK(&t6->hpd_work, t6_hpd_work);
	ret = drmm_add_action(drm, t6_release, NULL);
	if (ret) {
		usb_put_dev(t6->udev);
		return ret;
	}

	t6->ctrl_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	t6->bulk_buf = kmalloc(T6_CHUNK, GFP_KERNEL);
	t6->wq = alloc_ordered_workqueue("trigger6", 0);
	t6->intr_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!t6->ctrl_buf || !t6->bulk_buf || !t6->wq || !t6->intr_urb)
		return -ENOMEM;
	t6->intr_buf = usb_alloc_coherent(t6->udev, 64, GFP_KERNEL, &t6->intr_dma);
	if (!t6->intr_buf) {
		usb_free_urb(t6->intr_urb);
		t6->intr_urb = NULL;
		return -ENOMEM;
	}

	dma_dev = usb_intf_get_dma_device(intf);
	if (dma_dev) {
		drm_dev_set_dma_dev(drm, dma_dev);
		put_device(dma_dev);
	}

	ret = t6_hw_init(t6);
	if (ret)
		return ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = 64;
	drm->mode_config.min_height = 64;
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &t6_mode_config_funcs;
	drm->mode_config.helper_private = &t6_mode_config_helpers;

	for (i = 0; i < T6_NUM_OUTPUTS; i++) {
		ret = t6_output_init(t6, i);
		if (ret)
			return ret;
		t6_read_chip_modes(&t6->out[i]);
	}
	drm_mode_config_reset(drm);

	usb_fill_int_urb(t6->intr_urb, t6->udev, t6->intr_in_pipe, t6->intr_buf, 64,
			 t6_intr_complete, t6, ep_int->bInterval);
	t6->intr_urb->transfer_dma = t6->intr_dma;
	t6->intr_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	ret = usb_submit_urb(t6->intr_urb, GFP_KERNEL);
	if (ret)
		drm_warn(drm, "no interrupt endpoint (%d); hotplug is polled\n", ret);

	t6_set_active(t6, false);
	usb_set_intfdata(intf, t6);

	drmm_kms_helper_poll_init(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup(drm, NULL);
	return 0;
}

static void t6_disconnect(struct usb_interface *intf)
{
	struct t6_device *t6 = usb_get_intfdata(intf);

	if (!t6)
		return;
	usb_kill_urb(t6->intr_urb);
	cancel_work_sync(&t6->hpd_work);
	drm_kms_helper_poll_fini(&t6->drm);
	drm_dev_unplug(&t6->drm);
	drm_atomic_helper_shutdown(&t6->drm);
	flush_workqueue(t6->wq);
}

static int t6_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct t6_device *t6 = usb_get_intfdata(intf);
	int ret;

	ret = drm_mode_config_helper_suspend(&t6->drm);
	usb_kill_urb(t6->intr_urb);
	return ret;
}

static int t6_resume(struct usb_interface *intf)
{
	struct t6_device *t6 = usb_get_intfdata(intf);

	usb_submit_urb(t6->intr_urb, GFP_NOIO);
	return drm_mode_config_helper_resume(&t6->drm);
}

static int t6_reset_resume(struct usb_interface *intf)
{
	struct t6_device *t6 = usb_get_intfdata(intf);

	t6_hw_init(t6);
	return t6_resume(intf);
}

static const struct usb_device_id t6_ids[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5600, 0) },	/* T6-688L: JUA214, JUA254 */
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5601, 0) },	/* T6-688SL: JUA365, JCD543, ... */
	{ }
};
MODULE_DEVICE_TABLE(usb, t6_ids);

static struct usb_driver t6_usb_driver = {
	.name = DRIVER_NAME,
	.probe = t6_probe,
	.disconnect = t6_disconnect,
	.suspend = t6_suspend,
	.resume = t6_resume,
	.reset_resume = t6_reset_resume,
	.id_table = t6_ids,
	.supports_autosuspend = 0,
};
module_usb_driver(t6_usb_driver);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
