#define _POSIX_C_SOURCE 200809L

#include "lvgl/lvgl.h"
#include "fb_compat.h"
#include "lv_remote.h"
#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_FBDEV "/dev/fb0"
#define DEFAULT_INPUTDEV "/dev/input0"
#define DEFAULT_KBDDEV "/dev/kbd0"
#define DEFAULT_APPDIR "/etc/apps"
#define DEFAULT_WIDTH 800
#define DEFAULT_HEIGHT 480
#define BYTES_PER_PIXEL 2
#define FB_BUFFER_COUNT 2
#define MAX_REMOTE_WINDOWS 8
#define MAX_APPS 16
#define MAX_PENDING_CLIENTS 16
#define MAX_PENDING_MESSAGES 64
#define KEYBOARD_PRESS 1
#define LAUNCHER_WIDTH 180
#define LAUNCHER_BUTTON_HEIGHT 36
#define WINDOW_FRAME_PAD 4
#define WINDOW_TITLE_HEIGHT 24
#define WINDOW_EXTRA_W (WINDOW_FRAME_PAD * 2)
#define WINDOW_EXTRA_H (WINDOW_FRAME_PAD * 2 + WINDOW_TITLE_HEIGHT + 2)
#define WINDOW_RESIZE_HANDLE 16
#define WINDOW_MIN_CONTENT_W 160
#define WINDOW_MIN_CONTENT_H 120

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
    bool last_pressed;
};

struct keyboard_event {
    uint32_t key;
    uint8_t action;
};

struct keyboard_ctx {
    int fd;
    char path[64];
};

struct app_manifest {
    bool loaded;
    int manifest_version;
    char manifest_path[128];
    char app_id[64];
    char name[64];
    char type[32];
    char exec[128];
    char cwd[128];
    char icon[32];
    bool startup_notify;
    bool single_instance;
    bool topmost;
    bool autostart;
    bool hidden;
    lv_obj_t *button;
    pid_t running_pid;
};

struct remote_window {
    bool active;
    bool focused;
    bool topmost;
    int fd;
    int surface_fd;
    uint32_t window_id;
    uint32_t generation;
    uint32_t tx_serial;
    uint32_t width;
    uint32_t height;
    uint32_t pending_width;
    uint32_t pending_height;
    uint32_t stride;
    uint32_t format;
    pid_t pid;
    size_t surface_len;
    uint8_t *surface;
    uint8_t *image_pixels;
    char title[LV_REMOTE_MAX_TITLE];
    char path[LV_REMOTE_MAX_PATH];
    uint8_t rxbuf[sizeof(struct lv_remote_msg)];
    size_t rx_len;
    lv_obj_t *panel;
    lv_obj_t *title_bar;
    lv_obj_t *close_button;
    lv_obj_t *min_button;
    lv_obj_t *zoom_button;
    lv_obj_t *title_label;
    lv_obj_t *image;
    lv_obj_t *resize_handle;
    pthread_t rx_thread;
    bool rx_thread_started;
    pthread_t tx_thread;
    bool tx_thread_started;
    pthread_mutex_t tx_lock;
    pthread_cond_t tx_cond;
    bool tx_stop;
    struct lv_remote_msg tx_queue[64];
    uint8_t tx_head;
    uint8_t tx_tail;
    uint8_t tx_count;
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
static struct app_manifest g_apps[MAX_APPS];
static size_t g_app_count;
static char g_app_dir[128] = DEFAULT_APPDIR;
static struct remote_window g_windows[MAX_REMOTE_WINDOWS];
static struct remote_window *g_focused;
static int g_server_fd = -1;
static uint32_t g_next_window_id = 1;
static uint32_t g_next_window_generation = 1;
static lv_obj_t *g_launcher;
static lv_obj_t *g_workspace;
static lv_obj_t *g_status;

struct drag_state {
    struct remote_window *win;
    int16_t start_x;
    int16_t start_y;
    int32_t panel_x;
    int32_t panel_y;
    bool active;
};

static struct drag_state g_drag;

struct resize_state {
    struct remote_window *win;
    int16_t start_x;
    int16_t start_y;
    uint32_t start_w;
    uint32_t start_h;
    bool active;
};

static struct resize_state g_resize;

struct pending_clients {
    pthread_mutex_t lock;
    int fd[MAX_PENDING_CLIENTS];
    size_t head;
    size_t tail;
    size_t count;
};

static struct pending_clients g_pending_clients = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

struct pending_message {
    size_t slot;
    uint32_t generation;
    bool disconnect;
    struct lv_remote_msg msg;
};

struct pending_messages {
    pthread_mutex_t lock;
    struct pending_message msg[MAX_PENDING_MESSAGES];
    size_t head;
    size_t tail;
    size_t count;
};

static struct pending_messages g_pending_messages = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};
static pthread_t g_accept_thread;
static bool g_accept_thread_started;

static void remote_destroy(struct remote_window *win);
static void remote_composite_surfaces(uint8_t *fb);
static void sleep_ms(uint32_t ms);
static void ui_rebuild_launcher(void);

static struct remote_window *remote_from_panel(lv_obj_t *panel)
{
    for (size_t i = 0; i < MAX_REMOTE_WINDOWS; i++) {
        if (g_windows[i].active && g_windows[i].panel == panel) {
            return &g_windows[i];
        }
    }

    return NULL;
}

static void remote_tx_shutdown(struct remote_window *win)
{
    if (win == NULL) {
        return;
    }

    pthread_mutex_lock(&win->tx_lock);
    win->tx_stop = true;
    pthread_cond_broadcast(&win->tx_cond);
    pthread_mutex_unlock(&win->tx_lock);
}

static bool remote_tx_enqueue(struct remote_window *win,
                              const struct lv_remote_msg *msg)
{
    bool queued = false;

    if (win == NULL || msg == NULL || win->fd < 0) {
        return false;
    }

    pthread_mutex_lock(&win->tx_lock);
    if (!win->tx_stop && win->tx_count < (uint8_t)(sizeof(win->tx_queue) /
                                                   sizeof(win->tx_queue[0]))) {
        win->tx_queue[win->tx_tail] = *msg;
        win->tx_tail = (uint8_t)((win->tx_tail + 1U) %
                                 (sizeof(win->tx_queue) / sizeof(win->tx_queue[0])));
        win->tx_count++;
        queued = true;
        pthread_cond_signal(&win->tx_cond);
    }
    pthread_mutex_unlock(&win->tx_lock);

    return queued;
}

