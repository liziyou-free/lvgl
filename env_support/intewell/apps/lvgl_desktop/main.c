#define _POSIX_C_SOURCE 200809L

#include "lvgl/lvgl.h"
#include "fb_compat.h"
#include "lv_remote.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_FBDEV "/dev/fb0"
#define DEFAULT_INPUTDEV "/dev/input0"
#define DEFAULT_KBDDEV "/dev/kbd0"
#define DEFAULT_WIDTH 800
#define DEFAULT_HEIGHT 480
#define BYTES_PER_PIXEL 2
#define FB_BUFFER_COUNT 2
#define MAX_REMOTE_WINDOWS 8
#define KEYBOARD_PRESS 1

#define TOUCH_POS_VALID 0x02
#define TOUCH_DOWN 0x04
#define TOUCH_UP 0x08

#ifndef FB_SWAP_RB
#define FB_SWAP_RB 0
#endif

#ifndef FB_SWAP_BYTES
#define FB_SWAP_BYTES 0
#endif

struct fb_ctx {
    int fd;
    uint16_t *mem;
    void *draw_buf1;
    void *draw_buf2;
    size_t len;
    size_t frame_len;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint8_t active_index;
    bool mapped;
};

struct touch_point {
    int16_t x;
    int16_t y;
    uint8_t flags;
};

struct touch_sample {
    uint8_t npoints;
    struct touch_point point[1];
};

struct input_ctx {
    int fd;
    int16_t x;
    int16_t y;
    bool pressed;
};

struct keyboard_event {
    uint32_t key;
    uint8_t action;
};

struct keyboard_ctx {
    int fd;
};

struct remote_window {
    bool active;
    bool focused;
    int fd;
    int surface_fd;
    uint32_t window_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    size_t surface_len;
    uint8_t *surface;
    char title[LV_REMOTE_MAX_TITLE];
    char path[LV_REMOTE_MAX_PATH];
    uint8_t rxbuf[sizeof(struct lv_remote_msg)];
    size_t rx_len;
    lv_obj_t *panel;
    lv_obj_t *title_label;
    lv_obj_t *image;
    lv_image_dsc_t image_dsc;
};

static struct fb_ctx g_fb = {
    .fd = -1,
};
static struct input_ctx g_input = {
    .fd = -1,
};
static struct keyboard_ctx g_keyboard = {
    .fd = -1,
};
static struct remote_window g_windows[MAX_REMOTE_WINDOWS];
static struct remote_window *g_focused;
static int g_server_fd = -1;
static uint32_t g_next_window_id = 1;
static lv_obj_t *g_workspace;
static lv_obj_t *g_status;

static uint32_t tick_get_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

uint32_t lvgl_fb_get_idle_percent(void)
{
    return 0;
}

static void sleep_ms(uint32_t ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000U;
    ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
    nanosleep(&ts, NULL);
}

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static uint16_t fb_convert_rgb565(uint16_t color)
{
#if FB_SWAP_RB
    color = (uint16_t)(((color & 0x001fU) << 11) |
                       (color & 0x07e0U) |
                       ((color & 0xf800U) >> 11));
#endif

    return color;
}

