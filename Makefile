SHELL := /bin/bash

EMSDK ?= /Users/int/emsdk
EMSDK_ENV := $(EMSDK)/emsdk_env.sh
EMSDK_QUIET ?= 1
export EMSDK_QUIET

EMSCRIPTEN32_DIR := build/emscripten32
EMSCRIPTEN64_DIR := build/emscripten64

EMSCRIPTEN_COMMON_FLAGS := -G Ninja -DMOMO_ENABLE_RUST_CODE=Off
EMSCRIPTEN_TOOLCHAIN_FLAG := -DCMAKE_TOOLCHAIN_FILE="$$(dirname "$$(which emcc)")/cmake/Modules/Platform/Emscripten.cmake"

.PHONY: help wasm32-configure wasm32-build wasm64-configure wasm64-build wasm wasm-sync root-zip linux-root-zip page-install page-build page-dev page-preview page

help:
	@echo "Useful targets:"
	@echo "  wasm32-build    Configure/build 32-bit wasm analyzers"
	@echo "  wasm64-build    Configure/build 64-bit wasm analyzers"
	@echo "  wasm            Build both wasm32 and wasm64 analyzers"
	@echo "  wasm-sync       Copy wasm outputs into page/public"
	@echo "  root-zip        Create page/public/root.zip if root exists"
	@echo "  linux-root-zip  Create page/public/linux-root.zip from local libs"
	@echo "  page-build      Build wasm + page production bundle"
	@echo "  page-dev        Build wasm, sync assets, start Vite dev server"
	@echo "  page-preview    Build and preview production page"

wasm32-configure:
	@source "$(EMSDK_ENV)" >/dev/null && \
	cmake -S . -B "$(EMSCRIPTEN32_DIR)" $(EMSCRIPTEN_COMMON_FLAGS) $(EMSCRIPTEN_TOOLCHAIN_FLAG)

wasm32-build:
	@[ -f "$(EMSCRIPTEN32_DIR)/CMakeCache.txt" ] || $(MAKE) wasm32-configure
	@source "$(EMSDK_ENV)" >/dev/null && \
	cmake --build "$(EMSCRIPTEN32_DIR)" --target analyzer linux-analyzer

wasm64-configure:
	@source "$(EMSDK_ENV)" >/dev/null && \
	cmake -S . -B "$(EMSCRIPTEN64_DIR)" $(EMSCRIPTEN_COMMON_FLAGS) -DMOMO_EMSCRIPTEN_MEMORY64=On $(EMSCRIPTEN_TOOLCHAIN_FLAG)

wasm64-build:
	@[ -f "$(EMSCRIPTEN64_DIR)/CMakeCache.txt" ] || $(MAKE) wasm64-configure
	@source "$(EMSDK_ENV)" >/dev/null && \
	cmake --build "$(EMSCRIPTEN64_DIR)" --target analyzer linux-analyzer

wasm: wasm32-build wasm64-build

wasm-sync: wasm
	@mkdir -p page/public/32 page/public/64 page/public/linux32 page/public/linux64
	@cp "$(EMSCRIPTEN32_DIR)/artifacts/analyzer.js" page/public/32/
	@cp "$(EMSCRIPTEN32_DIR)/artifacts/analyzer.wasm" page/public/32/
	@cp "$(EMSCRIPTEN64_DIR)/artifacts/analyzer.js" page/public/64/
	@cp "$(EMSCRIPTEN64_DIR)/artifacts/analyzer.wasm" page/public/64/
	@cp "$(EMSCRIPTEN32_DIR)/artifacts/linux-analyzer.js" page/public/linux32/
	@cp "$(EMSCRIPTEN32_DIR)/artifacts/linux-analyzer.wasm" page/public/linux32/
	@cp "$(EMSCRIPTEN64_DIR)/artifacts/linux-analyzer.js" page/public/linux64/
	@cp "$(EMSCRIPTEN64_DIR)/artifacts/linux-analyzer.wasm" page/public/linux64/

root-zip:
	@if [ -d build/release/artifacts/root ]; then \
		rm -f page/public/root.zip; \
		(cd build/release/artifacts && zip -r "$(CURDIR)/page/public/root.zip" ./root >/dev/null); \
		echo "Created page/public/root.zip"; \
	else \
		echo "Skipping root.zip (build/release/artifacts/root not found)"; \
	fi

linux-root-zip:
	@if [ -f page/public/linux-root.zip ] && [ "$(FORCE_LINUX_ROOT_ZIP)" != "1" ]; then \
		echo "Using existing page/public/linux-root.zip"; \
	else \
		tmp="$$(mktemp -d)"; \
		cleanup() { rm -rf "$$tmp"; }; \
		trap cleanup EXIT; \
		mkdir -p "$$tmp/lib/x86_64-linux-gnu" "$$tmp/lib64"; \
		copy_first() { \
			dst="$$1"; shift; \
			for src in "$$@"; do \
				if [ -f "$$src" ]; then \
					mkdir -p "$$tmp/$$(dirname "$$dst")"; \
					cp -f "$$src" "$$tmp/$$dst"; \
					return 0; \
				fi; \
			done; \
			return 1; \
		}; \
		loader_src=""; \
		for cand in lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 lib64/ld-linux-x86-64.so.2 usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 usr/lib64/ld-linux-x86-64.so.2; do \
			if [ -f "$$cand" ]; then loader_src="$$cand"; break; fi; \
		done; \
		if [ -z "$$loader_src" ]; then \
			echo "Skipping linux-root.zip (ld-linux-x86-64.so.2 not found in local lib directories)"; \
			exit 0; \
		fi; \
		cp -f "$$loader_src" "$$tmp/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"; \
		cp -f "$$loader_src" "$$tmp/lib64/ld-linux-x86-64.so.2"; \
		for so in libc.so.6 libdl.so.2 libpthread.so.0 libm.so.6 librt.so.1 libgcc_s.so.1 libstdc++.so.6; do \
			copy_first "lib/x86_64-linux-gnu/$$so" \
				"lib/x86_64-linux-gnu/$$so" \
				"lib64/$$so" \
				"usr/lib/x86_64-linux-gnu/$$so" \
				"usr/lib64/$$so" \
				"usr/local/lib/$$so" || true; \
		done; \
		rm -f page/public/linux-root.zip; \
		(cd "$$tmp" && zip -r "$(CURDIR)/page/public/linux-root.zip" lib lib64 >/dev/null); \
		echo "Created page/public/linux-root.zip (minimal glibc sysroot)"; \
	fi

page-install:
	@[ -d page/node_modules ] || (cd page && npm ci)

page-build: wasm-sync root-zip linux-root-zip page-install
	@cd page && npm run build

page-dev: wasm-sync linux-root-zip page-install
	@cd page && npm run dev

page-preview: page-build
	@cd page && npm run preview

page: page-build
