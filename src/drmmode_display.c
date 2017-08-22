/*
 * Copyright © 2007 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Dave Airlie <airlied@redhat.com>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include "cursorstr.h"
#include "damagestr.h"
#include "micmap.h"
#include "xf86cmap.h"
#include "xf86Priv.h"
#include "sarea.h"

#include "drmmode_display.h"
#include "amdgpu_bo_helper.h"
#include "amdgpu_glamor.h"
#include "amdgpu_list.h"
#include "amdgpu_pixmap.h"

#ifdef AMDGPU_PIXMAP_SHARING
#include <dri.h>
#endif

/* DPMS */
#ifdef HAVE_XEXTPROTO_71
#include <X11/extensions/dpmsconst.h>
#else
#define DPMS_SERVER
#include <X11/extensions/dpms.h>
#endif

#include <gbm.h>

#define DEFAULT_NOMINAL_FRAME_RATE 60

static Bool drmmode_xf86crtc_resize(ScrnInfoPtr scrn, int width, int height);

static Bool
AMDGPUZaphodStringMatches(ScrnInfoPtr pScrn, const char *s, char *output_name)
{
	int i = 0;
	char s1[20];

	do {
		switch (*s) {
		case ',':
			s1[i] = '\0';
			i = 0;
			if (strcmp(s1, output_name) == 0)
				return TRUE;
			break;
		case ' ':
		case '\t':
		case '\n':
		case '\r':
			break;
		default:
			s1[i] = *s;
			i++;
			break;
		}
	} while (*s++);

	s1[i] = '\0';
	if (strcmp(s1, output_name) == 0)
		return TRUE;

	return FALSE;
}


/* Wait for the boolean condition to be FALSE */
#define drmmode_crtc_wait_pending_event(drmmode_crtc, fd, condition) \
	do {} while ((condition) && \
		     drmHandleEvent(fd, &drmmode_crtc->drmmode->event_context) \
		     > 0);


static PixmapPtr drmmode_create_bo_pixmap(ScrnInfoPtr pScrn,
					  int width, int height,
					  int depth, int bpp,
					  int pitch,
					  struct amdgpu_buffer *bo)
{
	ScreenPtr pScreen = pScrn->pScreen;
	PixmapPtr pixmap;

	pixmap = (*pScreen->CreatePixmap)(pScreen, 0, 0, depth,
					  AMDGPU_CREATE_PIXMAP_SCANOUT);
	if (!pixmap)
		return NULL;

	if (!(*pScreen->ModifyPixmapHeader) (pixmap, width, height,
					     depth, bpp, pitch, NULL))
		goto fail;

	if (!amdgpu_glamor_create_textured_pixmap(pixmap, bo))
		goto fail;

	if (amdgpu_set_pixmap_bo(pixmap, bo))
		return pixmap;

fail:
	pScreen->DestroyPixmap(pixmap);
	return NULL;
}

static void drmmode_destroy_bo_pixmap(PixmapPtr pixmap)
{
	ScreenPtr pScreen = pixmap->drawable.pScreen;

	(*pScreen->DestroyPixmap) (pixmap);
}

static void
drmmode_ConvertFromKMode(ScrnInfoPtr scrn,
			 drmModeModeInfo * kmode, DisplayModePtr mode)
{
	memset(mode, 0, sizeof(DisplayModeRec));
	mode->status = MODE_OK;

	mode->Clock = kmode->clock;

	mode->HDisplay = kmode->hdisplay;
	mode->HSyncStart = kmode->hsync_start;
	mode->HSyncEnd = kmode->hsync_end;
	mode->HTotal = kmode->htotal;
	mode->HSkew = kmode->hskew;

	mode->VDisplay = kmode->vdisplay;
	mode->VSyncStart = kmode->vsync_start;
	mode->VSyncEnd = kmode->vsync_end;
	mode->VTotal = kmode->vtotal;
	mode->VScan = kmode->vscan;

	mode->Flags = kmode->flags;	//& FLAG_BITS;
	mode->name = strdup(kmode->name);

	if (kmode->type & DRM_MODE_TYPE_DRIVER)
		mode->type = M_T_DRIVER;
	if (kmode->type & DRM_MODE_TYPE_PREFERRED)
		mode->type |= M_T_PREFERRED;
	xf86SetModeCrtc(mode, scrn->adjustFlags);
}

static void
drmmode_ConvertToKMode(ScrnInfoPtr scrn,
		       drmModeModeInfo * kmode, DisplayModePtr mode)
{
	memset(kmode, 0, sizeof(*kmode));

	kmode->clock = mode->Clock;
	kmode->hdisplay = mode->HDisplay;
	kmode->hsync_start = mode->HSyncStart;
	kmode->hsync_end = mode->HSyncEnd;
	kmode->htotal = mode->HTotal;
	kmode->hskew = mode->HSkew;

	kmode->vdisplay = mode->VDisplay;
	kmode->vsync_start = mode->VSyncStart;
	kmode->vsync_end = mode->VSyncEnd;
	kmode->vtotal = mode->VTotal;
	kmode->vscan = mode->VScan;

	kmode->flags = mode->Flags;	//& FLAG_BITS;
	if (mode->name)
		strncpy(kmode->name, mode->name, DRM_DISPLAY_MODE_LEN);
	kmode->name[DRM_DISPLAY_MODE_LEN - 1] = 0;

}

/*
 * Utility helper for drmWaitVBlank
 */
Bool
drmmode_wait_vblank(xf86CrtcPtr crtc, drmVBlankSeqType type,
		    uint32_t target_seq, unsigned long signal, uint64_t *ust,
		    uint32_t *result_seq)
{
	int crtc_id = drmmode_get_crtc_id(crtc);
	ScrnInfoPtr scrn = crtc->scrn;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
	drmVBlank vbl;

	if (crtc_id == 1)
		type |= DRM_VBLANK_SECONDARY;
	else if (crtc_id > 1)
		type |= (crtc_id << DRM_VBLANK_HIGH_CRTC_SHIFT) &
			DRM_VBLANK_HIGH_CRTC_MASK;

	vbl.request.type = type;
	vbl.request.sequence = target_seq;
	vbl.request.signal = signal;

	if (drmWaitVBlank(pAMDGPUEnt->fd, &vbl) != 0)
		return FALSE;

	if (ust)
		*ust = (uint64_t)vbl.reply.tval_sec * 1000000 +
			vbl.reply.tval_usec;
	if (result_seq)
		*result_seq = vbl.reply.sequence;

	return TRUE;
}

/*
 * Retrieves present time in microseconds that is compatible
 * with units used by vblank timestamps. Depending on the kernel
 * version and DRM kernel module configuration, the vblank
 * timestamp can either be in real time or monotonic time
 */
int drmmode_get_current_ust(int drm_fd, CARD64 * ust)
{
	uint64_t cap_value;
	int ret;
	struct timespec now;

	ret = drmGetCap(drm_fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap_value);
	if (ret || !cap_value)
		/* old kernel or drm_timestamp_monotonic turned off */
		ret = clock_gettime(CLOCK_REALTIME, &now);
	else
		ret = clock_gettime(CLOCK_MONOTONIC, &now);
	if (ret)
		return ret;
	*ust = ((CARD64) now.tv_sec * 1000000) + ((CARD64) now.tv_nsec / 1000);
	return 0;
}

/*
 * Get current frame count and frame count timestamp of the crtc.
 */
int drmmode_crtc_get_ust_msc(xf86CrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
	ScrnInfoPtr scrn = crtc->scrn;
	uint32_t seq;

	if (!drmmode_wait_vblank(crtc, DRM_VBLANK_RELATIVE, 0, 0, ust, &seq)) {
		xf86DrvMsg(scrn->scrnIndex, X_WARNING,
			   "get vblank counter failed: %s\n", strerror(errno));
		return -1;
	}

	*msc = seq;

	return Success;
}

static void
drmmode_do_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	ScrnInfoPtr scrn = crtc->scrn;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
	CARD64 ust;
	int ret;

	if (drmmode_crtc->dpms_mode == DPMSModeOn && mode != DPMSModeOn) {
		uint32_t seq;

		drmmode_crtc_wait_pending_event(drmmode_crtc, pAMDGPUEnt->fd,
						drmmode_crtc->flip_pending);

		/*
		 * On->Off transition: record the last vblank time,
		 * sequence number and frame period.
		 */
		if (!drmmode_wait_vblank(crtc, DRM_VBLANK_RELATIVE, 0, 0, &ust,
					 &seq))
			xf86DrvMsg(scrn->scrnIndex, X_ERROR,
				   "%s cannot get last vblank counter\n",
				   __func__);
		else {
			CARD64 nominal_frame_rate, pix_in_frame;

			drmmode_crtc->dpms_last_ust = ust;
			drmmode_crtc->dpms_last_seq = seq;
			nominal_frame_rate = crtc->mode.Clock;
			nominal_frame_rate *= 1000;
			pix_in_frame = crtc->mode.HTotal * crtc->mode.VTotal;
			if (nominal_frame_rate == 0 || pix_in_frame == 0)
				nominal_frame_rate = DEFAULT_NOMINAL_FRAME_RATE;
			else
				nominal_frame_rate /= pix_in_frame;
			drmmode_crtc->dpms_last_fps = nominal_frame_rate;
		}
	} else if (drmmode_crtc->dpms_mode != DPMSModeOn && mode == DPMSModeOn) {
		/*
		 * Off->On transition: calculate and accumulate the
		 * number of interpolated vblanks while we were in Off state
		 */
		ret = drmmode_get_current_ust(pAMDGPUEnt->fd, &ust);
		if (ret)
			xf86DrvMsg(scrn->scrnIndex, X_ERROR,
				   "%s cannot get current time\n", __func__);
		else if (drmmode_crtc->dpms_last_ust) {
			CARD64 time_elapsed, delta_seq;
			time_elapsed = ust - drmmode_crtc->dpms_last_ust;
			delta_seq = time_elapsed * drmmode_crtc->dpms_last_fps;
			delta_seq /= 1000000;
			drmmode_crtc->interpolated_vblanks += delta_seq;

		}
	}
	drmmode_crtc->dpms_mode = mode;
}

static void
drmmode_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(crtc->scrn);

	/* Disable unused CRTCs and enable/disable active CRTCs */
	if (!crtc->enabled || mode != DPMSModeOn) {
		drmmode_crtc_wait_pending_event(drmmode_crtc, pAMDGPUEnt->fd,
						drmmode_crtc->flip_pending);
		drmModeSetCrtc(pAMDGPUEnt->fd, drmmode_crtc->mode_crtc->crtc_id,
			       0, 0, 0, NULL, 0, NULL);
		drmmode_fb_reference(pAMDGPUEnt->fd, &drmmode_crtc->fb, NULL);
	} else if (drmmode_crtc->dpms_mode != DPMSModeOn)
		crtc->funcs->set_mode_major(crtc, &crtc->mode, crtc->rotation,
					    crtc->x, crtc->y);
}

static PixmapPtr
create_pixmap_for_fbcon(drmmode_ptr drmmode,
			ScrnInfoPtr pScrn, int fbcon_id)
{
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	PixmapPtr pixmap = info->fbcon_pixmap;
	struct amdgpu_buffer *bo;
	drmModeFBPtr fbcon;
	struct drm_gem_flink flink;
	struct amdgpu_bo_import_result import = {0};

	if (pixmap)
		return pixmap;

	fbcon = drmModeGetFB(pAMDGPUEnt->fd, fbcon_id);
	if (fbcon == NULL)
		return NULL;

	if (fbcon->depth != pScrn->depth ||
	    fbcon->width != pScrn->virtualX ||
	    fbcon->height != pScrn->virtualY)
		goto out_free_fb;

	flink.handle = fbcon->handle;
	if (ioctl(pAMDGPUEnt->fd, DRM_IOCTL_GEM_FLINK, &flink) < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Couldn't flink fbcon handle\n");
		goto out_free_fb;
	}

	bo = calloc(1, sizeof(struct amdgpu_buffer));
	if (bo == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Couldn't allocate bo for fbcon handle\n");
		goto out_free_fb;
	}
	bo->ref_count = 1;

	if (amdgpu_bo_import(pAMDGPUEnt->pDev,
			     amdgpu_bo_handle_type_gem_flink_name, flink.name,
			     &import) != 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Couldn't import BO for fbcon handle\n");
		goto out_free_bo;
	}
	bo->bo.amdgpu = import.buf_handle;

	pixmap = drmmode_create_bo_pixmap(pScrn, fbcon->width, fbcon->height,
					  fbcon->depth, fbcon->bpp,
					  fbcon->pitch, bo);
	info->fbcon_pixmap = pixmap;
out_free_bo:
	amdgpu_bo_unref(&bo);
out_free_fb:
	drmModeFreeFB(fbcon);
	return pixmap;
}

