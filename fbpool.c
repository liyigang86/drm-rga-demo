#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>

// The mmap might failed, for example in sshfs's direct_io mode
#define USE_MMAP

#ifdef DRM_DISPLAY
#include "drm_display.h"
#endif

#define DEBUG
#ifdef DEBUG
#define FBPOOL_DEBUG(fmt, ...) \
    if (getenv("FBPOOL_DEBUG")) \
    printf("FBPOOL_DEBUG: %s(%d) " fmt, __func__, __LINE__, __VA_ARGS__)
#else
#define FBPOOL_DEBUG(fmt, ...)
#endif

#ifdef DRM_DISPLAY
#undef FBPOOL_DEBUG
#define FBPOOL_DEBUG DRM_DEBUG
#endif

#define FBPOOL_MAGIC "FBPL"

typedef struct {
    char magic[4];
    int32_t width;
    int32_t height;
    int32_t bpp;
    int32_t num_fb;
    int32_t fb_size;
    int32_t current_fb;
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

void usage(const char *prog) {
#ifdef DRM_DISPLAY
    fprintf(stderr, "Usage: %s <source pool path>\n", prog);
#else
    fprintf(stderr, "Usage: %s <source pool path> <dest pool path>\n", prog);
#endif
    exit(-1);
}

#ifndef USE_MMAP
static inline int sync_area(int fd, uint8_t *buf,
                            size_t offset, size_t size, int is_read)
{
    if (lseek(fd, offset, SEEK_SET) < 0) {
        fprintf(stderr, "failed to seek file\n");
        return -1;
    }

    if (is_read) {
        if (read(fd, buf + offset, size) != size) {
            fprintf(stderr, "failed to read file\n");
            return -1;
        }
    } else {
        if (write(fd, buf + offset, size) != size) {
            fprintf(stderr, "failed to write file\n");
            return -1;
        }
    }

    return 0;
}
#endif

static inline void *map_buf(int fd, size_t offset, size_t size, int needs_read)
{
    void *buf;

#ifdef USE_MMAP
    buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (buf == MAP_FAILED) {
        fprintf(stderr, "mmap failed\n");
        return NULL;
    }
#else
    buf = malloc(size);
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        return NULL;
    }

    if (needs_read && sync_area(fd, buf, offset, size, 1) < 0) {
        fprintf(stderr, "read failed\n");
        return NULL;
    }
#endif

    return buf;
}

static inline void release_buf(void *buf, size_t size)
{
#ifdef USE_MMAP
    munmap(buf, size);
#else
    free(buf);
#endif
}

#ifndef USE_MMAP
#define SYNC_MEMBER(fd, s, m, is_read) \
    sync_area(fd, (void *)(s), (void *)&(s)->m - (void *)s, \
              sizeof((s)->m), is_read)
#endif

