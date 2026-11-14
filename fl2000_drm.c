// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define DRM_DRIVER_NAME "fl2000_drm"
#define DRM_DRIVER_DESC "USB-HDMI"
#define DRM_DRIVER_DATE "20181001"

#define DRM_DRIVER_MAJOR      0
#define DRM_DRIVER_MINOR      0
#define DRM_DRIVER_PATCHLEVEL 1

/* Maximum supported resolution, out-of-the-blue numbers */
#define FL20000_MAX_WIDTH  4000
#define FL20000_MAX_HEIGHT 4000

/* Force using 32-bit XRGB8888 on input for simplicity */
#define FL2000_FB_BPP 32
static const u32 fl2000_pixel_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* Maximum pixel clock set to 500MHz. It is hard to get more or less precise PLL configuration for
 * higher clock
 */
#define FL2000_MAX_PIXCLOCK 500000000

/* PLL computing precision is 6 digits after comma */
#define FL2000_PLL_PRECISION 1000000

/* Input xtal clock, Hz */
#define FL2000_XTAL 10000000 /* 10 MHz */

/* Internal vco clock min/max, Hz */
#define FL2000_VCOCLOCK_MIN 62500000 /* 62.5 MHz */
#define FL2000_VCOCLOCK_MAX 1000000000 /* 1GHz */

/* Maximum acceptable ppm error */
#define FL2000_PPM_ERR_MAX 500

/* Share of the USB link usable for sustained bulk pixel data. Uncompressed 1080p60 RGB888
 * (~373 MB/s) underruns on a 5 Gbps link behind a hub, so keep a conservative margin
 */
#define FL2000_BULK_BW_PERCENT 60

#define FL2000_BULK_BW_HIGH_SPEED	(480000000ull * FL2000_BULK_BW_PERCENT / 100 / 8)
#define FL2000_BULK_BW_SUPER_SPEED	(5000000000ull * FL2000_BULK_BW_PERCENT / 100 / 8)
#define FL2000_BULK_BW_SUPER_SPEED_PLUS (10000000000ull * FL2000_BULK_BW_PERCENT / 100 / 8)

/* pixclock in kHz, as in struct drm_display_mode */
static unsigned int fl2000_get_bytes_pix(enum usb_device_speed speed, unsigned int pixclock)
{
	unsigned int bytes_pix;
	unsigned int max_bw;

	/* Calculate maximum bandwidth, bytes per second */
	switch (speed) {
	case USB_SPEED_HIGH:
		max_bw = FL2000_BULK_BW_HIGH_SPEED;
		break;
	case USB_SPEED_SUPER:
		max_bw = FL2000_BULK_BW_SUPER_SPEED;
		break;
	case USB_SPEED_SUPER_PLUS:
		max_bw = FL2000_BULK_BW_SUPER_SPEED_PLUS;
		break;
	default:
		return 0;
	}

	/* Maximum bytes per pixel with maximum bandwidth */
	bytes_pix = max_bw / (pixclock * 1000u);
	switch (bytes_pix) {
	case 0: /* Not enough */
	case 1: /* RGB 332 not supported*/
		return 0;
	case 2: /* RGB 565 */
	case 3: /* RGB 888 */
		break;
	default:
		bytes_pix = 3;
		break;
	}

	return bytes_pix;
}

struct fl2000_drm_if {
	struct usb_device *usb_dev;
	struct device *master; /* component master (I2C adapter), not the DRM parent */
	struct device *dma_dev;
	struct drm_device *drm;
	struct drm_simple_display_pipe pipe;
	struct fl2000_stream *stream;
	struct fl2000_intr *intr;
	struct notifier_block pm_nb;
	bool suspended; /* display state saved by the PM notifier */
};

DEFINE_DRM_GEM_FOPS(fl2000_drm_driver_fops);

static void fl2000_drm_release(struct drm_device *drm)
{
	struct fl2000_drm_if *drm_if = drm->dev_private;

	put_device(drm_if->dma_dev);
}

