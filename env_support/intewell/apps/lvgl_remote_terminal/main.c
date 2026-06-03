#define _POSIX_C_SOURCE 200809L

#include "lvgl/lvgl.h"
#include "lv_remote.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
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
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PTMX "/dev/ptmx"
#define DEFAULT_CWD "/"
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 360
#define BYTES_PER_PIXEL 2
#define TERM_TEXT_CAP 32768
#define SHELL_LINE_CAP 512
#define SHELL_ARG_MAX 32
#define PATH_CAP 512
#define KEYBOARD_PRESS 1

#define XK_BACKSPACE 0xff08
#define XK_TAB 0xff09
#define XK_RETURN 0xff0d
#define XK_ESCAPE 0xff1b
#define XK_DELETE 0xffff
#define XK_KP_ENTER 0xff8d
#define XK_LEFT 0xff51
#define XK_UP 0xff52
#define XK_RIGHT 0xff53
#define XK_DOWN 0xff54
#define XK_HOME 0xff50
#define XK_END 0xff57
#define XK_CONTROL_L 0xffe3
#define XK_CONTROL_R 0xffe4

#define TOUCH_POS_VALID 0x02
#define TOUCH_DOWN 0x04
#define TOUCH_UP 0x08

#ifndef TIOCGPTN
#define TIOCGPTN 0x80045430
#endif

#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif

#ifndef FIONBIO
#define FIONBIO 0x5421
#endif

#ifndef KSHELL_IOC_EXEC
struct kshell_exec {
    unsigned long cmd;
    uint32_t cmd_len;
    unsigned long out;
    uint32_t out_cap;
    int32_t cmd_ret;
    uint32_t out_len;
    uint32_t flags;
};

#define KSHELL_IOC_EXEC _IOWR('K', 0x01, struct kshell_exec)
#endif

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

struct pty_ctx {
    int master_fd;
    pid_t child_pid;
    bool child_running;
    char slave_path[32];
};

struct shell_state {
    char line[SHELL_LINE_CAP];
    size_t len;
    char cwd[PATH_CAP];
    int pty_fd;
    int kshell_fd;
};

static struct remote_ctx g_remote = {
    .socket_fd = -1,
    .surface_fd = -1,
};

static struct pty_ctx g_pty = {
    .master_fd = -1,
    .child_pid = -1,
};

static lv_obj_t *g_content;
static lv_obj_t *g_output;
static lv_display_t *g_display;
static void *g_draw_buf;
static char g_term[TERM_TEXT_CAP];
static size_t g_term_len;
static bool g_ctrl_down;
static volatile sig_atomic_t g_exit_requested;

static void terminal_handle_key(uint32_t key, uint8_t action);
static void pty_poll_output(struct pty_ctx *pty);
static int remote_resize_surface(uint32_t width, uint32_t height);

