CC := /opt/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc
AR := /opt/aarch64-linux-musl-cross/bin/aarch64-linux-musl-ar

LVGL_DIR ?= ../../../..
TARGET := lvgl_remote_terminal
TFTP_ROOT ?= /home/freedomli/Development/tftp-server-root

CFLAGS := -O2 -g -Wall -Wextra $(filter-out -O0 -O1 -O2 -O3 -Os -Og,$(CFLAGS))
CFLAGS += -std=c99 -D_GNU_SOURCE -DLV_DISABLE_API_MAPPING -DLV_CONF_INCLUDE_SIMPLE=1 -DLV_CONF_PATH=\"lv_conf.h\"
CFLAGS += -I. -I$(LVGL_DIR) -I$(LVGL_DIR)/include -I$(LVGL_DIR)/env_support/intewell
LDFLAGS ?= -static
LDLIBS ?= -lm

LVGL_SRCS := \
	$(LVGL_DIR)/src/lv_init.c \
	$(wildcard $(LVGL_DIR)/src/core/*.c) \
	$(wildcard $(LVGL_DIR)/src/display/*.c) \
	$(wildcard $(LVGL_DIR)/src/indev/*.c) \
	$(wildcard $(LVGL_DIR)/src/layouts/*.c) \
	$(wildcard $(LVGL_DIR)/src/layouts/flex/*.c) \
	$(wildcard $(LVGL_DIR)/src/layouts/grid/*.c) \
	$(wildcard $(LVGL_DIR)/src/tick/*.c) \
	$(wildcard $(LVGL_DIR)/src/misc/*.c) \
	$(wildcard $(LVGL_DIR)/src/misc/cache/*.c) \
	$(wildcard $(LVGL_DIR)/src/misc/cache/class/*.c) \
	$(wildcard $(LVGL_DIR)/src/misc/cache/instance/*.c) \
	$(LVGL_DIR)/src/debugging/sysmon/lv_sysmon.c \
	$(LVGL_DIR)/src/osal/lv_os.c \
	$(LVGL_DIR)/src/osal/lv_os_none.c \
	$(LVGL_DIR)/src/stdlib/lv_mem.c \
	$(wildcard $(LVGL_DIR)/src/stdlib/clib/*.c) \
	$(LVGL_DIR)/src/font/lv_font.c \
	$(LVGL_DIR)/src/font/fmt_txt/lv_font_fmt_txt.c \
	$(LVGL_DIR)/src/font/lv_font_montserrat_14.c \
	$(LVGL_DIR)/src/font/lv_font_montserrat_18.c \
	$(LVGL_DIR)/src/font/lv_font_montserrat_20.c \
	$(LVGL_DIR)/src/font/lv_font_montserrat_24.c \
	$(wildcard $(LVGL_DIR)/src/draw/*.c) \
	$(LVGL_DIR)/src/draw/convert/lv_draw_buf_convert.c \
	$(wildcard $(LVGL_DIR)/src/draw/sw/*.c) \
	$(LVGL_DIR)/src/draw/sw/blend/lv_draw_sw_blend.c \
	$(LVGL_DIR)/src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c \
	$(LVGL_DIR)/src/draw/sw/blend/lv_draw_sw_blend_to_rgb888.c \
	$(LVGL_DIR)/src/draw/sw/blend/lv_draw_sw_blend_to_argb8888.c \
	$(LVGL_DIR)/src/draw/sw/blend/lv_draw_sw_blend_to_argb8888_premultiplied.c \
	$(LVGL_DIR)/src/draw/sw/blend/lv_draw_sw_blend_to_al88.c \
	$(LVGL_DIR)/src/draw/sw/blend/lv_draw_sw_blend_to_i1.c \
	$(LVGL_DIR)/src/themes/lv_theme.c \
	$(LVGL_DIR)/src/themes/default/lv_theme_default.c \
	$(LVGL_DIR)/src/libs/bin_decoder/lv_bin_decoder.c \
	$(LVGL_DIR)/src/libs/rle/lv_rle.c \
	$(LVGL_DIR)/src/widgets/label/lv_label.c \
	$(LVGL_DIR)/src/widgets/button/lv_button.c \
	$(LVGL_DIR)/src/widgets/textarea/lv_textarea.c \
	$(LVGL_DIR)/src/widgets/property/lv_obj_properties.c \
	$(LVGL_DIR)/src/widgets/property/lv_style_properties.c \
	$(LVGL_DIR)/src/widgets/property/lv_label_properties.c \
	$(LVGL_DIR)/src/widgets/property/lv_textarea_properties.c

LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,build/lvgl/%.o,$(LVGL_SRCS))
OBJS := build/main.o $(LVGL_OBJS)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

build/main.o: main.c lv_conf.h $(LVGL_DIR)/env_support/intewell/lv_remote.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/lvgl/%.o: $(LVGL_DIR)/%.c lv_conf.h $(LVGL_DIR)/env_support/intewell/lv_remote.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	cp $(TARGET) $(TFTP_ROOT)/

clean:
	rm -rf build $(TARGET)
