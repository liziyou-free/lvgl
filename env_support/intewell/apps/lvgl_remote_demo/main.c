#define _POSIX_C_SOURCE 200809L

#include "lvgl/lvgl.h"
#include "lv_remote.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_WIDTH 360
#define DEFAULT_HEIGHT 240
#define BYTES_PER_PIXEL 2

struct remote_ctx {
    int socket_fd;
    int surface_fd;
    uint8_t *surface;
    size_t surface_len;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t serial;
    char surface_path[LV_REMOTE_MAX_PATH];
    uint8_t rxbuf[sizeof(struct lv_remote_msg)];
    size_t rx_len;
    int16_t pointer_x;
    int16_t pointer_y;
    bool pointer_pressed;
    uint32_t last_key;
    bool key_pressed;
};

static struct remote_ctx g_remote = {
    .socket_fd = -1,
    .surface_fd = -1,
};
static lv_obj_t *g_counter_label;
static lv_display_t *g_display;
static void *g_draw_buf;
static int g_counter;

static int remote_resize_surface(uint32_t width, uint32_t height);

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

static int write_all_at(int fd, off_t offset, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    if (lseek(fd, offset, SEEK_SET) < 0) {
        return -1;
    }

    while (len > 0) {
        ssize_t nwritten = write(fd, p, len);

        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (nwritten == 0) {
            errno = EIO;
            return -1;
        }

        p += nwritten;
        len -= (size_t)nwritten;
    }

    return 0;
}

static int remote_connect(void)
{
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket AF_UNIX failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", LV_REMOTE_SOCKET_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect %s failed: %s\n", LV_REMOTE_SOCKET_PATH,
                strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int remote_create_surface(struct remote_ctx *remote)
{
    uint32_t stamp = tick_get_ms();
    int last_errno = EEXIST;

    mkdir(LV_REMOTE_SURFACE_DIR, 0777);

    for (uint32_t attempt = 0; attempt < 64U; attempt++) {
        snprintf(remote->surface_path, sizeof(remote->surface_path),
                 "%s/lvgl-surface-%ld-%08x-%u.fb", LV_REMOTE_SURFACE_DIR,
                 (long)getpid(), stamp, attempt);

        remote->surface_fd = open(remote->surface_path,
                                  O_RDWR | O_CREAT | O_EXCL | O_TRUNC, 0666);
        if (remote->surface_fd >= 0) {
            break;
        }

        last_errno = errno;
        if (errno != EEXIST) {
            break;
        }
    }

    if (remote->surface_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", remote->surface_path,
                strerror(last_errno));
        return -1;
    }

    if (ftruncate(remote->surface_fd, (off_t)remote->surface_len) < 0) {
        fprintf(stderr, "ftruncate %s failed: %s\n", remote->surface_path,
                strerror(errno));
        return -1;
    }

    remote->surface = mmap(NULL, remote->surface_len, PROT_READ | PROT_WRITE,
                           MAP_SHARED, remote->surface_fd, 0);
    if (remote->surface == MAP_FAILED) {
        fprintf(stderr, "mmap %s failed: %s\n", remote->surface_path,
                strerror(errno));
        remote->surface = NULL;
        return -1;
    }

    memset(remote->surface, 0xff, remote->surface_len);
    return 0;
}

static int remote_send_simple(uint32_t type)
{
    struct lv_remote_msg msg;

    lv_remote_msg_init(&msg, type);
    msg.serial = ++g_remote.serial;
    msg.pid = (uint32_t)getpid();
    return lv_remote_send_msg(g_remote.socket_fd, &msg);
}

static int remote_send_attach_buffer(void)
{
    struct lv_remote_msg msg;

    lv_remote_msg_init(&msg, LV_REMOTE_MSG_ATTACH_BUFFER);
    msg.serial = ++g_remote.serial;
    msg.width = g_remote.width;
    msg.height = g_remote.height;
    msg.stride = g_remote.stride;
    msg.format = LV_REMOTE_FORMAT_RGB565;
    msg.pid = (uint32_t)getpid();
    snprintf(msg.path, sizeof(msg.path), "%s", g_remote.surface_path);
    if (lv_remote_send_msg(g_remote.socket_fd, &msg) < 0) {
        fprintf(stderr, "send ATTACH_BUFFER failed: %s\n", strerror(errno));
        return -1;
    }

    set_nonblock(g_remote.socket_fd);
    return 0;
}