static int fb_open(struct fb_ctx *fb, const char *path)
{
    struct fb_videoinfo_s vinfo;
    struct fb_planeinfo_s pinfo;

    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    if (ioctl(fb->fd, FBIOGET_VIDEOINFO, &vinfo) < 0) {
        fprintf(stderr, "FBIOGET_VIDEOINFO %s failed: %s\n", path, strerror(errno));
        goto fail;
    }

    if (ioctl(fb->fd, FBIOGET_PLANEINFO, &pinfo) < 0) {
        fprintf(stderr, "FBIOGET_PLANEINFO %s failed: %s\n", path, strerror(errno));
        goto fail;
    }

    if (vinfo.fmt != FB_FMT_RGB16_565 || pinfo.bpp != 16) {
        fprintf(stderr, "unsupported fb format fmt=%u bpp=%u; expected RGB565/16bpp\n",
                vinfo.fmt, pinfo.bpp);
        goto fail;
    }

    fb->width = vinfo.xres ? vinfo.xres : DEFAULT_WIDTH;
    fb->height = vinfo.yres ? vinfo.yres : DEFAULT_HEIGHT;
    fb->stride = pinfo.stride ? pinfo.stride : fb->width * BYTES_PER_PIXEL;
    fb->frame_len = (size_t)fb->stride * fb->height;
    fb->len = pinfo.fblen ? pinfo.fblen : fb->frame_len;
    if (fb->len < fb->frame_len * FB_BUFFER_COUNT ||
        pinfo.yres_virtual < fb->height * FB_BUFFER_COUNT) {
        fprintf(stderr,
                "%s does not expose two framebuffers; len=%zu frame=%zu yvirt=%u\n",
                path, fb->len, fb->frame_len, pinfo.yres_virtual);
        goto fail;
    }

    fb->mem = mmap(NULL, fb->len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        fprintf(stderr, "mmap %s failed: %s; using write() fallback\n",
                path, strerror(errno));
        fb->mem = NULL;
        fb->draw_buf1 = malloc(fb->frame_len);
        fb->draw_buf2 = malloc(fb->frame_len);
        if (fb->draw_buf1 == NULL || fb->draw_buf2 == NULL) {
            fprintf(stderr, "malloc fallback draw buffers failed\n");
            goto fail;
        }
    } else {
        fb->mapped = true;
        fb->draw_buf1 = fb->mem;
        fb->draw_buf2 = (uint8_t *)fb->mem + fb->frame_len;
    }

    printf("lvgl_desktop: %s %ux%u RGB565 stride=%u len=%zu mode=%s\n",
           path, fb->width, fb->height, fb->stride, fb->len,
           fb->mapped ? "mmap" : "write");
    return 0;

fail:
    free(fb->draw_buf1);
    free(fb->draw_buf2);
    fb->draw_buf1 = NULL;
    fb->draw_buf2 = NULL;
    close(fb->fd);
    fb->fd = -1;
    return -1;
}

static void fb_close(struct fb_ctx *fb)
{
    if (fb->mapped && fb->mem != NULL) {
        munmap(fb->mem, fb->len);
    } else {
        free(fb->draw_buf1);
        free(fb->draw_buf2);
    }

    if (fb->fd >= 0) {
        close(fb->fd);
    }
}

static void fb_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uintptr_t rendered = (uintptr_t)px_map;
    uint8_t next_index;

    (void)area;

    if (rendered == (uintptr_t)g_fb.draw_buf1) {
        next_index = 0;
    } else if (rendered == (uintptr_t)g_fb.draw_buf2) {
        next_index = 1;
    } else {
        fprintf(stderr, "unexpected LVGL draw buffer %p\n", px_map);
        lv_display_flush_ready(disp);
        return;
    }

    if (FB_SWAP_RB) {
        uint16_t *pix = (uint16_t *)px_map;
        size_t count = g_fb.frame_len / sizeof(uint16_t);

        for (size_t i = 0; i < count; i++) {
            pix[i] = fb_convert_rgb565(pix[i]);
        }
    }

#if FB_SWAP_BYTES
    lv_draw_sw_rgb565_swap(px_map, g_fb.frame_len / sizeof(uint16_t));
#endif

    if (lv_display_flush_is_last(disp)) {
        if (!g_fb.mapped) {
            struct fb_area_s update;

            if (lseek(g_fb.fd, 0, SEEK_SET) < 0) {
                fprintf(stderr, "lseek /dev/fb0 failed: %s\n", strerror(errno));
            } else if (write(g_fb.fd, px_map, g_fb.frame_len) !=
                       (ssize_t)g_fb.frame_len) {
                fprintf(stderr, "write /dev/fb0 failed: %s\n", strerror(errno));
            }

            update.x = 0;
            update.y = 0;
            update.w = g_fb.width;
            update.h = g_fb.height;
            if (ioctl(g_fb.fd, FBIO_UPDATE, &update) < 0 && errno != ENOTTY) {
                fprintf(stderr, "FBIO_UPDATE failed: %s\n", strerror(errno));
            }

            g_fb.active_index = next_index;
            lv_display_flush_ready(disp);
            return;
        }

        struct fb_planeinfo_s pan;

        memset(&pan, 0, sizeof(pan));
        pan.fbmem = g_fb.mem;
        pan.fblen = g_fb.len;
        pan.stride = g_fb.stride;
        pan.display = 0;
        pan.bpp = 16;
        pan.xres_virtual = g_fb.width;
        pan.yres_virtual = g_fb.height * FB_BUFFER_COUNT;
        pan.xoffset = 0;
        pan.yoffset = g_fb.height * next_index;

        if (ioctl(g_fb.fd, FBIOPAN_DISPLAY, &pan) < 0) {
            fprintf(stderr, "FBIOPAN_DISPLAY failed: %s\n", strerror(errno));
        } else {
            g_fb.active_index = next_index;
        }
    }

    lv_display_flush_ready(disp);
}

