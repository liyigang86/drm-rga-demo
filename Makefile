OUT:=$(shell pwd)
$(shell mkdir -p $(OUT))

TARGET = drm-display
CFLAGS += -DDRM_DISPLAY
SOURCES = drm_display.c fbpool.c

all: $(OUT)/$(TARGET)

CINCLUDES := -I . -I include -I /usr/include/libdrm
LDFLAGS := -ldrm -lrga -lc -g -O0

$(OUT)/$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(CINCLUDES) \
		$(SOURCES) -o $@
