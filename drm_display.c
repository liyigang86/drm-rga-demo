#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "drm_display.h"

#define RGA // Use RGA to convert/scale images
#define DRM_RGB // Use RGB32 DRM format
//#define DRM_SCALE // Use DRM plane scaling
//#define DRM_OVERLAY // Use DRM overlay plane

#ifdef RGA
#include <rga/rga.h>
#include <rga/RgaApi.h>
#endif

#define MAX_FB      3

struct drm_bo {
    void *ptr;
    size_t size;
    size_t offset;
    size_t pitch;
    unsigned handle;
    int fb_id;
    int dma_fd;
};

struct device {
    int fd;

    struct {
        int hdisplay;
        int vdisplay;

        struct drm_bo *bo[MAX_FB];
        int current;
        int fb_num;
        int bpp;

        int fb_width;
        int fb_height;
    } mode;

    drmModeResPtr res;

    int crtc_id;
    int plane_id;
    int crtc_pipe;
    struct drm_bo *dummy_bo;
};

struct device *pdev;

static int bo_map(struct device *dev, struct drm_bo *bo) {
    struct drm_mode_map_dumb arg = {
        .handle = bo->handle,
    };
    int ret;

    ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);
    if (ret)
        return ret;

    bo->ptr = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   dev->fd, arg.offset);
    if (bo->ptr == MAP_FAILED) {
        bo->ptr = NULL;
        return -1;
    }

    return 0;
}

static void bo_unmap(struct device *dev, struct drm_bo *bo) {
    if (!bo->ptr)
        return;

    drmUnmap(bo->ptr, bo->size);
    bo->ptr = NULL;
}

static void bo_destroy(struct device *dev, struct drm_bo *bo) {
    struct drm_mode_destroy_dumb arg = {
        .handle = bo->handle,
    };

    if (bo->dma_fd > 0)
        close(bo->dma_fd);

    if (bo->fb_id)
        drmModeRmFB(dev->fd, bo->fb_id);

    bo_unmap(dev, bo);

    if (bo->handle)
        drmIoctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);

    free(bo);
}

static struct drm_bo *bo_create(struct device *dev, int width, int height,
                                int bpp) {
    struct drm_mode_create_dumb arg = {
        .bpp = bpp,
        .width = width,
        .height = height,
    };
    struct drm_bo *bo;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    int format;
    int ret;

    bo = malloc(sizeof(struct drm_bo));
    if (bo == NULL) {
        fprintf(stderr, "allocate bo failed\n");
        return NULL;
    }
    memset(bo, 0, sizeof(*bo));

    ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
    if (ret) {
        fprintf(stderr, "create dumb failed\n");
        goto err;
    }

    bo->handle = arg.handle;
    bo->size = arg.size;
    bo->pitch = arg.pitch;

    ret = bo_map(dev, bo);
    if (ret) {
        fprintf(stderr, "map bo failed\n");
        goto err;
    }

    switch (bpp) {
    case 12:
        handles[0] = handles[1] = bo->handle;
        pitches[0] = pitches[1] = bo->pitch * 2 / 3;
        offsets[1] = pitches[0] * height;
        format = DRM_FORMAT_NV12;
        break;
    case 16:
        handles[0] = bo->handle;
        pitches[0] = bo->pitch;
        format = DRM_FORMAT_RGB565;
        break;
    case 24:
    case 32:
        handles[0] = bo->handle;
        pitches[0] = bo->pitch;
        format = DRM_FORMAT_XRGB8888;
        break;
    }

    ret = drmModeAddFB2(dev->fd, width, height, format, handles,
                        pitches, offsets, (uint32_t *)&bo->fb_id, 0);
    if (ret) {
        fprintf(stderr, "add fb failed\n");
        goto err;
    }

    ret = drmPrimeHandleToFD(dev->fd, bo->handle, DRM_CLOEXEC, &bo->dma_fd);
    if (ret) {
        fprintf(stderr, "get dma fd failed\n");
        goto err;
    }

    DRM_DEBUG("Created bo: %d, %dx%d\n", bo->fb_id, width, height);

    return bo;
err:
    bo_destroy(dev, bo);
    return NULL;
}

static void free_fb(struct device *dev) {
    unsigned int i;

    DRM_DEBUG("Free fb, num: %d, bpp: %d\n", dev->mode.fb_num, dev->mode.bpp);
    for (i = 0; i < dev->mode.fb_num; i++) {
        if (dev->mode.bo[i])
            bo_destroy(dev, dev->mode.bo[i]);
    }

    dev->mode.fb_num = 0;
    dev->mode.bpp = 0;
    dev->mode.current = 0;
}