static void *remote_tx_thread_main(void *arg)
{
    struct remote_window *win = arg;

    for (;;) {
        struct lv_remote_msg msg;

        pthread_mutex_lock(&win->tx_lock);
        while (!win->tx_stop && win->tx_count == 0U) {
            pthread_cond_wait(&win->tx_cond, &win->tx_lock);
        }

        if (win->tx_stop && win->tx_count == 0U) {
            pthread_mutex_unlock(&win->tx_lock);
            return NULL;
        }

        msg = win->tx_queue[win->tx_head];
        win->tx_head = (uint8_t)((win->tx_head + 1U) %
                                 (sizeof(win->tx_queue) / sizeof(win->tx_queue[0])));
        win->tx_count--;
        pthread_mutex_unlock(&win->tx_lock);

        if (lv_remote_send_msg(win->fd, &msg) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                sleep_ms(1);
                if (!remote_tx_enqueue(win, &msg)) {
                    fprintf(stderr, "drop queued input for window %u\n",
                            win->window_id);
                }
                continue;
            }

            fprintf(stderr, "send input to window %u failed: %s\n",
                    win->window_id, strerror(errno));
        }
    }
}

static void remote_start_tx_thread(struct remote_window *win)
{
    int ret;

    if (win == NULL || win->tx_thread_started) {
        return;
    }

    ret = pthread_create(&win->tx_thread, NULL, remote_tx_thread_main, win);
    if (ret != 0) {
        fprintf(stderr, "pthread_create window %u tx failed: %s\n",
                win->window_id, strerror(ret));
        return;
    }

    win->tx_thread_started = true;
}

static bool app_manifest_load_file(const char *path, struct app_manifest *app)
{
    char json[4096];
    cJSON *root = NULL;
    cJSON *item;
    int fd;
    ssize_t nread;

    memset(app, 0, sizeof(*app));
    snprintf(app->manifest_path, sizeof(app->manifest_path), "%s", path);
    app->manifest_path[sizeof(app->manifest_path) - 1U] = '\0';
    app->startup_notify = true;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    nread = read(fd, json, sizeof(json) - 1U);
    close(fd);
    if (nread <= 0) {
        return false;
    }

    json[nread] = '\0';
    root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "manifest_version");
    if (!cJSON_IsNumber(item)) {
        goto fail;
    }
    app->manifest_version = item->valueint;
    if (app->manifest_version != 1) {
        goto fail;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "app_id");
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        goto fail;
    }
    snprintf(app->app_id, sizeof(app->app_id), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        goto fail;
    }
    snprintf(app->name, sizeof(app->name), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        goto fail;
    }
    snprintf(app->type, sizeof(app->type), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "exec");
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        goto fail;
    }
    snprintf(app->exec, sizeof(app->exec), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "cwd");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(app->cwd, sizeof(app->cwd), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "icon");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(app->icon, sizeof(app->icon), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "startup_notify");
    if (cJSON_IsBool(item)) {
        app->startup_notify = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "single_instance");
    if (cJSON_IsBool(item)) {
        app->single_instance = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "topmost");
    if (cJSON_IsBool(item)) {
        app->topmost = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "autostart");
    if (cJSON_IsBool(item)) {
        app->autostart = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "hidden");
    if (cJSON_IsBool(item)) {
        app->hidden = cJSON_IsTrue(item);
    }

    app->loaded = true;
    cJSON_Delete(root);
    return true;

fail:
    cJSON_Delete(root);
    return false;
}

static bool app_manifest_duplicate_id(const char *app_id)
{
    for (size_t i = 0; i < g_app_count; i++) {
        if (strcmp(g_apps[i].app_id, app_id) == 0) {
            return true;
        }
    }

    return false;
}

static void app_registry_load(const char *app_dir)
{
    DIR *dir;
    struct dirent *entry;

    g_app_count = 0;
    memset(g_apps, 0, sizeof(g_apps));

    dir = opendir(app_dir);
    if (dir == NULL) {
        fprintf(stderr, "open app dir %s failed: %s\n",
                app_dir, strerror(errno));
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct app_manifest app;
        char path[256];
        size_t len;

        len = strlen(entry->d_name);
        if (len < 6U || strcmp(entry->d_name + len - 5U, ".json") != 0) {
            continue;
        }

        if (g_app_count >= MAX_APPS) {
            break;
        }

        snprintf(path, sizeof(path), "%s/%s", app_dir, entry->d_name);
        if (!app_manifest_load_file(path, &app)) {
            fprintf(stderr, "ignore invalid manifest %s\n", path);
            continue;
        }

        if (app_manifest_duplicate_id(app.app_id)) {
            fprintf(stderr, "ignore duplicate app id %s\n", app.app_id);
            continue;
        }

        g_apps[g_app_count++] = app;
    }

    closedir(dir);
}

static int read_all_at(int fd, off_t offset, void *buf, size_t len)
{
    uint8_t *p = buf;

    if (lseek(fd, offset, SEEK_SET) < 0) {
        return -1;
    }

    while (len > 0) {
        ssize_t nread = read(fd, p, len);

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (nread == 0) {
            errno = EIO;
            return -1;
        }

        p += nread;
        len -= (size_t)nread;
    }

    return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;

    while (len > 0U) {
        ssize_t nread = read(fd, p, len);

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (nread == 0) {
            errno = ECONNRESET;
            return -1;
        }

        p += (size_t)nread;
        len -= (size_t)nread;
    }

    return 0;
}

static void app_mark_pid_exited(pid_t pid)
{
    for (size_t i = 0; i < g_app_count; i++) {
        if (g_apps[i].running_pid == pid) {
            g_apps[i].running_pid = 0;
            return;
        }
    }
}

static void app_reap_children(void)
{
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid <= 0) {
            return;
        }

        app_mark_pid_exited(pid);
    }
}

static int app_launch(struct app_manifest *app)
{
    pid_t pid;

    if (app == NULL || !app->loaded || app->exec[0] == '\0') {
        return -1;
    }

    if (app->single_instance && app->running_pid > 0) {
        return 0;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "launch %s fork failed: %s\n",
                app->app_id, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        const char *cwd = app->cwd[0] != '\0' ? app->cwd : "/";

        chdir(cwd);
        execl("/bin/sh", "sh", "-c", app->exec, (char *)NULL);
        _exit(127);
    }

    app->running_pid = pid;
    printf("lvgl_desktop: launched app %s pid=%ld\n",
           app->app_id, (long)pid);
    return 0;
}