static void signal_handler(int signo)
{
    (void)signo;
    g_exit_requested = 1;
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

static int remote_commit_area(int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct lv_remote_msg msg;

    lv_remote_msg_init(&msg, LV_REMOTE_MSG_COMMIT);
    msg.serial = ++g_remote.serial;
    msg.x = x;
    msg.y = y;
    msg.w = w;
    msg.h = h;
    msg.pid = (uint32_t)getpid();
    return lv_remote_send_msg(g_remote.socket_fd, &msg);
}

static int remote_sync_area_to_file(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    for (uint32_t row = 0; row < h; row++) {
        size_t offset = ((size_t)y + row) * g_remote.stride +
                        (size_t)x * BYTES_PER_PIXEL;

        if (write_all_at(g_remote.surface_fd, (off_t)offset,
                         g_remote.surface + offset,
                         (size_t)w * BYTES_PER_PIXEL) < 0) {
            return -1;
        }
    }

    return 0;
}

static void remote_debug_paint_surface(void)
{
    uint16_t *pixels = (uint16_t *)g_remote.surface;

    if (g_remote.surface == NULL) {
        return;
    }

    for (uint32_t y = 0; y < g_remote.height; y++) {
        uint16_t color = 0x001f;

        if (y < g_remote.height / 3U) {
            color = 0xf800;
        } else if (y < (g_remote.height * 2U) / 3U) {
            color = 0x07e0;
        }

        for (uint32_t x = 0; x < g_remote.width; x++) {
            uint8_t *row = g_remote.surface + y * g_remote.stride;
            pixels = (uint16_t *)row;
            pixels[x] = color;
        }
    }

    printf("lvgl_remote_terminal: debug surface bars painted first=%04x\n",
           ((uint16_t *)g_remote.surface)[0]);
    fflush(stdout);
    if (remote_sync_area_to_file(0, 0, g_remote.width, g_remote.height) < 0) {
        fprintf(stderr, "sync debug surface failed: %s\n", strerror(errno));
    }
    (void)remote_commit_area(0, 0, (int32_t)g_remote.width,
                             (int32_t)g_remote.height);
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

    printf("lvgl_remote_terminal: registered '%s' %ux%u %s\n",
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
    static int flush_log_count;

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
        uint8_t *dst = g_remote.surface + ((uint32_t)y1 + row) *
                       g_remote.stride + (uint32_t)x1 * BYTES_PER_PIXEL;
        const uint8_t *src = px_map + ((uint32_t)y1 + row) *
                             g_remote.stride + (uint32_t)x1 * BYTES_PER_PIXEL;

        memcpy(dst, src, width * BYTES_PER_PIXEL);
    }

    if (remote_sync_area_to_file(x1, y1, width, height) < 0) {
        fprintf(stderr, "sync surface failed: %s\n", strerror(errno));
        lv_display_flush_ready(disp);
        return;
    }

    if (flush_log_count < 8) {
        const uint16_t *sample = (const uint16_t *)px_map;

        printf("lvgl_remote_terminal: flush %ld,%ld-%ld,%ld clipped %ld,%ld %ux%u sample=%04x\n",
               (long)area->x1, (long)area->y1, (long)area->x2, (long)area->y2,
               (long)x1, (long)y1, width, height, sample[0]);
        fflush(stdout);
        flush_log_count++;
    }

    if (remote_commit_area(x1, y1, (int32_t)width, (int32_t)height) < 0 &&
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
        g_exit_requested = 1;
        break;
    case LV_REMOTE_MSG_INPUT_POINTER:
        g_remote.pointer_x = clamp_i16(msg->x, 0, (int32_t)g_remote.width - 1);
        g_remote.pointer_y = clamp_i16(msg->y, 0, (int32_t)g_remote.height - 1);
        g_remote.pointer_pressed = msg->buttons != 0U;
        break;
    case LV_REMOTE_MSG_INPUT_KEY:
        terminal_handle_key(msg->key, (uint8_t)msg->action);
        pty_poll_output(&g_pty);
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
            g_exit_requested = 1;
            return;
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

static void term_refresh(void)
{
    bool follow_tail = true;

    if (g_output == NULL) {
        return;
    }

    if (g_content != NULL) {
        lv_obj_update_layout(g_content);
        follow_tail = lv_obj_get_scroll_bottom(g_content) <= 24;
    }

    lv_label_set_text(g_output, g_term);

    if (follow_tail && g_content != NULL) {
        int32_t scroll_y;

        lv_obj_update_layout(g_content);
        scroll_y = lv_obj_get_scroll_y(g_content) +
                   lv_obj_get_scroll_bottom(g_content);
        lv_obj_scroll_to_y(g_content, scroll_y, LV_ANIM_OFF);
    }

    lv_obj_invalidate(lv_screen_active());
}

static void term_append(const char *text)
{
    size_t len = strlen(text);

    if (len >= TERM_TEXT_CAP) {
        text += len - (TERM_TEXT_CAP - 1U);
        len = strlen(text);
    }

    if (g_term_len + len >= TERM_TEXT_CAP) {
        size_t drop = g_term_len + len - (TERM_TEXT_CAP - 1U);
        memmove(g_term, g_term + drop, g_term_len - drop + 1U);
        g_term_len -= drop;
    }

    memcpy(g_term + g_term_len, text, len + 1U);
    g_term_len += len;
    term_refresh();
}

static void term_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_append(buf);
}

static void term_clear_history(void)
{
    g_term[0] = '\0';
    g_term_len = 0;
    term_refresh();
}

static void term_append_data(const char *data, size_t len)
{
    static char clean[1024];
    static int esc_state;
    size_t out = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        if (esc_state == 1) {
            esc_state = ch == '[' ? 2 : 0;
            continue;
        }

        if (esc_state == 2) {
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
                if (ch == 'J') {
                    term_clear_history();
                }
                esc_state = 0;
            }
            continue;
        }

        if (ch == '\033') {
            esc_state = 1;
            continue;
        }

        if (ch == '\r') {
            continue;
        }

        if (ch == '\b' || ch == 0x7f) {
            if (out > 0) {
                out--;
            } else if (g_term_len > 0) {
                g_term[--g_term_len] = '\0';
                term_refresh();
            }
            continue;
        }

        if (ch == '\t') {
            ch = ' ';
        }

        if (ch == '\n' || ch >= 0x20) {
            clean[out++] = (char)ch;
        }

        if (out + 4U >= sizeof(clean)) {
            clean[out] = '\0';
            term_append(clean);
            out = 0;
        }
    }

    if (out > 0) {
        clean[out] = '\0';
        term_append(clean);
    }
}