static int remote_register_window(const char *title)
{
    struct lv_remote_msg msg;

    if (remote_send_simple(LV_REMOTE_MSG_HELLO) < 0) {
        fprintf(stderr, "send HELLO failed: %s\n", strerror(errno));
        return -1;
    }

    lv_remote_msg_init(&msg, LV_REMOTE_MSG_CREATE_WINDOW);
    msg.serial = ++g_remote.serial;
    msg.width = g_remote.width;
    msg.height = g_remote.height;
    msg.pid = (uint32_t)getpid();
    snprintf(msg.title, sizeof(msg.title), "%s", title);
    if (lv_remote_send_msg(g_remote.socket_fd, &msg) < 0) {
        fprintf(stderr, "send CREATE_WINDOW failed: %s\n", strerror(errno));
        return -1;
    }

    if (remote_send_attach_buffer() < 0) {
        return -1;
    }

    printf("lvgl_remote_demo: registered '%s' %ux%u %s\n",
           title, g_remote.width, g_remote.height, g_remote.surface_path);
    return 0;
}

static void remote_flush(lv_display_t *disp, const lv_area_t *area,
                         uint8_t *px_map)
{
    int32_t x1 = area->x1;
    int32_t y1 = area->y1;
    int32_t x2 = area->x2;
    int32_t y2 = area->y2;
    uint32_t width;
    uint32_t height;
    struct lv_remote_msg msg;

    if (x2 < 0 || y2 < 0 ||
        x1 >= (int32_t)g_remote.width ||
        y1 >= (int32_t)g_remote.height) {
        lv_display_flush_ready(disp);
        return;
    }

    if (x1 < 0) {
        x1 = 0;
    }

    if (y1 < 0) {
        y1 = 0;
    }

    if (x2 >= (int32_t)g_remote.width) {
        x2 = (int32_t)g_remote.width - 1;
    }

    if (y2 >= (int32_t)g_remote.height) {
        y2 = (int32_t)g_remote.height - 1;
    }

    width = (uint32_t)(x2 - x1 + 1);
    height = (uint32_t)(y2 - y1 + 1);

    for (uint32_t row = 0; row < height; row++) {
        size_t offset = ((size_t)y1 + row) * g_remote.stride +
                        (size_t)x1 * BYTES_PER_PIXEL;
        uint8_t *dst = g_remote.surface + ((uint32_t)y1 + row) *
                       g_remote.stride + (uint32_t)x1 * BYTES_PER_PIXEL;
        const uint8_t *src = px_map + offset;

        memcpy(dst, src, width * BYTES_PER_PIXEL);
        if (write_all_at(g_remote.surface_fd, (off_t)offset, dst,
                         (size_t)width * BYTES_PER_PIXEL) < 0) {
            fprintf(stderr, "sync surface failed: %s\n", strerror(errno));
            lv_display_flush_ready(disp);
            return;
        }
    }

    lv_remote_msg_init(&msg, LV_REMOTE_MSG_COMMIT);
    msg.serial = ++g_remote.serial;
    msg.x = x1;
    msg.y = y1;
    msg.w = (int32_t)width;
    msg.h = (int32_t)height;
    msg.pid = (uint32_t)getpid();
    if (lv_remote_send_msg(g_remote.socket_fd, &msg) < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK) {
        fprintf(stderr, "commit failed: %s\n", strerror(errno));
    }

    lv_display_flush_ready(disp);
}

static int16_t clamp_i16(int32_t value, int32_t min, int32_t max)
{
    if (value < min) {
        return (int16_t)min;
    }

    if (value > max) {
        return (int16_t)max;
    }

    return (int16_t)value;
}