static void launcher_button_event_cb(lv_event_t *event)
{
    struct app_manifest *app = lv_event_get_user_data(event);

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    (void)app_launch(app);
}

static struct remote_window *remote_top_window(void)
{
    uint32_t child_count;

    if (g_workspace == NULL) {
        return NULL;
    }

    child_count = lv_obj_get_child_count(g_workspace);
    for (int32_t i = (int32_t)child_count - 1; i >= 0; i--) {
        lv_obj_t *child = lv_obj_get_child(g_workspace, i);
        struct remote_window *win = remote_from_panel(child);

        if (win != NULL && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            return win;
        }
    }

    return NULL;
}

static bool remote_has_windows(void)
{
    for (size_t i = 0; i < MAX_REMOTE_WINDOWS; i++) {
        if (g_windows[i].active && g_windows[i].panel != NULL) {
            return true;
        }
    }

    return false;
}

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

    if (lv_display_flush_is_last(disp)) {
        remote_composite_surfaces(px_map);

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
    msg.pid = (uint32_t)win->pid;
    msg.x = x;
    msg.y = y;
    msg.buttons = pressed ? 1U : 0U;
    msg.action = pressed ? LV_REMOTE_KEY_PRESS : LV_REMOTE_KEY_RELEASE;
    if (!remote_tx_enqueue(win, &msg)) {
        fprintf(stderr, "queue pointer to window %u failed\n",
                win->window_id);
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
    msg.pid = (uint32_t)win->pid;
    msg.key = key;
    msg.action = action;
    if (!remote_tx_enqueue(win, &msg)) {
        fprintf(stderr, "queue key to window %u failed\n",
                win->window_id);
    }
}

static int32_t remote_panel_width(uint32_t content_w)
{
    return (int32_t)content_w + WINDOW_EXTRA_W;
}

static int32_t remote_panel_height(uint32_t content_h)
{
    return (int32_t)content_h + WINDOW_EXTRA_H;
}

static void remote_resize_ui(struct remote_window *win, uint32_t content_w,
                             uint32_t content_h)
{
    if (win == NULL || win->panel == NULL) {
        return;
    }

    lv_obj_set_size(win->panel, remote_panel_width(content_w),
                    remote_panel_height(content_h));
    if (win->image != NULL) {
        lv_obj_set_size(win->image, content_w, content_h);
        lv_obj_align(win->image, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    if (win->resize_handle != NULL) {
        lv_obj_align(win->resize_handle, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }
}

static void remote_send_configure(struct remote_window *win, uint32_t width,
                                  uint32_t height)
{
    struct lv_remote_msg msg;

    if (win == NULL || !win->active) {
        return;
    }

    lv_remote_msg_init(&msg, LV_REMOTE_MSG_CONFIGURE_WINDOW);
    msg.serial = ++win->tx_serial;
    msg.window_id = win->window_id;
    msg.pid = (uint32_t)win->pid;
    msg.width = width;
    msg.height = height;
    if (!remote_tx_enqueue(win, &msg)) {
        fprintf(stderr, "queue configure to window %u failed\n",
                win->window_id);
        return;
    }

    win->pending_width = width;
    win->pending_height = height;
    printf("lvgl_desktop: configure window %u %ux%u serial=%u\n",
           win->window_id, width, height, msg.serial);
}

static void remote_request_close(struct remote_window *win)
{
    struct lv_remote_msg msg;

    if (win == NULL || !win->active) {
        return;
    }

    lv_remote_msg_init(&msg, LV_REMOTE_MSG_CLOSE_WINDOW);
    msg.window_id = win->window_id;
    msg.pid = (uint32_t)win->pid;
    (void)remote_tx_enqueue(win, &msg);

    if (win->pid > 0) {
        kill(win->pid, SIGTERM);
    }

    remote_destroy(win);
}

static void remote_raise_topmost_windows(void)
{
    if (g_workspace == NULL) {
        return;
    }

    for (uint32_t i = 0; i < lv_obj_get_child_count(g_workspace); i++) {
        struct remote_window *win = remote_from_panel(lv_obj_get_child(g_workspace, i));

        if (win != NULL && win->topmost &&
            !lv_obj_has_flag(win->panel, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_move_to_index(win->panel,
                                 lv_obj_get_child_count(g_workspace) - 1);
        }
    }
}

static void remote_raise_window(struct remote_window *win)
{
    if (win == NULL || win->panel == NULL || g_workspace == NULL) {
        return;
    }

    lv_obj_move_to_index(win->panel, lv_obj_get_child_count(g_workspace) - 1);
    remote_raise_topmost_windows();
}

static void remote_focus(struct remote_window *win)
{
    if (g_focused != NULL && g_focused != win && g_focused->panel != NULL) {
        g_focused->focused = false;
        lv_obj_set_style_border_color(g_focused->panel, lv_color_hex(0x64748b),
                                      LV_PART_MAIN);
    }

    g_focused = win;
    if (g_focused != NULL && g_focused->panel != NULL) {
        g_focused->focused = true;
        lv_obj_set_style_border_color(g_focused->panel, lv_color_hex(0x2563eb),
                                      LV_PART_MAIN);
        remote_raise_window(g_focused);
    }
}

static struct remote_window *remote_from_obj(lv_obj_t *obj)
{
    for (size_t i = 0; i < MAX_REMOTE_WINDOWS; i++) {
        struct remote_window *win = &g_windows[i];

        if (!win->active) {
            continue;
        }

        if (obj == win->panel || obj == win->title_bar ||
            obj == win->close_button || obj == win->min_button ||
            obj == win->zoom_button || obj == win->title_label ||
            obj == win->image || obj == win->resize_handle) {
            return win;
        }
    }

    return NULL;
}

static void window_close_event_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    struct remote_window *win = remote_from_obj(target);

    remote_request_close(win);
}

static void window_focus_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);
    struct remote_window *win = remote_from_obj(target);

    if (code == LV_EVENT_PRESSED) {
        remote_focus(win);
    }
}

static void window_drag_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);
    struct remote_window *win = remote_from_obj(target);

    if (win == NULL || win->panel == NULL) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        remote_focus(win);
        g_drag.win = win;
        g_drag.start_x = g_input.x;
        g_drag.start_y = g_input.y;
        g_drag.panel_x = lv_obj_get_x(win->panel);
        g_drag.panel_y = lv_obj_get_y(win->panel);
        g_drag.active = true;
    } else if (code == LV_EVENT_PRESSING && g_drag.active && g_drag.win == win) {
        int32_t next_x = g_drag.panel_x + (int32_t)g_input.x - g_drag.start_x;
        int32_t next_y = g_drag.panel_y + (int32_t)g_input.y - g_drag.start_y;
        int32_t max_x = (int32_t)lv_obj_get_width(g_workspace) -
                        (int32_t)lv_obj_get_width(win->panel);
        int32_t max_y = (int32_t)lv_obj_get_height(g_workspace) -
                        (int32_t)lv_obj_get_height(win->panel);

        if (max_x < 0) {
            max_x = 0;
        }

        if (max_y < 0) {
            max_y = 0;
        }

        if (next_x < 0) {
            next_x = 0;
        } else if (next_x > max_x) {
            next_x = max_x;
        }

        if (next_y < 0) {
            next_y = 0;
        } else if (next_y > max_y) {
            next_y = max_y;
        }

        lv_obj_set_pos(win->panel, next_x, next_y);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (g_drag.win == win) {
            memset(&g_drag, 0, sizeof(g_drag));
        }
    }
}

