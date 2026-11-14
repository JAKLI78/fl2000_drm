// SPDX-License-Identifier: GPL-2.0
/*
 * Original driver uses default altsetting (#0) of streaming interface, which allows bursts of bulk
 * transfers of 15x1024 bytes on output. But the HW actually works incorrectly here: it uses same
 * endpoint #1 across interfaces 1 and 2, which is not allowed by USB specification: endpoint
 * addresses can be shared only between alternate settings, not interfaces. In order to workaround
 * this we use isochronous transfers instead of bulk. There is a possibility that we still can use
 * bulk transfers with interface 0, but this is yet to be checked.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

/* Stream buffers hold frames in the device format; the worker converts the newest screen content
 * into a free buffer right before transmission, several buffers are in flight at a time
 */
#define FL2000_SB_MIN 3
#define FL2000_SB_NUM (FL2000_SB_MIN + 1)

#define FL2000_URB_TIMEOUT 100

struct fl2000_stream_buf {
	struct list_head list;
	struct sg_table sgt;
	struct page **pages;
	unsigned int nr_pages;
	void *vaddr;
};

struct fl2000_stream {
	struct usb_device *usb_dev;
	struct drm_crtc *crtc;
	/* Each buffer journey: render (free) -> wait (in flight) -> render. The worker detaches a
	 * buffer from the lists while it fills it
	 */
	struct list_head render_list;
	struct list_head wait_list;
	spinlock_t list_lock; /* List access from bh and interrupt contexts */
	size_t buf_size;
	u32 bytes_pix;
	/* Buffer holding the newest frame, NULL if none (e.g. after a mode change). Worker only */
	struct fl2000_stream_buf *last_sb;
	/* XRGB8888 copy of the screen, written by DRM updates and converted by the worker, so that
	 * the expensive conversion does not run in the context of the client updating the screen
	 */
	struct mutex staging_lock; /* staging buffer, its size and dirty lines */
	u32 *staging;
	unsigned int st_width;
	unsigned int st_height;
	unsigned int dirty_y1; /* dirty lines [y1, y2), none if y1 >= y2 */
	unsigned int dirty_y2;
	/* Worker-private copy of the dirty staging lines, converted after dropping staging_lock so
	 * that updates never wait for a conversion
	 */
	u32 *conv;
	size_t conv_size;
	/* Cursor image (premultiplied ARGB8888) and position, blended into converted lines. Also
	 * under staging_lock
	 */
	u32 *cursor;
	size_t cursor_alloc;
	unsigned int cur_w; /* 0 if hidden */
	unsigned int cur_h;
	int cur_x;
	int cur_y;
	bool dithered; /* dither setting of last_sb's content */
	struct work_struct work;
	struct workqueue_struct *work_queue;
	struct semaphore work_sem;
	bool enabled;
	struct usb_anchor anchor;
};

static void fl2000_free_sb(struct fl2000_stream_buf *sb)
{
	vunmap(sb->vaddr);

	sg_free_table(&sb->sgt);

	for (int i = 0; i < sb->nr_pages && sb->pages[i]; i++)
		__free_page(sb->pages[i]);

	kfree(sb->pages);

	kfree(sb);
}

static struct fl2000_stream_buf *fl2000_alloc_sb(unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *sb;
	unsigned int nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	if (!sb)
		return NULL;

	sb->nr_pages = nr_pages;

	sb->pages = kcalloc(nr_pages, sizeof(*sb->pages), GFP_KERNEL);
	if (!sb->pages)
		goto error;

	for (int i = 0; i < nr_pages; i++) {
		sb->pages[i] = alloc_page(GFP_KERNEL);
		if (!sb->pages[i])
			goto error;
	}

	ret = sg_alloc_table_from_pages(&sb->sgt, sb->pages, nr_pages, 0, size, GFP_KERNEL);
	if (ret != 0)
		goto error;

	sb->vaddr = vmap(sb->pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!sb->vaddr)
		goto error;

	INIT_LIST_HEAD(&sb->list);
	memset(sb->vaddr, 0, nr_pages << PAGE_SHIFT);

	return sb;

error:
	fl2000_free_sb(sb);

	return NULL;
}