static int alloc_fb(struct device *dev, int fb_num, int bpp) {
    unsigned int i;

    DRM_DEBUG("Alloc fb num: %d, bpp: %d\n", fb_num, bpp);

    dev->mode.fb_num = fb_num;
    dev->mode.bpp = bpp;
    dev->mode.current = 0;

    for (i = 0; i < dev->mode.fb_num; i++) {
        dev->mode.bo[i] =
            bo_create(dev, dev->mode.fb_width, dev->mode.fb_height, bpp);
        if (!dev->mode.bo[i]) {
            fprintf(stderr, "create bo failed\n");
            free_fb(dev);
            return -1;
        }
    }

    return 0;
}

#ifdef DRM_OVERLAY
static drmModeCrtcPtr drm_find_current_crtc(struct device *dev, int *pipe) {
    drmModeResPtr res = dev->res;
    drmModeCrtcPtr crtc;
    int i;

    for (i = 0; i < res->count_crtcs; i++) {
        crtc = drmModeGetCrtc(dev->fd, res->crtcs[i]);
        if (crtc && crtc->mode_valid) {
            *pipe = i;
            return crtc;
        }
        drmModeFreeCrtc(crtc);
    }
    return NULL;
}
#else
static drmModeConnectorPtr drm_get_connector(struct device *dev,
                                             int connector_id) {
    drmModeConnectorPtr conn;

    conn = drmModeGetConnector(dev->fd, connector_id);
    if (!conn)
        return NULL;

    DRM_DEBUG("Connector id: %d, %sconnected, modes: %d\n", connector_id,
              (conn->connection == DRM_MODE_CONNECTED) ? "" : "dis",
              conn->count_modes);
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes)
        return conn;

    drmModeFreeConnector(conn);
    return NULL;
}

static drmModeConnectorPtr drm_find_best_connector(struct device *dev) {
    drmModeResPtr res = dev->res;
    drmModeConnectorPtr conn;
    int i;

    for (i = 0; i < res->count_connectors; i++) {
        conn = drm_get_connector(dev, res->connectors[i]);
        if (conn)
            return conn;
    }
    return NULL;
}

static drmModeCrtcPtr drm_find_best_crtc(struct device *dev,
                                         drmModeConnectorPtr conn,
                                         int *pipe) {
    drmModeResPtr res = dev->res;
    drmModeEncoderPtr encoder;
    drmModeCrtcPtr crtc;
    int i, preferred_crtc_id = 0;
    int crtcs_for_connector = 0;

    encoder = drmModeGetEncoder(dev->fd, conn->encoder_id);
    if (encoder) {
        preferred_crtc_id = encoder->crtc_id;
        drmModeFreeEncoder(encoder);
    }
    DRM_DEBUG("Preferred crtc: %d\n", preferred_crtc_id);

    crtc = drmModeGetCrtc(dev->fd, preferred_crtc_id);
    if (crtc) {
        for (i = 0; i < res->count_crtcs; i++) {
            if (res->crtcs[i] == preferred_crtc_id) {
                *pipe = i;
                return crtc;
            }
        }
        drmModeFreeCrtc(crtc);
    }

    for (i = 0; i < res->count_encoders; i++) {
        encoder = drmModeGetEncoder(dev->fd, res->encoders[i]);
        if (encoder)
            crtcs_for_connector |= encoder->possible_crtcs;
        drmModeFreeEncoder(encoder);
    }
    DRM_DEBUG("Possible crtcs: %x\n", crtcs_for_connector);
    if (!crtcs_for_connector)
        return NULL;

    *pipe = ffs(crtcs_for_connector) - 1;
    return drmModeGetCrtc(dev->fd, res->crtcs[*pipe]);
}
#endif

static int drm_plane_match_type(struct device *dev, int plane_id, int type) {
    drmModeObjectPropertiesPtr props;
    drmModePropertyPtr prop;
    int i, matched = 0;

    props = drmModeObjectGetProperties(dev->fd, plane_id,
                                       DRM_MODE_OBJECT_PLANE);
    if (!props)
        return 0;

    for (i = 0; i < props->count_props; i++) {
        prop = drmModeGetProperty(dev->fd, props->props[i]);
        if (prop && !strcmp(prop->name, "type"))
            matched = props->prop_values[i] == type;
        drmModeFreeProperty(prop);
    }
    DRM_DEBUG("Plane: %d, matched: %d\n", plane_id, matched);

    drmModeFreeObjectProperties(props);
    return matched;
}