void drmmode_copy_fb(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
	xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	ScreenPtr pScreen = pScrn->pScreen;
	PixmapPtr src, dst = pScreen->GetScreenPixmap(pScreen);
	struct drmmode_fb *fb = amdgpu_pixmap_get_fb(dst);
	int fbcon_id = 0;
	GCPtr gc;
	int i;

	for (i = 0; i < xf86_config->num_crtc; i++) {
		drmmode_crtc_private_ptr drmmode_crtc = xf86_config->crtc[i]->driver_private;

		if (drmmode_crtc->mode_crtc->buffer_id)
			fbcon_id = drmmode_crtc->mode_crtc->buffer_id;
	}

	if (!fbcon_id)
		return;

	if (fbcon_id == fb->handle) {
		/* in some rare case there might be no fbcon and we might already
		 * be the one with the current fb to avoid a false deadlck in
		 * kernel ttm code just do nothing as anyway there is nothing
		 * to do
		 */
		return;
	}

	src = create_pixmap_for_fbcon(drmmode, pScrn, fbcon_id);
	if (!src)
		return;

	gc = GetScratchGC(pScrn->depth, pScreen);
	ValidateGC(&dst->drawable, gc);

	(*gc->ops->CopyArea)(&src->drawable, &dst->drawable, gc, 0, 0,
			     pScrn->virtualX, pScrn->virtualY, 0, 0);

	FreeScratchGC(gc);

	pScreen->canDoBGNoneRoot = TRUE;

	if (info->fbcon_pixmap)
		pScrn->pScreen->DestroyPixmap(info->fbcon_pixmap);
	info->fbcon_pixmap = NULL;

	return;
}

void
drmmode_crtc_scanout_destroy(drmmode_ptr drmmode,
			     struct drmmode_scanout *scanout)
{

	if (scanout->pixmap) {
		drmmode_destroy_bo_pixmap(scanout->pixmap);
		scanout->pixmap = NULL;
	}

	if (scanout->bo) {
		amdgpu_bo_unref(&scanout->bo);
		scanout->bo = NULL;
	}
}

static void
drmmode_crtc_scanout_free(drmmode_crtc_private_ptr drmmode_crtc)
{
	drmmode_crtc_scanout_destroy(drmmode_crtc->drmmode,
				     &drmmode_crtc->scanout[0]);
	drmmode_crtc_scanout_destroy(drmmode_crtc->drmmode,
				     &drmmode_crtc->scanout[1]);

	if (drmmode_crtc->scanout_damage)
		DamageDestroy(drmmode_crtc->scanout_damage);
}

void
drmmode_scanout_free(ScrnInfoPtr scrn)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
	int c;

	for (c = 0; c < xf86_config->num_crtc; c++)
		drmmode_crtc_scanout_free(xf86_config->crtc[c]->driver_private);
}

static PixmapPtr
drmmode_crtc_scanout_create(xf86CrtcPtr crtc, struct drmmode_scanout *scanout,
			    int width, int height)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	int pitch;

	if (scanout->pixmap) {
		if (scanout->width == width && scanout->height == height)
			return scanout->pixmap;

		drmmode_crtc_scanout_destroy(drmmode, scanout);
	}

	scanout->bo = amdgpu_alloc_pixmap_bo(pScrn, width, height,
					     pScrn->depth, 0,
					     pScrn->bitsPerPixel, &pitch);
	if (!scanout->bo) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to allocate scanout buffer memory\n");
		return NULL;
	}

	scanout->pixmap = drmmode_create_bo_pixmap(pScrn,
						 width, height,
						 pScrn->depth,
						 pScrn->bitsPerPixel,
						 pitch, scanout->bo);
	if (!scanout->pixmap) {
		ErrorF("failed to create CRTC scanout pixmap\n");
		goto error;
	}

	if (amdgpu_pixmap_get_fb(scanout->pixmap)) {
		scanout->width = width;
		scanout->height = height;
	} else {
		ErrorF("failed to create CRTC scanout FB\n");
error:		
		drmmode_crtc_scanout_destroy(drmmode, scanout);
	}

	return scanout->pixmap;
}

static void
amdgpu_screen_damage_report(DamagePtr damage, RegionPtr region, void *closure)
{
	drmmode_crtc_private_ptr drmmode_crtc = closure;

	if (drmmode_crtc->ignore_damage) {
		RegionEmpty(&damage->damage);
		drmmode_crtc->ignore_damage = FALSE;
		return;
	}

	/* Only keep track of the extents */
	RegionUninit(&damage->damage);
	damage->damage.data = NULL;
}

static void
drmmode_screen_damage_destroy(DamagePtr damage, void *closure)
{
	drmmode_crtc_private_ptr drmmode_crtc = closure;

	drmmode_crtc->scanout_damage = NULL;
	RegionUninit(&drmmode_crtc->scanout_last_region);
}

static Bool
drmmode_can_use_hw_cursor(xf86CrtcPtr crtc)
{
	AMDGPUInfoPtr info = AMDGPUPTR(crtc->scrn);

	/* Check for Option "SWcursor" */
	if (xf86ReturnOptValBool(info->Options, OPTION_SW_CURSOR, FALSE))
		return FALSE;

	/* Fall back to SW cursor if the CRTC is transformed */
	if (crtc->transformPresent)
		return FALSE;

#if XF86_CRTC_VERSION >= 4 && XF86_CRTC_VERSION < 7
	/* Xorg doesn't correctly handle cursor position transform in the
	 * rotation case
	 */
	if (crtc->driverIsPerformingTransform &&
	    (crtc->rotation & 0xf) != RR_Rotate_0)
		return FALSE;
#endif

#if defined(AMDGPU_PIXMAP_SHARING)
	/* HW cursor not supported with RandR 1.4 multihead up to 1.18.99.901 */
	if (xorgGetVersion() <= XORG_VERSION_NUMERIC(1,18,99,901,0) &&
	    !xorg_list_is_empty(&crtc->scrn->pScreen->pixmap_dirty_list))
		return FALSE;
#endif

	return TRUE;
}

static void
drmmode_crtc_update_tear_free(xf86CrtcPtr crtc)
{
	AMDGPUInfoPtr info = AMDGPUPTR(crtc->scrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	int i;

	drmmode_crtc->tear_free = FALSE;

	for (i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];
		drmmode_output_private_ptr drmmode_output = output->driver_private;

		if (output->crtc != crtc)
			continue;

		if (drmmode_output->tear_free == 1 ||
		    (drmmode_output->tear_free == 2 &&
		     (amdgpu_is_gpu_screen(crtc->scrn->pScreen) ||
		      info->shadow_primary ||
		      crtc->transformPresent || crtc->rotation != RR_Rotate_0))) {
			drmmode_crtc->tear_free = TRUE;
			return;
		}
	}
}

#if XF86_CRTC_VERSION >= 4

#if XF86_CRTC_VERSION < 7
#define XF86DriverTransformOutput TRUE
#define XF86DriverTransformNone FALSE
#endif

static Bool
drmmode_handle_transform(xf86CrtcPtr crtc)
{
	Bool ret;

#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,15,99,903,0)
	if (crtc->transformPresent || crtc->rotation != RR_Rotate_0)
	    crtc->driverIsPerformingTransform = XF86DriverTransformOutput;
	else
	    crtc->driverIsPerformingTransform = XF86DriverTransformNone;
#else
	crtc->driverIsPerformingTransform = !crtc->transformPresent &&
		crtc->rotation != RR_Rotate_0 &&
		(crtc->rotation & 0xf) == RR_Rotate_0;
#endif

	ret = xf86CrtcRotate(crtc);

	crtc->driverIsPerformingTransform &= ret && crtc->transform_in_use;

	return ret;
}

#else

static Bool
drmmode_handle_transform(xf86CrtcPtr crtc)
{
	return xf86CrtcRotate(crtc);
}

#endif

#ifdef AMDGPU_PIXMAP_SHARING

static void
drmmode_crtc_prime_scanout_update(xf86CrtcPtr crtc, DisplayModePtr mode,
				  unsigned scanout_id, struct drmmode_fb **fb,
				  int *x, int *y)
{
	ScrnInfoPtr scrn = crtc->scrn;
	ScreenPtr screen = scrn->pScreen;
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

	if (drmmode_crtc->tear_free &&
	    !drmmode_crtc->scanout[1].pixmap) {
		RegionPtr region;
		BoxPtr box;

		drmmode_crtc_scanout_create(crtc, &drmmode_crtc->scanout[1],
					    mode->HDisplay,
					    mode->VDisplay);
		region = &drmmode_crtc->scanout_last_region;
		RegionUninit(region);
		region->data = NULL;
		box = RegionExtents(region);
		box->x1 = crtc->x;
		box->y1 = crtc->y;
		box->x2 = crtc->x + mode->HDisplay;
		box->y2 = crtc->y + mode->VDisplay;
	}

	if (scanout_id != drmmode_crtc->scanout_id) {
		PixmapDirtyUpdatePtr dirty = NULL;

		xorg_list_for_each_entry(dirty, &screen->pixmap_dirty_list,
					 ent) {
			if (amdgpu_dirty_src_equals(dirty, drmmode_crtc->prime_scanout_pixmap)) {
				dirty->slave_dst =
					drmmode_crtc->scanout[scanout_id].pixmap;
				break;
			}
		}

		if (!drmmode_crtc->tear_free) {
			GCPtr gc = GetScratchGC(scrn->depth, screen);

			ValidateGC(&drmmode_crtc->scanout[0].pixmap->drawable, gc);
			gc->ops->CopyArea(&drmmode_crtc->scanout[1].pixmap->drawable,
					  &drmmode_crtc->scanout[0].pixmap->drawable,
					  gc, 0, 0, mode->HDisplay, mode->VDisplay,
					  0, 0);
			FreeScratchGC(gc);
			amdgpu_glamor_finish(scrn);
		}
	}

	*fb = amdgpu_pixmap_get_fb(drmmode_crtc->scanout[scanout_id].pixmap);
	*x = *y = 0;
	drmmode_crtc->scanout_id = scanout_id;
}
	
#endif /* AMDGPU_PIXMAP_SHARING */

static void
drmmode_crtc_scanout_update(xf86CrtcPtr crtc, DisplayModePtr mode,
			    unsigned scanout_id, struct drmmode_fb **fb, int *x,
			    int *y)
{
	ScrnInfoPtr scrn = crtc->scrn;
	ScreenPtr screen = scrn->pScreen;
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

	drmmode_crtc_scanout_create(crtc, &drmmode_crtc->scanout[scanout_id],
				    mode->HDisplay, mode->VDisplay);
	if (drmmode_crtc->tear_free) {
		drmmode_crtc_scanout_create(crtc,
					    &drmmode_crtc->scanout[scanout_id ^ 1],
					    mode->HDisplay, mode->VDisplay);
	}

	if (drmmode_crtc->scanout[scanout_id].pixmap &&
	    (!drmmode_crtc->tear_free ||
	     drmmode_crtc->scanout[scanout_id ^ 1].pixmap)) {
		RegionPtr region;
		BoxPtr box;

		if (!drmmode_crtc->scanout_damage) {
			drmmode_crtc->scanout_damage =
				DamageCreate(amdgpu_screen_damage_report,
					     drmmode_screen_damage_destroy,
					     DamageReportRawRegion,
					     TRUE, screen, drmmode_crtc);
			DamageRegister(&screen->root->drawable,
				       drmmode_crtc->scanout_damage);
		}

		region = DamageRegion(drmmode_crtc->scanout_damage);
		RegionUninit(region);
		region->data = NULL;
		box = RegionExtents(region);
		box->x1 = 0;
		box->y1 = 0;
		box->x2 = max(box->x2, scrn->virtualX);
		box->y2 = max(box->y2, scrn->virtualY);

		*fb = amdgpu_pixmap_get_fb(drmmode_crtc->scanout[scanout_id].pixmap);
		*x = *y = 0;

		amdgpu_scanout_do_update(crtc, scanout_id,
					 screen->GetWindowPixmap(screen->root),
					 box);
		amdgpu_glamor_finish(scrn);
	}
}