static void fl2000_stream_put_buffers(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *temp_sb;

	stream->last_sb = NULL;
	list_for_each_entry_safe(cur_sb, temp_sb, &stream->render_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
}

static int fl2000_stream_get_buffers(struct fl2000_stream *stream, unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *cur_sb;

	BUG_ON(!list_empty(&stream->render_list));

	for (int i = 0; i < FL2000_SB_NUM; i++) {
		cur_sb = fl2000_alloc_sb(size);
		if (!cur_sb) {
			ret = -ENOMEM;
			goto error;
		}

		list_add(&cur_sb->list, &stream->render_list);
	}

	return 0;

error:
	fl2000_stream_put_buffers(stream);
	return ret;
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	struct fl2000_stream *stream = res;

	UNUSED(dev);

	fl2000_stream_disable(stream);
	destroy_workqueue(stream->work_queue);
	fl2000_stream_put_buffers(stream);
	vfree(stream->staging);
	vfree(stream->conv);
	kvfree(stream->cursor);
}

static void fl2000_stream_data_completion(struct urb *urb)
{
	struct fl2000_stream_buf *cur_sb = urb->transfer_buffer;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = urb->context;
	unsigned long flags;

	if (stream) {
		spin_lock_irqsave(&stream->list_lock, flags);
		list_move_tail(&cur_sb->list, &stream->render_list);
		spin_unlock_irqrestore(&stream->list_lock, flags);

		/* Kick transmit workqueue */
		up(&stream->work_sem);

		fl2000_urb_status(usb_dev, urb->status, urb->pipe);
	}

	usb_free_urb(urb);
}

#ifdef CONFIG_X86_64
#include <asm/fpu/api.h>

/* Vectorized conversion from fl2000_simd.c, if the CPU has SSSE3 and SSE4.1 */
static bool fl2000_simd_usable(void)
{
	return boot_cpu_has(X86_FEATURE_SSSE3) && boot_cpu_has(X86_FEATURE_XMM4_1);
}

#define fl2000_simd_begin()	  kernel_fpu_begin()
#define fl2000_simd_end()	  kernel_fpu_end()
#define fl2000_simd_565(d, s, n, t) fl2000_simd_rgb565_line((u16 *)(d), s, n, t)
#define fl2000_simd_888(d, s, n) fl2000_simd_rgb888_line(d, s, n)
#else
static bool fl2000_simd_usable(void)
{
	return false;
}

#define fl2000_simd_begin()	  do { } while (0)
#define fl2000_simd_end()	  do { } while (0)
#define fl2000_simd_565(d, s, n, t) do { } while (0)
#define fl2000_simd_888(d, s, n) do { } while (0)
#endif

/* Lines converted per SIMD section: a 1080p line takes about a microsecond */
#define FL2000_SIMD_LINES 64

static void fl2000_xrgb888_to_rgb888_line(u8 *dbuf, u32 *sbuf, u32 pixels)
{
	unsigned int xx = 0;

	for (unsigned int x = 0; x < pixels; x++) {
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x000000FF) >> 0;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x0000FF00) >> 8;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x00FF0000) >> 16;
	}
}

static bool dither = true;
module_param(dither, bool, 0644);
MODULE_PARM_DESC(dither, "Ordered dithering for 16-bit color (default on)");

/* 4x4 Bayer matrix as per-channel offsets added before dropping low bits: 0..7 for the 5-bit red
 * and blue (3 bits dropped), 0..3 for the 6-bit green (2 bits dropped). Each offset occurs equally
 * often, so the average color is preserved
 */
