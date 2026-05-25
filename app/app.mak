APP_DIR=app
LVGL_DIR=app/lvgl

APP_C_SOURCES = \
$(APP_DIR)/app.c \
$(APP_DIR)/led.c \
$(APP_DIR)/lcd_bl.c \
$(APP_DIR)/sdram.c \
$(APP_DIR)/ft5406.c \
$(APP_DIR)/lvgl_app.c \
$(APP_DIR)/dwt.c \

#$(APP_DIR)/lcd_app.c

LVGL_SOURCES := $(shell find $(LVGL_DIR)/src -name "*.c")
LVGL_DEMO_SOURCES := $(shell find $(LVGL_DIR)/demos -name "*.c")

C_SOURCES += $(APP_C_SOURCES) 
C_SOURCES += $(LVGL_SOURCES)
C_SOURCES += $(LVGL_DEMO_SOURCES)

CFLAGS += -I$(APP_DIR) -I$(LVGL_DIR)
C_DEFS += 
LIBS += 
