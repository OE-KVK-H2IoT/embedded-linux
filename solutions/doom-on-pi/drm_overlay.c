/*
 * DRM overlay plane — render button labels in the letterbox margins.
 *
 * Uses a hardware overlay plane (ARGB8888) so the button labels are
 * composited by the display controller with zero CPU after setup.
 * Doom renders to the primary plane; this program renders to a
 * separate overlay plane.
 *
 * Build:
 *   gcc -o drm_overlay drm_overlay.c $(pkg-config --cflags --libs libdrm)
 *
 * Usage:
 *   sudo ./drm_overlay          # must run BEFORE Doom
 *   # Then start Doom in another terminal
 *
 * The program sets up the overlay, drops DRM master (so Doom can
 * claim it), and sleeps until killed. The display controller reads
 * the overlay buffer automatically on every refresh — 0% CPU.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static volatile int running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* Fill a rectangle in the framebuffer (ARGB8888) */
static void fill_rect(uint32_t *fb, int pitch4, int x, int y,
                       int w, int h, uint32_t color)
{
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            fb[row * pitch4 + col] = color;
        }
    }
}

/* Draw a simple text label using a minimal 5x7 bitmap font.
 * Only uppercase A-Z, 0-9, and a few symbols. */
static const uint8_t font5x7[][5] = {
    /* A */ {0x7C, 0x12, 0x11, 0x12, 0x7C},
    /* B */ {0x7F, 0x49, 0x49, 0x49, 0x36},
    /* C */ {0x3E, 0x41, 0x41, 0x41, 0x22},
    /* D */ {0x7F, 0x41, 0x41, 0x41, 0x3E},
    /* E */ {0x7F, 0x49, 0x49, 0x49, 0x41},
    /* F */ {0x7F, 0x09, 0x09, 0x09, 0x01},
    /* G */ {0x3E, 0x41, 0x49, 0x49, 0x3A},
    /* H */ {0x7F, 0x08, 0x08, 0x08, 0x7F},
    /* I */ {0x00, 0x41, 0x7F, 0x41, 0x00},
    /* J */ {0x20, 0x40, 0x41, 0x3F, 0x01},
    /* K */ {0x7F, 0x08, 0x14, 0x22, 0x41},
    /* L */ {0x7F, 0x40, 0x40, 0x40, 0x40},
    /* M */ {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    /* N */ {0x7F, 0x04, 0x08, 0x10, 0x7F},
    /* O */ {0x3E, 0x41, 0x41, 0x41, 0x3E},
    /* P */ {0x7F, 0x09, 0x09, 0x09, 0x06},
    /* Q */ {0x3E, 0x41, 0x51, 0x21, 0x5E},
    /* R */ {0x7F, 0x09, 0x19, 0x29, 0x46},
    /* S */ {0x46, 0x49, 0x49, 0x49, 0x31},
    /* T */ {0x01, 0x01, 0x7F, 0x01, 0x01},
    /* U */ {0x3F, 0x40, 0x40, 0x40, 0x3F},
    /* V */ {0x1F, 0x20, 0x40, 0x20, 0x1F},
    /* W */ {0x3F, 0x40, 0x30, 0x40, 0x3F},
    /* X */ {0x63, 0x14, 0x08, 0x14, 0x63},
    /* Y */ {0x07, 0x08, 0x70, 0x08, 0x07},
    /* Z */ {0x61, 0x51, 0x49, 0x45, 0x43},
};

static void draw_char(uint32_t *fb, int pitch4, int cx, int cy,
                       char ch, uint32_t color, int scale)
{
    int idx = -1;
    if (ch >= 'A' && ch <= 'Z') idx = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') idx = ch - 'a';
    if (idx < 0) return;

    for (int col = 0; col < 5; col++) {
        uint8_t bits = font5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        fb[(cy + row * scale + sy) * pitch4 +
                           (cx + col * scale + sx)] = color;
            }
        }
    }
}

static void draw_text(uint32_t *fb, int pitch4, int x, int y,
                       const char *text, uint32_t color, int scale)
{
    for (int i = 0; text[i]; i++) {
        draw_char(fb, pitch4, x + i * 6 * scale, y, text[i], color, scale);
    }
}