static void normalize_path(char *path)
{
    char tmp[PATH_CAP];
    char *parts[64];
    size_t n = 0;
    char *saveptr = NULL;
    char *tok;

    snprintf(tmp, sizeof(tmp), "%s", path);
    tok = strtok_r(tmp, "/", &saveptr);
    while (tok != NULL && n < 64U) {
        if (strcmp(tok, ".") == 0) {
        } else if (strcmp(tok, "..") == 0) {
            if (n > 0) {
                n--;
            }
        } else {
            parts[n++] = tok;
        }

        tok = strtok_r(NULL, "/", &saveptr);
    }

    path[0] = '/';
    path[1] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (strlen(path) > 1U) {
            strncat(path, "/", PATH_CAP - strlen(path) - 1U);
        }

        strncat(path, parts[i], PATH_CAP - strlen(path) - 1U);
    }
}

static void shell_prompt(struct shell_state *shell)
{
    dprintf(shell->pty_fd, "%s $ ", shell->cwd);
}

static int shell_make_path(struct shell_state *shell, char *out, size_t out_len,
                           const char *arg)
{
    int ret;
    size_t len;

    if (arg[0] == '/') {
        ret = snprintf(out, out_len, "%s", arg);
    } else {
        len = strlen(shell->cwd);
        ret = snprintf(out, out_len, len > 0 && shell->cwd[len - 1] == '/' ?
                       "%s%s" : "%s/%s", shell->cwd, arg);
    }

    if (ret < 0 || (size_t)ret >= out_len) {
        errno = ENAMETOOLONG;
        return -1;
    }

    normalize_path(out);
    return 0;
}

static int shell_split_line(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }

        if (*p != '\0') {
            *p++ = '\0';
        }
    }

    argv[argc] = NULL;
    return argc;
}

static int shell_run_kshell(struct shell_state *shell, const char *line)
{
    char out[8192];
    struct kshell_exec req;
    int ret;

    if (shell->kshell_fd < 0) {
        shell->kshell_fd = open("/dev/kshell", O_RDWR);
        if (shell->kshell_fd < 0) {
            dprintf(shell->pty_fd, "kshell: open /dev/kshell failed: %s\r\n",
                    strerror(errno));
            return -1;
        }
    }

    memset(&req, 0, sizeof(req));
    req.cmd = (unsigned long)line;
    req.cmd_len = (uint32_t)strlen(line);
    req.out = (unsigned long)out;
    req.out_cap = sizeof(out) - 1U;

    ret = ioctl(shell->kshell_fd, KSHELL_IOC_EXEC, &req);
    if (ret < 0 && errno != ENOSPC) {
        dprintf(shell->pty_fd, "kshell: ioctl failed: %s\r\n", strerror(errno));
        return -1;
    }

    if (req.out_len > 0) {
        size_t n = req.out_len < sizeof(out) - 1U ? req.out_len : sizeof(out) - 1U;
        out[n] = '\0';
        write(shell->pty_fd, out, n);
        if (ret < 0 && errno == ENOSPC) {
            dprintf(shell->pty_fd, "\r\n... output truncated ...\r\n");
        }
    }

    return req.cmd_ret;
}

