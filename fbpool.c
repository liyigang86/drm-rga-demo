#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "drm_display.h"

#define FBPOOL_DEBUG DRM_DEBUG

#include <sys/ioctl.h>

#define IOC_MAGIC 'c'
#define APP_ACM_READ _IOR(IOC_MAGIC, 0, int)

#define FBPOOL_MAGIC "FBPL"

typedef struct {
    char magic[4];
    int32_t width;
    int32_t height;
    int32_t bpp;
    int32_t fb_size;
} fbpool_header;

#define FPS_UPDATE_INTERVAL 60

static void log_fps(void) {
    struct timeval tv;
    uint64_t curr_time;
    float fps;

    static uint64_t last_fps_time = 0;
    static unsigned frames = 0;

    if (!last_fps_time) {
        gettimeofday(&tv, NULL);
        last_fps_time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }

    if (++frames % FPS_UPDATE_INTERVAL)
        return;

    gettimeofday(&tv, NULL);
    curr_time = tv.tv_sec * 1000 + tv.tv_usec / 1000;

    fps = 1000.0f * FPS_UPDATE_INTERVAL / (curr_time - last_fps_time);
    last_fps_time = curr_time;

    printf("[FBPOOL] FPS: %6.1f || Frames: %u\n", fps, frames);
}

static inline void *map_buf(int fd, size_t offset, size_t size)
{
    void *buf;

    buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (buf == MAP_FAILED) {
        fprintf(stderr, "mmap failed\n");
        return NULL;
    }

    return buf;
}

static inline void release_buf(void *buf, size_t size)
{
    munmap(buf, size);
}

int main(int argc, char **argv)
{
    fbpool_header *src;
    uint8_t *src_ptr;
    char *src_file;
    int src_fd;
    size_t size;

    src_file = "/dev/app_acm_usb";

    while (1) {
        src_fd = open(src_file, O_RDWR);
        if (src_fd >= 0)
            break;

        fprintf(stderr, "open %s failed, retrying in 1s\n", src_file);
        sleep(1);
    }

    size = sizeof(fbpool_header);

    src = (fbpool_header *)map_buf(src_fd, 0, size);
    if (!src) {
        fprintf(stderr, "map %s failed\n", src_file);
        goto err_close_src;
    }

    while (strncmp(src->magic, FBPOOL_MAGIC, 4)) {
        FBPOOL_DEBUG("magic not matched: %4s\n", src->magic);
        sleep(1);
        ioctl(src_fd, APP_ACM_READ, &size);
    }

    FBPOOL_DEBUG("Source fb pool with fb size: %dx%d(%d), bpp: %d\n",
                 src->width, src->height, src->fb_size, src->bpp);

    size = sizeof(fbpool_header) + src->fb_size;

    release_buf(src, sizeof(fbpool_header));

    src = (fbpool_header *)map_buf(src_fd, 0, size);
    if (!src) {
        fprintf(stderr, "map %s failed\n", src_file);
        goto err_close_src;
    }

    if (drm_init(2, src->bpp, src->width, src->height) < 0) {
        fprintf(stderr, "init drm failed\n");
        goto err_unmap_src;
    }

    src_ptr = (uint8_t *)src + sizeof(fbpool_header);

    while (1) {
        if (ioctl(src_fd, APP_ACM_READ, &size)) {
            usleep(1000);
            continue;
        }

        drm_render(src_ptr, src->bpp,
                   src->width, src->height, src->width * src->bpp / 8);
        log_fps();
    }

    drm_deinit();
err_unmap_src:
    release_buf((void *)src, size);
err_close_src:
    close(src_fd);

    return 0;
}
