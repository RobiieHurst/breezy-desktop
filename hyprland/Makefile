CXX ?= g++
PKG_CONFIG ?= pkg-config

TARGET := build/libbreezy-hyprland.so
SOURCES := src/main.cpp src/pose_data.cpp src/monitor_discovery.cpp
VERSION := $(shell test -f ../VERSION && tr -d '\n' < ../VERSION || printf dev)

HYPRLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags hyprland)

CXXFLAGS ?= -O2 -pipe
CXXFLAGS += -std=c++2b -Wall -Wextra -Wpedantic -fPIC -fno-gnu-unique
CPPFLAGS += $(HYPRLAND_CFLAGS) -DBREEZY_DESKTOP_VERSION_STR=\"$(VERSION)\"
ifeq ($(DEBUG),1)
CPPFLAGS += -DBREEZY_HYPRLAND_DEBUG
endif
LDFLAGS += -shared

.PHONY: all clean check-deps

all: check-deps $(TARGET)

check-deps:
	@$(PKG_CONFIG) --exists hyprland || \
		(printf '%s\n' 'Missing Hyprland development files. Install Hyprland headers/pkg-config metadata first.' >&2; exit 1)

$(TARGET): $(SOURCES)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

clean:
	rm -rf build