/* The DRM device's parent (I2C adapter) cannot do DMA, so attach imported buffers to the USB host
 * controller instead, like other USB display drivers do
 */
static struct drm_gem_object *fl2000_gem_prime_import(struct drm_device *drm,
						      struct dma_buf *dma_buf)
{
	struct fl2000_drm_if *drm_if = drm->dev_private;

	if (!drm_if->dma_dev)
		return ERR_PTR(-ENODEV);

	return drm_gem_prime_import_dev(drm, dma_buf, drm_if->dma_dev);
}

static struct drm_driver fl2000_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.lastclose = drm_fb_helper_lastclose,
	.ioctls = NULL,
	.fops = &fl2000_drm_driver_fops,
	.release = fl2000_drm_release,

	DRM_GEM_SHMEM_DRIVER_OPS,
	.gem_prime_import = fl2000_gem_prime_import,

	.name = DRM_DRIVER_NAME,
	.desc = DRM_DRIVER_DESC,
	.date = DRM_DRIVER_DATE,
	.major = DRM_DRIVER_MAJOR,
	.minor = DRM_DRIVER_MINOR,
	.patchlevel = DRM_DRIVER_PATCHLEVEL,
};

static const struct drm_mode_config_funcs fl2000_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Integer division compute of ppm error */
static u64 fl2000_pll_ppm_err(u64 clock_mil, u32 vco_clk, u32 divisor)
{
	u64 pll_clk_mil = (u64)vco_clk * FL2000_PLL_PRECISION / divisor;
	u64 pll_clk_err;

	/* Not using abs() here to avoid possible overflow */
	if (pll_clk_mil > clock_mil)
		pll_clk_err = pll_clk_mil - clock_mil;
	else
		pll_clk_err = clock_mil - pll_clk_mil;

	return pll_clk_err / (clock_mil / FL2000_PLL_PRECISION);
}

static inline u32 fl2000_pll_get_divisor(u64 clock_mil, u32 vco_clk, u64 *min_ppm_err)
{
	static const u32 divisor_arr[] = {
		2,   4,	  6,   7,   8,	 9,   10,  11,	12,  13,  14,  15,  16,	 17,  18,  19,
		20,  21,  22,  23,  24,	 25,  26,  27,	28,  29,  30,  31,  32,	 33,  34,  35,
		36,  37,  38,  39,  40,	 41,  42,  43,	44,  45,  46,  47,  48,	 49,  50,  51,
		52,  53,  54,  55,  56,	 57,  58,  59,	60,  61,  62,  63,  64,	 65,  66,  67,
		68,  69,  70,  71,  72,	 73,  74,  75,	76,  77,  78,  79,  80,	 81,  82,  83,
		84,  85,  86,  87,  88,	 89,  90,  91,	92,  93,  94,  95,  96,	 97,  98,  99,
		100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
		116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128
	};
	u32 best_divisor = 0;

	/* Iterate over array */
	for (int i = 0; i < ARRAY_SIZE(divisor_arr); i++) {
		u32 divisor = divisor_arr[i];
		u64 ppm_err = fl2000_pll_ppm_err(clock_mil, vco_clk, divisor);

		if (ppm_err < *min_ppm_err) {
			*min_ppm_err = ppm_err;
			best_divisor = divisor;
		}
	}

	return best_divisor;
}

/* Try to match pixel clock - find parameters with minimal PLL error */
static u64 fl2000_pll_calc(u64 clock_mil, struct fl2000_pll *pll, u32 *clock_calculated)
{
	static const u32 prescaler_max = 2;
	static const u32 multiplier_max = 128;
	u64 min_ppm_err = (u64)(-1);

	for (u32 prescaler = 1; prescaler <= prescaler_max; prescaler++)
		for (u32 multiplier = 1; multiplier <= multiplier_max; multiplier++) {
			/* Do not need precision here yet, no 10^6 multiply */
			u32 vco_clk = FL2000_XTAL / prescaler * multiplier;
			u32 divisor;

			if (vco_clk < FL2000_VCOCLOCK_MIN || vco_clk > FL2000_VCOCLOCK_MAX)
				continue;

			divisor = fl2000_pll_get_divisor(clock_mil, vco_clk, &min_ppm_err);
			if (divisor == 0)
				continue;

			pll->prescaler = prescaler;
			pll->multiplier = multiplier;
			pll->divisor = divisor;
			if (vco_clk < 125000000) {
				pll->function = 0;
			} else if (vco_clk < 250000000) {
				pll->function = 1;
			} else if (vco_clk < 500000000) {
				pll->function = 2;
			} else {
				pll->function = 3;
			}
			*clock_calculated = vco_clk / divisor;
		}

	return min_ppm_err;
}

