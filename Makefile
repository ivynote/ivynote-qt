# Thin wrapper around CMake; `make help` lists the targets.
BUILD_DIR   ?= build
BUILD_TYPE  ?= Release
GENERATOR   ?= Ninja
PREFIX      ?= /usr/local
CMAKE_FLAGS ?=
# Server binary for the integration tests, e.g. ../ivynote/ivynote
IVY_SERVER_BIN ?=

# AppImage uses a separate build tree and always configures it so changed
# version/dependency flags cannot silently reuse an old CMake cache.
APPIMAGE_BUILD_DIR ?= build-appimage
OUT_DIR ?= dist
TOOLS_DIR ?= $(CURDIR)/tools
export TOOLS_DIR
VERSION ?= $(shell sh packaging/linux/version.sh)
APPIMAGE_PREFIX_PATH ?= $(DEPS);$(QT_ROOT_DIR)

.PHONY: appimage-tools appimage help build debug configure run test test-integration install clean rebuild

help:
	@echo "make build              Release build in $(BUILD_DIR)/"
	@echo "make appimage [VERSION=x.y.z]  Build, test and package into $(OUT_DIR)/"
	@echo "make appimage-tools     Download missing AppImage packaging tools"
	@echo "make debug              Debug build in build-debug/"
	@echo "make run                Build and start the app"
	@echo "make test               Build and run all tests (integration skipped without IVY_SERVER_BIN)"
	@echo "make test-integration IVY_SERVER_BIN=/path/to/ivynote"
	@echo "make install PREFIX=..  Install the app (default $(PREFIX))"
	@echo "make clean / rebuild"

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $(CMAKE_FLAGS)

configure: $(BUILD_DIR)/CMakeCache.txt

build: configure
	cmake --build $(BUILD_DIR)

debug:
	$(MAKE) build BUILD_DIR=build-debug BUILD_TYPE=Debug

run: build
	./$(BUILD_DIR)/ivynote

test: build
	QT_QPA_PLATFORM=offscreen IVY_SERVER_BIN=$(IVY_SERVER_BIN) ctest --test-dir $(BUILD_DIR) --output-on-failure

test-integration: build
	@test -n "$(IVY_SERVER_BIN)" || { echo "set IVY_SERVER_BIN=/path/to/ivynote"; exit 1; }
	QT_QPA_PLATFORM=offscreen IVY_SERVER_BIN=$(IVY_SERVER_BIN) ctest --test-dir $(BUILD_DIR) -R integration --output-on-failure

install: build
	cmake --install $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) build-debug

rebuild: clean build

appimage:
	cmake -S . -B "$(APPIMAGE_BUILD_DIR)" -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DIVY_VERSION="$(VERSION)" -DCMAKE_PREFIX_PATH="$(APPIMAGE_PREFIX_PATH)" $(CMAKE_FLAGS)
	cmake --build "$(APPIMAGE_BUILD_DIR)"
	QT_QPA_PLATFORM=offscreen ctest --test-dir "$(APPIMAGE_BUILD_DIR)" --output-on-failure
	packaging/linux/build-appimage.sh "$(APPIMAGE_BUILD_DIR)" "$(VERSION)" "$(OUT_DIR)"

appimage-tools:
	bash packaging/linux/prepare-appimage-tools.sh