#define FL2000_BAYER(m) ((((m) / 2) << 16) | (((m) / 4) << 8) | ((m) / 2))
static const u32 fl2000_dither[4][4] __aligned(16) = {
	{ FL2000_BAYER(0), FL2000_BAYER(8), FL2000_BAYER(2), FL2000_BAYER(10) },
	{ FL2000_BAYER(12), FL2000_BAYER(4), FL2000_BAYER(14), FL2000_BAYER(6) },
	{ FL2000_BAYER(3), FL2000_BAYER(11), FL2000_BAYER(1), FL2000_BAYER(9) },
	{ FL2000_BAYER(15), FL2000_BAYER(7), FL2000_BAYER(13), FL2000_BAYER(5) },
};

static u32 fl2000_dither_add(u32 p, u32 d)
{
	u32 q = 0;

	for (int c = 0; c < 24; c += 8)
		q |= min_t(u32, ((p >> c) & 0xFF) + ((d >> c) & 0xFF), 0xFF) << c;

	return q;
}

static void fl2000_xrgb888_to_rgb565_line(u16 *dbuf, u32 *sbuf, u32 pixels, const u32 *dither)
{
	for (unsigned int x = 0; x < pixels; x++) {
		u32 p = dither ? fl2000_dither_add(sbuf[x], dither[x & 3]) : sbuf[x];
		u16 val565 = ((p & 0x00F80000) >> 8) | ((p & 0x0000FC00) >> 5) |
			     ((p & 0x000000F8) >> 3);
		dbuf[x ^ 2] = val565;
	}
}

/* Premultiplied alpha blending: dst = src + dst * (1 - alpha) */
static u32 fl2000_blend(u32 dst, u32 src)
{
	u32 inv = 255 - (src >> 24);
	u32 out = 0;

	if (inv == 255)
		return dst;

	for (int c = 0; c < 24; c += 8) {
		u32 v = ((src >> c) & 0xFF) + (((dst >> c) & 0xFF) * inv + 127) / 255;

		out |= min_t(u32, v, 0xFF) << c;
	}

	return out;
}

/* Blend the cursor into the given screen lines of the conversion copy. Under staging_lock */
static void fl2000_stream_blend_cursor(struct fl2000_stream *stream, unsigned int y1,
				       unsigned int y2)
{
	const int width = (int)stream->st_width;
	int x0, x1, cy0, cy1;

	if (!stream->cur_w)
		return;

	x0 = max(stream->cur_x, 0);
	x1 = min(stream->cur_x + (int)stream->cur_w, width);
	cy0 = max(stream->cur_y, (int)y1);
	cy1 = min(stream->cur_y + (int)stream->cur_h, (int)y2);

	for (int y = cy0; y < cy1; y++) {
		u32 *dst = stream->conv + (size_t)y * width;
		const u32 *src = stream->cursor + (size_t)(y - stream->cur_y) * stream->cur_w;

		for (int x = x0; x < x1; x++)
			dst[x] = fl2000_blend(dst[x], src[x - stream->cur_x]);
	}
}

/* Fill a detached buffer with the newest frame: start from the previous frame and convert only
 * the lines changed since, or everything if there is no previous frame in this format
 */