static int fl2000_mode_calc(const struct drm_display_mode *mode,
			    struct drm_display_mode *adjusted_mode, struct fl2000_pll *pll)
{
	u64 ppm_err;
	u32 clock_calculated;
	u64 clock_mil_adjusted;
	const u64 clock_mil = (u64)mode->clock * 1000 * FL2000_PLL_PRECISION;
	const int max_h_adjustment = 10;

	if (mode->clock * 1000 > FL2000_MAX_PIXCLOCK)
		return -1;

	/* Try to match pixel clock slightly adjusting htotal value, sequence is:
	   0, -1, 1, -2, 2, -3, 3, -3, 4, -4, 5, -5, ...
	   Here, 's' is used for sign, 'm' is used for modulo, and 'd' is the adjustment value
	 */
	for (int m = 0, s = 0, d = 0; m <= max_h_adjustment * 2; m++, s = -s, d += m * s) {
		/* Maximum pixel clock 1GHz, or 10^9Hz. Multiply by 10^6 we get 10^15Hz. Assume
		 * maximum htotal is 10000 pix (no way) we get 10^19 max value and using u64 which
		 * is 1.8*10^19 no overflow can occur. Assume all this was checked before
		 */
		clock_mil_adjusted = clock_mil * (mode->htotal + d) / mode->htotal;

		/* To keep precision use clock multiplied by 10^6 */
		ppm_err = fl2000_pll_calc(clock_mil_adjusted, pll, &clock_calculated);

		/* Stop searching as soon as the first valid option found */
		if (ppm_err < FL2000_PPM_ERR_MAX) {
			if (adjusted_mode) {
				drm_mode_copy(adjusted_mode, mode);
				adjusted_mode->htotal += d;
				adjusted_mode->clock = clock_calculated / 1000;
			}

			return 0;
		}
	}

	/* Cannot find PLL configuration that satisfy requirements */
	return -1;
}

static enum drm_mode_status fl2000_display_mode_valid(struct drm_simple_display_pipe *pipe,
						      const struct drm_display_mode *mode)
{
	struct drm_device *drm = pipe->crtc.dev;
	struct drm_display_mode adjusted_mode;
	struct fl2000_pll pll;
	struct fl2000_drm_if *drm_if = drm->dev_private;
	struct usb_device *usb_dev = drm_if->usb_dev;

	/* Get PLL configuration and check if mode adjustments needed */
	if (fl2000_mode_calc(mode, &adjusted_mode, &pll))
		return MODE_BAD;

	if (fl2000_get_bytes_pix(usb_dev->speed, adjusted_mode.clock) == 0)
		return MODE_BAD;

	return MODE_OK;
}

static void fl2000_display_enable(struct drm_simple_display_pipe *pipe,
				  struct drm_crtc_state *cstate,
				  struct drm_plane_state *plane_state)
{
	struct drm_device *drm = pipe->crtc.dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;

	/* TODO: check cstate/pstate? */
	UNUSED(cstate);
	UNUSED(plane_state);

	fl2000_stream_enable(drm_if->stream);
}

static void fl2000_display_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_device *drm = pipe->crtc.dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;

	fl2000_stream_disable(drm_if->stream);
}

static void fb2000_dirty(struct drm_framebuffer *fb, const struct iosys_map *map,
			 struct drm_rect *rect)
{
	int ret;
	int idx;
	struct drm_device *drm = fb->dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;

