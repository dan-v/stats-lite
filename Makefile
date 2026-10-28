# Stats Lite — build with the Command Line Tools only (no Xcode project).
#   make            build build/StatsLite.app (release, ad-hoc signed)
#   make run        build and launch
#   make test       unit tests for the C core
#   make bench      benchmark against the original (see bench/)
BUILD    ?= build
APP      := $(BUILD)/StatsLite.app
BIN      := $(APP)/Contents/MacOS/StatsLite
CORE     := src/core/fastio.c src/core/sampler.c src/core/smc.c src/core/procs.c src/core/render.c src/core/history.c src/core/blit.c src/core/panel.c src/core/blit_arm64.S
APPSRC   := src/app/main.m
ARCH     ?= arm64
STRIP    ?= strip -x
MINOS    := 13.0
DEV      ?= 0
CFLAGS   := $(EXTRA) -DLITE_DEV=$(DEV) -arch $(ARCH) -mmacosx-version-min=$(MINOS) -O3 -flto -fvisibility=hidden -fno-common \
            -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fstack-protector-strong
OBJCFLAGS:= $(CFLAGS) -fobjc-arc
FRAMEWORKS := -framework AppKit -framework QuartzCore -framework CoreText -framework IOKit \
              -framework SystemConfiguration -framework ServiceManagement -framework CoreServices -framework IOSurface -framework UserNotifications
LDFLAGS  := -arch $(ARCH) -mmacosx-version-min=$(MINOS) -flto -Wl,-dead_strip -Wl,-x $(FRAMEWORKS)

all: $(APP)

$(BIN): $(CORE) $(APPSRC) src/core/*.h Makefile
	@mkdir -p $(dir $@)
	clang $(OBJCFLAGS) $(APPSRC) $(CORE) $(LDFLAGS) -o $@
	$(STRIP) $@

$(APP): $(BIN) resources/Info.plist
	cp resources/Info.plist $(APP)/Contents/Info.plist
	codesign --force --sign - --options runtime $(APP)

run: $(APP)
	-pkill -x StatsLite; open $(APP)

test: build/test_core
	./build/test_core

build/test_core: tests/test_core.c $(CORE) src/core/*.h
	@mkdir -p build
	clang $(CFLAGS) -O1 -fsanitize=address,undefined tests/test_core.c $(CORE) -framework CoreText -framework CoreGraphics \
	  -framework IOKit -framework CoreFoundation -framework SystemConfiguration -framework CoreServices -framework IOSurface -o $@

clean:
	rm -rf build

.PHONY: all run test clean