static void fl2000_stream_fill(struct fl2000_stream *stream, struct fl2000_stream_buf *cur_sb)
{
	struct fl2000_stream_buf *last_sb = stream->last_sb;
	const bool dithered = READ_ONCE(dither);
	unsigned int y1, y2, width;
	size_t line_len, size;
	u32 dst_line_len;
	u32 *src;
	u8 *dst;

	/* Dither setting changed: redraw everything in the new style */
	if (dithered != stream->dithered) {
		stream->dithered = dithered;
		last_sb = NULL;
	}

	if (last_sb && last_sb != cur_sb)
		memcpy(cur_sb->vaddr, last_sb->vaddr, stream->buf_size);

	mutex_lock(&stream->staging_lock);

	width = stream->st_width;
	line_len = (size_t)width * sizeof(u32);
	size = line_len * stream->st_height;
	dst_line_len = width * stream->bytes_pix;
	if (!stream->staging || (size_t)dst_line_len * stream->st_height > stream->buf_size)
		goto keep; /* nothing drawn yet, or not matching the mode */

	if (last_sb) {
		y1 = stream->dirty_y1;
		y2 = stream->dirty_y2;
	} else {
		y1 = 0;
		y2 = stream->st_height;
	}
	if (y1 >= y2)
		goto keep;

	if (stream->conv_size != size) {
		vfree(stream->conv);
		stream->conv = vmalloc(size);
		stream->conv_size = stream->conv ? size : 0;
		if (!stream->conv)
			goto keep;
	}
	memcpy((u8 *)stream->conv + y1 * line_len, (u8 *)stream->staging + y1 * line_len,
	       (y2 - y1) * line_len);
	fl2000_stream_blend_cursor(stream, y1, y2);
	stream->dirty_y1 = stream->dirty_y2 = 0;

	mutex_unlock(&stream->staging_lock);

	/* Dither pattern row for a line, by absolute position so partial updates match */
#define drow(y) (dithered ? fl2000_dither[(y) & 3] : NULL)

	/* Whole lines only: pixels are packed in swapped 8-byte groups */
	src = stream->conv + (size_t)y1 * width;
	dst = (u8 *)cur_sb->vaddr + (size_t)y1 * dst_line_len;
	for (unsigned int y = y1; y < y2;) {
		/* SIMD sections disable preemption: keep each one short */
		unsigned int chunk_end = min(y2, y + FL2000_SIMD_LINES);
		bool simd = fl2000_simd_usable();

		if (simd)
			fl2000_simd_begin();
		for (; y < chunk_end; y++) {
			switch (stream->bytes_pix) {
			case 2:
				if (simd)
					fl2000_simd_565(dst, src, width, drow(y));
				else
					fl2000_xrgb888_to_rgb565_line((u16 *)dst, src, width, drow(y));
				break;
			case 3:
				if (simd)
					fl2000_simd_888(dst, src, width);
				else
					fl2000_xrgb888_to_rgb888_line(dst, src, width);
				break;
			default: /* Shouldn't happen */
				break;
			}
			src += width;
			dst += dst_line_len;
		}
		if (simd)
			fl2000_simd_end();
	}

#undef drow
	stream->last_sb = cur_sb;
	return;

keep:
	mutex_unlock(&stream->staging_lock);
	/* cur_sb now repeats the previous frame, if there is one */
	if (last_sb)
		stream->last_sb = cur_sb;
}

/* TODO: convert to tasklet */
static void fl2000_stream_work(struct work_struct *work)
{
	int ret;
	struct fl2000_stream *stream = container_of(work, struct fl2000_stream, work);
	struct usb_device *usb_dev = stream->usb_dev;
	struct fl2000_stream_buf *cur_sb;
	struct urb *data_urb;

	while (stream->enabled) {
		ret = down_interruptible(&stream->work_sem);
		if (ret) {
			dev_err(&usb_dev->dev, "Work interrupt error %d", ret);
			stream->enabled = false;
			return;
		}

		spin_lock_irq(&stream->list_lock);
		if (list_empty(&stream->render_list)) {
			/* Everything in flight; a completion kicks us again */
			spin_unlock_irq(&stream->list_lock);
			continue;
		}
		cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf, list);
		list_del_init(&cur_sb->list);
		spin_unlock_irq(&stream->list_lock);

		fl2000_stream_fill(stream, cur_sb);

		spin_lock_irq(&stream->list_lock);
		list_add_tail(&cur_sb->list, &stream->wait_list);
		spin_unlock_irq(&stream->list_lock);

		data_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!data_urb) {
			dev_err(&usb_dev->dev, "Data URB allocation error");
			stream->enabled = false;
			return;
		}

		/* Endpoint 1 bulk out. We store pointer to current stream buffer structure in
		 * transfer_buffer field of URB which is unused due to SGT
		 */
		usb_fill_bulk_urb(data_urb, usb_dev, usb_sndbulkpipe(usb_dev, 1), cur_sb,
				  (int)stream->buf_size, fl2000_stream_data_completion, stream);
		data_urb->interval = 0;
		data_urb->sg = cur_sb->sgt.sgl;
		data_urb->num_sgs = cur_sb->sgt.nents;
		data_urb->transfer_flags |= URB_ZERO_PACKET;

		usb_anchor_urb(data_urb, &stream->anchor);
		ret = fl2000_submit_urb(data_urb);
		if (ret) {
			dev_err(&usb_dev->dev, "Data URB error %d", ret);
			/* Never submitted, so never completes: unanchor or usb_kill_anchored_urbs()
			 * in fl2000_stream_disable() loops on it forever
			 */
			usb_unanchor_urb(data_urb);
			usb_free_urb(data_urb);
			stream->enabled = false;
		}
	}
}