static void window_resize_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);
    struct remote_window *win = remote_from_obj(target);

    if (win == NULL || win->panel == NULL) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        remote_focus(win);
        g_resize.win = win;
        g_resize.start_x = g_input.x;
        g_resize.start_y = g_input.y;
        g_resize.start_w = (uint32_t)lv_obj_get_width(win->image);
        g_resize.start_h = (uint32_t)lv_obj_get_height(win->image);
        g_resize.active = true;
    } else if (code == LV_EVENT_PRESSING && g_resize.active &&
               g_resize.win == win) {
        int32_t next_w = (int32_t)g_resize.start_w +
                         (int32_t)g_input.x - g_resize.start_x;
        int32_t next_h = (int32_t)g_resize.start_h +
                         (int32_t)g_input.y - g_resize.start_y;
        int32_t max_w = (int32_t)lv_obj_get_width(g_workspace) -
                        lv_obj_get_x(win->panel) - WINDOW_EXTRA_W;
        int32_t max_h = (int32_t)lv_obj_get_height(g_workspace) -
                        lv_obj_get_y(win->panel) - WINDOW_EXTRA_H;

        if (max_w < WINDOW_MIN_CONTENT_W) {
            max_w = WINDOW_MIN_CONTENT_W;
        }

        if (max_h < WINDOW_MIN_CONTENT_H) {
            max_h = WINDOW_MIN_CONTENT_H;
        }

        if (next_w < WINDOW_MIN_CONTENT_W) {
            next_w = WINDOW_MIN_CONTENT_W;
        } else if (next_w > max_w) {
            next_w = max_w;
        }

        if (next_h < WINDOW_MIN_CONTENT_H) {
            next_h = WINDOW_MIN_CONTENT_H;
        } else if (next_h > max_h) {
            next_h = max_h;
        }

        remote_resize_ui(win, (uint32_t)next_w, (uint32_t)next_h);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (g_resize.win == win) {
            uint32_t next_w = (uint32_t)lv_obj_get_width(win->image);
            uint32_t next_h = (uint32_t)lv_obj_get_height(win->image);

            if (next_w != win->width || next_h != win->height) {
                remote_send_configure(win, next_w, next_h);
            }
            memset(&g_resize, 0, sizeof(g_resize));
        }
    }
}

static void remote_surface_draw_cb(lv_event_t *event)
{
    (void)event;
}

static void remote_pick_initial_position(struct remote_window *win,
                                         int32_t panel_w, int32_t panel_h,
                                         int32_t *out_x, int32_t *out_y)
{
    static uint32_t spawn_serial;
    const int32_t start = 20;
    const int32_t step = 28;
    int32_t max_x = (int32_t)lv_obj_get_width(g_workspace) - panel_w;
    int32_t max_y = (int32_t)lv_obj_get_height(g_workspace) - panel_h;
    int32_t x = start + (int32_t)(spawn_serial * step);
    int32_t y = start + (int32_t)(spawn_serial * step);

    (void)win;

    if (max_x < 0) {
        max_x = 0;
    }

    if (max_y < 0) {
        max_y = 0;
    }

    spawn_serial++;
    if (x > max_x || y > max_y) {
        x = start + (int32_t)((spawn_serial % 4U) * step);
        y = start + (int32_t)((spawn_serial % 4U) * step);
    }

    if (x > max_x) {
        x = max_x;
    }

    if (y > max_y) {
        y = max_y;
    }

    *out_x = x < 0 ? 0 : x;
    *out_y = y < 0 ? 0 : y;
}