static int shell_run_program(struct shell_state *shell, char **argv)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0) {
        dprintf(shell->pty_fd, "%s: fork failed: %s\r\n", argv[0], strerror(errno));
        return -1;
    }

    if (pid == 0) {
        dup2(shell->pty_fd, STDIN_FILENO);
        dup2(shell->pty_fd, STDOUT_FILENO);
        dup2(shell->pty_fd, STDERR_FILENO);
        if (shell->pty_fd > STDERR_FILENO) {
            close(shell->pty_fd);
        }

        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "%s: exec failed: %s\r\n", argv[0], strerror(errno));
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            dprintf(shell->pty_fd, "%s: wait failed: %s\r\n", argv[0], strerror(errno));
            return -1;
        }
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void shell_execute_line(struct shell_state *shell, const char *input)
{
    char line[SHELL_LINE_CAP];
    char path[PATH_CAP];
    char *argv[SHELL_ARG_MAX];
    int argc;

    snprintf(line, sizeof(line), "%s", input);
    argc = shell_split_line(line, argv, SHELL_ARG_MAX);
    if (argc == 0) {
        return;
    }

    if (strcmp(argv[0], "exit") == 0) {
        _exit(0);
    } else if (strcmp(argv[0], "clear") == 0) {
        dprintf(shell->pty_fd, "\033[2J\033[H");
    } else if (strcmp(argv[0], "pwd") == 0) {
        dprintf(shell->pty_fd, "%s\r\n", shell->cwd);
    } else if (strcmp(argv[0], "cd") == 0) {
        const char *target = argc > 1 ? argv[1] : "/";
        struct stat st;

        if (shell_make_path(shell, path, sizeof(path), target) < 0) {
            dprintf(shell->pty_fd, "cd: %s: %s\r\n", target, strerror(errno));
        } else if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
            dprintf(shell->pty_fd, "cd: %s: %s\r\n", path, strerror(errno));
        } else {
            snprintf(shell->cwd, sizeof(shell->cwd), "%s", path);
            chdir(shell->cwd);
        }
    } else if (strcmp(argv[0], "help") == 0) {
        dprintf(shell->pty_fd,
                "builtins: cd pwd clear exit help kshell\r\n"
                "other commands are started with fork/execvp on this PTY\r\n");
    } else if (strcmp(argv[0], "kshell") == 0) {
        if (argc < 2) {
            dprintf(shell->pty_fd, "usage: kshell <kernel-command>\r\n");
            return;
        }

        shell_run_kshell(shell, input + (argv[1] - line));
    } else {
        shell_run_program(shell, argv);
    }
}

static void shell_handle_byte(struct shell_state *shell, char ch)
{
    if (ch == '\r' || ch == '\n') {
        dprintf(shell->pty_fd, "\r\n");
        shell->line[shell->len] = '\0';
        shell_execute_line(shell, shell->line);
        shell->len = 0;
        shell->line[0] = '\0';
        shell_prompt(shell);
        return;
    }

    if (ch == '\b' || ch == 0x7f) {
        if (shell->len > 0) {
            shell->len--;
            shell->line[shell->len] = '\0';
            dprintf(shell->pty_fd, "\b \b");
        }
        return;
    }

    if (isprint((unsigned char)ch) && shell->len + 1U < sizeof(shell->line)) {
        shell->line[shell->len++] = ch;
        shell->line[shell->len] = '\0';
        dprintf(shell->pty_fd, "%c", ch);
    }
}

static void shell_loop(int slave_fd, const char *cwd)
{
    struct shell_state shell = {
        .pty_fd = slave_fd,
        .kshell_fd = -1,
    };
    char buf[128];
    ssize_t nread;

    snprintf(shell.cwd, sizeof(shell.cwd), "%s", cwd);
    normalize_path(shell.cwd);
    chdir(shell.cwd);

    dprintf(shell.pty_fd, "LVGL PTY terminal ready\r\n");
    dprintf(shell.pty_fd, "type 'help' for builtins, or run uploaded programs directly\r\n");
    shell_prompt(&shell);

    for (;;) {
        nread = read(shell.pty_fd, buf, sizeof(buf));
        if (nread < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            break;
        }

        if (nread == 0) {
            continue;
        }

        for (ssize_t i = 0; i < nread; i++) {
            shell_handle_byte(&shell, buf[i]);
        }
    }

    if (shell.kshell_fd >= 0) {
        close(shell.kshell_fd);
    }
}