	if (!drm_dev_enter(fb->dev, &idx)) {
		dev_err(drm->dev, "DRM enter failed!");
		return;
	}

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (!ret) {
		fl2000_stream_update(drm_if->stream, map->vaddr, fb->width, fb->height,
				     fb->pitches[0], rect);
		drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	}

	drm_dev_exit(idx);
}

static void fl2000_display_update(struct drm_simple_display_pipe *pipe,
				  struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct drm_pending_vblank_event *event = crtc->state->event;
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(state);
	struct drm_rect rect;

	/* Only feed the stream while it runs: with the CRTC off (e.g. DPMS) nothing consumes frames.
	 * Turning it on again is a modeset, which updates the whole plane before enable
	 */
	if (crtc->state->active && state->fb &&
	    drm_atomic_helper_damage_merged(old_state, state, &rect))
		fb2000_dirty(state->fb, &shadow->data[0], &rect);

	/* No vblank: the new frame is taken over by the stream worker, so complete right away */
	if (event) {
		crtc->state->event = NULL;

		spin_lock_irq(&drm->event_lock);
		drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&drm->event_lock);
	}
}

/* Logical pipe management (no HW configuration here) */
static const struct drm_simple_display_pipe_funcs fl2000_display_funcs = {
	.mode_valid = fl2000_display_mode_valid,
	.enable = fl2000_display_enable,
	.disable = fl2000_display_disable,
	.update = fl2000_display_update,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};

static void fl2000_output_mode_set(struct drm_encoder *encoder, struct drm_display_mode *mode,
				   struct drm_display_mode *adjusted_mode)
{
	struct drm_device *drm = encoder->dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;
	struct usb_device *usb_dev = drm_if->usb_dev;
	struct fl2000_timings timings;
	struct fl2000_pll pll;
	unsigned int bytes_pix;

	/* Get PLL configuration and cehc if mode adjustments needed */
	if (fl2000_mode_calc(mode, adjusted_mode, &pll))
		return;

	/* Check how many bytes per pixel shall be used with adjusted clock */
	bytes_pix = fl2000_get_bytes_pix(usb_dev->speed, adjusted_mode->clock);
	if (!bytes_pix)
		return;

	dev_info(drm->dev, "Mode requested:  " DRM_MODE_FMT, DRM_MODE_ARG(mode));
	dev_info(drm->dev, "Mode configured: " DRM_MODE_FMT, DRM_MODE_ARG(adjusted_mode));

	/* Prepare timing configuration */
	timings.hactive = adjusted_mode->hdisplay;
	timings.htotal = adjusted_mode->htotal;
	timings.hsync_width = adjusted_mode->hsync_end - adjusted_mode->hsync_start;
	timings.hstart = adjusted_mode->htotal - adjusted_mode->hsync_start + 1;
	timings.vactive = adjusted_mode->vdisplay;
	timings.vtotal = adjusted_mode->vtotal;
	timings.vsync_width = adjusted_mode->vsync_end - adjusted_mode->vsync_start;
	timings.vstart = adjusted_mode->vtotal - adjusted_mode->vsync_start + 1;

	/* Set PLL settings */
	fl2000_set_pll(usb_dev, &pll);

	/* Reset FL2000 & confirm PLL settings */
	fl2000_reset(usb_dev);

	/* Set timings settings */
	fl2000_set_timings(usb_dev, &timings);

	/* Pixel format according to number of bytes per pixel */
	fl2000_set_pixfmt(usb_dev, bytes_pix);

	/* Configure frame transfers */
	fl2000_set_transfers(usb_dev);

	/* Enable interrupts */
	fl2000_enable_interrupts(usb_dev);

	fl2000_afe_magic(usb_dev);

	fl2000_stream_mode_set(drm_if->stream, mode->hdisplay * mode->vdisplay, bytes_pix);
}