static void
drmmode_crtc_gamma_do_set(xf86CrtcPtr crtc, uint16_t *red, uint16_t *green,
			  uint16_t *blue, int size)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(crtc->scrn);

	drmModeCrtcSetGamma(pAMDGPUEnt->fd, drmmode_crtc->mode_crtc->crtc_id,
			    size, red, green, blue);
}

static Bool
drmmode_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
		       Rotation rotation, int x, int y)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	ScreenPtr pScreen = pScrn->pScreen;
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	unsigned scanout_id = 0;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	int saved_x, saved_y;
	Rotation saved_rotation;
	DisplayModeRec saved_mode;
	uint32_t *output_ids = NULL;
	int output_count = 0;
	Bool ret = FALSE;
	int i;
	struct drmmode_fb *fb = NULL;
	drmModeModeInfo kmode;

	/* The root window contents may be undefined before the WindowExposures
	 * hook is called for it, so bail if we get here before that
	 */
	if (pScreen->WindowExposures == AMDGPUWindowExposures_oneshot)
		return FALSE;

	saved_mode = crtc->mode;
	saved_x = crtc->x;
	saved_y = crtc->y;
	saved_rotation = crtc->rotation;

	if (mode) {
		crtc->mode = *mode;
		crtc->x = x;
		crtc->y = y;
		crtc->rotation = rotation;

		output_ids = calloc(sizeof(uint32_t), xf86_config->num_output);
		if (!output_ids)
			goto done;

		for (i = 0; i < xf86_config->num_output; i++) {
			xf86OutputPtr output = xf86_config->output[i];
			drmmode_output_private_ptr drmmode_output;

			if (output->crtc != crtc)
				continue;

			drmmode_output = output->driver_private;
			output_ids[output_count] =
			    drmmode_output->mode_output->connector_id;
			output_count++;
		}

		if (!drmmode_handle_transform(crtc))
			goto done;

		drmmode_crtc_update_tear_free(crtc);
		if (drmmode_crtc->tear_free)
			scanout_id = drmmode_crtc->scanout_id;

		drmmode_crtc_gamma_do_set(crtc, crtc->gamma_red, crtc->gamma_green,
					  crtc->gamma_blue, crtc->gamma_size);

		drmmode_ConvertToKMode(crtc->scrn, &kmode, mode);

#ifdef AMDGPU_PIXMAP_SHARING
		if (drmmode_crtc->prime_scanout_pixmap) {
			drmmode_crtc_prime_scanout_update(crtc, mode, scanout_id,
							  &fb, &x, &y);
		} else
#endif
		if (drmmode_crtc->rotate.pixmap) {
			fb = amdgpu_pixmap_get_fb(drmmode_crtc->rotate.pixmap);
			x = y = 0;

		} else if (!amdgpu_is_gpu_screen(pScreen) &&
			   (drmmode_crtc->tear_free ||
#if XF86_CRTC_VERSION >= 4
			    crtc->driverIsPerformingTransform ||
#endif
			    info->shadow_primary)) {
			drmmode_crtc_scanout_update(crtc, mode, scanout_id,
						    &fb, &x, &y);
		}

		if (!fb)
			fb = amdgpu_pixmap_get_fb(pScreen->GetWindowPixmap(pScreen->root));
		if (!fb) {
			union gbm_bo_handle bo_handle;

			bo_handle = gbm_bo_get_handle(info->front_buffer->bo.gbm);
			fb = amdgpu_fb_create(pAMDGPUEnt->fd, pScrn->virtualX,
					      pScrn->virtualY, pScrn->depth,
					      pScrn->bitsPerPixel,
					      pScrn->displayWidth * info->pixel_bytes,
					      bo_handle.u32);
			/* Prevent refcnt of ad-hoc FBs from reaching 2 */
			drmmode_fb_reference(pAMDGPUEnt->fd, &drmmode_crtc->fb, NULL);
			drmmode_crtc->fb = fb;
		}
		if (!fb) {
			ErrorF("failed to add FB for modeset\n");
			goto done;
		}

		drmmode_crtc_wait_pending_event(drmmode_crtc, pAMDGPUEnt->fd,
						drmmode_crtc->flip_pending);

		if (drmModeSetCrtc(pAMDGPUEnt->fd,
				   drmmode_crtc->mode_crtc->crtc_id,
				   fb->handle, x, y, output_ids,
				   output_count, &kmode) != 0) {
			xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
				   "failed to set mode: %s\n", strerror(errno));
			goto done;
		} else {
			ret = TRUE;
			drmmode_fb_reference(pAMDGPUEnt->fd, &drmmode_crtc->fb, fb);
		}

		if (pScreen)
			xf86CrtcSetScreenSubpixelOrder(pScreen);

		drmmode_crtc->need_modeset = FALSE;

		/* go through all the outputs and force DPMS them back on? */
		for (i = 0; i < xf86_config->num_output; i++) {
			xf86OutputPtr output = xf86_config->output[i];

			if (output->crtc != crtc)
				continue;

			output->funcs->dpms(output, DPMSModeOn);
		}
	}

	/* Compute index of this CRTC into xf86_config->crtc */
	for (i = 0; i < xf86_config->num_crtc; i++) {
		if (xf86_config->crtc[i] != crtc)
			continue;

		if (!crtc->enabled || drmmode_can_use_hw_cursor(crtc))
			info->hwcursor_disabled &= ~(1 << i);
		else
			info->hwcursor_disabled |= 1 << i;

		break;
	}

#ifndef HAVE_XF86_CURSOR_RESET_CURSOR
	if (!info->hwcursor_disabled)
		xf86_reload_cursors(pScreen);
#endif

done:
	free(output_ids);
	if (!ret) {
		crtc->x = saved_x;
		crtc->y = saved_y;
		crtc->rotation = saved_rotation;
		crtc->mode = saved_mode;
	} else {
		crtc->active = TRUE;

		if (drmmode_crtc->scanout[scanout_id].pixmap &&
		    fb != amdgpu_pixmap_get_fb(drmmode_crtc->
					       scanout[scanout_id].pixmap))
			drmmode_crtc_scanout_free(drmmode_crtc);
		else if (!drmmode_crtc->tear_free) {
			drmmode_crtc_scanout_destroy(drmmode,
						     &drmmode_crtc->scanout[1]);
		}
	}

	return ret;
}

static void drmmode_set_cursor_colors(xf86CrtcPtr crtc, int bg, int fg)
{

}

static void drmmode_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(crtc->scrn);

#if XF86_CRTC_VERSION >= 4 && XF86_CRTC_VERSION < 7
	if (crtc->driverIsPerformingTransform) {
		x += crtc->x;
		y += crtc->y;
		xf86CrtcTransformCursorPos(crtc, &x, &y);
	}
#endif

	drmModeMoveCursor(pAMDGPUEnt->fd, drmmode_crtc->mode_crtc->crtc_id, x, y);
}

#if XF86_CRTC_VERSION >= 4 && XF86_CRTC_VERSION < 7

static int
drmmode_cursor_src_offset(Rotation rotation, int width, int height,
			  int x_dst, int y_dst)
{
	int t;

	switch (rotation & 0xf) {
	case RR_Rotate_90:
		t = x_dst;
		x_dst = height - y_dst - 1;
		y_dst = t;
		break;
	case RR_Rotate_180:
		x_dst = width - x_dst - 1;
		y_dst = height - y_dst - 1;
		break;
	case RR_Rotate_270:
		t = x_dst;
		x_dst = y_dst;
		y_dst = width - t - 1;
		break;
	}

	if (rotation & RR_Reflect_X)
		x_dst = width - x_dst - 1;
	if (rotation & RR_Reflect_Y)
		y_dst = height - y_dst - 1;

	return y_dst * height + x_dst;
}

#endif

static uint32_t
drmmode_cursor_gamma(xf86CrtcPtr crtc, uint32_t argb)
{
	uint32_t alpha = argb >> 24;
	uint32_t rgb[3];
	int i;

	if (!alpha)
		return 0;

	if (crtc->scrn->depth != 24 && crtc->scrn->depth != 32)
		return argb;

	/* Un-premultiply alpha */
	for (i = 0; i < 3; i++)
		rgb[i] = ((argb >> (i * 8)) & 0xff) * 0xff / alpha;

	/* Apply gamma correction and pre-multiply alpha */
	rgb[0] = (crtc->gamma_blue[rgb[0]] >> 8) * alpha / 0xff;
	rgb[1] = (crtc->gamma_green[rgb[1]] >> 8) * alpha / 0xff;
	rgb[2] = (crtc->gamma_red[rgb[2]] >> 8) * alpha / 0xff;

	return alpha << 24 | rgb[2] << 16 | rgb[1] << 8 | rgb[0];
}

static void drmmode_do_load_cursor_argb(xf86CrtcPtr crtc, CARD32 *image, uint32_t *ptr)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

#if XF86_CRTC_VERSION >= 4 && XF86_CRTC_VERSION < 7
	if (crtc->driverIsPerformingTransform) {
		uint32_t cursor_w = info->cursor_w, cursor_h = info->cursor_h;
		int dstx, dsty;
		int srcoffset;

		for (dsty = 0; dsty < cursor_h; dsty++) {
			for (dstx = 0; dstx < cursor_w; dstx++) {
				srcoffset = drmmode_cursor_src_offset(crtc->rotation,
								      cursor_w,
								      cursor_h,
								      dstx, dsty);

				ptr[dsty * info->cursor_w + dstx] =
					cpu_to_le32(drmmode_cursor_gamma(crtc,
									 image[srcoffset]));
			}
		}
	} else
#endif
	{
		uint32_t cursor_size = info->cursor_w * info->cursor_h;
		int i;

		for (i = 0; i < cursor_size; i++)
			ptr[i] = cpu_to_le32(drmmode_cursor_gamma(crtc, image[i]));
	}
}

static void drmmode_load_cursor_argb(xf86CrtcPtr crtc, CARD32 * image)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	uint32_t cursor_size = info->cursor_w * info->cursor_h;

	if (info->gbm) {
		uint32_t ptr[cursor_size];

		drmmode_do_load_cursor_argb(crtc, image, ptr);
		gbm_bo_write(drmmode_crtc->cursor_buffer->bo.gbm, ptr, cursor_size * 4);
	} else {
		/* cursor should be mapped already */
		uint32_t *ptr = (uint32_t *) (drmmode_crtc->cursor_buffer->cpu_ptr);

		drmmode_do_load_cursor_argb(crtc, image, ptr);
	}
}

#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,15,99,903,0)

static Bool drmmode_load_cursor_argb_check(xf86CrtcPtr crtc, CARD32 * image)
{
	if (!drmmode_can_use_hw_cursor(crtc))
		return FALSE;

	drmmode_load_cursor_argb(crtc, image);
	return TRUE;
}

#endif

static void drmmode_hide_cursor(xf86CrtcPtr crtc)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

	drmModeSetCursor(pAMDGPUEnt->fd, drmmode_crtc->mode_crtc->crtc_id, 0,
			 info->cursor_w, info->cursor_h);

}

