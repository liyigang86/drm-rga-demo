#ifndef _DRM_DISPLAY_H
#define _DRM_DISPLAY_H

#define DEBUG
#ifdef DEBUG
#define DRM_DEBUG(fmt, ...) \
    if (getenv("DRM_DEBUG")) \
    printf("DRM_DEBUG: %s(%d) " fmt, __func__, __LINE__, __VA_ARGS__)
#else
#define DRM_DEBUG(fmt, ...)
#endif

int drm_init(int fb_num, int bpp, int fb_width, int fb_height);
int drm_render(void *buf, int bpp, int width, int height, int pitch);
void drm_deinit(void);

#endif // _DRM_DISPLAY_H
