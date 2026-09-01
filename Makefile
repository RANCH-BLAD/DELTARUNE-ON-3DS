.SUFFIXES:

.DEFAULT_GOAL := help

ROOT_DIR := $(CURDIR)
export ROOT_DIR

BUILD_3DS := build/n3ds
TARGET_3DS := cinnamon
SOURCES_3DS := src src/n3ds
INCLUDES_3DS := src src/n3ds vendor vendor/stb/ds

BUILD_WIIU := build/wiiu

CMAKE ?= cmake
ifeq ($(OS),Windows_NT)
ifneq ($(wildcard /opt/devkitpro/msys2/usr/bin/cmake.exe),)
CMAKE := /opt/devkitpro/msys2/usr/bin/cmake.exe
endif
endif

DEVKITPRO_CMAKE_PATH := $(DEVKITPRO)
ifeq ($(OS),Windows_NT)
ifneq ($(strip $(DEVKITPRO)),)
CYGPATH := $(firstword $(wildcard C:/devkitPro/msys2/usr/bin/cygpath.exe) $(wildcard /usr/bin/cygpath))
ifneq ($(strip $(CYGPATH)),)
DEVKITPRO_CMAKE_PATH := $(shell "$(CYGPATH)" -u "$(DEVKITPRO)")
endif
endif
endif

.PHONY: help 3ds 3ds-clean wiiu wiiu-clean

help:
	@echo "Available targets: 3ds, 3ds-clean, wiiu, wiiu-clean"

wiiu:
	@"$(CMAKE)" --fresh -S "$(ROOT_DIR)" -B "$(ROOT_DIR)/$(BUILD_WIIU)" -G "Unix Makefiles" \
		-DCMAKE_TOOLCHAIN_FILE="$(DEVKITPRO_CMAKE_PATH)/cmake/WiiU.cmake" \
		-DPLATFORM=wiiu \
		-DCMAKE_BUILD_TYPE=Release
	@"$(CMAKE)" --build "$(ROOT_DIR)/$(BUILD_WIIU)"

wiiu-clean:
	@rm -rf "$(ROOT_DIR)/$(BUILD_WIIU)"

THREEDS_GOALS := 3ds

ifneq ($(filter $(THREEDS_GOALS),$(MAKECMDGOALS)),)

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to devkitPro>")
endif

3ds:
	@"$(CMAKE)" --fresh -S "$(ROOT_DIR)" -B "$(ROOT_DIR)/$(BUILD_3DS)" -G "Unix Makefiles" \
		-DCMAKE_TOOLCHAIN_FILE="$(DEVKITPRO_CMAKE_PATH)/cmake/3DS.cmake" \
		-DPLATFORM=n3ds \
		-DCMAKE_BUILD_TYPE=Release
	@"$(CMAKE)" --build "$(ROOT_DIR)/$(BUILD_3DS)"

endif

3ds-clean:
	@rm -rf "$(ROOT_DIR)/$(BUILD_3DS)"
