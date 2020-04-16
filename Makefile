OUT:=$(shell pwd)
$(shell mkdir -p $(OUT))

ifdef DRM_DISPLAY
TARGET = drm-display
CFLAGS += -DDRM_DISPLAY
SOURCES = drm_display.c fbpool.c
else
TARGET = fbpool
SOURCES = fbpool.c
endif

all: $(OUT)/$(TARGET)

CINCLUDES := -I . -I include -I /usr/include/libdrm
LDFLAGS := -ldrm -lrga -lc -g -O0

$(OUT)/$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(CINCLUDES) \
		$(SOURCES) -o $@