/* Add screen lines [y1, y2) to those the worker converts next. Under staging_lock */
static void fl2000_stream_dirty(struct fl2000_stream *stream, int y1, int y2)
{
	y1 = clamp(y1, 0, (int)stream->st_height);
	y2 = clamp(y2, 0, (int)stream->st_height);
	if (y1 >= y2)
		return;

	if (stream->dirty_y1 >= stream->dirty_y2) {
		stream->dirty_y1 = y1;
		stream->dirty_y2 = y2;
	} else {
		stream->dirty_y1 = min(stream->dirty_y1, (unsigned int)y1);
		stream->dirty_y2 = max(stream->dirty_y2, (unsigned int)y2);
	}
}

/**
 * fl2000_stream_update() - take over new screen content
 * @stream:	stream
 * @src:	XRGB8888 framebuffer mapping
 * @width:	framebuffer width
 * @height:	framebuffer height
 * @pitch:	framebuffer line length in bytes
 * @rect:	damaged area, NULL for all
 *
 * Only copies the damaged lines; the worker converts them before the next transmission
 */
void fl2000_stream_update(struct fl2000_stream *stream, const void *src, unsigned int width,
			  unsigned int height, unsigned int pitch, const struct drm_rect *rect)
{
	unsigned int y1 = 0, y2 = height;
	size_t line_len = (size_t)width * sizeof(u32);

	mutex_lock(&stream->staging_lock);

	if (!stream->staging || stream->st_width != width || stream->st_height != height) {
		vfree(stream->staging);
		stream->staging = vzalloc(line_len * height);
		if (!stream->staging) {
			stream->st_width = 0;
			stream->st_height = 0;
			mutex_unlock(&stream->staging_lock);
			return;
		}
		stream->st_width = width;
		stream->st_height = height;
		stream->dirty_y1 = stream->dirty_y2 = 0;
		rect = NULL;
	}

	if (rect) {
		y1 = min_t(unsigned int, max(rect->y1, 0), height);
		y2 = min_t(unsigned int, max(rect->y2, 0), height);
	}
	if (y1 >= y2)
		goto out;

	if (pitch == line_len) {
		memcpy((u8 *)stream->staging + y1 * line_len, src + (size_t)y1 * pitch,
		       (y2 - y1) * line_len);
	} else {
		for (unsigned int y = y1; y < y2; y++)
			memcpy((u8 *)stream->staging + y * line_len, src + (size_t)y * pitch,
			       line_len);
	}

	fl2000_stream_dirty(stream, y1, y2);
out:
	mutex_unlock(&stream->staging_lock);
}

/**
 * fl2000_stream_cursor() - set cursor image and position
 * @stream:	stream
 * @src:	premultiplied ARGB8888 image, NULL to hide the cursor
 * @width:	image width
 * @height:	image height
 * @pitch:	image line length in bytes
 * @x:		position of the top left corner on screen, may be negative
 * @y:		same, vertical
 */