static void remote_handle_msg(const struct lv_remote_msg *msg)
{
    if (!lv_remote_msg_valid(msg)) {
        return;
    }

    if (msg->pid != 0U && msg->pid != (uint32_t)getpid()) {
        return;
    }

    switch (msg->type) {
    case LV_REMOTE_MSG_CLOSE_WINDOW:
    case LV_REMOTE_MSG_TERMINATE:
        exit(0);
        break;
    case LV_REMOTE_MSG_INPUT_POINTER:
        g_remote.pointer_x = clamp_i16(msg->x, 0, (int32_t)g_remote.width - 1);
        g_remote.pointer_y = clamp_i16(msg->y, 0, (int32_t)g_remote.height - 1);
        g_remote.pointer_pressed = msg->buttons != 0U;
        break;
    case LV_REMOTE_MSG_INPUT_KEY:
        g_remote.last_key = msg->key;
        g_remote.key_pressed = msg->action == LV_REMOTE_KEY_PRESS;
        break;
    case LV_REMOTE_MSG_CONFIGURE_WINDOW:
        if (remote_resize_surface(msg->width, msg->height) < 0) {
            fprintf(stderr, "resize to %ux%u failed\n",
                    msg->width, msg->height);
        }
        break;
    default:
        break;
    }
}

static void remote_poll(void)
{
    for (;;) {
        ssize_t nread;

        nread = read(g_remote.socket_fd, g_remote.rxbuf + g_remote.rx_len,
                     sizeof(g_remote.rxbuf) - g_remote.rx_len);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            fprintf(stderr, "desktop socket read failed: %s\n", strerror(errno));
            return;
        }

        if (nread == 0) {
            fprintf(stderr, "desktop disconnected\n");
            exit(0);
        }

        g_remote.rx_len += (size_t)nread;
        if (g_remote.rx_len == sizeof(struct lv_remote_msg)) {
            struct lv_remote_msg msg;

            memcpy(&msg, g_remote.rxbuf, sizeof(msg));
            g_remote.rx_len = 0;
            remote_handle_msg(&msg);
        }
    }
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    data->point.x = g_remote.pointer_x;
    data->point.y = g_remote.pointer_y;
    data->state = g_remote.pointer_pressed ? LV_INDEV_STATE_PRESSED :
                  LV_INDEV_STATE_RELEASED;
}

static void pointer_register(lv_display_t *disp)
{
    lv_indev_t *indev = lv_indev_create();

    if (indev == NULL) {
        return;
    }

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, pointer_read);
    lv_indev_set_display(indev, disp);
}

static void counter_event_cb(lv_event_t *event)
{
    (void)event;

    g_counter++;
    lv_label_set_text_fmt(g_counter_label, "Clicked %d", g_counter);
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *title;
    lv_obj_t *button;
    lv_obj_t *button_label;
    lv_obj_t *slider;
    lv_obj_t *bar;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    title = lv_label_create(scr);
    lv_label_set_text(title, "Remote LVGL App");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);

    g_counter_label = lv_label_create(scr);
    lv_label_set_text(g_counter_label, "Clicked 0");
    lv_obj_set_style_text_font(g_counter_label, &lv_font_montserrat_18, LV_PART_MAIN);

    button = lv_button_create(scr);
    lv_obj_set_size(button, 150, 46);
    lv_obj_add_event_cb(button, counter_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2563eb), LV_PART_MAIN);

    button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Click");
    lv_obj_set_style_text_color(button_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_center(button_label);

    slider = lv_slider_create(scr);
    lv_obj_set_width(slider, 260);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 35, LV_ANIM_OFF);

    bar = lv_bar_create(scr);
    lv_obj_set_width(bar, 260);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 70, LV_ANIM_OFF);
}

static void remote_close_surface(struct remote_ctx *remote)
{
    if (remote->surface != NULL) {
        munmap(remote->surface, remote->surface_len);
        remote->surface = NULL;
    }

    if (remote->surface_fd >= 0) {
        close(remote->surface_fd);
        remote->surface_fd = -1;
    }

    if (remote->surface_path[0] != '\0') {
        unlink(remote->surface_path);
        remote->surface_path[0] = '\0';
    }
}