static void drmmode_show_cursor(xf86CrtcPtr crtc)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	uint32_t bo_handle;
	static Bool use_set_cursor2 = TRUE;

	if (!amdgpu_bo_get_handle(drmmode_crtc->cursor_buffer, &bo_handle)) {
		ErrorF("failed to get BO handle for cursor\n");
		return;
	}

	if (use_set_cursor2) {
		xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
		CursorPtr cursor = xf86_config->cursor;
		int xhot = cursor->bits->xhot;
		int yhot = cursor->bits->yhot;
		int ret;

		if (crtc->rotation != RR_Rotate_0 &&
		    crtc->rotation != (RR_Rotate_180 | RR_Reflect_X |
				       RR_Reflect_Y)) {
			int t;

			/* Reflect & rotate hotspot position */
			if (crtc->rotation & RR_Reflect_X)
				xhot = info->cursor_w - xhot - 1;
			if (crtc->rotation & RR_Reflect_Y)
				yhot = info->cursor_h - yhot - 1;

			switch (crtc->rotation & 0xf) {
			case RR_Rotate_90:
				t = xhot;
				xhot = yhot;
				yhot = info->cursor_w - t - 1;
				break;
			case RR_Rotate_180:
				xhot = info->cursor_w - xhot - 1;
				yhot = info->cursor_h - yhot - 1;
				break;
			case RR_Rotate_270:
				t = xhot;
				xhot = info->cursor_h - yhot - 1;
				yhot = t;
			}
		}

		ret = drmModeSetCursor2(pAMDGPUEnt->fd,
					drmmode_crtc->mode_crtc->crtc_id,
					bo_handle,
					info->cursor_w, info->cursor_h,
					xhot, yhot);
		if (ret == -EINVAL)
			use_set_cursor2 = FALSE;
		else
			return;
	}

	drmModeSetCursor(pAMDGPUEnt->fd, drmmode_crtc->mode_crtc->crtc_id, bo_handle,
			 info->cursor_w, info->cursor_h);
}

/* Xorg expects a non-NULL return value from drmmode_crtc_shadow_allocate, and
 * passes that back to drmmode_crtc_scanout_create; it doesn't use it for
 * anything else.
 */
static void *
drmmode_crtc_shadow_allocate(xf86CrtcPtr crtc, int width, int height)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

	if (!drmmode_crtc_scanout_create(crtc, &drmmode_crtc->rotate, width,
					 height))
		return NULL;

	return (void*)~0UL;
}

static PixmapPtr
drmmode_crtc_shadow_create(xf86CrtcPtr crtc, void *data, int width, int height)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

	if (!data) {
		drmmode_crtc_scanout_create(crtc, &drmmode_crtc->rotate, width,
					    height);
	}

	return drmmode_crtc->rotate.pixmap;
}

static void
drmmode_crtc_shadow_destroy(xf86CrtcPtr crtc, PixmapPtr rotate_pixmap,
			    void *data)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;

	drmmode_crtc_scanout_destroy(drmmode, &drmmode_crtc->rotate);
}

static void
drmmode_crtc_gamma_set(xf86CrtcPtr crtc, uint16_t * red, uint16_t * green,
		       uint16_t * blue, int size)
{
	ScrnInfoPtr scrn = crtc->scrn;
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);
	int i;

	drmmode_crtc_gamma_do_set(crtc, red, green, blue, size);

	/* Compute index of this CRTC into xf86_config->crtc */
	for (i = 0; xf86_config->crtc[i] != crtc; i++) {}

	if (info->hwcursor_disabled & (1 << i))
		return;

#ifdef HAVE_XF86_CURSOR_RESET_CURSOR
	xf86CursorResetCursor(scrn->pScreen);
#else
	xf86_reload_cursors(scrn->pScreen);
#endif
}

#ifdef AMDGPU_PIXMAP_SHARING
static Bool drmmode_set_scanout_pixmap(xf86CrtcPtr crtc, PixmapPtr ppix)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	unsigned scanout_id = drmmode_crtc->scanout_id;
	ScreenPtr screen = crtc->scrn->pScreen;
	PixmapDirtyUpdatePtr dirty;

	xorg_list_for_each_entry(dirty, &screen->pixmap_dirty_list, ent) {
		if (amdgpu_dirty_src_equals(dirty, drmmode_crtc->prime_scanout_pixmap)) {
			PixmapStopDirtyTracking(dirty->src, dirty->slave_dst);
			break;
		}
	}

	drmmode_crtc_scanout_free(drmmode_crtc);
	drmmode_crtc->prime_scanout_pixmap = NULL;

	if (!ppix)
		return TRUE;

	if (!drmmode_crtc_scanout_create(crtc, &drmmode_crtc->scanout[0],
					 ppix->drawable.width,
					 ppix->drawable.height))
		return FALSE;

	if (drmmode_crtc->tear_free &&
	    !drmmode_crtc_scanout_create(crtc, &drmmode_crtc->scanout[1],
					 ppix->drawable.width,
					 ppix->drawable.height)) {
		drmmode_crtc_scanout_free(drmmode_crtc);
		return FALSE;
	}

	drmmode_crtc->prime_scanout_pixmap = ppix;

#ifdef HAS_DIRTYTRACKING_DRAWABLE_SRC
	PixmapStartDirtyTracking(&ppix->drawable,
				 drmmode_crtc->scanout[scanout_id].pixmap,
				 0, 0, 0, 0, RR_Rotate_0);
#elif defined(HAS_DIRTYTRACKING_ROTATION)
	PixmapStartDirtyTracking(ppix, drmmode_crtc->scanout[scanout_id].pixmap,
				 0, 0, 0, 0, RR_Rotate_0);
#elif defined(HAS_DIRTYTRACKING2)
	PixmapStartDirtyTracking2(ppix, drmmode_crtc->scanout[scanout_id].pixmap,
				  0, 0, 0, 0);
#else
	PixmapStartDirtyTracking(ppix, drmmode_crtc->scanout[scanout_id].pixmap, 0, 0);
#endif
	return TRUE;
}
#endif

static xf86CrtcFuncsRec drmmode_crtc_funcs = {
	.dpms = drmmode_crtc_dpms,
	.set_mode_major = drmmode_set_mode_major,
	.set_cursor_colors = drmmode_set_cursor_colors,
	.set_cursor_position = drmmode_set_cursor_position,
	.show_cursor = drmmode_show_cursor,
	.hide_cursor = drmmode_hide_cursor,
	.load_cursor_argb = drmmode_load_cursor_argb,
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,15,99,903,0)
	.load_cursor_argb_check = drmmode_load_cursor_argb_check,
#endif

	.gamma_set = drmmode_crtc_gamma_set,
	.shadow_create = drmmode_crtc_shadow_create,
	.shadow_allocate = drmmode_crtc_shadow_allocate,
	.shadow_destroy = drmmode_crtc_shadow_destroy,
	.destroy = NULL,	/* XXX */
#ifdef AMDGPU_PIXMAP_SHARING
	.set_scanout_pixmap = drmmode_set_scanout_pixmap,
#endif
};

int drmmode_get_crtc_id(xf86CrtcPtr crtc)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	return drmmode_crtc->hw_id;
}

void drmmode_crtc_hw_id(xf86CrtcPtr crtc)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	ScrnInfoPtr pScrn = crtc->scrn;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	int r;

	r = amdgpu_query_crtc_from_id(pAMDGPUEnt->pDev,
				      drmmode_crtc->mode_crtc->crtc_id,
				      &drmmode_crtc->hw_id);
	if (r)
		drmmode_crtc->hw_id = -1;
}

static unsigned int
drmmode_crtc_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode, drmModeResPtr mode_res, int num)
{
	xf86CrtcPtr crtc;
	drmmode_crtc_private_ptr drmmode_crtc;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);

	crtc = xf86CrtcCreate(pScrn, &drmmode_crtc_funcs);
	if (crtc == NULL)
		return 0;

	drmmode_crtc = xnfcalloc(sizeof(drmmode_crtc_private_rec), 1);
	drmmode_crtc->mode_crtc =
	    drmModeGetCrtc(pAMDGPUEnt->fd, mode_res->crtcs[num]);
	drmmode_crtc->drmmode = drmmode;
	drmmode_crtc->dpms_mode = DPMSModeOff;
	crtc->driver_private = drmmode_crtc;
	drmmode_crtc_hw_id(crtc);

	/* Mark num'th crtc as in use on this device. */
	pAMDGPUEnt->assigned_crtcs |= (1 << num);
	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "Allocated crtc nr. %d to this screen.\n", num);

	return 1;
}

static xf86OutputStatus drmmode_output_detect(xf86OutputPtr output)
{
	/* go to the hw and retrieve a new output struct */
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(output->scrn);
	xf86OutputStatus status;
	drmModeFreeConnector(drmmode_output->mode_output);

	drmmode_output->mode_output =
	    drmModeGetConnector(pAMDGPUEnt->fd, drmmode_output->output_id);
	if (!drmmode_output->mode_output)
		return XF86OutputStatusDisconnected;

	switch (drmmode_output->mode_output->connection) {
	case DRM_MODE_CONNECTED:
		status = XF86OutputStatusConnected;
		break;
	case DRM_MODE_DISCONNECTED:
		status = XF86OutputStatusDisconnected;
		break;
	default:
	case DRM_MODE_UNKNOWNCONNECTION:
		status = XF86OutputStatusUnknown;
		break;
	}
	return status;
}

static Bool
drmmode_output_mode_valid(xf86OutputPtr output, DisplayModePtr pModes)
{
	return MODE_OK;
}

static DisplayModePtr drmmode_output_get_modes(xf86OutputPtr output)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmModeConnectorPtr koutput = drmmode_output->mode_output;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(output->scrn);
	int i;
	DisplayModePtr Modes = NULL, Mode;
	drmModePropertyPtr props;
	xf86MonPtr mon = NULL;

	if (!koutput)
		return NULL;

	/* look for an EDID property */
	for (i = 0; i < koutput->count_props; i++) {
		props = drmModeGetProperty(pAMDGPUEnt->fd, koutput->props[i]);
		if (props && (props->flags & DRM_MODE_PROP_BLOB)) {
			if (!strcmp(props->name, "EDID")) {
				if (drmmode_output->edid_blob)
					drmModeFreePropertyBlob
					    (drmmode_output->edid_blob);
				drmmode_output->edid_blob =
				    drmModeGetPropertyBlob(pAMDGPUEnt->fd,
							   koutput->prop_values
							   [i]);
			}
		}
		if (props)
			drmModeFreeProperty(props);
	}

	if (drmmode_output->edid_blob) {
		mon = xf86InterpretEDID(output->scrn->scrnIndex,
					drmmode_output->edid_blob->data);
		if (mon && drmmode_output->edid_blob->length > 128)
			mon->flags |= MONITOR_EDID_COMPLETE_RAWDATA;
	}
	xf86OutputSetEDID(output, mon);

	/* modes should already be available */
	for (i = 0; i < koutput->count_modes; i++) {
		Mode = xnfalloc(sizeof(DisplayModeRec));

		drmmode_ConvertFromKMode(output->scrn, &koutput->modes[i],
					 Mode);
		Modes = xf86ModesAdd(Modes, Mode);

	}
	return Modes;
}

static void drmmode_output_destroy(xf86OutputPtr output)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	int i;

	if (drmmode_output->edid_blob)
		drmModeFreePropertyBlob(drmmode_output->edid_blob);
	for (i = 0; i < drmmode_output->num_props; i++) {
		drmModeFreeProperty(drmmode_output->props[i].mode_prop);
		free(drmmode_output->props[i].atoms);
	}
	for (i = 0; i < drmmode_output->mode_output->count_encoders; i++) {
		drmModeFreeEncoder(drmmode_output->mode_encoders[i]);
	}
	free(drmmode_output->mode_encoders);
	free(drmmode_output->props);
	drmModeFreeConnector(drmmode_output->mode_output);
	free(drmmode_output);
	output->driver_private = NULL;
}

static void drmmode_output_dpms(xf86OutputPtr output, int mode)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	xf86CrtcPtr crtc = output->crtc;
	drmModeConnectorPtr koutput = drmmode_output->mode_output;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(output->scrn);

	if (!koutput)
		return;

	if (mode != DPMSModeOn && crtc)
		drmmode_do_crtc_dpms(crtc, mode);

	drmModeConnectorSetProperty(pAMDGPUEnt->fd, koutput->connector_id,
				    drmmode_output->dpms_enum_id, mode);

	if (mode == DPMSModeOn && crtc) {
		drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

		if (drmmode_crtc->need_modeset)
			drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation,
					       crtc->x, crtc->y);
		else
			drmmode_do_crtc_dpms(output->crtc, mode);
	}
}

