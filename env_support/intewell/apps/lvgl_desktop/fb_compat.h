#ifndef FB_COMPAT_H
#define FB_COMPAT_H

#include <stddef.h>
#include <stdint.h>

typedef uint16_t fb_coord_t;

#define FB_FMT_RGB16_565 11

#define _FBIOCBASE 0x2800
#define _FBIOC(nr) (_FBIOCBASE | (nr))

#define FBIOGET_VIDEOINFO _FBIOC(0x0001)
#define FBIOGET_PLANEINFO _FBIOC(0x0002)
#define FBIO_UPDATE _FBIOC(0x0007)
#define FBIOPAN_DISPLAY _FBIOC(0x0018)
#define FBIOGET_VSCREENINFO _FBIOC(0x001b)
#define FBIOGET_FSCREENINFO _FBIOC(0x001c)

struct fb_videoinfo_s {
    uint8_t fmt;
    fb_coord_t xres;
    fb_coord_t yres;
    uint8_t nplanes;
};

struct fb_planeinfo_s {
    void *fbmem;
    size_t fblen;
    fb_coord_t stride;
    uint8_t display;
    uint8_t bpp;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
};

struct fb_area_s {
    fb_coord_t x;
    fb_coord_t y;
    fb_coord_t w;
    fb_coord_t h;
};

#endif /* FB_COMPAT_H */