/* FL2000 HW control functions: mode configuration, turn on/off */
static const struct drm_encoder_helper_funcs fl2000_encoder_funcs = {
	.mode_set = fl2000_output_mode_set,
};

/* Registered on the USB device in bind, released from unbind (or on USB disconnect) */
static void fl2000_drm_if_release(struct device *dev, void *res)
{
	struct fl2000_drm_if *drm_if = *(struct fl2000_drm_if **)res;
	struct drm_device *drm = drm_if->drm;
	struct device *master = drm_if->master;
	struct usb_device *usb_dev = drm_if->usb_dev;

	dev_info(master, "Unbinding FL2000 master");

	unregister_pm_notifier(&drm_if->pm_nb);
	if (drm_if->suspended) {
		drm_atomic_state_put(drm->mode_config.suspend_state);
		drm->mode_config.suspend_state = NULL;
	}

	/* Stop userspace access and turn the display off while bridge, stream and interrupts are
	 * still alive: disabling the pipe uses all of them
	 */
	drm_kms_helper_poll_fini(drm);
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	/* Detach bridge */
	component_unbind_all(master, &drm_if->pipe);

	/* Start streaming interface */
	fl2000_stream_destroy(usb_dev);

	/* Start interrupts interface */
	fl2000_intr_destroy(usb_dev);

	/* Final release happens once userspace closes it */
	drm_dev_put(drm);
}

static struct fl2000_drm_if *fl2000_drm_if_get(struct usb_device *usb_dev)
{
	struct fl2000_drm_if **res = devres_find(&usb_dev->dev, fl2000_drm_if_release, NULL, NULL);

	return res ? *res : NULL;
}

/* The display scans out buffers shared by another GPU (PRIME), which is suspended in parallel with
 * this USB device. Turn the display off before any device suspends and restore it after all have
 * resumed, so the exporting GPU is awake whenever the shared buffer is touched
 */
static int fl2000_drm_pm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	int ret;
	struct fl2000_drm_if *drm_if = container_of(nb, struct fl2000_drm_if, pm_nb);
	struct drm_device *drm = drm_if->drm;

	UNUSED(data);

	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		ret = drm_mode_config_helper_suspend(drm);
		if (ret)
			dev_err(drm->dev, "Cannot suspend display (%d)", ret);
		else
			drm_if->suspended = true;
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		/* Not set if the device was rebound during resume: the new one is not suspended */
		if (drm_if->suspended) {
			drm_if->suspended = false;
			ret = drm_mode_config_helper_resume(drm);
			if (ret)
				dev_err(drm->dev, "Cannot resume display (%d)", ret);
		}
		break;
	}

	return NOTIFY_DONE;
}

/* Interrupt transfers stop while the USB device sleeps. Nothing to do if the DRM device was never
 * bound (e.g. no bridge found)
 */
int fl2000_drm_suspend(struct usb_device *usb_dev)
{
	struct fl2000_drm_if *drm_if = fl2000_drm_if_get(usb_dev);

	if (drm_if)
		fl2000_intr_suspend(drm_if->intr);

	return 0;
}

int fl2000_drm_resume(struct usb_device *usb_dev)
{
	int ret;
	struct fl2000_drm_if *drm_if = fl2000_drm_if_get(usb_dev);

	if (!drm_if)
		return 0;

	ret = fl2000_intr_resume(drm_if->intr);
	if (ret)
		dev_err(&usb_dev->dev, "Cannot restart interrupts (%d)", ret);

	return 0;
}