static int input_open(struct input_ctx *input, const char *path)
{
    input->fd = open(path, O_RDONLY | O_NONBLOCK);
    if (input->fd < 0) {
        fprintf(stderr, "open %s failed: %s; continue without pointer input\n",
                path, strerror(errno));
        return -1;
    }

    input->x = 0;
    input->y = 0;
    input->pressed = false;
    printf("lvgl_desktop: pointer input %s\n", path);
    return 0;
}

static void input_close(struct input_ctx *input)
{
    if (input->fd >= 0) {
        close(input->fd);
        input->fd = -1;
    }
}

static int16_t clamp_i16(int16_t value, int16_t min, int16_t max)
{
    if (value < min) {
        return min;
    }

    if (value > max) {
        return max;
    }

    return value;
}

static void remote_send_pointer(struct remote_window *win, int32_t x, int32_t y,
                                bool pressed)
{
    struct lv_remote_msg msg;

    if (win == NULL || !win->active) {
        return;
    }

    lv_remote_msg_init(&msg, LV_REMOTE_MSG_INPUT_POINTER);
    msg.window_id = win->window_id;
    msg.x = x;
    msg.y = y;
    msg.buttons = pressed ? 1U : 0U;
    msg.action = pressed ? LV_REMOTE_KEY_PRESS : LV_REMOTE_KEY_RELEASE;
    if (lv_remote_send_msg(win->fd, &msg) < 0) {
        fprintf(stderr, "send pointer to window %u failed: %s\n",
                win->window_id, strerror(errno));
    }
}

static void remote_send_key(struct remote_window *win, uint32_t key, uint32_t action)
{
    struct lv_remote_msg msg;

    if (win == NULL || !win->active) {
        return;
    }

    lv_remote_msg_init(&msg, LV_REMOTE_MSG_INPUT_KEY);
    msg.window_id = win->window_id;
    msg.key = key;
    msg.action = action;
    if (lv_remote_send_msg(win->fd, &msg) < 0) {
        fprintf(stderr, "send key to window %u failed: %s\n",
                win->window_id, strerror(errno));
    }
}

static void remote_focus(struct remote_window *win)
{
    if (g_focused == win) {
        return;
    }

    if (g_focused != NULL && g_focused->panel != NULL) {
        g_focused->focused = false;
        lv_obj_set_style_border_color(g_focused->panel, lv_color_hex(0x64748b),
                                      LV_PART_MAIN);
    }

    g_focused = win;
    if (g_focused != NULL && g_focused->panel != NULL) {
        g_focused->focused = true;
        lv_obj_set_style_border_color(g_focused->panel, lv_color_hex(0x2563eb),
                                      LV_PART_MAIN);
        lv_obj_move_to_index(g_focused->panel,
                             lv_obj_get_child_count(g_workspace) - 1);
    }
}

static void route_pointer_to_remote(void)
{
    for (int i = MAX_REMOTE_WINDOWS - 1; i >= 0; i--) {
        struct remote_window *win = &g_windows[i];
        lv_area_t coords;

        if (!win->active || win->image == NULL || win->surface == NULL) {
            continue;
        }

        lv_obj_get_coords(win->image, &coords);
        if (g_input.x < coords.x1 || g_input.x > coords.x2 ||
            g_input.y < coords.y1 || g_input.y > coords.y2) {
            continue;
        }

        remote_focus(win);
        remote_send_pointer(win, g_input.x - coords.x1, g_input.y - coords.y1,
                            g_input.pressed);
        return;
    }
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    if (g_input.fd >= 0) {
        struct touch_sample sample;
        ssize_t nread;

        do {
            nread = read(g_input.fd, &sample, sizeof(sample));
            if (nread == (ssize_t)sizeof(sample) && sample.npoints > 0) {
                const struct touch_point *point = &sample.point[0];

                if ((point->flags & TOUCH_POS_VALID) != 0) {
                    g_input.x = clamp_i16(point->x, 0, (int16_t)(g_fb.width - 1));
                    g_input.y = clamp_i16(point->y, 0, (int16_t)(g_fb.height - 1));
                }

                if ((point->flags & TOUCH_DOWN) != 0) {
                    g_input.pressed = true;
                } else if ((point->flags & TOUCH_UP) != 0) {
                    g_input.pressed = false;
                }

                route_pointer_to_remote();
            }
        } while (nread == (ssize_t)sizeof(sample));
    }

    data->point.x = g_input.x;
    data->point.y = g_input.y;
    data->state = g_input.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void pointer_register(lv_display_t *disp, const char *path)
{
    lv_indev_t *indev;

    if (input_open(&g_input, path) < 0) {
        return;
    }

    indev = lv_indev_create();
    if (indev == NULL) {
        fprintf(stderr, "lv_indev_create failed; continue without pointer input\n");
        input_close(&g_input);
        return;
    }

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, pointer_read);
    lv_indev_set_display(indev, disp);
}