static Bool drmmode_property_ignore(drmModePropertyPtr prop)
{
	if (!prop)
		return TRUE;
	/* ignore blob prop */
	if (prop->flags & DRM_MODE_PROP_BLOB)
		return TRUE;
	/* ignore standard property */
	if (!strcmp(prop->name, "EDID") || !strcmp(prop->name, "DPMS"))
		return TRUE;

	return FALSE;
}

static void drmmode_output_create_resources(xf86OutputPtr output)
{
	AMDGPUInfoPtr info = AMDGPUPTR(output->scrn);
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmModeConnectorPtr mode_output = drmmode_output->mode_output;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(output->scrn);
	drmModePropertyPtr drmmode_prop, tearfree_prop;
	int i, j, err;

	drmmode_output->props =
		calloc(mode_output->count_props + 1, sizeof(drmmode_prop_rec));
	if (!drmmode_output->props)
		return;

	drmmode_output->num_props = 0;
	for (i = 0, j = 0; i < mode_output->count_props; i++) {
		drmmode_prop =
		    drmModeGetProperty(pAMDGPUEnt->fd, mode_output->props[i]);
		if (drmmode_property_ignore(drmmode_prop)) {
			drmModeFreeProperty(drmmode_prop);
			continue;
		}
		drmmode_output->props[j].mode_prop = drmmode_prop;
		drmmode_output->props[j].value = mode_output->prop_values[i];
		drmmode_output->num_props++;
		j++;
	}

	/* Userspace-only property for TearFree */
	tearfree_prop = calloc(1, sizeof(*tearfree_prop));
	tearfree_prop->flags = DRM_MODE_PROP_ENUM;
	strncpy(tearfree_prop->name, "TearFree", 8);
	tearfree_prop->count_enums = 3;
	tearfree_prop->enums = calloc(tearfree_prop->count_enums,
				      sizeof(*tearfree_prop->enums));
	strncpy(tearfree_prop->enums[0].name, "off", 3);
	strncpy(tearfree_prop->enums[1].name, "on", 2);
	tearfree_prop->enums[1].value = 1;
	strncpy(tearfree_prop->enums[2].name, "auto", 4);
	tearfree_prop->enums[2].value = 2;
	drmmode_output->props[j].mode_prop = tearfree_prop;
	drmmode_output->props[j].value = info->tear_free;
	drmmode_output->tear_free = info->tear_free;
	drmmode_output->num_props++;

	for (i = 0; i < drmmode_output->num_props; i++) {
		drmmode_prop_ptr p = &drmmode_output->props[i];
		drmmode_prop = p->mode_prop;

		if (drmmode_prop->flags & DRM_MODE_PROP_RANGE) {
			INT32 range[2];
			INT32 value = p->value;

			p->num_atoms = 1;
			p->atoms = calloc(p->num_atoms, sizeof(Atom));
			if (!p->atoms)
				continue;
			p->atoms[0] =
			    MakeAtom(drmmode_prop->name,
				     strlen(drmmode_prop->name), TRUE);
			range[0] = drmmode_prop->values[0];
			range[1] = drmmode_prop->values[1];
			err =
			    RRConfigureOutputProperty(output->randr_output,
						      p->atoms[0], FALSE, TRUE,
						      drmmode_prop->flags &
						      DRM_MODE_PROP_IMMUTABLE ?
						      TRUE : FALSE, 2, range);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRConfigureOutputProperty error, %d\n",
					   err);
			}
			err =
			    RRChangeOutputProperty(output->randr_output,
						   p->atoms[0], XA_INTEGER, 32,
						   PropModeReplace, 1, &value,
						   FALSE, TRUE);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRChangeOutputProperty error, %d\n",
					   err);
			}
		} else if (drmmode_prop->flags & DRM_MODE_PROP_ENUM) {
			p->num_atoms = drmmode_prop->count_enums + 1;
			p->atoms = calloc(p->num_atoms, sizeof(Atom));
			if (!p->atoms)
				continue;
			p->atoms[0] =
			    MakeAtom(drmmode_prop->name,
				     strlen(drmmode_prop->name), TRUE);
			for (j = 1; j <= drmmode_prop->count_enums; j++) {
				struct drm_mode_property_enum *e =
				    &drmmode_prop->enums[j - 1];
				p->atoms[j] =
				    MakeAtom(e->name, strlen(e->name), TRUE);
			}
			err =
			    RRConfigureOutputProperty(output->randr_output,
						      p->atoms[0], FALSE, FALSE,
						      drmmode_prop->flags &
						      DRM_MODE_PROP_IMMUTABLE ?
						      TRUE : FALSE,
						      p->num_atoms - 1,
						      (INT32 *) & p->atoms[1]);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRConfigureOutputProperty error, %d\n",
					   err);
			}
			for (j = 0; j < drmmode_prop->count_enums; j++)
				if (drmmode_prop->enums[j].value == p->value)
					break;
			/* there's always a matching value */
			err =
			    RRChangeOutputProperty(output->randr_output,
						   p->atoms[0], XA_ATOM, 32,
						   PropModeReplace, 1,
						   &p->atoms[j + 1], FALSE,
						   TRUE);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRChangeOutputProperty error, %d\n",
					   err);
			}
		}
	}
}

static Bool
drmmode_output_set_property(xf86OutputPtr output, Atom property,
			    RRPropertyValuePtr value)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(output->scrn);
	int i;

	for (i = 0; i < drmmode_output->num_props; i++) {
		drmmode_prop_ptr p = &drmmode_output->props[i];

		if (p->atoms[0] != property)
			continue;

		if (p->mode_prop->flags & DRM_MODE_PROP_RANGE) {
			uint32_t val;

			if (value->type != XA_INTEGER || value->format != 32 ||
			    value->size != 1)
				return FALSE;
			val = *(uint32_t *) value->data;

			drmModeConnectorSetProperty(pAMDGPUEnt->fd,
						    drmmode_output->output_id,
						    p->mode_prop->prop_id,
						    (uint64_t) val);
			return TRUE;
		} else if (p->mode_prop->flags & DRM_MODE_PROP_ENUM) {
			Atom atom;
			const char *name;
			int j;

			if (value->type != XA_ATOM || value->format != 32
			    || value->size != 1)
				return FALSE;
			memcpy(&atom, value->data, 4);
			if (!(name = NameForAtom(atom)))
				return FALSE;

			/* search for matching name string, then set its value down */
			for (j = 0; j < p->mode_prop->count_enums; j++) {
				if (!strcmp(p->mode_prop->enums[j].name, name)) {
					if (i == (drmmode_output->num_props - 1)) {
						if (drmmode_output->tear_free != j) {
							xf86CrtcPtr crtc = output->crtc;

							drmmode_output->tear_free = j;
							if (crtc) {
								drmmode_set_mode_major(crtc,
										       &crtc->mode,
										       crtc->rotation,
										       crtc->x,
										       crtc->y);
							}
						}
					} else {
						drmModeConnectorSetProperty(pAMDGPUEnt->fd,
									    drmmode_output->output_id,
									    p->mode_prop->prop_id,
									    p->mode_prop->enums[j].value);
					}

					return TRUE;
				}
			}
		}
	}

	return TRUE;
}

static Bool drmmode_output_get_property(xf86OutputPtr output, Atom property)
{
	return TRUE;
}

static const xf86OutputFuncsRec drmmode_output_funcs = {
	.dpms = drmmode_output_dpms,
	.create_resources = drmmode_output_create_resources,
	.set_property = drmmode_output_set_property,
	.get_property = drmmode_output_get_property,
#if 0

	.save = drmmode_crt_save,
	.restore = drmmode_crt_restore,
	.mode_fixup = drmmode_crt_mode_fixup,
	.prepare = drmmode_output_prepare,
	.mode_set = drmmode_crt_mode_set,
	.commit = drmmode_output_commit,
#endif
	.detect = drmmode_output_detect,
	.mode_valid = drmmode_output_mode_valid,

	.get_modes = drmmode_output_get_modes,
	.destroy = drmmode_output_destroy
};

static int subpixel_conv_table[7] = { 0, SubPixelUnknown,
	SubPixelHorizontalRGB,
	SubPixelHorizontalBGR,
	SubPixelVerticalRGB,
	SubPixelVerticalBGR,
	SubPixelNone
};

const char *output_names[] = { "None",
	"VGA",
	"DVI-I",
	"DVI-D",
	"DVI-A",
	"Composite",
	"S-video",
	"LVDS",
	"CTV",
	"DIN",
	"DisplayPort",
	"HDMI-A",
	"HDMI-B",
	"TV",
	"eDP",
	"Virtual",
	"DSI",
};

#define NUM_OUTPUT_NAMES (sizeof(output_names) / sizeof(output_names[0]))

static xf86OutputPtr find_output(ScrnInfoPtr pScrn, int id)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	int i;
	for (i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];
		drmmode_output_private_ptr drmmode_output;
		drmmode_output = output->driver_private;
		if (drmmode_output->output_id == id)
			return output;
	}
	return NULL;
}

static int parse_path_blob(drmModePropertyBlobPtr path_blob, int *conn_base_id, char **path)
{
	char *conn;
	char conn_id[5];
	int id, len;
	char *blob_data;

	if (!path_blob)
		return -1;

	blob_data = path_blob->data;
	/* we only handle MST paths for now */
	if (strncmp(blob_data, "mst:", 4))
		return -1;

	conn = strchr(blob_data + 4, '-');
	if (!conn)
		return -1;
	len = conn - (blob_data + 4);
	if (len + 1 > 5)
		return -1;
	memcpy(conn_id, blob_data + 4, len);
	conn_id[len] = '\0';
	id = strtoul(conn_id, NULL, 10);

	*conn_base_id = id;

	*path = conn + 1;
	return 0;
}

static void
drmmode_create_name(ScrnInfoPtr pScrn, drmModeConnectorPtr koutput, char *name,
		    drmModePropertyBlobPtr path_blob, int *num_dvi, int *num_hdmi)
{
	xf86OutputPtr output;
	int conn_id;
	char *extra_path;

	output = NULL;
	if (parse_path_blob(path_blob, &conn_id, &extra_path) == 0)
		output = find_output(pScrn, conn_id);
	if (output) {
		snprintf(name, 32, "%s-%s", output->name, extra_path);
	} else {
		if (koutput->connector_type >= NUM_OUTPUT_NAMES)
			snprintf(name, 32, "Unknown%d-%d", koutput->connector_type, koutput->connector_type_id - 1);
#ifdef AMDGPU_PIXMAP_SHARING
		else if (pScrn->is_gpu)
			snprintf(name, 32, "%s-%d-%d", output_names[koutput->connector_type],
				 pScrn->scrnIndex - GPU_SCREEN_OFFSET + 1, koutput->connector_type_id - 1);
#endif
		else {
			/* need to do smart conversion here for compat with non-kms ATI driver */
			if (koutput->connector_type_id == 1) {
				switch(koutput->connector_type) {
				case DRM_MODE_CONNECTOR_DVII:
				case DRM_MODE_CONNECTOR_DVID:
				case DRM_MODE_CONNECTOR_DVIA:
					snprintf(name, 32, "%s-%d", output_names[koutput->connector_type], *num_dvi);
					(*num_dvi)++;
					break;
				case DRM_MODE_CONNECTOR_HDMIA:
				case DRM_MODE_CONNECTOR_HDMIB:
					snprintf(name, 32, "%s-%d", output_names[koutput->connector_type], *num_hdmi);
					(*num_hdmi)++;
					break;
				case DRM_MODE_CONNECTOR_VGA:
				case DRM_MODE_CONNECTOR_DisplayPort:
					snprintf(name, 32, "%s-%d", output_names[koutput->connector_type], koutput->connector_type_id - 1);
					break;
				default:
					snprintf(name, 32, "%s", output_names[koutput->connector_type]);
					break;
				}
			} else {
				snprintf(name, 32, "%s-%d", output_names[koutput->connector_type], koutput->connector_type_id - 1);
			}
		}
	}
}


