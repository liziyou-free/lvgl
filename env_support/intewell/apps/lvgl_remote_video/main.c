#define _POSIX_C_SOURCE 200809L

#include "lvgl/lvgl.h"
#include "lvgl/drivers/ffmpeg/lv_ffmpeg.h"
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

#define DEFAULT_WIDTH 800
#define DEFAULT_HEIGHT 480
#define DEFAULT_VIDEO_PATH "/root/video.mp4"
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
};

static struct remote_ctx g_remote = {
    .socket_fd = -1,
    .surface_fd = -1,
};
static const char *g_video_path;
static lv_obj_t *g_player;
static lv_obj_t *g_status_label;
static lv_obj_t *g_path_label;
static bool g_playing;
static bool g_loop = true;

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

    memset(remote->surface, 0, remote->surface_len);
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
    printf("lvgl_remote_video: registered '%s' %ux%u %s\n",
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

static void set_status(const char *text)
{
    if (g_status_label != NULL) {
        lv_label_set_text(g_status_label, text);
    }

    fprintf(stderr, "lvgl_remote_video: status=%s\n", text);
}

static void play_event_cb(lv_event_t *event)
{
    (void)event;

    if (g_player == NULL || g_video_path == NULL) {
        return;
    }

    if (g_playing) {
        lv_ffmpeg_player_set_cmd(g_player, LV_FFMPEG_PLAYER_CMD_PAUSE);
        set_status("Paused");
        g_playing = false;
    }
    else {
        lv_ffmpeg_player_set_cmd(g_player, LV_FFMPEG_PLAYER_CMD_RESUME);
        set_status("Playing");
        g_playing = true;
    }
}

static void stop_event_cb(lv_event_t *event)
{
    (void)event;

    if (g_player == NULL || g_video_path == NULL) {
        return;
    }

    lv_ffmpeg_player_set_cmd(g_player, LV_FFMPEG_PLAYER_CMD_STOP);
    set_status("Stopped");
    g_playing = false;
}

static void loop_event_cb(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_target_obj(event);

    g_loop = !g_loop;
    if (g_player != NULL) {
        lv_ffmpeg_player_set_auto_restart(g_player, g_loop);
    }
    lv_obj_set_style_bg_color(button, lv_color_hex(g_loop ? 0x2563eb : 0x475569),
                              LV_PART_MAIN);
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t cb, uint32_t color)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *label;

    lv_obj_set_size(button, 96, 40);
    lv_obj_set_style_radius(button, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

    label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

static void ready_event_cb(lv_event_t *event)
{
    (void)event;

    set_status(g_loop ? "Restarting" : "Finished");
    g_playing = g_loop;
}

static int file_exists(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *top;
    lv_obj_t *title;
    lv_obj_t *controls;
    lv_obj_t *hint;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b1120), LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xe5e7eb), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, 48);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    title = lv_label_create(top);
    lv_label_set_text(title, "LVGL Video Player");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);

    g_status_label = lv_label_create(top);
    lv_label_set_text(g_status_label, "Idle");
    lv_obj_set_style_text_color(g_status_label, lv_color_hex(0x93c5fd), LV_PART_MAIN);

    if (g_video_path != NULL && file_exists(g_video_path)) {
        g_player = lv_ffmpeg_player_create(scr);
        lv_obj_set_size(g_player, lv_pct(100), g_remote.height - 128);
        lv_obj_set_style_bg_color(g_player, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_player, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_event_cb(g_player, ready_event_cb, LV_EVENT_READY, NULL);

        fprintf(stderr, "lvgl_remote_video: opening %s\n", g_video_path);
        if (lv_ffmpeg_player_set_src(g_player, g_video_path) == LV_RESULT_OK) {
            lv_obj_set_size(g_player, lv_pct(100), g_remote.height - 128);
            lv_image_set_inner_align(g_player, LV_IMAGE_ALIGN_CONTAIN);
            lv_ffmpeg_player_set_auto_restart(g_player, g_loop);
            lv_ffmpeg_player_set_cmd(g_player, LV_FFMPEG_PLAYER_CMD_START);
            lv_obj_invalidate(g_player);
            set_status("Playing");
            g_playing = true;
        }
        else {
            set_status("Open failed");
        }
    }
    else {
        hint = lv_label_create(scr);
        lv_label_set_text(hint, "Run: ./lvgl_remote_video /root/video.mp4");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(0xfacc15), LV_PART_MAIN);
        lv_obj_set_height(hint, g_remote.height - 128);
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        set_status("No file");
    }

    g_path_label = lv_label_create(scr);
    lv_label_set_text(g_path_label, g_video_path != NULL ? g_video_path : "No video file");
    lv_label_set_long_mode(g_path_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(g_path_label, lv_pct(100));
    lv_obj_set_style_text_color(g_path_label, lv_color_hex(0x94a3b8), LV_PART_MAIN);

    controls = lv_obj_create(scr);
    lv_obj_remove_style_all(controls);
    lv_obj_set_width(controls, lv_pct(100));
    lv_obj_set_height(controls, 48);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(controls, 10, LV_PART_MAIN);

    create_button(controls, "Play", play_event_cb, 0x2563eb);
    create_button(controls, "Stop", stop_event_cb, 0xdc2626);
    create_button(controls, "Loop", loop_event_cb, 0x2563eb);
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

static void parse_args(int argc, char **argv, uint32_t *width, uint32_t *height)
{
    *width = DEFAULT_WIDTH;
    *height = DEFAULT_HEIGHT;

    g_video_path = DEFAULT_VIDEO_PATH;

    if (argc > 1 && argv[1][0] != '\0') {
        g_video_path = argv[1];
    }

    if (argc > 2) {
        unsigned long value = strtoul(argv[2], NULL, 0);
        if (value > 0 && value <= 1280) {
            *width = (uint32_t)value;
        }
    }

    if (argc > 3) {
        unsigned long value = strtoul(argv[3], NULL, 0);
        if (value > 0 && value <= 720) {
            *height = (uint32_t)value;
        }
    }
}

int main(int argc, char **argv)
{
    lv_display_t *disp;
    void *draw_buf;

    parse_args(argc, argv, &g_remote.width, &g_remote.height);
    g_remote.stride = g_remote.width * BYTES_PER_PIXEL;
    g_remote.surface_len = (size_t)g_remote.stride * g_remote.height;

    if (remote_create_surface(&g_remote) < 0) {
        return 1;
    }

    g_remote.socket_fd = remote_connect();
    if (g_remote.socket_fd < 0) {
        remote_close_surface(&g_remote);
        return 1;
    }

    draw_buf = malloc(g_remote.surface_len);
    if (draw_buf == NULL) {
        fprintf(stderr, "malloc draw buffer failed\n");
        remote_close_surface(&g_remote);
        return 1;
    }

    lv_init();
    lv_tick_set_cb(tick_get_ms);

    disp = lv_display_create(g_remote.width, g_remote.height);
    if (disp == NULL) {
        fprintf(stderr, "lv_display_create failed\n");
        remote_close_surface(&g_remote);
        return 1;
    }

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, remote_flush);
    lv_display_set_buffers_with_stride(disp, draw_buf, NULL, g_remote.surface_len,
                                       g_remote.stride, LV_DISPLAY_RENDER_MODE_FULL);

    pointer_register(disp);
    ui_create();
    lv_obj_invalidate(lv_screen_active());

    if (remote_register_window("Video Player") < 0) {
        fprintf(stderr, "register window failed: %s\n", strerror(errno));
        remote_close_surface(&g_remote);
        return 1;
    }

    printf("lvgl_remote_video: file=%s %ux%u RGB565 surface=%s\n",
           g_video_path != NULL ? g_video_path : "(none)",
           g_remote.width, g_remote.height, g_remote.surface_path);

    for (;;) {
        uint32_t idle;

        remote_poll();
        idle = lv_timer_handler();
        sleep_ms(idle == 0 ? 2U : idle > 8U ? 8U : idle);
    }

    remote_close_surface(&g_remote);
    return 0;
}