static int keyboard_open(struct keyboard_ctx *kbd, const char *path)
{
    kbd->fd = open(path, O_RDONLY | O_NONBLOCK);
    if (kbd->fd < 0) {
        fprintf(stderr, "open %s failed: %s; continue without keyboard\n",
                path, strerror(errno));
        return -1;
    }

    printf("lvgl_desktop: keyboard input %s\n", path);
    return 0;
}

static void keyboard_close(struct keyboard_ctx *kbd)
{
    if (kbd->fd >= 0) {
        close(kbd->fd);
        kbd->fd = -1;
    }
}

static void keyboard_poll(void)
{
    if (g_keyboard.fd >= 0) {
        struct keyboard_event event;
        ssize_t nread;

        do {
            nread = read(g_keyboard.fd, &event, sizeof(event));
            if (nread == (ssize_t)sizeof(event)) {
                remote_send_key(g_focused, event.key, event.action);
            }
        } while (nread == (ssize_t)sizeof(event));
    }
}

static int desktop_listen(void)
{
    struct sockaddr_un addr;
    int fd;

    mkdir(LV_REMOTE_SURFACE_DIR, 0777);
    unlink(LV_REMOTE_SOCKET_PATH);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket AF_UNIX failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", LV_REMOTE_SOCKET_PATH);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind %s failed: %s\n", LV_REMOTE_SOCKET_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        fprintf(stderr, "listen %s failed: %s\n", LV_REMOTE_SOCKET_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    printf("lvgl_desktop: listening %s\n", LV_REMOTE_SOCKET_PATH);
    return fd;
}

static struct remote_window *remote_alloc(int fd)
{
    for (size_t i = 0; i < MAX_REMOTE_WINDOWS; i++) {
        if (!g_windows[i].active) {
            struct remote_window *win = &g_windows[i];

            memset(win, 0, sizeof(*win));
            win->active = true;
            win->fd = fd;
            win->surface_fd = -1;
            win->window_id = g_next_window_id++;
            snprintf(win->title, sizeof(win->title), "Remote App");
            return win;
        }
    }

    return NULL;
}

static void remote_destroy(struct remote_window *win)
{
    if (win == NULL || !win->active) {
        return;
    }

    printf("lvgl_desktop: close window %u\n", win->window_id);
    if (g_focused == win) {
        g_focused = NULL;
    }

    if (win->panel != NULL) {
        lv_obj_delete(win->panel);
    }

    if (win->surface != NULL) {
        munmap(win->surface, win->surface_len);
    }

    if (win->surface_fd >= 0) {
        close(win->surface_fd);
    }

    if (win->fd >= 0) {
        close(win->fd);
    }

    memset(win, 0, sizeof(*win));
    win->fd = -1;
    win->surface_fd = -1;
}

static void remote_create_ui(struct remote_window *win)
{
    int idx = (int)(win - g_windows);
    lv_obj_t *bar;

    if (win->panel != NULL) {
        return;
    }

    win->panel = lv_obj_create(g_workspace);
    lv_obj_set_size(win->panel, win->width + 8, win->height + 34);
    lv_obj_set_pos(win->panel, 20 + idx * 26, 22 + idx * 22);
    lv_obj_set_style_radius(win->panel, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(win->panel, lv_color_hex(0xf8fafc), LV_PART_MAIN);
    lv_obj_set_style_border_width(win->panel, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(win->panel, lv_color_hex(0x64748b), LV_PART_MAIN);
    lv_obj_set_style_pad_all(win->panel, 4, LV_PART_MAIN);

    bar = lv_obj_create(win->panel);
    lv_obj_set_size(bar, LV_PCT(100), 24);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1f2937), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 3, LV_PART_MAIN);

    win->title_label = lv_label_create(bar);
    lv_label_set_text(win->title_label, win->title);
    lv_obj_set_style_text_color(win->title_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(win->title_label, LV_ALIGN_LEFT_MID, 0, 0);

    win->image = lv_image_create(win->panel);
    lv_obj_set_size(win->image, win->width, win->height);
    lv_obj_align(win->image, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static int remote_attach_buffer(struct remote_window *win, const struct lv_remote_msg *msg)
{
    size_t len;

    if (msg->format != LV_REMOTE_FORMAT_RGB565 || msg->width == 0 ||
        msg->height == 0 || msg->stride < msg->width * BYTES_PER_PIXEL) {
        fprintf(stderr, "window %u unsupported surface %ux%u stride=%u format=%u\n",
                win->window_id, msg->width, msg->height, msg->stride, msg->format);
        return -1;
    }

    len = (size_t)msg->stride * msg->height;
    win->surface_fd = open(msg->path, O_RDWR);
    if (win->surface_fd < 0) {
        fprintf(stderr, "open surface %s failed: %s\n", msg->path, strerror(errno));
        return -1;
    }

    win->surface = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
                        win->surface_fd, 0);
    if (win->surface == MAP_FAILED) {
        fprintf(stderr, "mmap surface %s failed: %s\n", msg->path, strerror(errno));
        win->surface = NULL;
        close(win->surface_fd);
        win->surface_fd = -1;
        return -1;
    }

    win->width = msg->width;
    win->height = msg->height;
    win->stride = msg->stride;
    win->format = msg->format;
    win->surface_len = len;
    snprintf(win->path, sizeof(win->path), "%s", msg->path);

    remote_create_ui(win);

    memset(&win->image_dsc, 0, sizeof(win->image_dsc));
    win->image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    win->image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    win->image_dsc.header.w = win->width;
    win->image_dsc.header.h = win->height;
    win->image_dsc.header.stride = win->stride;
    win->image_dsc.data_size = (uint32_t)win->surface_len;
    win->image_dsc.data = win->surface;

    lv_image_set_src(win->image, &win->image_dsc);
    remote_focus(win);
    printf("lvgl_desktop: window %u attached %ux%u stride=%u %s\n",
           win->window_id, win->width, win->height, win->stride, win->path);
    return 0;
}

static void remote_handle_msg(struct remote_window *win,
                              const struct lv_remote_msg *msg)
{
    if (!lv_remote_msg_valid(msg)) {
        fprintf(stderr, "drop invalid remote message on fd %d\n", win->fd);
        return;
    }

    switch (msg->type) {
    case LV_REMOTE_MSG_HELLO:
        printf("lvgl_desktop: window %u hello\n", win->window_id);
        break;
    case LV_REMOTE_MSG_CREATE_WINDOW:
        if (msg->title[0] != '\0') {
            snprintf(win->title, sizeof(win->title), "%s", msg->title);
        }
        if (msg->width > 0) {
            win->width = msg->width;
        }
        if (msg->height > 0) {
            win->height = msg->height;
        }
        printf("lvgl_desktop: window %u create '%s' %ux%u\n",
               win->window_id, win->title, win->width, win->height);
        break;
    case LV_REMOTE_MSG_ATTACH_BUFFER:
        if (remote_attach_buffer(win, msg) < 0) {
            remote_destroy(win);
        }
        break;
    case LV_REMOTE_MSG_COMMIT:
        if (win->image != NULL) {
            lv_obj_invalidate(win->image);
        }
        break;
    case LV_REMOTE_MSG_CLOSE_WINDOW:
        remote_destroy(win);
        break;
    default:
        fprintf(stderr, "window %u unknown message type %u\n",
                win->window_id, msg->type);
        break;
    }
}

static void remote_poll_client(struct remote_window *win)
{
    struct pollfd pfd;
    ssize_t nread;

    pfd.fd = win->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) <= 0) {
        return;
    }

    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        remote_destroy(win);
        return;
    }

    if ((pfd.revents & POLLIN) == 0) {
        return;
    }

    nread = read(win->fd, win->rxbuf + win->rx_len,
                 sizeof(win->rxbuf) - win->rx_len);
    if (nread < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        remote_destroy(win);
        return;
    }

    if (nread == 0) {
        remote_destroy(win);
        return;
    }

    win->rx_len += (size_t)nread;
    if (win->rx_len == sizeof(struct lv_remote_msg)) {
        struct lv_remote_msg msg;

        memcpy(&msg, win->rxbuf, sizeof(msg));
        win->rx_len = 0;
        remote_handle_msg(win, &msg);
    }
}