static unsigned int
drmmode_output_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode, drmModeResPtr mode_res, int num, int *num_dvi, int *num_hdmi, int dynamic)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	xf86OutputPtr output;
	drmModeConnectorPtr koutput;
	drmModeEncoderPtr *kencoders = NULL;
	drmmode_output_private_ptr drmmode_output;
	drmModePropertyPtr props;
	drmModePropertyBlobPtr path_blob = NULL;
	char name[32];
	int i;
	const char *s;

	koutput =
	    drmModeGetConnector(pAMDGPUEnt->fd,
				mode_res->connectors[num]);
	if (!koutput)
		return 0;

	for (i = 0; i < koutput->count_props; i++) {
		props = drmModeGetProperty(pAMDGPUEnt->fd, koutput->props[i]);
		if (props && (props->flags & DRM_MODE_PROP_BLOB)) {
			if (!strcmp(props->name, "PATH")) {
				path_blob = drmModeGetPropertyBlob(pAMDGPUEnt->fd, koutput->prop_values[i]);
				drmModeFreeProperty(props);
				break;
			}
			drmModeFreeProperty(props);
		}
	}

	kencoders = calloc(sizeof(drmModeEncoderPtr), koutput->count_encoders);
	if (!kencoders) {
		goto out_free_encoders;
	}

	for (i = 0; i < koutput->count_encoders; i++) {
		kencoders[i] =
		    drmModeGetEncoder(pAMDGPUEnt->fd, koutput->encoders[i]);
		if (!kencoders[i]) {
			goto out_free_encoders;
		}
	}

	drmmode_create_name(pScrn, koutput, name, path_blob, num_dvi, num_hdmi);
	if (path_blob) {
		drmModeFreePropertyBlob(path_blob);
	}

	if (path_blob && dynamic) {
		/* See if we have an output with this name already
		 * and hook stuff up.
		 */
		for (i = 0; i < xf86_config->num_output; i++) {
			output = xf86_config->output[i];

			if (strncmp(output->name, name, 32))
				continue;

			drmmode_output = output->driver_private;
			drmmode_output->output_id = mode_res->connectors[num];
			drmmode_output->mode_output = koutput;
			for (i = 0; i < koutput->count_encoders; i++) {
				drmModeFreeEncoder(kencoders[i]);
			}
			free(kencoders);
			return 1;
		}
	}

	if (xf86IsEntityShared(pScrn->entityList[0])) {
		if ((s =
		     xf86GetOptValString(info->Options, OPTION_ZAPHOD_HEADS))) {
			if (!AMDGPUZaphodStringMatches(pScrn, s, name))
				goto out_free_encoders;
		} else {
			if (!info->IsSecondary && (num != 0))
				goto out_free_encoders;
			else if (info->IsSecondary && (num != 1))
				goto out_free_encoders;
		}
	}

	output = xf86OutputCreate(pScrn, &drmmode_output_funcs, name);
	if (!output) {
		goto out_free_encoders;
	}

	drmmode_output = calloc(sizeof(drmmode_output_private_rec), 1);
	if (!drmmode_output) {
		xf86OutputDestroy(output);
		goto out_free_encoders;
	}

	drmmode_output->output_id = mode_res->connectors[num];
	drmmode_output->mode_output = koutput;
	drmmode_output->mode_encoders = kencoders;
	drmmode_output->drmmode = drmmode;
	output->mm_width = koutput->mmWidth;
	output->mm_height = koutput->mmHeight;

	output->subpixel_order = subpixel_conv_table[koutput->subpixel];
	output->interlaceAllowed = TRUE;
	output->doubleScanAllowed = TRUE;
	output->driver_private = drmmode_output;

	output->possible_crtcs = 0xffffffff;
	for (i = 0; i < koutput->count_encoders; i++) {
		output->possible_crtcs &= kencoders[i]->possible_crtcs;
	}
	/* work out the possible clones later */
	output->possible_clones = 0;

	for (i = 0; i < koutput->count_props; i++) {
		props = drmModeGetProperty(pAMDGPUEnt->fd, koutput->props[i]);
		if (props && (props->flags & DRM_MODE_PROP_ENUM)) {
			if (!strcmp(props->name, "DPMS")) {
				drmmode_output->dpms_enum_id =
				    koutput->props[i];
				drmModeFreeProperty(props);
				break;
			}
			drmModeFreeProperty(props);
		}
	}

	if (dynamic) {
		output->randr_output = RROutputCreate(xf86ScrnToScreen(pScrn), output->name, strlen(output->name), output);
		drmmode_output_create_resources(output);
	}

	return 1;
out_free_encoders:
	if (kencoders) {
		for (i = 0; i < koutput->count_encoders; i++)
			drmModeFreeEncoder(kencoders[i]);
		free(kencoders);
	}
	drmModeFreeConnector(koutput);
	return 0;
}

uint32_t find_clones(ScrnInfoPtr scrn, xf86OutputPtr output)
{
	drmmode_output_private_ptr drmmode_output =
	    output->driver_private, clone_drmout;
	int i;
	xf86OutputPtr clone_output;
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
	int index_mask = 0;

	if (drmmode_output->enc_clone_mask == 0)
		return index_mask;

	for (i = 0; i < xf86_config->num_output; i++) {
		clone_output = xf86_config->output[i];
		clone_drmout = clone_output->driver_private;
		if (output == clone_output)
			continue;

		if (clone_drmout->enc_mask == 0)
			continue;
		if (drmmode_output->enc_clone_mask == clone_drmout->enc_mask)
			index_mask |= (1 << i);
	}
	return index_mask;
}

static void drmmode_clones_init(ScrnInfoPtr scrn, drmmode_ptr drmmode, drmModeResPtr mode_res)
{
	int i, j;
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);

	for (i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];
		drmmode_output_private_ptr drmmode_output;

		drmmode_output = output->driver_private;
		drmmode_output->enc_clone_mask = 0xff;
		/* and all the possible encoder clones for this output together */
		for (j = 0; j < drmmode_output->mode_output->count_encoders;
		     j++) {
			int k;
			for (k = 0; k < mode_res->count_encoders; k++) {
				if (mode_res->encoders[k] ==
				    drmmode_output->
				    mode_encoders[j]->encoder_id)
					drmmode_output->enc_mask |= (1 << k);
			}

			drmmode_output->enc_clone_mask &=
			    drmmode_output->mode_encoders[j]->possible_clones;
		}
	}

	for (i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];
		output->possible_clones = find_clones(scrn, output);
	}
}

/* returns pitch alignment in pixels */
int drmmode_get_pitch_align(ScrnInfoPtr scrn, int bpe)
{
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);

	if (info->have_tiling_info)
		/* linear aligned requirements */
		return MAX(64, info->group_bytes / bpe);
	else
		/* default to 512 elements if we don't know the real
		 * group size otherwise the kernel may reject the CS
		 * if the group sizes don't match as the pitch won't
		 * be aligned properly.
		 */
		return 512;
}

static Bool drmmode_xf86crtc_resize(ScrnInfoPtr scrn, int width, int height)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);
	struct amdgpu_buffer *old_front = NULL;
	ScreenPtr screen = xf86ScrnToScreen(scrn);
	int i, pitch, old_width, old_height, old_pitch;
	int cpp = info->pixel_bytes;
	PixmapPtr ppix = screen->GetScreenPixmap(screen);
	void *fb_shadow;
	int hint = 0;
	xRectangle rect;
	GCPtr gc;

	if (scrn->virtualX == width && scrn->virtualY == height)
		return TRUE;

	if (info->shadow_primary)
		hint = AMDGPU_CREATE_PIXMAP_LINEAR | AMDGPU_CREATE_PIXMAP_GTT;
	else if (!info->use_glamor)
		hint = AMDGPU_CREATE_PIXMAP_LINEAR;

	xf86DrvMsg(scrn->scrnIndex, X_INFO,
		   "Allocate new frame buffer %dx%d\n", width, height);

	old_width = scrn->virtualX;
	old_height = scrn->virtualY;
	old_pitch = scrn->displayWidth;
	old_front = info->front_buffer;

	scrn->virtualX = width;
	scrn->virtualY = height;

	info->front_buffer =
		amdgpu_alloc_pixmap_bo(scrn, scrn->virtualX, scrn->virtualY,
				       scrn->depth, hint, scrn->bitsPerPixel,
				       &pitch);
	if (!info->front_buffer) {
		xf86DrvMsg(scrn->scrnIndex, X_ERROR,
			   "Failed to allocate front buffer memory\n");
		goto fail;
	}

	if (!info->use_glamor && amdgpu_bo_map(scrn, info->front_buffer) != 0) {
		xf86DrvMsg(scrn->scrnIndex, X_ERROR,
			   "Failed to map front buffer memory\n");
		goto fail;
	}

	xf86DrvMsg(scrn->scrnIndex, X_INFO, " => pitch %d bytes\n", pitch);
	scrn->displayWidth = pitch / cpp;

	if (info->use_glamor ||
	    (info->front_buffer->flags & AMDGPU_BO_FLAGS_GBM)) {
		screen->ModifyPixmapHeader(ppix,
					   width, height, -1, -1, pitch, info->front_buffer->cpu_ptr);
	} else {
		fb_shadow = calloc(1, pitch * scrn->virtualY);
		if (fb_shadow == NULL)
			goto fail;
		free(info->fb_shadow);
		info->fb_shadow = fb_shadow;
		screen->ModifyPixmapHeader(ppix,
					   width, height, -1, -1, pitch,
					   info->fb_shadow);
	}

	if (!amdgpu_glamor_create_screen_resources(scrn->pScreen))
		goto fail;

	if (info->use_glamor ||
	    (info->front_buffer->flags & AMDGPU_BO_FLAGS_GBM)) {
		if (!amdgpu_set_pixmap_bo(ppix, info->front_buffer))
			goto fail;
	}

	/* Clear new buffer */
	gc = GetScratchGC(ppix->drawable.depth, scrn->pScreen);
	ValidateGC(&ppix->drawable, gc);
	rect.x = 0;
	rect.y = 0;
	rect.width = width;
	rect.height = height;
	info->force_accel = TRUE;
	(*gc->ops->PolyFillRect)(&ppix->drawable, gc, 1, &rect);
	info->force_accel = FALSE;
	FreeScratchGC(gc);
	amdgpu_glamor_finish(scrn);

	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];

		if (!crtc->enabled)
			continue;

		drmmode_set_mode_major(crtc, &crtc->mode,
				       crtc->rotation, crtc->x, crtc->y);
	}

	if (old_front) {
		amdgpu_bo_unref(&old_front);
	}

	return TRUE;

fail:
	if (info->front_buffer) {
		amdgpu_bo_unref(&info->front_buffer);
	}
	info->front_buffer = old_front;
	scrn->virtualX = old_width;
	scrn->virtualY = old_height;
	scrn->displayWidth = old_pitch;

	return FALSE;
}

static const xf86CrtcConfigFuncsRec drmmode_xf86crtc_config_funcs = {
	drmmode_xf86crtc_resize
};

static void
drmmode_flip_abort(xf86CrtcPtr crtc, void *event_data)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(crtc->scrn);
	drmmode_flipdata_ptr flipdata = event_data;

	if (--flipdata->flip_count == 0) {
		if (!flipdata->fe_crtc)
			flipdata->fe_crtc = crtc;
		flipdata->abort(flipdata->fe_crtc, flipdata->event_data);
		drmmode_fb_reference(pAMDGPUEnt->fd, &flipdata->fb, NULL);
		free(flipdata);
	}

	drmmode_fb_reference(pAMDGPUEnt->fd, &drmmode_crtc->flip_pending,
			     NULL);
}

