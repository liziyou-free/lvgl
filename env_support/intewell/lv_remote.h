#ifndef INTEWELL_LV_REMOTE_H
#define INTEWELL_LV_REMOTE_H

#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

#define LV_REMOTE_SOCKET_PATH "/run/lvgl-desktop.sock"
#define LV_REMOTE_SURFACE_DIR "/run"
#define LV_REMOTE_MAGIC 0x4c56524dU
#define LV_REMOTE_VERSION 1U
#define LV_REMOTE_MAX_TITLE 64U
#define LV_REMOTE_MAX_PATH 128U
#define LV_REMOTE_WINDOW_TOPMOST 0x00000001U

enum lv_remote_format {
    LV_REMOTE_FORMAT_RGB565 = 1,
};

enum lv_remote_msg_type {
    LV_REMOTE_MSG_HELLO = 1,
    LV_REMOTE_MSG_CREATE_WINDOW,
    LV_REMOTE_MSG_ATTACH_BUFFER,
    LV_REMOTE_MSG_COMMIT,
    LV_REMOTE_MSG_INPUT_POINTER,
    LV_REMOTE_MSG_INPUT_KEY,
    LV_REMOTE_MSG_CLOSE_WINDOW,
    LV_REMOTE_MSG_TERMINATE,
};

enum lv_remote_key_action {
    LV_REMOTE_KEY_RELEASE = 0,
    LV_REMOTE_KEY_PRESS = 1,
};

struct lv_remote_msg {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t size;
    uint32_t serial;
    uint32_t window_id;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t flags;
    uint32_t buttons;
    uint32_t key;
    uint32_t action;
    uint32_t pid;
    uint32_t reserved;
    char title[LV_REMOTE_MAX_TITLE];
    char path[LV_REMOTE_MAX_PATH];
};

static inline void lv_remote_msg_init(struct lv_remote_msg *msg, uint32_t type)
{
    uint8_t *p = (uint8_t *)msg;

    for (size_t i = 0; i < sizeof(*msg); i++) {
        p[i] = 0;
    }

    msg->magic = LV_REMOTE_MAGIC;
    msg->version = LV_REMOTE_VERSION;
    msg->type = type;
    msg->size = sizeof(*msg);
}

static inline int lv_remote_msg_valid(const struct lv_remote_msg *msg)
{
    return msg->magic == LV_REMOTE_MAGIC &&
           msg->version == LV_REMOTE_VERSION &&
           msg->size == sizeof(*msg);
}

static inline int lv_remote_write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0U) {
        ssize_t nwritten = write(fd, p, len);

        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return -1;
            }

            return -1;
        }

        if (nwritten == 0) {
            errno = EPIPE;
            return -1;
        }

        p += (size_t)nwritten;
        len -= (size_t)nwritten;
    }

    return 0;
}

static inline int lv_remote_send_msg(int fd, const struct lv_remote_msg *msg)
{
    return lv_remote_write_full(fd, msg, sizeof(*msg));
}

#endif /* INTEWELL_LV_REMOTE_H */