static void remote_accept_clients(void)
{
    struct pollfd pfd;
    int fd;
    struct remote_window *win;

    pfd.fd = g_server_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLIN) == 0) {
        return;
    }

    fd = accept(g_server_fd, NULL, NULL);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            fprintf(stderr, "accept failed: %s\n", strerror(errno));
        }
        return;
    }

    set_nonblock(fd);
    win = remote_alloc(fd);
    if (win == NULL) {
        fprintf(stderr, "too many remote windows\n");
        close(fd);
        return;
    }

    printf("lvgl_desktop: accepted client window %u\n", win->window_id);
}

static void remote_poll(void)
{
    if (g_server_fd >= 0) {
        remote_accept_clients();
    }

    for (size_t i = 0; i < MAX_REMOTE_WINDOWS; i++) {
        if (g_windows[i].active) {
            remote_poll_client(&g_windows[i]);
        }
    }
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *bar;
    lv_obj_t *title;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f172a), LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xe5e7eb), LV_PART_MAIN);

    bar = lv_obj_create(scr);
    lv_obj_set_size(bar, LV_PCT(100), 40);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 7, LV_PART_MAIN);

    title = lv_label_create(bar);
    lv_label_set_text(title, "Intewell Desktop");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    g_status = lv_label_create(bar);
    lv_label_set_text(g_status, LV_REMOTE_SOCKET_PATH);
    lv_obj_set_style_text_color(g_status, lv_color_hex(0x9ca3af), LV_PART_MAIN);
    lv_obj_align(g_status, LV_ALIGN_RIGHT_MID, 0, 0);

    g_workspace = lv_obj_create(scr);
    lv_obj_set_size(g_workspace, LV_PCT(100), g_fb.height - 40);
    lv_obj_align(g_workspace, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(g_workspace, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_workspace, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_workspace, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_workspace, 0, LV_PART_MAIN);
}