/* TODO: release on errors! */
int fl2000_drm_bind(struct device *master)
{
	int ret = 0;
	struct usb_device *usb_dev = to_usb_device(master->parent);
	struct fl2000_drm_if *drm_if;
	struct fl2000_drm_if **drm_if_res;
	struct drm_device *drm;
	struct drm_mode_config *mode_config;

	dev_info(master, "Binding FL2000 master");

	/* Not devm: the I2C adapter (master) has no driver, so unbind releases it explicitly.
	 * Parent is the USB interface, not the I2C adapter: a DRM device kept open by userspace
	 * after unplug must not hold up adapter removal (i2c_del_adapter waits for all references)
	 */
	drm = drm_dev_alloc(&fl2000_drm_driver,
			    &usb_ifnum_to_if(usb_dev, FL2000_USBIF_AVCONTROL)->dev);
	if (IS_ERR(drm)) {
		dev_err(master, "Cannot allocate DRM structure (%ld)", PTR_ERR(drm));
		return (int)PTR_ERR(drm);
	}
	drm_if = drmm_kzalloc(drm, sizeof(*drm_if), GFP_KERNEL);
	if (!drm_if) {
		drm_dev_put(drm);
		return -ENOMEM;
	}
	drm_if->drm = drm;
	drm_if->master = master;
	drm_if->usb_dev = usb_dev;
	drm->dev_private = drm_if;

	drm_if_res = devres_alloc(fl2000_drm_if_release, sizeof(*drm_if_res), GFP_KERNEL);
	if (!drm_if_res) {
		drm_dev_put(drm);
		return -ENOMEM;
	}
	*drm_if_res = drm_if;

	ret = drmm_mode_config_init(drm);
	if (ret) {
		dev_err(master, "Cannot initialize DRM mode (%d)", ret);
		return ret;
	}

	mode_config = &drm->mode_config;
	mode_config->funcs = &fl2000_mode_config_funcs;
	mode_config->min_width = 1;
	mode_config->max_width = FL20000_MAX_WIDTH;
	mode_config->min_height = 1;
	mode_config->max_height = FL20000_MAX_HEIGHT;

	/* Imported buffers are mapped by the USB host controller */
	drm_if->dma_dev = usb_intf_get_dma_device(usb_ifnum_to_if(usb_dev, FL2000_USBIF_AVCONTROL));
	if (!drm_if->dma_dev)
		dev_warn(drm->dev, "No DMA device, buffer sharing is disabled");

	ret = drm_simple_display_pipe_init(drm, &drm_if->pipe, &fl2000_display_funcs,
					   fl2000_pixel_formats, ARRAY_SIZE(fl2000_pixel_formats),
					   NULL, NULL);
	if (ret) {
		dev_err(drm->dev, "Cannot configure simple display pipe (%d)", ret);
		return ret;
	}

	/* Register 'mode_set' function to operate prior to bridge */
	drm_encoder_helper_add(&drm_if->pipe.encoder, &fl2000_encoder_funcs);

	/* Start streaming interface */
	drm_if->stream = fl2000_stream_create(usb_dev, &drm_if->pipe.crtc);

	/* Start interrupts interface */
	drm_if->intr = fl2000_intr_create(usb_dev, drm);

	/* Attach bridge */
	ret = component_bind_all(master, &drm_if->pipe);
	if (ret) {
		dev_err(drm->dev, "Cannot attach bridge (%d)", ret);
		return ret;
	}

	drm_mode_config_reset(drm);

	/* No vblank support on purpose, like other USB displays: frames are paced by USB transfers,
	 * and with vblank every atomic commit (e.g. each DIRTYFB from Xorg) would block until the
	 * next transfer completes, stalling the X server
	 */

	drm_kms_helper_poll_init(drm);

	drm_plane_enable_fb_damage_clips(&drm_if->pipe.plane);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(drm->dev, "Cannot register DRM device (%d)", ret);
		return ret;
	}

	fl2000_reset(usb_dev);
	fl2000_usb_magic(usb_dev);

	drm_fbdev_generic_setup(drm, FL2000_FB_BPP);

	drm_if->pm_nb.notifier_call = fl2000_drm_pm_notify;
	register_pm_notifier(&drm_if->pm_nb);

	/* From here on unbind (fl2000_drm_unbind) tears everything down */
	devres_add(&usb_dev->dev, drm_if_res);

	return 0;
}

void fl2000_drm_unbind(struct device *master)
{
	struct usb_device *usb_dev = to_usb_device(master->parent);

	devres_release(&usb_dev->dev, fl2000_drm_if_release, NULL, NULL);
}
