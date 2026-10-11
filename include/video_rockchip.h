/*
 * (C) Copyright 2017 Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */

#ifndef VIDEO_ROCKCHIP_H
#define VIDEO_ROCKCHIP_H

#if defined(CONFIG_PLATFORM_ODROID_GOADV)
	/*
	 * Room for the largest panel of the consoles this U-Boot starts, the R46H's
	 * 1024x768. 640x480 was too small for anything bigger: the logo drawn on a
	 * 720x720 or 1024x768 panel ran over the logo file loaded right after it.
	 */
	#define DRM_ROCKCHIP_FB_WIDTH		1024
	#define DRM_ROCKCHIP_FB_HEIGHT	768
	#define DRM_ROCKCHIP_FB_BPP		VIDEO_BPP32
#else
	#ifdef CONFIG_DRM_ROCKCHIP_VIDEO_FRAMEBUFFER
		#define DRM_ROCKCHIP_FB_WIDTH		1920
		#define DRM_ROCKCHIP_FB_HEIGHT		1080
		#define DRM_ROCKCHIP_FB_BPP		VIDEO_BPP32
	#else
		#define DRM_ROCKCHIP_FB_WIDTH		0
		#define DRM_ROCKCHIP_FB_HEIGHT		0
		#define DRM_ROCKCHIP_FB_BPP		VIDEO_BPP32
	#endif
#endif

#define MEMORY_POOL_SIZE	32 * 1024 * 1024
#define DRM_ROCKCHIP_FB_SIZE \
	VNBYTES(DRM_ROCKCHIP_FB_BPP) * DRM_ROCKCHIP_FB_WIDTH * DRM_ROCKCHIP_FB_HEIGHT

int rockchip_show_bmp(const char *bmp);
int rockchip_show_logo(void);
void rockchip_display_fixup(void *blob);

#endif