int main(int argc, char **argv)
{
    fbpool_header *src;
#if defined(DRM_DISPLAY) || defined(USE_MMAP)
    uint8_t *src_ptr;
#endif
    char *src_file;
    int src_fd, old_fb, fb;
    size_t size, offset;

#ifndef DRM_DISPLAY
    fbpool_header *dst;
#if defined(USE_MMAP)
    uint8_t *dst_ptr;
#endif
    char *dst_file;
    int dst_fd;

    if (argc != 3)
        usage(argv[0]);
#else // DRM_DISPLAY
    if (argc != 2)
        usage(argv[0]);
#endif // DRM_DISPLAY

    src_file = argv[1];

    while (1) {
        src_fd = open(src_file, O_RDWR);
        if (src_fd >= 0)
            break;

        fprintf(stderr, "open %s failed, retrying in 1s\n", src_file);
        sleep(1);
    }

    size = sizeof(fbpool_header);

    src = (fbpool_header *)map_buf(src_fd, 0, size, 1);
    if (!src) {
        fprintf(stderr, "map %s failed\n", src_file);
        goto err_close_src;
    }

    while (strncmp(src->magic, FBPOOL_MAGIC, 4)) {
#ifdef DRM_DISPLAY
        FBPOOL_DEBUG("magic not matched: %4s\n", src->magic);
        sleep(1);
#else
        fprintf(stderr, "magic not matched: %4s\n", src->magic);
        goto err_unmap_src;
#endif
    }

    FBPOOL_DEBUG("Source fb pool with %d fb, size: %dx%d(%d), bpp: %d\n",
                 src->num_fb, src->width, src->height, src->fb_size, src->bpp);

    size = sizeof(fbpool_header) + src->num_fb * src->fb_size;

    release_buf(src, sizeof(fbpool_header));

    src = (fbpool_header *)map_buf(src_fd, 0, size, 0);
    if (!src) {
        fprintf(stderr, "map %s failed\n", src_file);
        goto err_close_src;
    }

#ifndef USE_MMAP
    if (sync_area(src_fd, (void *)src, 0, sizeof(fbpool_header), 1) < 0) {
        fprintf(stderr, "read %s failed\n", src_file);
        goto err_close_src;
    }
#endif

#ifdef DRM_DISPLAY
    if (drm_init(2, src->bpp, src->width, src->height) < 0) {
        fprintf(stderr, "init drm failed\n");
        goto err_unmap_src;
    }
#else
    dst_file = argv[2];

    dst_fd = open(dst_file, O_RDWR | O_CREAT, 0666);
    if (dst_fd < 0) {
        fprintf(stderr, "open %s failed\n", dst_file);
        goto err_unmap_src;
    }

    if (lseek(dst_fd, size, SEEK_SET) < 0 && ftruncate(dst_fd, size) < 0) {
        fprintf(stderr, "truncate %s failed\n", dst_file);
        goto err_close_dst;
    }

    dst = (fbpool_header *)map_buf(dst_fd, 0, size, 0);
    if (!dst) {
        fprintf(stderr, "map %s failed\n", dst_file);
        goto err_close_dst;
    }

    memcpy(dst, src, sizeof(fbpool_header));

    dst_ptr = (uint8_t *)dst + sizeof(fbpool_header);
    dst->current_fb = -1;
#endif // DRM_DISPLAY

#if defined(DRM_DISPLAY) || defined(USE_MMAP)
    src_ptr = (uint8_t *)src + sizeof(fbpool_header);
#endif
    old_fb = -1;

    while (1) {
#ifndef USE_MMAP
        if (SYNC_MEMBER(src_fd, src, current_fb, 1) < 0)
            continue;
#endif
        if (src->current_fb == old_fb) {
            usleep(1000);
            continue;
        }

        fb = src->current_fb;
        offset = fb * src->fb_size;

        if (fb < 0) {
            FBPOOL_DEBUG("Flushing fb: %d\n", fb);
            old_fb = -1;

#ifndef DRM_DISPLAY
            dst->current_fb = -1;
#ifndef USE_MMAP
            if (SYNC_MEMBER(dst_fd, dst, current_fb, 0) < 0)
                continue;
#endif
#endif // DRM_DISPLAY

            continue;
        } else if (fb >= src->num_fb) {
            fprintf(stderr, "invalid fb: %d\n", fb);
            break;
        } else if (fb != ((old_fb + 1) % src->num_fb)) {
            if (old_fb != -1)
                FBPOOL_DEBUG("Lost fb between: %d - %d\n", old_fb, fb);
        }

        FBPOOL_DEBUG("Sending fb: %d\n", fb);

#ifdef DRM_DISPLAY
#ifndef USE_MMAP
        if (sync_area(src_fd, (void *)src, offset + sizeof(fbpool_header),
                      src->fb_size, 1) < 0)
            continue;
#endif
        drm_render(src_ptr + offset, src->bpp,
                   src->width, src->height, src->width * src->bpp / 8);
#else // DRM_DISPLAY
#ifdef USE_MMAP
        memcpy(dst_ptr + offset, src_ptr + offset, src->fb_size);
#else
        if (sync_area(dst_fd, (void *)dst, offset + sizeof(fbpool_header),
                      src->fb_size, 0) < 0)
            continue;
#endif
        fsync(dst_fd);
#endif // DRM_DISPLAY

#ifndef DRM_DISPLAY
        dst->current_fb = fb;
#ifndef USE_MMAP
        if (SYNC_MEMBER(dst_fd, dst, current_fb, 0) < 0)
            continue;
#endif
        fsync(dst_fd);
#endif // DRM_DISPLAY

        old_fb = fb;
        log_fps();
    }

#ifdef DRM_DISPLAY
    drm_deinit();
#else
    release_buf((void *)dst, size);

err_close_dst:
    close(dst_fd);
#endif
err_unmap_src:
    release_buf((void *)src, size);
err_close_src:
    close(src_fd);

    return 0;
}