static drmModePlanePtr drm_get_plane(struct device *dev,
                                     int plane_id, int pipe, int type) {
    drmModePlanePtr plane;

    plane = drmModeGetPlane(dev->fd, plane_id);
    if (!plane)
        return NULL;

    DRM_DEBUG("Check plane: %d, possible_crtcs: %x\n", plane_id,
              plane->possible_crtcs);
    if (drm_plane_match_type(dev, plane_id, type)) {
        if (plane->possible_crtcs & (1 << pipe))
            return plane;
    }

    drmModeFreePlane(plane);
    return NULL;
}

static drmModePlanePtr drm_find_best_plane(struct device *dev, int crtc_pipe) {
    drmModePlaneResPtr pres;
    drmModePlanePtr plane;
    int i, type;

#ifdef DRM_OVERLAY
    type = DRM_PLANE_TYPE_OVERLAY;
#else
    type = DRM_PLANE_TYPE_PRIMARY;
#endif

    pres = drmModeGetPlaneResources(dev->fd);
    if (!pres)
        return NULL;

    for (i = 0; i < pres->count_planes; i++) {
        plane = drm_get_plane(dev, pres->planes[i], crtc_pipe, type);
        if (plane) {
            drmModeFreePlaneResources(pres);
            return plane;
        }
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(pres);
    return NULL;
}

#ifndef DRM_OVERLAY
static drmModeModeInfoPtr drm_find_best_mode(struct device *dev,
                                             drmModeConnectorPtr conn) {
    drmModeModeInfoPtr mode;
    int i, preferred_width = 1920, preferred_height = 1080;

    DRM_DEBUG("Preferred mode: %dx%d\n", preferred_width, preferred_height);

    mode = &conn->modes[0];

    for (i = 0; i < conn->count_modes; i++) {
        DRM_DEBUG("Check mode: %dx%d\n",
                conn->modes[i].hdisplay, conn->modes[i].vdisplay);
        if (conn->modes[i].hdisplay == preferred_width &&
                conn->modes[i].vdisplay == preferred_height) {
            mode = &conn->modes[i];
            break;
        }
    }

    return mode;
}
#endif

static void drm_free(struct device *dev) {
    if (dev->res) {
        drmModeFreeResources(dev->res);
        dev->res = NULL;
    }

    dev->crtc_id = 0;
    dev->plane_id = 0;
    dev->mode.hdisplay = 0;
    dev->mode.vdisplay = 0;
}

static int drm_setup(struct device *dev, int fb_width, int fb_height) {
#ifndef DRM_OVERLAY
    drmModeConnectorPtr conn = NULL;
    drmModeModeInfoPtr mode;
#endif
    drmModePlanePtr plane = NULL;
    drmModeCrtcPtr crtc = NULL;
    int crtc_pipe, success = 0;

    dev->res = drmModeGetResources(dev->fd);
    if (!dev->res) {
        fprintf(stderr, "drm get resource failed\n");
        goto err;
    }

#ifndef DRM_OVERLAY
    conn = drm_find_best_connector(dev);
    if (!conn) {
        fprintf(stderr, "drm find connector failed\n");
        goto err;
    }
    DRM_DEBUG("Best connector id: %d\n", conn->connector_id);

    mode = drm_find_best_mode(dev, conn);
    if (!mode) {
        fprintf(stderr, "drm find mode failed\n");
        goto err;
    }
    DRM_DEBUG("Best mode: %dx%d\n", mode->hdisplay, mode->vdisplay);

    crtc = drm_find_best_crtc(dev, conn, &crtc_pipe);
    if (!crtc) {
        fprintf(stderr, "drm find crtc failed\n");
        goto err;
    }

    DRM_DEBUG("Best crtc: %d\n", crtc->crtc_id);
#else
    crtc = drm_find_current_crtc(dev, &crtc_pipe);
    if (!crtc) {
        fprintf(stderr, "drm find crtc failed\n");
        goto err;
    }

    DRM_DEBUG("Current crtc: %d with mode: %dx%d\n", crtc->crtc_id,
              crtc->width, crtc->height);
#endif

    plane = drm_find_best_plane(dev, crtc_pipe);
    if (!plane) {
        fprintf(stderr, "drm find plane failed\n");
        goto err;
    }

    DRM_DEBUG("Best plane: %d\n", plane->plane_id);

#ifndef DRM_OVERLAY
    dev->dummy_bo = bo_create(dev, mode->hdisplay, mode->vdisplay, 32);
    if (!dev->dummy_bo) {
        fprintf(stderr, "create dummy bo failed\n");
        goto err;
    }
    DRM_DEBUG("Created dummy bo fb: %d\n", dev->dummy_bo->fb_id);

    DRM_DEBUG("Set CRTC: %d(%d) with connector: %d, mode: %dx%d\n",
              crtc->crtc_id, crtc_pipe, conn->connector_id,
              mode->hdisplay, mode->vdisplay);
    if (drmModeSetCrtc(dev->fd, crtc->crtc_id,
                       dev->dummy_bo->fb_id, 0, 0,
                       &conn->connector_id, 1, mode) < 0) {
        fprintf(stderr, "drm set mode failed\n");
        goto err;
    }
#endif

    dev->crtc_id = crtc->crtc_id;
    dev->crtc_pipe = crtc_pipe;
    dev->plane_id = plane->plane_id;
#ifdef DRM_OVERLAY
    dev->mode.hdisplay = crtc->width;
    dev->mode.vdisplay = crtc->height;
#else
    dev->mode.hdisplay = mode->hdisplay;
    dev->mode.vdisplay = mode->vdisplay;
#endif

#ifdef DRM_SCALE
    dev->mode.fb_width = fb_width;
    dev->mode.fb_height = fb_height;
#else
    dev->mode.fb_width = dev->mode.hdisplay;
    dev->mode.fb_height = dev->mode.vdisplay;
#endif

    success = 1;
err:
    drmModeFreePlane(plane);
    drmModeFreeCrtc(crtc);
    if (!success) {
        drm_free(dev);
        return -1;
    }
    return 0;
}

int drm_init(int fb_num, int bpp, int fb_width, int fb_height) {
    int ret;

    if (fb_num > MAX_FB)
        return -1;

    pdev = malloc(sizeof(struct device));
    if (pdev == NULL) {
        fprintf(stderr, "allocate device failed\n");
        return -1;
    }
    memset(pdev, 0, sizeof(*pdev));

    pdev->fd = drmOpen(NULL, NULL);
    if (pdev->fd < 0)
        pdev->fd = open("/dev/dri/card0", O_RDWR);
    if (pdev->fd < 0) {
        fprintf(stderr, "drm open failed\n");
        goto err_drm_open;
    }
    fcntl(pdev->fd, F_SETFD, FD_CLOEXEC);

    drmSetClientCap(pdev->fd, DRM_CLIENT_CAP_ATOMIC, 1);
    drmSetClientCap(pdev->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    ret = drm_setup(pdev, fb_width, fb_height);
    if (ret) {
        fprintf(stderr, "drm setup failed\n");
        goto err_drm_setup;
    }

#ifdef DRM_RGB
    bpp = 32;
#endif

    ret = alloc_fb(pdev, fb_num, bpp);
    if (ret) {
        fprintf(stderr, "alloc fb failed\n");
        goto err_alloc_fb;
    }

    return 0;
err_alloc_fb:
    drm_free(pdev);
err_drm_setup:
    drmClose(pdev->fd);
err_drm_open:
    free(pdev);
    pdev = NULL;
    return -1;
}

void drm_deinit(void) {
    struct device *dev = pdev;
    if (!dev)
        return;

    if (dev->dummy_bo)
        bo_destroy(dev, dev->dummy_bo);

    free_fb(dev);
    drm_free(dev);

    if (pdev->fd > 0)
        drmClose(dev->fd);

    free(pdev);
    pdev = NULL;
}

static inline struct drm_bo *drm_get_bo(void) {
    return pdev->mode.bo[pdev->mode.current];
}

static void drm_next_bo(void) {
    pdev->mode.current ++;
    if (pdev->mode.current >= MAX_FB || pdev->mode.current >= pdev->mode.fb_num)
        pdev->mode.current = 0;
}

static void sync_handler(int fd, uint32_t frame,
                         uint32_t sec, uint32_t usec, void *data) {
    int *waiting = data;

    *waiting = 0;
}

static int drm_sync(void) {
    struct device *dev = pdev;
    int ret, waiting = 1;

    drmEventContext evctxt = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .vblank_handler = sync_handler,
    };
    drmVBlank vbl = {
        .request = {
            .type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT,
            .sequence = 1,
            .signal = (uint64_t)&waiting,
        },
    };

    struct pollfd fds[1] = {
        {
            .events = POLLIN,
            .fd = dev->fd,
        },
    };

    if (dev->crtc_pipe == 1)
        vbl.request.type |= DRM_VBLANK_SECONDARY;
    else if (dev->crtc_pipe > 1)
        vbl.request.type |= dev->crtc_pipe << DRM_VBLANK_HIGH_CRTC_SHIFT;

    if (drmWaitVBlank(dev->fd, &vbl) < 0)
        return -1;

    while (waiting) {
        do {
            poll(fds, 1, 3000);
        } while (ret == -1 && (errno == EAGAIN || errno == EINTR));

        ret = drmHandleEvent(dev->fd, &evctxt);
        if (ret < 0)
            return -1;
    }

    return 0;
}

static int drm_display(void) {
    struct device *dev = pdev;
    struct drm_bo *bo = drm_get_bo();
    int crtc_x, crtc_y, crtc_w, crtc_h;
    int sw, sh;
    int ret;

    sw = dev->mode.fb_width;
    sh = dev->mode.fb_height;
    crtc_w = dev->mode.hdisplay;
    crtc_h = dev->mode.vdisplay;
    crtc_x = 0;
    crtc_y = 0;

    // Set fb to main plane
    DRM_DEBUG("Display bo %d(%dx%d) at (%d,%d) %dx%d\n", bo->fb_id, sw, sh,
              crtc_x, crtc_y, crtc_w, crtc_h);
    ret = drmModeSetPlane(dev->fd, dev->plane_id, dev->crtc_id, bo->fb_id, 0,
                          crtc_x, crtc_y, crtc_w, crtc_h,
                          0, 0, sw << 16, sh << 16);
    if (ret) {
        fprintf(stderr, "drm set plane failed\n");
        return -1;
    }

    drm_sync();

    return 0;
}

#ifdef RGA
static int rga_prepare_info(int bpp, int width, int height, int pitch,
                            rga_info_t *info) {
    RgaSURF_FORMAT format;

    memset(info, 0, sizeof(rga_info_t));

    info->fd = -1;
    info->mmuFlag = 1;

    switch (bpp) {
    case 12:
        format = RK_FORMAT_YCbCr_420_SP;
        break;
    case 16:
        format = RK_FORMAT_RGB_565;
        break;
    case 32:
        format = RK_FORMAT_BGRA_8888;
        break;
    default:
        return -1;
    }

    rga_set_rect(&info->rect, 0, 0, width, height,
                 pitch * 8 / bpp, height, format);
    return 0;
}

static int drm_render_rga(void *buf, int bpp,
                          int width, int height, int pitch) {
    struct device *dev = pdev;
    struct drm_bo *bo = drm_get_bo();
    rga_info_t src_info = {0};
    rga_info_t dst_info = {0};

    static int rga_supported = 1;
    static int rga_inited = 0;

    if (!rga_supported)
        return -1;

    if (!rga_inited) {
        if (c_RkRgaInit() < 0) {
            rga_supported = 0;
            return -1;
        }
        rga_inited = 1;
    }

    if (rga_prepare_info(bpp, width, height, pitch, &src_info) < 0)
        return -1;

    if (rga_prepare_info(dev->mode.bpp, dev->mode.fb_width,
                         dev->mode.fb_height, bo->pitch, &dst_info) < 0)
        return -1;

    src_info.virAddr = buf;
    dst_info.virAddr = bo->ptr;

    return c_RkRgaBlit(&src_info, &dst_info, NULL);
}
#endif

int drm_render(void *buf, int bpp, int width, int height, int pitch) {
    struct device *dev = pdev;
    struct drm_bo *bo = drm_get_bo();
    int ret;

#ifdef RGA
    ret = drm_render_rga(buf, bpp, width, height, pitch);
#endif

    if (ret && bpp == dev->mode.bpp && pitch == bo->pitch &&
        width == dev->mode.fb_width && height == dev->mode.fb_height) {
        memcpy(bo->ptr, buf, pitch * height);
        ret = 0;
    }

    if (ret)
        fprintf(stderr, "render failed\n");
    else
        ret = drm_display();

    drm_next_bo();

    return ret;
}