static void remote_composite_window(struct remote_window *win, uint8_t *fb,
                                    uint32_t child_index)
{
    lv_area_t area;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint32_t width;
    uint32_t height;
    static int composite_log_count;

    if (win == NULL || !win->active || win->image == NULL ||
        win->image_pixels == NULL || fb == NULL ||
        win->width == 0 || win->height == 0) {
        return;
    }

    lv_obj_get_coords(win->image, &area);
    area.x2 = area.x1 + (int32_t)win->width - 1;
    area.y2 = area.y1 + (int32_t)win->height - 1;
    x1 = area.x1;
    y1 = area.y1;
    x2 = area.x2;
    y2 = area.y2;

    if (x2 < 0 || y2 < 0 ||
        x1 >= (int32_t)g_fb.width ||
        y1 >= (int32_t)g_fb.height) {
        return;
    }

    if (x1 < 0) {
        x1 = 0;
    }

    if (y1 < 0) {
        y1 = 0;
    }

    if (x2 >= (int32_t)g_fb.width) {
        x2 = (int32_t)g_fb.width - 1;
    }

    if (y2 >= (int32_t)g_fb.height) {
        y2 = (int32_t)g_fb.height - 1;
    }

    if (x1 > x2 || y1 > y2) {
        return;
    }

    width = (uint32_t)(x2 - x1 + 1);
    height = (uint32_t)(y2 - y1 + 1);

    for (uint32_t row = 0; row < height; row++) {
        uint32_t src_y = (uint32_t)(y1 - area.y1) + row;
        int32_t seg_x1[MAX_REMOTE_WINDOWS + 1];
        int32_t seg_x2[MAX_REMOTE_WINDOWS + 1];
        size_t seg_count = 1;
        uint32_t child_count = lv_obj_get_child_count(g_workspace);

        seg_x1[0] = x1;
        seg_x2[0] = x2;

        for (uint32_t child = child_index + 1;
             child < child_count && seg_count > 0; child++) {
            lv_obj_t *cover_panel = lv_obj_get_child(g_workspace, child);
            struct remote_window *cover_win = remote_from_panel(cover_panel);
            lv_area_t cover;
            size_t next_count = 0;

            if (cover_win == NULL ||
                lv_obj_has_flag(cover_panel, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }

            lv_obj_get_coords(cover_panel, &cover);
            if ((int32_t)(y1 + (int32_t)row) < cover.y1 ||
                (int32_t)(y1 + (int32_t)row) > cover.y2 ||
                cover.x2 < x1 || cover.x1 > x2) {
                continue;
            }

            for (size_t seg = 0; seg < seg_count; seg++) {
                int32_t ox1 = cover.x1 > seg_x1[seg] ? cover.x1 : seg_x1[seg];
                int32_t ox2 = cover.x2 < seg_x2[seg] ? cover.x2 : seg_x2[seg];

                if (ox1 > ox2) {
                    seg_x1[next_count] = seg_x1[seg];
                    seg_x2[next_count] = seg_x2[seg];
                    next_count++;
                    continue;
                }

                if (seg_x1[seg] < ox1 && next_count < MAX_REMOTE_WINDOWS + 1) {
                    seg_x1[next_count] = seg_x1[seg];
                    seg_x2[next_count] = ox1 - 1;
                    next_count++;
                }

                if (ox2 < seg_x2[seg] && next_count < MAX_REMOTE_WINDOWS + 1) {
                    seg_x1[next_count] = ox2 + 1;
                    seg_x2[next_count] = seg_x2[seg];
                    next_count++;
                }
            }

            seg_count = next_count;
        }

        for (size_t seg = 0; seg < seg_count; seg++) {
            uint32_t src_x = (uint32_t)(seg_x1[seg] - area.x1);
            uint32_t copy_width = (uint32_t)(seg_x2[seg] - seg_x1[seg] + 1);
            const uint8_t *src = win->image_pixels + (size_t)src_y * win->stride +
                                 (size_t)src_x * BYTES_PER_PIXEL;
            uint8_t *dst = fb + (size_t)(y1 + (int32_t)row) * g_fb.stride +
                           (size_t)seg_x1[seg] * BYTES_PER_PIXEL;

            memcpy(dst, src, (size_t)copy_width * BYTES_PER_PIXEL);
        }
    }

    if (composite_log_count < 16) {
        printf("lvgl_desktop: composite window %u dst %ld,%ld %ux%u sample=%04x\n",
               win->window_id, (long)x1, (long)y1, width, height,
               ((uint16_t *)win->image_pixels)[0]);
        fflush(stdout);
        composite_log_count++;
    }
}

static void remote_composite_surfaces(uint8_t *fb)
{
    if (g_workspace == NULL) {
        return;
    }

    for (uint32_t i = 0; i < lv_obj_get_child_count(g_workspace); i++) {
        lv_obj_t *child = lv_obj_get_child(g_workspace, i);

        for (size_t j = 0; j < MAX_REMOTE_WINDOWS; j++) {
            if (g_windows[j].active && g_windows[j].panel == child) {
                remote_composite_window(&g_windows[j], fb, i);
                break;
            }
        }
    }
}

static void route_pointer_to_remote(void)
{
    uint32_t child_count;

    if (g_workspace == NULL) {
        return;
    }

    child_count = lv_obj_get_child_count(g_workspace);
    for (int32_t i = (int32_t)child_count - 1; i >= 0; i--) {
        lv_obj_t *child = lv_obj_get_child(g_workspace, i);
        struct remote_window *win = remote_from_panel(child);
        lv_area_t coords;

        if (win == NULL || !win->active || win->image == NULL ||
            win->surface_fd < 0 || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }

        if (g_drag.active || g_resize.active) {
            return;
        }

        lv_obj_get_coords(win->image, &coords);
        if (g_input.x < coords.x1 || g_input.x > coords.x2 ||
            g_input.y < coords.y1 || g_input.y > coords.y2) {
            continue;
        }

        if (g_input.pressed && !g_input.last_pressed) {
            remote_focus(win);
        }
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

                g_input.last_pressed = g_input.pressed;
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
    if (kbd->fd < 0 && strcmp(path, "/dev/kbd0") == 0) {
        kbd->fd = open("/dev/kbd", O_RDONLY | O_NONBLOCK);
        if (kbd->fd >= 0) {
            path = "/dev/kbd";
        }
    }

    if (kbd->fd < 0) {
        fprintf(stderr, "open %s failed: %s; continue without keyboard\n",
                path, strerror(errno));
        return -1;
    }

    snprintf(kbd->path, sizeof(kbd->path), "%s", path);
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
            memset(&event, 0, sizeof(event));
            nread = read(g_keyboard.fd, &event, sizeof(event));
            if (nread == (ssize_t)sizeof(event)) {
                struct remote_window *target = g_focused != NULL ?
                                               g_focused : remote_top_window();

                if (target != NULL) {
                    if (target != g_focused) {
                        remote_focus(target);
                    }
                    remote_send_key(target, event.key, event.action);
                } else {
                    printf("lvgl_desktop: key %u action=%u dropped: no window\n",
                           event.key, event.action);
                    fflush(stdout);
                }
            } else if (nread > 0) {
                printf("lvgl_desktop: short keyboard read %zd from %s\n",
                       nread, g_keyboard.path);
                fflush(stdout);
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

    printf("lvgl_desktop: listening %s\n", LV_REMOTE_SOCKET_PATH);
    return fd;
}

static bool pending_push_client(int fd)
{
    bool queued = false;

    pthread_mutex_lock(&g_pending_clients.lock);
    if (g_pending_clients.count < MAX_PENDING_CLIENTS) {
        g_pending_clients.fd[g_pending_clients.tail] = fd;
        g_pending_clients.tail = (g_pending_clients.tail + 1U) % MAX_PENDING_CLIENTS;
        g_pending_clients.count++;
        queued = true;
    }
    pthread_mutex_unlock(&g_pending_clients.lock);

    return queued;
}

static int pending_pop_client(void)
{
    int fd = -1;

    pthread_mutex_lock(&g_pending_clients.lock);
    if (g_pending_clients.count > 0U) {
        fd = g_pending_clients.fd[g_pending_clients.head];
        g_pending_clients.head = (g_pending_clients.head + 1U) % MAX_PENDING_CLIENTS;
        g_pending_clients.count--;
    }
    pthread_mutex_unlock(&g_pending_clients.lock);

    return fd;
}

static bool pending_push_message(size_t slot, uint32_t generation,
                                 const struct lv_remote_msg *msg,
                                 bool disconnect)
{
    bool queued = false;

    pthread_mutex_lock(&g_pending_messages.lock);
    if (g_pending_messages.count < MAX_PENDING_MESSAGES) {
        struct pending_message *pending =
            &g_pending_messages.msg[g_pending_messages.tail];

        pending->slot = slot;
        pending->generation = generation;
        pending->disconnect = disconnect;
        if (msg != NULL) {
            pending->msg = *msg;
        } else {
            memset(&pending->msg, 0, sizeof(pending->msg));
        }
        g_pending_messages.tail =
            (g_pending_messages.tail + 1U) % MAX_PENDING_MESSAGES;
        g_pending_messages.count++;
        queued = true;
    }
    pthread_mutex_unlock(&g_pending_messages.lock);

    return queued;
}

static bool pending_pop_message(struct pending_message *out)
{
    bool found = false;

    pthread_mutex_lock(&g_pending_messages.lock);
    if (g_pending_messages.count > 0U) {
        *out = g_pending_messages.msg[g_pending_messages.head];
        g_pending_messages.head =
            (g_pending_messages.head + 1U) % MAX_PENDING_MESSAGES;
        g_pending_messages.count--;
        found = true;
    }
    pthread_mutex_unlock(&g_pending_messages.lock);

    return found;
}

static void *remote_client_thread_main(void *arg)
{
    uintptr_t slot_value = (uintptr_t)arg;
    size_t slot = (size_t)slot_value;
    struct remote_window *win = &g_windows[slot];
    uint32_t generation = win->generation;
    uint32_t window_id = win->window_id;
    int fd = win->fd;
    unsigned int rx_log_count = 0;

    for (;;) {
        struct lv_remote_msg msg;

        if (read_full(fd, &msg, sizeof(msg)) < 0) {
            pending_push_message(slot, generation, NULL, true);
            return NULL;
        }

        if (rx_log_count < 16U || msg.type != LV_REMOTE_MSG_COMMIT) {
            printf("lvgl_desktop: rx slot=%zu window=%u gen=%u fd=%d type=%u pid=%u\n",
                   slot, window_id, generation, fd, msg.type, msg.pid);
            fflush(stdout);
            rx_log_count++;
        }

        if (!pending_push_message(slot, generation, &msg, false)) {
            fprintf(stderr, "remote message queue full for window %u\n",
                    window_id);
            pending_push_message(slot, generation, NULL, true);
            return NULL;
        }
    }
}

static void remote_start_client_thread(size_t slot)
{
    int ret;
    struct remote_window *win = &g_windows[slot];

    ret = pthread_create(&win->rx_thread, NULL, remote_client_thread_main,
                         (void *)(uintptr_t)slot);
    if (ret != 0) {
        fprintf(stderr, "pthread_create window %u rx failed: %s\n",
                win->window_id, strerror(ret));
        remote_destroy(win);
        return;
    }

    win->rx_thread_started = true;
}

static void *remote_accept_thread_main(void *arg)
{
    int server_fd = *(int *)arg;

    for (;;) {
        int fd = accept(server_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "accept failed: %s\n", strerror(errno));
            sleep_ms(20);
            continue;
        }

        if (!pending_push_client(fd)) {
            fprintf(stderr, "remote client queue full; dropping fd %d\n", fd);
            close(fd);
            continue;
        }

        printf("lvgl_desktop: accepted remote fd %d\n", fd);
        fflush(stdout);
    }

    return NULL;
}

static void remote_start_accept_thread(void)
{
    int ret;

    if (g_server_fd < 0 || g_accept_thread_started) {
        return;
    }

    ret = pthread_create(&g_accept_thread, NULL, remote_accept_thread_main,
                         &g_server_fd);
    if (ret != 0) {
        fprintf(stderr, "pthread_create accept failed: %s\n", strerror(ret));
        return;
    }

    g_accept_thread_started = true;
    printf("lvgl_desktop: accept thread started\n");
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
            win->generation = g_next_window_generation++;
            pthread_mutex_init(&win->tx_lock, NULL);
            pthread_cond_init(&win->tx_cond, NULL);
            snprintf(win->title, sizeof(win->title), "Remote App");
            remote_start_tx_thread(win);
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

    if (win->path[0] != '\0') {
        unlink(win->path);
    }

    free(win->image_pixels);

    remote_tx_shutdown(win);
    if (win->tx_thread_started) {
        pthread_join(win->tx_thread, NULL);
        win->tx_thread_started = false;
    }
    pthread_cond_destroy(&win->tx_cond);
    pthread_mutex_destroy(&win->tx_lock);

    if (win->fd >= 0) {
        close(win->fd);
    }

    memset(win, 0, sizeof(*win));
    win->fd = -1;
    win->surface_fd = -1;
}

static void remote_create_ui(struct remote_window *win)
{
    int32_t panel_w = remote_panel_width(win->width);
    int32_t panel_h = remote_panel_height(win->height);
    int32_t panel_x;
    int32_t panel_y;
    lv_obj_t *btn;

    if (win->panel != NULL) {
        return;
    }

    remote_pick_initial_position(win, panel_w, panel_h, &panel_x, &panel_y);

    win->panel = lv_obj_create(g_workspace);
    lv_obj_set_size(win->panel, panel_w, panel_h);
    lv_obj_set_pos(win->panel, panel_x, panel_y);
    lv_obj_add_flag(win->panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(win->panel, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(win->panel, lv_color_hex(0xf8fafc), LV_PART_MAIN);
    lv_obj_set_style_border_width(win->panel, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(win->panel, lv_color_hex(0x64748b), LV_PART_MAIN);
    lv_obj_set_style_pad_all(win->panel, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(win->panel, window_focus_event_cb, LV_EVENT_PRESSED, NULL);

    win->title_bar = lv_obj_create(win->panel);
    lv_obj_set_size(win->title_bar, LV_PCT(100), 24);
    lv_obj_align(win->title_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(win->title_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(win->title_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(win->title_bar, lv_color_hex(0x1f2937), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(win->title_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(win->title_bar, 3, LV_PART_MAIN);
    lv_obj_add_event_cb(win->title_bar, window_drag_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(win->title_bar, window_drag_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(win->title_bar, window_drag_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(win->title_bar, window_drag_event_cb, LV_EVENT_PRESS_LOST, NULL);

    btn = lv_button_create(win->title_bar);
    win->close_button = btn;
    lv_obj_set_size(btn, 12, 12);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xff5f57), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, window_close_event_cb, LV_EVENT_CLICKED, NULL);

    btn = lv_button_create(win->title_bar);
    win->min_button = btn;
    lv_obj_set_size(btn, 12, 12);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xffbd2e), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);

    btn = lv_button_create(win->title_bar);
    win->zoom_button = btn;
    lv_obj_set_size(btn, 12, 12);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 36, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x28c840), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);

    win->title_label = lv_label_create(win->title_bar);
    lv_label_set_text_fmt(win->title_label, "%s #%u", win->title, win->window_id);
    lv_obj_set_style_text_color(win->title_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(win->title_label, LV_ALIGN_LEFT_MID, 58, 0);

    win->image = lv_obj_create(win->panel);
    lv_obj_set_size(win->image, win->width, win->height);
    lv_obj_align(win->image, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(win->image, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(win->image, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(win->image, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(win->image, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(win->image, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(win->image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(win->image, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(win->image, window_focus_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(win->image, remote_surface_draw_cb, LV_EVENT_DRAW_POST,
                        NULL);

    win->resize_handle = lv_obj_create(win->panel);
    lv_obj_set_size(win->resize_handle, WINDOW_RESIZE_HANDLE, WINDOW_RESIZE_HANDLE);
    lv_obj_align(win->resize_handle, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(win->resize_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(win->resize_handle, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(win->resize_handle, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(win->resize_handle, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(win->resize_handle, lv_color_hex(0x94a3b8), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(win->resize_handle, LV_OPA_60, LV_PART_MAIN);
    lv_obj_add_event_cb(win->resize_handle, window_resize_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(win->resize_handle, window_resize_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(win->resize_handle, window_resize_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(win->resize_handle, window_resize_event_cb, LV_EVENT_PRESS_LOST, NULL);
}

static int remote_attach_buffer(struct remote_window *win, const struct lv_remote_msg *msg)
{
    size_t len;
    uint8_t *pixels;
    int fd;

    if (msg->format != LV_REMOTE_FORMAT_RGB565 || msg->width == 0 ||
        msg->height == 0 || msg->stride < msg->width * BYTES_PER_PIXEL) {
        fprintf(stderr, "window %u unsupported surface %ux%u stride=%u format=%u\n",
                win->window_id, msg->width, msg->height, msg->stride, msg->format);
        return -1;
    }

    len = (size_t)msg->stride * msg->height;
    fd = open(msg->path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open surface %s failed: %s\n", msg->path, strerror(errno));
        return -1;
    }

    pixels = malloc(len);
    if (pixels == NULL) {
        close(fd);
        fprintf(stderr, "malloc image pixels for window %u failed\n",
                win->window_id);
        return -1;
    }

    if (read_all_at(fd, 0, pixels, len) < 0) {
        fprintf(stderr, "read surface %s failed: %s; start with black frame\n",
                msg->path, strerror(errno));
        memset(pixels, 0, len);
    }

    if (win->surface != NULL) {
        munmap(win->surface, win->surface_len);
        win->surface = NULL;
    }
    if (win->surface_fd >= 0) {
        close(win->surface_fd);
    }
    free(win->image_pixels);

    win->surface_fd = fd;
    win->width = msg->width;
    win->height = msg->height;
    win->pending_width = 0;
    win->pending_height = 0;
    win->stride = msg->stride;
    win->format = msg->format;
    win->surface_len = len;
    snprintf(win->path, sizeof(win->path), "%s", msg->path);
    win->image_pixels = pixels;

    remote_create_ui(win);
    remote_resize_ui(win, win->width, win->height);

    remote_focus(win);
    lv_obj_invalidate(win->image);
    printf("lvgl_desktop: window %u attached %ux%u stride=%u pos=%ld,%ld %s\n",
           win->window_id, win->width, win->height, win->stride,
           (long)lv_obj_get_x(win->panel), (long)lv_obj_get_y(win->panel),
           win->path);
    return 0;
}

static void remote_copy_surface(struct remote_window *win,
                                const struct lv_remote_msg *msg)
{
    int32_t x1 = msg->x;
    int32_t y1 = msg->y;
    int32_t x2 = msg->x + msg->w - 1;
    int32_t y2 = msg->y + msg->h - 1;
    uint32_t width;
    uint32_t height;
    static int copy_log_count;

    if (win->surface_fd < 0 || win->image_pixels == NULL ||
        msg->w <= 0 || msg->h <= 0) {
        return;
    }

    if (x2 < 0 || y2 < 0 ||
        x1 >= (int32_t)win->width ||
        y1 >= (int32_t)win->height) {
        return;
    }

    if (x1 < 0) {
        x1 = 0;
    }

    if (y1 < 0) {
        y1 = 0;
    }

    if (x2 >= (int32_t)win->width) {
        x2 = (int32_t)win->width - 1;
    }

    if (y2 >= (int32_t)win->height) {
        y2 = (int32_t)win->height - 1;
    }

    width = (uint32_t)(x2 - x1 + 1);
    height = (uint32_t)(y2 - y1 + 1);

    for (uint32_t row = 0; row < height; row++) {
        size_t offset = ((size_t)y1 + row) * win->stride +
                        (size_t)x1 * BYTES_PER_PIXEL;

        if (read_all_at(win->surface_fd, (off_t)offset,
                        win->image_pixels + offset,
                        (size_t)width * BYTES_PER_PIXEL) < 0) {
            fprintf(stderr, "read surface row for window %u failed: %s\n",
                    win->window_id, strerror(errno));
            return;
        }
    }

    if (copy_log_count < 8) {
        printf("lvgl_desktop: copied window %u area %ld,%ld %ux%u sample=%04x\n",
               win->window_id, (long)x1, (long)y1, width, height,
               ((uint16_t *)win->image_pixels)[0]);
        fflush(stdout);
        copy_log_count++;
    }
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
        printf("lvgl_desktop: window %u hello pid=%u\n",
               win->window_id, msg->pid);
        break;
    case LV_REMOTE_MSG_CREATE_WINDOW:
        if (msg->title[0] != '\0') {
            snprintf(win->title, sizeof(win->title), "%s", msg->title);
        }
        if (msg->pid != 0U) {
            win->pid = (pid_t)msg->pid;
        }
        if (msg->width > 0) {
            win->width = msg->width;
        }
        if (msg->height > 0) {
            win->height = msg->height;
        }
        win->topmost = (msg->flags & LV_REMOTE_WINDOW_TOPMOST) != 0U;
        printf("lvgl_desktop: window %u create '%s' %ux%u pid=%ld\n",
               win->window_id, win->title, win->width, win->height,
               (long)win->pid);
        break;
    case LV_REMOTE_MSG_ATTACH_BUFFER:
        if (remote_attach_buffer(win, msg) < 0) {
            remote_destroy(win);
        }
        break;
    case LV_REMOTE_MSG_COMMIT:
        if (win->image != NULL) {
            remote_copy_surface(win, msg);
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

static void remote_process_messages(void)
{
    static unsigned int dispatch_log_count;

    for (;;) {
        struct pending_message pending;
        struct remote_window *win;

        if (!pending_pop_message(&pending)) {
            return;
        }

        if (pending.slot >= MAX_REMOTE_WINDOWS) {
            continue;
        }

        win = &g_windows[pending.slot];
        if (!win->active || win->generation != pending.generation) {
            continue;
        }

        if (pending.disconnect) {
            remote_destroy(win);
            continue;
        }

        if (pending.msg.pid != 0U && win->pid == 0) {
            win->pid = (pid_t)pending.msg.pid;
        } else if (pending.msg.pid != 0U &&
                   win->pid != (pid_t)pending.msg.pid) {
            fprintf(stderr,
                    "lvgl_desktop: keep fd-bound window %u; ignore pid remap %ld -> %u\n",
                    win->window_id, (long)win->pid, pending.msg.pid);
        }

        if (dispatch_log_count < 64U ||
            pending.msg.type != LV_REMOTE_MSG_COMMIT) {
            printf("lvgl_desktop: dispatch slot=%zu -> window=%u gen=%u type=%u pid=%u\n",
                   pending.slot, win->window_id, win->generation,
                   pending.msg.type, pending.msg.pid);
            fflush(stdout);
            dispatch_log_count++;
        }
        remote_handle_msg(win, &pending.msg);
    }
}

static void remote_accept_clients(void)
{
    int fd;

    for (;;) {
        struct remote_window *win;
        size_t slot;

        fd = pending_pop_client();
        if (fd < 0) {
            return;
        }

        win = remote_alloc(fd);
        if (win == NULL) {
            fprintf(stderr, "too many remote windows\n");
            close(fd);
            continue;
        }

        slot = (size_t)(win - g_windows);
        printf("lvgl_desktop: registered client slot=%zu window=%u gen=%u fd=%d\n",
               slot, win->window_id, win->generation, fd);
        remote_start_client_thread(slot);
    }
}

static void remote_poll(void)
{
    if (g_server_fd >= 0) {
        remote_accept_clients();
    }

    remote_process_messages();
}

static void ui_rebuild_launcher(void)
{
    bool added = false;

    if (g_launcher == NULL) {
        return;
    }

    while (lv_obj_get_child_count(g_launcher) > 0U) {
        lv_obj_delete(lv_obj_get_child(g_launcher, 0));
    }

    for (size_t i = 0; i < g_app_count; i++) {
        lv_obj_t *btn;
        lv_obj_t *label;
        struct app_manifest *app = &g_apps[i];

        if (app->hidden) {
            continue;
        }

        btn = lv_button_create(g_launcher);
        app->button = btn;
        lv_obj_set_size(btn, LV_PCT(100), LAUNCHER_BUTTON_HEIGHT);
        lv_obj_add_event_cb(btn, launcher_button_event_cb, LV_EVENT_CLICKED, app);
        added = true;

        label = lv_label_create(btn);
        lv_label_set_text(label, app->name);
        lv_obj_center(label);
    }

    if (!added) {
        lv_obj_t *label = lv_label_create(g_launcher);
        char text[192];

        snprintf(text, sizeof(text), "No apps\n%s", g_app_dir);
        lv_label_set_text(label, text);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_color(label, lv_color_hex(0xcbd5e1), LV_PART_MAIN);
    }
}

static void app_registry_autostart(void)
{
    for (size_t i = 0; i < g_app_count; i++) {
        if (g_apps[i].autostart) {
            (void)app_launch(&g_apps[i]);
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

    g_launcher = lv_obj_create(scr);
    lv_obj_set_size(g_launcher, LAUNCHER_WIDTH, g_fb.height - 40);
    lv_obj_align(g_launcher, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(g_launcher, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_launcher, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_launcher, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_launcher, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(g_launcher, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(g_launcher, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_launcher, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    g_workspace = lv_obj_create(scr);
    lv_obj_set_size(g_workspace, g_fb.width - LAUNCHER_WIDTH, g_fb.height - 40);
    lv_obj_align(g_workspace, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(g_workspace, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_workspace, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_workspace, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_workspace, 0, LV_PART_MAIN);
}

static void parse_args(int argc, char **argv, const char **fbdev,
                       const char **inputdev, const char **kbddev,
                       const char **appdir)
{
    *fbdev = DEFAULT_FBDEV;
    *inputdev = DEFAULT_INPUTDEV;
    *kbddev = DEFAULT_KBDDEV;
    *appdir = DEFAULT_APPDIR;

    if (argc > 1) {
        *fbdev = argv[1];
    }

    if (argc > 2) {
        *inputdev = argv[2];
    }

    if (argc > 3) {
        *kbddev = argv[3];
    }

    if (argc > 4) {
        *appdir = argv[4];
    }
}

int main(int argc, char **argv)
{
    const char *fbdev;
    const char *inputdev;
    const char *kbddev;
    const char *appdir;
    lv_display_t *disp;
    void *draw_buf1;
    void *draw_buf2;

    parse_args(argc, argv, &fbdev, &inputdev, &kbddev, &appdir);
    snprintf(g_app_dir, sizeof(g_app_dir), "%s", appdir);
    printf("lvgl_desktop: fb=%s input=%s kbd=%s apps=%s\n",
           fbdev, inputdev, kbddev, g_app_dir);

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
    app_registry_load(g_app_dir);
    ui_rebuild_launcher();
    app_registry_autostart();
    lv_obj_invalidate(lv_screen_active());

    g_server_fd = desktop_listen();
    if (g_server_fd < 0) {
        fprintf(stderr, "lvgl_desktop: remote apps disabled\n");
    } else {
        remote_start_accept_thread();
    }

    for (;;) {
        uint32_t idle;

        app_reap_children();
        keyboard_poll();
        remote_poll();
        if (remote_has_windows()) {
            lv_obj_invalidate(lv_screen_active());
        }

        idle = lv_timer_handler();
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