static int pty_open_master(struct pty_ctx *pty, const char *path)
{
    int unlock = 0;
    int nonblock = 1;
    int ptyno = -1;

    memset(pty, 0, sizeof(*pty));
    pty->master_fd = -1;
    pty->child_pid = -1;

    pty->master_fd = open(path, O_RDWR | O_NOCTTY);
    if (pty->master_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    if (ioctl(pty->master_fd, TIOCGPTN, &ptyno) < 0) {
        fprintf(stderr, "TIOCGPTN failed: %s\n", strerror(errno));
        goto fail;
    }

    if (ioctl(pty->master_fd, TIOCSPTLCK, &unlock) < 0) {
        fprintf(stderr, "TIOCSPTLCK unlock failed: %s\n", strerror(errno));
        goto fail;
    }

    if (ioctl(pty->master_fd, FIONBIO, &nonblock) < 0) {
        fprintf(stderr, "FIONBIO master failed: %s\n", strerror(errno));
    }

    snprintf(pty->slave_path, sizeof(pty->slave_path), "/dev/pts/%d", ptyno);
    return 0;

fail:
    close(pty->master_fd);
    pty->master_fd = -1;
    return -1;
}

static void pty_configure_slave(int slave_fd)
{
    struct termios tio;

    if (tcgetattr(slave_fd, &tio) < 0) {
        return;
    }

    tio.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    tio.c_oflag &= (tcflag_t)~OPOST;
    tio.c_cflag |= CS8;
    tio.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    (void)tcsetattr(slave_fd, TCSANOW, &tio);
}

static int pty_spawn_shell(struct pty_ctx *pty, const char *cwd)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork shell failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        int slave_fd;

        close(pty->master_fd);
        setsid();
        slave_fd = open(pty->slave_path, O_RDWR);
        if (slave_fd < 0) {
            _exit(127);
        }

        ioctl(slave_fd, TIOCSCTTY, 0);
        pty_configure_slave(slave_fd);
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }

        shell_loop(STDIN_FILENO, cwd);
        _exit(0);
    }

    pty->child_pid = pid;
    pty->child_running = true;
    printf("lvgl_terminal_demo: pty master=%s slave=%s shell_pid=%ld\n",
           DEFAULT_PTMX, pty->slave_path, (long)pid);
    return 0;
}