static void parse_args(int argc, char **argv, const char **fbdev,
                       const char **inputdev, const char **kbddev)
{
    *fbdev = DEFAULT_FBDEV;
    *inputdev = DEFAULT_INPUTDEV;
    *kbddev = DEFAULT_KBDDEV;

    if (argc > 1) {
        *fbdev = argv[1];
    }

    if (argc > 2) {
        *inputdev = argv[2];
    }

    if (argc > 3) {
        *kbddev = argv[3];
    }
}

int main(int argc, char **argv)
{
    const char *fbdev;
    const char *inputdev;
    const char *kbddev;
    lv_display_t *disp;
    void *draw_buf1;
    void *draw_buf2;

    parse_args(argc, argv, &fbdev, &inputdev, &kbddev);
    printf("lvgl_desktop: fb=%s input=%s kbd=%s\n", fbdev, inputdev, kbddev);

    if (fb_open(&g_fb, fbdev) < 0) {
        return 1;
    }

    lv_init();
    lv_tick_set_cb(tick_get_ms);

    disp = lv_display_create(g_fb.width, g_fb.height);
    if (disp == NULL) {
        fprintf(stderr, "lv_display_create failed\n");
        fb_close(&g_fb);
        return 1;
    }

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, fb_flush);

    draw_buf1 = g_fb.draw_buf1;
    draw_buf2 = g_fb.draw_buf2;
    lv_display_set_buffers_with_stride(disp, draw_buf1, draw_buf2, g_fb.frame_len,
                                       g_fb.stride, LV_DISPLAY_RENDER_MODE_DIRECT);

    pointer_register(disp, inputdev);
    keyboard_open(&g_keyboard, kbddev);
    ui_create();

    g_server_fd = desktop_listen();
    if (g_server_fd < 0) {
        fprintf(stderr, "lvgl_desktop: remote apps disabled\n");
    }

    for (;;) {
        uint32_t idle = lv_timer_handler();

        keyboard_poll();
        remote_poll();
        sleep_ms(idle == 0 ? 5U : idle > 10U ? 10U : idle);
    }

    fb_close(&g_fb);
    input_close(&g_input);
    keyboard_close(&g_keyboard);
    if (g_server_fd >= 0) {
        close(g_server_fd);
        unlink(LV_REMOTE_SOCKET_PATH);
    }
    return 0;
}
