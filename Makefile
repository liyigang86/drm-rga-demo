define all-c-files-under
$(patsubst ./%,%, \
  $(shell find -L $(1) -name "*.c" -and -not -name ".*") \
 )
endef

OUT:=$(shell pwd)
$(shell mkdir -p $(OUT))

TARGET = drm-display
all: $(OUT)/$(TARGET)

SOURCES = $(call all-c-files-under,.)

CINCLUDES := -I . -I include -I /usr/include/libdrm
LDFLAGS := -ldrm -lrga -lc -g -O0

$(OUT)/$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(CINCLUDES) \
		$(SOURCES) -o $@