static void pty_close(struct pty_ctx *pty)
{
    if (pty->master_fd >= 0) {
        close(pty->master_fd);
        pty->master_fd = -1;
    }
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

    if (width == 0 || height <= 44 || width > 4096 || height > 4096) {
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
    if (g_content != NULL) {
        lv_obj_set_size(g_content, LV_PCT(100), g_remote.height - 44);
        lv_obj_align(g_content, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    term_refresh();
    lv_obj_invalidate(lv_screen_active());

    if (remote_send_attach_buffer() < 0) {
        return -1;
    }

    printf("lvgl_remote_terminal: resized %ux%u %s\n",
           g_remote.width, g_remote.height, g_remote.surface_path);
    return 0;
}

static void pty_poll_output(struct pty_ctx *pty)
{
    bool updated = false;

    if (pty->master_fd >= 0) {
        char buf[512];
        ssize_t nread;

        do {
            nread = read(pty->master_fd, buf, sizeof(buf));
            if (nread > 0) {
                term_append_data(buf, (size_t)nread);
                updated = true;
            }
        } while (nread > 0);
    }

    if (pty->child_running) {
        int status;
        pid_t ret = waitpid(pty->child_pid, &status, WNOHANG);

        if (ret == pty->child_pid) {
            pty->child_running = false;
            term_printf("\n[terminal shell exited: %d]\n",
                        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
    }

    if (updated) {
        lv_obj_invalidate(lv_screen_active());
    }
}

static void pty_write_text(struct pty_ctx *pty, const char *text)
{
    const char *p = text;
    size_t len = strlen(text);

    if (pty->master_fd >= 0) {
        while (len > 0U) {
            ssize_t nwritten = write(pty->master_fd, p, len);

            if (nwritten < 0) {
                if (errno == EINTR) {
                    continue;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    sleep_ms(1);
                    continue;
                }

                return;
            }

            if (nwritten == 0) {
                return;
            }

            p += (size_t)nwritten;
            len -= (size_t)nwritten;
        }
    }
}

static void terminal_handle_key(uint32_t key, uint8_t action)
{
    char ch;

    if (key == XK_CONTROL_L || key == XK_CONTROL_R) {
        g_ctrl_down = action == KEYBOARD_PRESS;
        return;
    }

    if (action != KEYBOARD_PRESS) {
        return;
    }

    if (key == XK_RETURN || key == XK_KP_ENTER || key == '\r' || key == '\n') {
        pty_write_text(&g_pty, "\r");
    } else if (key == XK_BACKSPACE || key == XK_DELETE || key == 0x7f ||
               key == '\b') {
        pty_write_text(&g_pty, "\x7f");
    } else if (key == XK_TAB || key == '\t') {
        pty_write_text(&g_pty, "\t");
    } else if (key == XK_ESCAPE) {
        pty_write_text(&g_pty, "\033");
    } else if (key == XK_UP) {
        pty_write_text(&g_pty, "\033[A");
    } else if (key == XK_DOWN) {
        pty_write_text(&g_pty, "\033[B");
    } else if (key == XK_RIGHT) {
        pty_write_text(&g_pty, "\033[C");
    } else if (key == XK_LEFT) {
        pty_write_text(&g_pty, "\033[D");
    } else if (key == XK_HOME) {
        pty_write_text(&g_pty, "\033[H");
    } else if (key == XK_END) {
        pty_write_text(&g_pty, "\033[F");
    } else if (key >= 0x20 && key < 0x7f) {
        ch = (char)key;
        if (g_ctrl_down && key >= 'a' && key <= 'z') {
            ch = (char)(key - 'a' + 1);
        } else if (g_ctrl_down && key >= 'A' && key <= 'Z') {
            ch = (char)(key - 'A' + 1);
        }

        char text[2] = { ch, '\0' };

        pty_write_text(&g_pty, text);
    }
}

static void ui_create(const char *cwd)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *header;
    lv_obj_t *title;

    (void)cwd;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xe5e7eb), LV_PART_MAIN);

    header = lv_obj_create(scr);
    lv_obj_set_size(header, LV_PCT(100), 44);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x263238), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(header, 7, LV_PART_MAIN);

    title = lv_label_create(header);
    lv_label_set_text(title, LV_SYMBOL_HOME "  LVGL Terminal");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    g_content = lv_obj_create(scr);
    lv_obj_set_size(g_content, LV_PCT(100), g_remote.height - 44);
    lv_obj_align(g_content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(g_content, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_content, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_content, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_flex_flow(g_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    g_output = lv_label_create(g_content);
    lv_obj_set_width(g_output, LV_PCT(100));
    lv_label_set_long_mode(g_output, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_bg_opa(g_output, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_output, lv_color_hex(0xd1fae5), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_output, &lv_font_montserrat_14, LV_PART_MAIN);

    term_refresh();
}

static void parse_args(int argc, char **argv, const char **cwd)
{
    *cwd = DEFAULT_CWD;

    if (argc > 1) {
        *cwd = argv[1];
    }
}

int main(int argc, char **argv)
{
    const char *cwd;

    parse_args(argc, argv, &cwd);
    g_remote.width = DEFAULT_WIDTH;
    g_remote.height = DEFAULT_HEIGHT;
    g_remote.stride = g_remote.width * BYTES_PER_PIXEL;
    g_remote.surface_len = (size_t)g_remote.stride * g_remote.height;

    if (remote_create_surface(&g_remote) < 0) {
        return 1;
    }

    g_remote.socket_fd = remote_connect();
    if (g_remote.socket_fd < 0) {
        return 1;
    }

    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

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
    ui_create(cwd);
    term_append("LVGL remote terminal UI ready\n");

    if (pty_open_master(&g_pty, DEFAULT_PTMX) == 0) {
        if (pty_spawn_shell(&g_pty, cwd) < 0) {
            term_append("failed to start PTY shell\n");
        }
    } else {
        term_append("failed to open /dev/ptmx; rebuild kernel with CONFIG_PSEUDOTERM\n");
    }

    pty_poll_output(&g_pty);
    lv_obj_invalidate(lv_screen_active());

    if (remote_register_window("Terminal") < 0) {
        fprintf(stderr, "register window failed: %s\n", strerror(errno));
        return 1;
    }
    remote_debug_paint_surface();

    for (;;) {
        uint32_t idle;

        if (g_exit_requested) {
            if (g_pty.child_running && g_pty.child_pid > 0) {
                kill(g_pty.child_pid, SIGTERM);
                waitpid(g_pty.child_pid, NULL, 0);
                g_pty.child_running = false;
            }
            pty_close(&g_pty);
            remote_close_surface(&g_remote);
            return 0;
        }

        remote_poll();
        pty_poll_output(&g_pty);
        idle = lv_timer_handler();
        sleep_ms(idle == 0 ? 5U : idle > 10U ? 10U : idle);
    }

    pty_close(&g_pty);
    remote_close_surface(&g_remote);
    return 0;
}