static void
drmmode_flip_handler(xf86CrtcPtr crtc, uint32_t frame, uint64_t usec, void *event_data)
{
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(crtc->scrn);
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_flipdata_ptr flipdata = event_data;

	/* Is this the event whose info shall be delivered to higher level? */
	if (crtc == flipdata->fe_crtc) {
		/* Yes: Cache msc, ust for later delivery. */
		flipdata->fe_frame = frame;
		flipdata->fe_usec = usec;
	}

	drmmode_fb_reference(pAMDGPUEnt->fd, &drmmode_crtc->fb,
			     flipdata->fb);
	if (drmmode_crtc->tear_free ||
	    drmmode_crtc->flip_pending == flipdata->fb) {
		drmmode_fb_reference(pAMDGPUEnt->fd,
				     &drmmode_crtc->flip_pending, NULL);
	}

	if (--flipdata->flip_count == 0) {
		/* Deliver MSC & UST from reference/current CRTC to flip event
		 * handler
		 */
		if (flipdata->fe_crtc)
			flipdata->handler(flipdata->fe_crtc, flipdata->fe_frame,
					  flipdata->fe_usec, flipdata->event_data);
		else
			flipdata->handler(crtc, frame, usec, flipdata->event_data);

		drmmode_fb_reference(pAMDGPUEnt->fd, &flipdata->fb, NULL);
		free(flipdata);
	}
}

#if HAVE_NOTIFY_FD
static void drmmode_notify_fd(int fd, int notify, void *data)
{
	drmmode_ptr drmmode = data;
	drmHandleEvent(fd, &drmmode->event_context);
}
#else
static void drm_wakeup_handler(pointer data, int err, pointer p)
{
	drmmode_ptr drmmode = data;
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(drmmode->scrn);
	fd_set *read_mask = p;

	if (err >= 0 && FD_ISSET(pAMDGPUEnt->fd, read_mask)) {
		drmHandleEvent(pAMDGPUEnt->fd, &drmmode->event_context);
	}
}
#endif

static Bool drmmode_probe_page_flip_target(AMDGPUEntPtr pAMDGPUEnt)
{
	uint64_t cap_value;

	return drmGetCap(pAMDGPUEnt->fd, DRM_CAP_PAGE_FLIP_TARGET,
			 &cap_value) == 0 && cap_value != 0;
}

static int
drmmode_page_flip(AMDGPUEntPtr pAMDGPUEnt, drmmode_crtc_private_ptr drmmode_crtc,
		  int fb_id, uint32_t flags, uintptr_t drm_queue_seq)
{
	flags |= DRM_MODE_PAGE_FLIP_EVENT;
	return drmModePageFlip(pAMDGPUEnt->fd, drmmode_crtc->mode_crtc->crtc_id,
			       fb_id, flags, (void*)drm_queue_seq);
}

int
drmmode_page_flip_target_absolute(AMDGPUEntPtr pAMDGPUEnt,
				  drmmode_crtc_private_ptr drmmode_crtc,
				  int fb_id, uint32_t flags,
				  uintptr_t drm_queue_seq, uint32_t target_msc)
{
	if (pAMDGPUEnt->has_page_flip_target) {
		flags |= DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE;
		return drmModePageFlipTarget(pAMDGPUEnt->fd,
					     drmmode_crtc->mode_crtc->crtc_id,
					     fb_id, flags, (void*)drm_queue_seq,
					     target_msc);
	}

	return drmmode_page_flip(pAMDGPUEnt, drmmode_crtc, fb_id, flags,
				 drm_queue_seq);
}

int
drmmode_page_flip_target_relative(AMDGPUEntPtr pAMDGPUEnt,
				  drmmode_crtc_private_ptr drmmode_crtc,
				  int fb_id, uint32_t flags,
				  uintptr_t drm_queue_seq, uint32_t target_msc)
{
	if (pAMDGPUEnt->has_page_flip_target) {
		flags |= DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_TARGET_RELATIVE;
		return drmModePageFlipTarget(pAMDGPUEnt->fd,
					     drmmode_crtc->mode_crtc->crtc_id,
					     fb_id, flags, (void*)drm_queue_seq,
					     target_msc);
	}

	return drmmode_page_flip(pAMDGPUEnt, drmmode_crtc, fb_id, flags,
				 drm_queue_seq);
}

Bool drmmode_pre_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode, int cpp)
{
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	int i, num_dvi = 0, num_hdmi = 0;
	unsigned int crtcs_needed = 0;
	drmModeResPtr mode_res;
#ifdef AMDGPU_PIXMAP_SHARING
	char *bus_id_string, *provider_name;
#endif

	xf86CrtcConfigInit(pScrn, &drmmode_xf86crtc_config_funcs);

	drmmode->scrn = pScrn;
	mode_res = drmModeGetResources(pAMDGPUEnt->fd);
	if (!mode_res)
		return FALSE;

	drmmode->count_crtcs = mode_res->count_crtcs;
	xf86CrtcSetSizeRange(pScrn, 320, 200, mode_res->max_width,
			     mode_res->max_height);

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "Initializing outputs ...\n");
	for (i = 0; i < mode_res->count_connectors; i++)
		crtcs_needed += drmmode_output_init(pScrn, drmmode, mode_res, i, &num_dvi, &num_hdmi, 0);

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "%d crtcs needed for screen.\n", crtcs_needed);

	if (!info->use_glamor) {
		/* Rotation requires hardware acceleration */
		drmmode_crtc_funcs.shadow_allocate = NULL;
		drmmode_crtc_funcs.shadow_create = NULL;
		drmmode_crtc_funcs.shadow_destroy = NULL;
	}

	for (i = 0; i < mode_res->count_crtcs; i++)
		if (!xf86IsEntityShared(pScrn->entityList[0]) ||
		    (crtcs_needed && !(pAMDGPUEnt->assigned_crtcs & (1 << i))))
			crtcs_needed -= drmmode_crtc_init(pScrn, drmmode, mode_res, i);

	/* All ZaphodHeads outputs provided with matching crtcs? */
	if (xf86IsEntityShared(pScrn->entityList[0]) && (crtcs_needed > 0))
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "%d ZaphodHeads crtcs unavailable. Some outputs will stay off.\n",
			   crtcs_needed);

	/* workout clones */
	drmmode_clones_init(pScrn, drmmode, mode_res);

#ifdef AMDGPU_PIXMAP_SHARING
	bus_id_string = DRICreatePCIBusID(info->PciInfo);
	XNFasprintf(&provider_name, "%s @ %s", pScrn->chipset, bus_id_string);
	free(bus_id_string);
	xf86ProviderSetup(pScrn, NULL, provider_name);
	free(provider_name);
#endif

	xf86InitialConfiguration(pScrn, TRUE);

	drmmode->event_context.version = 2;
	drmmode->event_context.vblank_handler = amdgpu_drm_queue_handler;
	drmmode->event_context.page_flip_handler = amdgpu_drm_queue_handler;

	pAMDGPUEnt->has_page_flip_target = drmmode_probe_page_flip_target(pAMDGPUEnt);

	drmModeFreeResources(mode_res);
	return TRUE;
}

void drmmode_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

	info->drmmode_inited = TRUE;
	if (pAMDGPUEnt->fd_wakeup_registered != serverGeneration) {
#if HAVE_NOTIFY_FD
		SetNotifyFd(pAMDGPUEnt->fd, drmmode_notify_fd, X_NOTIFY_READ, drmmode);
#else
		AddGeneralSocket(pAMDGPUEnt->fd);
		RegisterBlockAndWakeupHandlers((BlockHandlerProcPtr) NoopDDA,
					       drm_wakeup_handler, drmmode);
#endif
		pAMDGPUEnt->fd_wakeup_registered = serverGeneration;
		pAMDGPUEnt->fd_wakeup_ref = 1;
	} else
		pAMDGPUEnt->fd_wakeup_ref++;
}

void drmmode_fini(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	int c;

	if (!info->drmmode_inited)
		return;

	if (pAMDGPUEnt->fd_wakeup_registered == serverGeneration &&
	    !--pAMDGPUEnt->fd_wakeup_ref) {
#if HAVE_NOTIFY_FD
		RemoveNotifyFd(pAMDGPUEnt->fd);
#else
		RemoveGeneralSocket(pAMDGPUEnt->fd);
		RemoveBlockAndWakeupHandlers((BlockHandlerProcPtr) NoopDDA,
					     drm_wakeup_handler, drmmode);
#endif
	}

	for (c = 0; c < config->num_crtc; c++)
		drmmode_crtc_scanout_free(config->crtc[c]->driver_private);
}

void drmmode_set_cursor(ScrnInfoPtr scrn, drmmode_ptr drmmode, int id,
			struct amdgpu_buffer *bo)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
	xf86CrtcPtr crtc = xf86_config->crtc[id];
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

	drmmode_crtc->cursor_buffer = bo;
}

void drmmode_adjust_frame(ScrnInfoPtr pScrn, drmmode_ptr drmmode, int x, int y)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output = config->output[config->compat_output];
	xf86CrtcPtr crtc = output->crtc;

	if (crtc && crtc->enabled) {
		drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation, x, y);
	}
}

Bool drmmode_set_desired_modes(ScrnInfoPtr pScrn, drmmode_ptr drmmode,
			       Bool set_hw)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	int c;

	for (c = 0; c < config->num_crtc; c++) {
		xf86CrtcPtr crtc = config->crtc[c];
		drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
		xf86OutputPtr output = NULL;
		int o;

		/* Skip disabled CRTCs */
		if (!crtc->enabled) {
			if (set_hw) {
				drmmode_do_crtc_dpms(crtc, DPMSModeOff);
				drmModeSetCrtc(pAMDGPUEnt->fd,
					       drmmode_crtc->mode_crtc->crtc_id,
					       0, 0, 0, NULL, 0, NULL);
				drmmode_fb_reference(pAMDGPUEnt->fd,
						     &drmmode_crtc->fb, NULL);
			}
			continue;
		}

		if (config->output[config->compat_output]->crtc == crtc)
			output = config->output[config->compat_output];
		else {
			for (o = 0; o < config->num_output; o++)
				if (config->output[o]->crtc == crtc) {
					output = config->output[o];
					break;
				}
		}
		/* paranoia */
		if (!output)
			continue;

		/* Mark that we'll need to re-set the mode for sure */
		memset(&crtc->mode, 0, sizeof(crtc->mode));
		if (!crtc->desiredMode.CrtcHDisplay) {
			DisplayModePtr mode = xf86OutputFindClosestMode(output,
									pScrn->
									currentMode);

			if (!mode)
				return FALSE;
			crtc->desiredMode = *mode;
			crtc->desiredRotation = RR_Rotate_0;
			crtc->desiredX = 0;
			crtc->desiredY = 0;
		}

		if (set_hw) {
			if (!crtc->funcs->set_mode_major(crtc, &crtc->desiredMode,
							 crtc->desiredRotation,
							 crtc->desiredX,
							 crtc->desiredY))
				return FALSE;
		} else {
			crtc->mode = crtc->desiredMode;
			crtc->rotation = crtc->desiredRotation;
			crtc->x = crtc->desiredX;
			crtc->y = crtc->desiredY;
			if (!drmmode_handle_transform(crtc))
				return FALSE;
		}
	}
	return TRUE;
}

Bool drmmode_setup_colormap(ScreenPtr pScreen, ScrnInfoPtr pScrn)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);

	if (xf86_config->num_crtc) {
		xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
			       "Initializing kms color map\n");
		if (!miCreateDefColormap(pScreen))
			return FALSE;
		/* all amdgpus support 10 bit CLUTs */
		if (!xf86HandleColormaps(pScreen, 256, 10,
					 NULL, NULL,
					 CMAP_PALETTED_TRUECOLOR
#if 0				/* This option messes up text mode! (eich@suse.de) */
					 | CMAP_LOAD_EVEN_IF_OFFSCREEN
#endif
					 | CMAP_RELOAD_ON_MODE_SWITCH))
			return FALSE;
	}
	return TRUE;
}