int main(void)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* 1. Open DRM device */
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open /dev/dri/card0"); return 1; }

    /* Enable universal planes (required to see overlay planes) */
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    /* 2. Find the active CRTC */
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { perror("drmModeGetResources"); return 1; }

    drmModeCrtc *crtc = NULL;
    uint32_t crtc_id = 0;
    int crtc_index = -1;
    for (int i = 0; i < res->count_crtcs; i++) {
        drmModeCrtc *c = drmModeGetCrtc(fd, res->crtcs[i]);
        if (c && c->mode_valid) {
            crtc = c;
            crtc_id = c->crtc_id;
            crtc_index = i;
            break;
        }
        drmModeFreeCrtc(c);
    }
    if (!crtc) {
        fprintf(stderr, "No active CRTC found\n");
        return 1;
    }

    int disp_w = crtc->mode.hdisplay;
    int disp_h = crtc->mode.vdisplay;
    printf("Active CRTC %u: %dx%d\n", crtc_id, disp_w, disp_h);

    /* 3. Find an overlay plane for this CRTC */
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes) { perror("drmModeGetPlaneResources"); return 1; }

    uint32_t plane_id = 0;
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(fd, planes->planes[i]);
        if (!p) continue;

        /* Check if this plane is compatible with our CRTC */
        if (!(p->possible_crtcs & (1 << crtc_index))) {
            drmModeFreePlane(p);
            continue;
        }

        /* Check plane type via properties */
        drmModeObjectProperties *props = drmModeObjectGetProperties(fd, p->plane_id,
                                                                      DRM_MODE_OBJECT_PLANE);
        if (props) {
            for (uint32_t j = 0; j < props->count_props; j++) {
                drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[j]);
                if (prop && strcmp(prop->name, "type") == 0) {
                    if (props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY) {
                        plane_id = p->plane_id;
                    }
                }
                drmModeFreeProperty(prop);
            }
            drmModeFreeObjectProperties(props);
        }
        drmModeFreePlane(p);
        if (plane_id) break;
    }
    drmModeFreePlaneResources(planes);

    if (!plane_id) {
        fprintf(stderr, "No overlay plane found for CRTC %u\n", crtc_id);
        return 1;
    }
    printf("Using overlay plane %u\n", plane_id);

    /* 4. Create a dumb buffer (ARGB8888) */
    struct drm_mode_create_dumb cr = {
        .width = disp_w,
        .height = disp_h,
        .bpp = 32,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return 1;
    }

    uint32_t handles[4] = {cr.handle, 0, 0, 0};
    uint32_t strides[4] = {cr.pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    uint32_t fb_id = 0;
    if (drmModeAddFB2(fd, disp_w, disp_h, DRM_FORMAT_ARGB8888,
                       handles, strides, offsets, &fb_id, 0) < 0) {
        perror("drmModeAddFB2");
        return 1;
    }

    /* Map the buffer for CPU access */
    struct drm_mode_map_dumb map_req = { .handle = cr.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        return 1;
    }

    uint32_t *px = mmap(NULL, cr.size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, map_req.offset);
    if (px == MAP_FAILED) { perror("mmap"); return 1; }

    int pitch4 = cr.pitch / 4;  /* stride in uint32_t units */

    /* 5. Draw button labels (alpha=0 for transparent center) */
    memset(px, 0, cr.size);  /* fully transparent */

    /* 4:3 game area */
    int game_w = disp_h * 4 / 3;
    int margin = (disp_w - game_w) / 2;

    if (margin < 20) {
        fprintf(stderr, "Margins too narrow (%dpx). Skipping overlay.\n", margin);
        /* Still set up the plane with transparent buffer */
    } else {
        int row_h = disp_h / 4;

        /* Button definitions: label, ARGB color */
        struct { const char *label; uint32_t color; } left_btns[4] = {
            {"FWD",    0xB03C8C3C},  /* green */
            {"BACK",   0xB03C648C},  /* blue */
            {"STRAFE", 0xB08C643C},  /* orange */
            {"ESC",    0xB08C3C3C},  /* red */
        };
        struct { const char *label; uint32_t color; } right_btns[4] = {
            {"FIRE",   0xB0B43232},  /* red */
            {"USE",    0xB0328CB4},  /* cyan */
            {"ENTER",  0xB032B450},  /* green */
            {"MAP",    0xB08C8C32},  /* yellow */
        };

        for (int i = 0; i < 4; i++) {
            int by = i * row_h;

            /* Left margin button */
            fill_rect(px, pitch4, 2, by + 2, margin - 4, row_h - 4,
                       left_btns[i].color);
            /* Border */
            fill_rect(px, pitch4, 1, by + 1, margin - 2, 2, 0xC0FFFFFF);
            fill_rect(px, pitch4, 1, by + row_h - 3, margin - 2, 2, 0xC0FFFFFF);
            fill_rect(px, pitch4, 1, by + 1, 2, row_h - 2, 0xC0FFFFFF);
            fill_rect(px, pitch4, margin - 3, by + 1, 2, row_h - 2, 0xC0FFFFFF);
            /* Label */
            int lx = (margin - (int)strlen(left_btns[i].label) * 12) / 2;
            int ly = by + row_h / 2 - 7;
            draw_text(px, pitch4, lx, ly, left_btns[i].label, 0xFFFFFFFF, 2);

            /* Right margin button */
            int rx0 = disp_w - margin;
            fill_rect(px, pitch4, rx0 + 2, by + 2, margin - 4, row_h - 4,
                       right_btns[i].color);
            /* Border */
            fill_rect(px, pitch4, rx0 + 1, by + 1, margin - 2, 2, 0xC0FFFFFF);
            fill_rect(px, pitch4, rx0 + 1, by + row_h - 3, margin - 2, 2, 0xC0FFFFFF);
            fill_rect(px, pitch4, rx0 + 1, by + 1, 2, row_h - 2, 0xC0FFFFFF);
            fill_rect(px, pitch4, rx0 + margin - 3, by + 1, 2, row_h - 2, 0xC0FFFFFF);
            /* Label */
            int rlx = rx0 + (margin - (int)strlen(right_btns[i].label) * 12) / 2;
            int rly = by + row_h / 2 - 7;
            draw_text(px, pitch4, rlx, rly, right_btns[i].label, 0xFFFFFFFF, 2);
        }
    }

    /* 6. Activate overlay plane */
    if (drmModeSetPlane(fd, plane_id, crtc_id, fb_id, 0,
                         0, 0, disp_w, disp_h,
                         0, 0, disp_w << 16, disp_h << 16) < 0) {
        perror("drmModeSetPlane");
        return 1;
    }
    printf("Overlay plane active\n");

    /* 7. Release DRM master so Doom can claim it */
    drmDropMaster(fd);
    printf("DRM master released — start Doom now\n");

    /* 8. Sleep until killed — display controller reads buffer automatically */
    while (running) {
        sleep(1);
    }

    /* Cleanup */
    munmap(px, cr.size);
    drmModeRmFB(fd, fb_id);
    struct drm_mode_destroy_dumb destroy = { .handle = cr.handle };
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    drmModeFreeCrtc(crtc);
    drmModeFreeResources(res);
    close(fd);

    printf("Overlay stopped\n");
    return 0;
}