static int remote_resize_surface(uint32_t width, uint32_t height)
{
    void *new_draw_buf;

    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        return -1;
    }

    remote_close_surface(&g_remote);
    free(g_draw_buf);
    g_draw_buf = NULL;

    g_remote.width = width;
    g_remote.height = height;
    g_remote.stride = g_remote.width * BYTES_PER_PIXEL;
    g_remote.surface_len = (size_t)g_remote.stride * g_remote.height;

    if (remote_create_surface(&g_remote) < 0) {
        return -1;
    }

    new_draw_buf = malloc(g_remote.surface_len);
    if (new_draw_buf == NULL) {
        fprintf(stderr, "malloc resized draw buffer failed\n");
        return -1;
    }

    g_draw_buf = new_draw_buf;
    lv_display_set_resolution(g_display, g_remote.width, g_remote.height);
    lv_display_set_buffers_with_stride(g_display, g_draw_buf, NULL,
                                       g_remote.surface_len, g_remote.stride,
                                       LV_DISPLAY_RENDER_MODE_FULL);
    lv_obj_set_size(lv_screen_active(), g_remote.width, g_remote.height);
    lv_obj_invalidate(lv_screen_active());

    if (remote_send_attach_buffer() < 0) {
        return -1;
    }

    printf("lvgl_remote_demo: resized %ux%u %s\n",
           g_remote.width, g_remote.height, g_remote.surface_path);
    return 0;
}

static void parse_args(int argc, char **argv, uint32_t *width, uint32_t *height)
{
    *width = DEFAULT_WIDTH;
    *height = DEFAULT_HEIGHT;

    if (argc > 1) {
        unsigned long value = strtoul(argv[1], NULL, 0);
        if (value > 0 && value <= 800) {
            *width = (uint32_t)value;
        }
    }

    if (argc > 2) {
        unsigned long value = strtoul(argv[2], NULL, 0);
        if (value > 0 && value <= 480) {
            *height = (uint32_t)value;
        }
    }
}

int main(int argc, char **argv)
{
    parse_args(argc, argv, &g_remote.width, &g_remote.height);
    g_remote.stride = g_remote.width * BYTES_PER_PIXEL;
    g_remote.surface_len = (size_t)g_remote.stride * g_remote.height;

    if (remote_create_surface(&g_remote) < 0) {
        return 1;
    }

    g_remote.socket_fd = remote_connect();
    if (g_remote.socket_fd < 0) {
        return 1;
    }

    g_draw_buf = malloc(g_remote.surface_len);
    if (g_draw_buf == NULL) {
        fprintf(stderr, "malloc draw buffer failed\n");
        return 1;
    }

    lv_init();
    lv_tick_set_cb(tick_get_ms);

    g_display = lv_display_create(g_remote.width, g_remote.height);
    if (g_display == NULL) {
        fprintf(stderr, "lv_display_create failed\n");
        return 1;
    }

    lv_display_set_color_format(g_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(g_display, remote_flush);
    lv_display_set_buffers_with_stride(g_display, g_draw_buf, NULL, g_remote.surface_len,
                                       g_remote.stride, LV_DISPLAY_RENDER_MODE_FULL);

    pointer_register(g_display);
    ui_create();
    lv_obj_invalidate(lv_screen_active());

    if (remote_register_window("Remote LVGL Demo") < 0) {
        fprintf(stderr, "register window failed: %s\n", strerror(errno));
        return 1;
    }

    printf("lvgl_remote_demo: %ux%u RGB565 surface=%s socket=%s\n",
           g_remote.width, g_remote.height, g_remote.surface_path,
           LV_REMOTE_SOCKET_PATH);

    for (;;) {
        uint32_t idle;

        remote_poll();
        idle = lv_timer_handler();
        sleep_ms(idle == 0 ? 5U : idle > 10U ? 10U : idle);
    }

    remote_close_surface(&g_remote);
    return 0;
}