static Bool
drmmode_find_output(ScrnInfoPtr scrn, int output_id, int *num_dvi,
		    int *num_hdmi)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
	int i;

	for (i = 0; i < config->num_output; i++) {
		xf86OutputPtr output = config->output[i];
		drmmode_output_private_ptr drmmode_output = output->driver_private;

		if (drmmode_output->output_id == output_id) {
			switch(drmmode_output->mode_output->connector_type) {
			case DRM_MODE_CONNECTOR_DVII:
			case DRM_MODE_CONNECTOR_DVID:
			case DRM_MODE_CONNECTOR_DVIA:
				(*num_dvi)++;
				break;
			case DRM_MODE_CONNECTOR_HDMIA:
			case DRM_MODE_CONNECTOR_HDMIB:
				(*num_hdmi)++;
				break;
			}

			return TRUE;
		}
	}

	return FALSE;
}

void
amdgpu_mode_hotplug(ScrnInfoPtr scrn, drmmode_ptr drmmode)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
	drmModeResPtr mode_res;
	int i, j;
	Bool found;
	Bool changed = FALSE;
	int num_dvi = 0, num_hdmi = 0;

	/* Try to re-set the mode on all the connectors with a BAD link-state:
	 * This may happen if a link degrades and a new modeset is necessary, using
	 * different link-training parameters. If the kernel found that the current
	 * mode is not achievable anymore, it should have pruned the mode before
	 * sending the hotplug event. Try to re-set the currently-set mode to keep
	 * the display alive, this will fail if the mode has been pruned.
	 * In any case, we will send randr events for the Desktop Environment to
	 * deal with it, if it wants to.
	 */
	for (i = 0; i < config->num_output; i++) {
		xf86OutputPtr output = config->output[i];
		drmmode_output_private_ptr drmmode_output = output->driver_private;
		uint32_t con_id = drmmode_output->mode_output->connector_id;
		drmModeConnectorPtr koutput;

		/* Get an updated view of the properties for the current connector and
		 * look for the link-status property
		 */
		koutput = drmModeGetConnectorCurrent(pAMDGPUEnt->fd, con_id);
		for (j = 0; koutput && j < koutput->count_props; j++) {
			drmModePropertyPtr props;
			props = drmModeGetProperty(pAMDGPUEnt->fd, koutput->props[j]);
			if (props && props->flags & DRM_MODE_PROP_ENUM &&
			    !strcmp(props->name, "link-status") &&
			    koutput->prop_values[j] == DRM_MODE_LINK_STATUS_BAD) {
				xf86CrtcPtr crtc = output->crtc;
				if (!crtc)
					continue;

				/* the connector got a link failure, re-set the current mode */
				drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation,
						       crtc->x, crtc->y);

				xf86DrvMsg(scrn->scrnIndex, X_WARNING,
					   "hotplug event: connector %u's link-state is BAD, "
					   "tried resetting the current mode. You may be left "
					   "with a black screen if this fails...\n", con_id);
			}
			drmModeFreeProperty(props);
		}
		drmModeFreeConnector(koutput);
	}

	mode_res = drmModeGetResources(pAMDGPUEnt->fd);
	if (!mode_res)
		goto out;

restart_destroy:
	for (i = 0; i < config->num_output; i++) {
		xf86OutputPtr output = config->output[i];
		drmmode_output_private_ptr drmmode_output = output->driver_private;
		found = FALSE;
		for (j = 0; j < mode_res->count_connectors; j++) {
			if (mode_res->connectors[j] == drmmode_output->output_id) {
				found = TRUE;
				break;
			}
		}
		if (found)
			continue;

		drmModeFreeConnector(drmmode_output->mode_output);
		drmmode_output->mode_output = NULL;
		drmmode_output->output_id = -1;

		changed = TRUE;
		if (drmmode->delete_dp_12_displays) {
			RROutputDestroy(output->randr_output);
			xf86OutputDestroy(output);
			goto restart_destroy;
		}
	}

	/* find new output ids we don't have outputs for */
	for (i = 0; i < mode_res->count_connectors; i++) {
		if (drmmode_find_output(pAMDGPUEnt->primary_scrn,
					mode_res->connectors[i],
					&num_dvi, &num_hdmi) ||
		    (pAMDGPUEnt->secondary_scrn &&
		     drmmode_find_output(pAMDGPUEnt->secondary_scrn,
					 mode_res->connectors[i],
					 &num_dvi, &num_hdmi)))
			continue;

		if (drmmode_output_init(scrn, drmmode, mode_res, i, &num_dvi,
					&num_hdmi, 1) != 0)
			changed = TRUE;
	}

	if (changed && dixPrivateKeyRegistered(rrPrivKey)) {
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,14,99,2,0)
		RRSetChanged(xf86ScrnToScreen(scrn));
#else
		rrScrPrivPtr rrScrPriv = rrGetScrPriv(scrn->pScreen);
		rrScrPriv->changed = TRUE;
#endif
		RRTellChanged(xf86ScrnToScreen(scrn));
	}

	drmModeFreeResources(mode_res);
out:
	RRGetInfo(xf86ScrnToScreen(scrn), TRUE);
}

#ifdef HAVE_LIBUDEV
static void drmmode_handle_uevents(int fd, void *closure)
{
	drmmode_ptr drmmode = closure;
	ScrnInfoPtr scrn = drmmode->scrn;
	struct udev_device *dev;
	Bool received = FALSE;
	struct timeval tv = { 0, 0 };
	fd_set readfd;

	FD_ZERO(&readfd);
	FD_SET(fd, &readfd);

	while (select(fd + 1, &readfd, NULL, NULL, &tv) > 0 &&
	       FD_ISSET(fd, &readfd)) {
		/* select() ensured that this will not block */
		dev = udev_monitor_receive_device(drmmode->uevent_monitor);
		if (dev) {
			udev_device_unref(dev);
			received = TRUE;
		}
	}

	if (received)
		amdgpu_mode_hotplug(scrn, drmmode);
}
#endif

void drmmode_uevent_init(ScrnInfoPtr scrn, drmmode_ptr drmmode)
{
#ifdef HAVE_LIBUDEV
	struct udev *u;
	struct udev_monitor *mon;

	u = udev_new();
	if (!u)
		return;
	mon = udev_monitor_new_from_netlink(u, "udev");
	if (!mon) {
		udev_unref(u);
		return;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(mon,
							    "drm",
							    "drm_minor") < 0 ||
	    udev_monitor_enable_receiving(mon) < 0) {
		udev_monitor_unref(mon);
		udev_unref(u);
		return;
	}

	drmmode->uevent_handler =
	    xf86AddGeneralHandler(udev_monitor_get_fd(mon),
				  drmmode_handle_uevents, drmmode);

	drmmode->uevent_monitor = mon;
#endif
}

void drmmode_uevent_fini(ScrnInfoPtr scrn, drmmode_ptr drmmode)
{
#ifdef HAVE_LIBUDEV
	if (drmmode->uevent_handler) {
		struct udev *u = udev_monitor_get_udev(drmmode->uevent_monitor);
		xf86RemoveGeneralHandler(drmmode->uevent_handler);

		udev_monitor_unref(drmmode->uevent_monitor);
		udev_unref(u);
	}
#endif
}

Bool amdgpu_do_pageflip(ScrnInfoPtr scrn, ClientPtr client,
			PixmapPtr new_front, uint64_t id, void *data,
			xf86CrtcPtr ref_crtc, amdgpu_drm_handler_proc handler,
			amdgpu_drm_abort_proc abort,
			enum drmmode_flip_sync flip_sync,
			uint32_t target_msc)
{
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
	xf86CrtcPtr crtc = NULL;
	drmmode_crtc_private_ptr drmmode_crtc = config->crtc[0]->driver_private;
	int i;
	uint32_t flip_flags = flip_sync == FLIP_ASYNC ? DRM_MODE_PAGE_FLIP_ASYNC : 0;
	drmmode_flipdata_ptr flipdata;
	uintptr_t drm_queue_seq = 0;

	flipdata = calloc(1, sizeof(drmmode_flipdata_rec));
	if (!flipdata) {
		xf86DrvMsg(scrn->scrnIndex, X_WARNING,
			   "flip queue: data alloc failed.\n");
		goto error;
	}

	drmmode_fb_reference(pAMDGPUEnt->fd, &flipdata->fb,
			     amdgpu_pixmap_get_fb(new_front));
	if (!flipdata->fb) {
		ErrorF("Failed to get FB for flip\n");
		goto error;
	}

	/*
	 * Queue flips on all enabled CRTCs
	 * Note that if/when we get per-CRTC buffers, we'll have to update this.
	 * Right now it assumes a single shared fb across all CRTCs, with the
	 * kernel fixing up the offset of each CRTC as necessary.
	 *
	 * Also, flips queued on disabled or incorrectly configured displays
	 * may never complete; this is a configuration error.
	 */

	flipdata->event_data = data;
	flipdata->handler = handler;
	flipdata->abort = abort;
	flipdata->fe_crtc = ref_crtc;

	for (i = 0; i < config->num_crtc; i++) {
		struct drmmode_fb *fb = flipdata->fb;

		crtc = config->crtc[i];
		drmmode_crtc = crtc->driver_private;

		if (!drmmode_crtc_can_flip(crtc) ||
		    (drmmode_crtc->tear_free && crtc != ref_crtc))
			continue;

		flipdata->flip_count++;

		drm_queue_seq = amdgpu_drm_queue_alloc(crtc, client, id,
						       flipdata,
						       drmmode_flip_handler,
						       drmmode_flip_abort);
		if (drm_queue_seq == AMDGPU_DRM_QUEUE_ERROR) {
			xf86DrvMsg(scrn->scrnIndex, X_WARNING,
				   "Allocating DRM queue event entry failed.\n");
			goto error;
		}

		if (drmmode_crtc->tear_free) {
			BoxRec extents = { .x1 = 0, .y1 = 0,
					   .x2 = new_front->drawable.width,
					   .y2 = new_front->drawable.height };
			int scanout_id = drmmode_crtc->scanout_id ^ 1;

			if (flip_sync == FLIP_ASYNC) {
				if (!drmmode_wait_vblank(crtc,
							 DRM_VBLANK_RELATIVE |
							 DRM_VBLANK_EVENT,
							 0, drm_queue_seq,
							 NULL, NULL))
					goto flip_error;
				goto next;
			}

			fb = amdgpu_pixmap_get_fb(drmmode_crtc->scanout[scanout_id].pixmap);
			if (!fb) {
				ErrorF("Failed to get FB for TearFree flip\n");
				goto error;
			}

			amdgpu_scanout_do_update(crtc, scanout_id, new_front,
						 &extents);

			drmmode_crtc_wait_pending_event(drmmode_crtc, pAMDGPUEnt->fd,
							drmmode_crtc->scanout_update_pending);
		}

		if (crtc == ref_crtc) {
			if (drmmode_page_flip_target_absolute(pAMDGPUEnt,
							      drmmode_crtc,
							      fb->handle,
							      flip_flags,
							      drm_queue_seq,
							      target_msc) != 0)
				goto flip_error;
		} else {
			if (drmmode_page_flip_target_relative(pAMDGPUEnt,
							      drmmode_crtc,
							      fb->handle,
							      flip_flags,
							      drm_queue_seq, 0) != 0)
				goto flip_error;
		}

		if (drmmode_crtc->tear_free) {
			drmmode_crtc->scanout_id ^= 1;
			drmmode_crtc->ignore_damage = TRUE;
		}

	next:
		drmmode_fb_reference(pAMDGPUEnt->fd,
				     &drmmode_crtc->flip_pending, fb);
		drm_queue_seq = 0;
	}

	if (flipdata->flip_count > 0)
		return TRUE;

flip_error:
	xf86DrvMsg(scrn->scrnIndex, X_WARNING, "flip queue failed: %s\n",
		   strerror(errno));

error:
	if (drm_queue_seq)
		amdgpu_drm_abort_entry(drm_queue_seq);
	else if (crtc)
		drmmode_flip_abort(crtc, flipdata);
	else {
		abort(NULL, data);
		drmmode_fb_reference(pAMDGPUEnt->fd, &flipdata->fb, NULL);
		free(flipdata);
	}

	xf86DrvMsg(scrn->scrnIndex, X_WARNING, "Page flip failed: %s\n",
		   strerror(errno));
	return FALSE;
}
