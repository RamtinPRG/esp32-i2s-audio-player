CC := gcc

PROJECT_ROOT := ..
LVGL_DIR := $(PROJECT_ROOT)/managed_components/lvgl__lvgl
MAIN_DIR := $(PROJECT_ROOT)/main

TARGET := ui_simulator
BUILD_DIR := build

CFLAGS := \
	-Wall \
	-Wextra \
	-g \
	-O0 \
	-std=c11 \
	-DLV_CONF_INCLUDE_SIMPLE \
	-I$(CURDIR) \
	-I$(MAIN_DIR) \
	-I$(LVGL_DIR)

CFLAGS += $(shell pkg-config --cflags sdl2)
LDLIBS := $(shell pkg-config --libs sdl2)

MAIN_SRC := main_sdl.c
UI_SRC := $(MAIN_DIR)/ui.c
LVGL_SRC := $(shell find $(LVGL_DIR)/src -name '*.c')

MAIN_OBJ := $(BUILD_DIR)/main_sdl.o
UI_OBJ := $(BUILD_DIR)/ui.o
LVGL_OBJ := $(patsubst $(LVGL_DIR)/src/%.c,$(BUILD_DIR)/lvgl/%.o,$(LVGL_SRC))

OBJECTS := $(MAIN_OBJ) $(UI_OBJ) $(LVGL_OBJ)


.PHONY: all clean run

all: $(TARGET)


$(TARGET): $(OBJECTS)
	@echo "  LD    $@"
	$(CC) $^ -o $@ $(LDLIBS)


$(MAIN_OBJ): $(MAIN_SRC)
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c $< -o $@


$(UI_OBJ): $(UI_SRC)
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD_DIR)/lvgl/%.o: $(LVGL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c $< -o $@


run: $(TARGET)
	./$(TARGET)


clean:
	rm -rf $(BUILD_DIR) $(TARGET)