void fl2000_stream_cursor(struct fl2000_stream *stream, const void *src, unsigned int width,
			  unsigned int height, unsigned int pitch, int x, int y)
{
	size_t size = (size_t)width * height * sizeof(u32);

	mutex_lock(&stream->staging_lock);

	/* Old position gets the plain screen content back */
	if (stream->cur_w)
		fl2000_stream_dirty(stream, stream->cur_y, stream->cur_y + (int)stream->cur_h);
	stream->cur_w = 0;

	if (!src || !size)
		goto out;

	if (stream->cursor_alloc < size) {
		kvfree(stream->cursor);
		stream->cursor = kvmalloc(size, GFP_KERNEL);
		stream->cursor_alloc = stream->cursor ? size : 0;
		if (!stream->cursor)
			goto out;
	}

	for (unsigned int i = 0; i < height; i++)
		memcpy(stream->cursor + (size_t)i * width, src + (size_t)i * pitch,
		       width * sizeof(u32));

	stream->cur_w = width;
	stream->cur_h = height;
	stream->cur_x = x;
	stream->cur_y = y;
	fl2000_stream_dirty(stream, y, y + (int)height);
out:
	mutex_unlock(&stream->staging_lock);
}

int fl2000_stream_mode_set(struct fl2000_stream *stream, int pixels, u32 bytes_pix)
{
	int ret;
	unsigned int size;

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = (pixels * bytes_pix + 7) & ~(unsigned int)7;

	stream->bytes_pix = bytes_pix;
	stream->last_sb = NULL; /* previous frame may be in another format */

	/* If there are buffers with same size - keep them */
	if (stream->buf_size == size)
		return 0;

	/* Destroy wrong size buffers if they exist */
	if (!list_empty(&stream->render_list))
		fl2000_stream_put_buffers(stream);

	/* Allocate new buffers */
	ret = fl2000_stream_get_buffers(stream, size);
	if (ret) {
		fl2000_stream_put_buffers(stream);
		stream->buf_size = 0;
		return ret;
	}

	stream->buf_size = size;

	return 0;
}

int fl2000_stream_enable(struct fl2000_stream *stream)
{
	sema_init(&stream->work_sem, 0);
	stream->enabled = true;
	queue_work(stream->work_queue, &stream->work);

	/* Kick transmit workqueue with minimum buffers submitted */
	for (int i = 0; i < FL2000_SB_MIN; i++)
		up(&stream->work_sem);

	return 0;
}

void fl2000_stream_disable(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;

	stream->enabled = false;

	drain_workqueue(stream->work_queue);

	if (!usb_wait_anchor_empty_timeout(&stream->anchor, 1000))
		usb_kill_anchored_urbs(&stream->anchor);

	spin_lock_irq(&stream->list_lock);
	while (!list_empty(&stream->wait_list)) {
		cur_sb = list_first_entry(&stream->wait_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	spin_unlock_irq(&stream->list_lock);
}

/**
 * fl2000_stream_create() - streaming processing context creation
 * @interface:	streaming transfers interface
 *
 * This function is called only on Streaming interface probe
 *
 * It shall not initiate any USB transfers. URB is not allocated here because we do not know the
 * stream requirements yet.
 *
 * Return: Operation result
 */
struct fl2000_stream *fl2000_stream_create(struct usb_device *usb_dev, struct drm_crtc *crtc)
{
	int ret;
	struct fl2000_stream *stream;

	/* Altsettung 1 on interface 0 */
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface for bulk transfers");
		return ERR_PTR(ret);
	}

	stream = devres_alloc(&fl2000_stream_release, sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot allocate stream");
		return ERR_PTR(-ENOMEM);
	}
	devres_add(&usb_dev->dev, stream);

	INIT_WORK(&stream->work, &fl2000_stream_work);
	INIT_LIST_HEAD(&stream->render_list);
	INIT_LIST_HEAD(&stream->wait_list);
	spin_lock_init(&stream->list_lock);
	mutex_init(&stream->staging_lock);
	init_usb_anchor(&stream->anchor);
	sema_init(&stream->work_sem, 0);
	stream->usb_dev = usb_dev;
	stream->crtc = crtc;

	/* Unbound: the worker runs for as long as the stream does and converts whole frames */
	stream->work_queue = alloc_workqueue("fl2000_stream", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!stream->work_queue) {
		dev_err(&usb_dev->dev, "Allocate streaming workqueue failed");
		devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}

	return stream;
}

void fl2000_stream_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
