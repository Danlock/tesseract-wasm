include third_party_versions.mk

ROOT?=$(PWD)

EMSDK_DIR=$(ROOT)/third_party/emsdk/upstream/emscripten
INSTALL_DIR=$(ROOT)/install

DIST_TARGETS=\
  dist/tesseract-core.wasm

.PHONY: lib
lib: $(DIST_TARGETS)

clean:
	rm -rf build dist install

clean-lib:
	rm build/*.{js,wasm}
	rm -rf dist

docker-build:
	docker build -t tess-builder .
	docker run \
		-u $(id -u):$(id -g) \
		--rm \
		-v $(PWD):/tess-wasm \
		-w /tess-wasm \
		-e ROOT=/tess-wasm \
		tess-builder \
		make -B lib

# nb. This is an order-only dependency in other targets.
build:
	mkdir -p build/

.PHONY: format
format:
	clang-format -i --style=google src/*.cpp

.PHONY: checkformat
checkformat:
	clang-format -Werror --dry-run --style=google src/*.cpp

.PHONY: release
release: clean lib typecheck test
	@which np || (echo "Install np from https://github.com/sindresorhus/np" && false)
	np minor

.PHONY: gh-pages
gh-pages:
	./update-gh-pages.sh

third_party/emsdk: third_party_versions.mk
	mkdir -p third_party/emsdk
	test -d $@/.git || git clone --depth 1 https://github.com/emscripten-core/emsdk.git $@
	cd $@ && git fetch origin $(EMSDK_COMMIT) && git checkout $(EMSDK_COMMIT)
	touch $@

build/emsdk.uptodate: third_party/emsdk | build
	third_party/emsdk/emsdk install latest
	third_party/emsdk/emsdk activate latest
	touch build/emsdk.uptodate

# Emscripten provides precompiled ports for popular libs.
EMCC_PORTS=\
	-sUSE_GIFLIB=1 \
	-sUSE_ZLIB=1 \
	-sUSE_LIBPNG=1 \
	-sUSE_LIBJPEG=1

# Compile flags for Leptonica. These turn off support for various image formats to
# reduce size. We don't need this since the browser includes this functionality.
LEPTONICA_FLAGS=\
	-DLIBWEBP_SUPPORT=OFF \
	-DOPENJPEG_SUPPORT=OFF \
	-DCMAKE_C_FLAGS="$(EMCC_PORTS)" \
	-DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR)

third_party/leptonica: third_party_versions.mk
	mkdir -p third_party/leptonica
	test -d $@/.git || git clone --depth 1 https://github.com/DanBloomberg/leptonica.git $@
	cd $@ && git fetch origin $(LEPTONICA_COMMIT) && git checkout $(LEPTONICA_COMMIT)
	touch $@

build/leptonica.uptodate: third_party/leptonica build/emsdk.uptodate
	mkdir -p build/leptonica
	cd build/leptonica && $(EMSDK_DIR)/emcmake cmake -G Ninja ../../third_party/leptonica $(LEPTONICA_FLAGS)
	cd build/leptonica && $(EMSDK_DIR)/emmake ninja
	cd build/leptonica && $(EMSDK_DIR)/emmake ninja install
	touch build/leptonica.uptodate

# Additional preprocessor defines for Tesseract.
#
# Defining `TESSERACT_IMAGEDATA_AS_PIX` disables some unnecessary internal use
# of the PNG format. See Tesseract commit 6bcb941bcff5e73b62ecc8d2aa5691d3e0e7afc0.
TESSERACT_DEFINES=-DTESSERACT_IMAGEDATA_AS_PIX

# Compile flags for Tesseract. These turn off support for unused features and
# utility programs to reduce size and build times.
#
# 128-bit wide SIMD is enabled via `HAVE_SSE4_1` and the `-msimd128` flags. The
# AVX flags are disabled because they require instructions beyond what WASM SIMD
# supports.
TESSERACT_FLAGS=\
  -DBUILD_TESSERACT_BINARY=OFF \
  -DBUILD_TRAINING_TOOLS=OFF \
  -DDISABLE_CURL=ON \
  -DDISABLED_LEGACY_ENGINE=ON \
  -DENABLE_LTO=ON \
  -DGRAPHICS_DISABLED=ON \
  -DHAVE_AVX=OFF \
  -DHAVE_AVX2=OFF \
  -DHAVE_AVX512F=OFF \
  -DHAVE_FMA=OFF \
  -DHAVE_SSE4_1=ON \
  -DLeptonica_DIR=$(INSTALL_DIR)/lib/cmake/leptonica \
  -DCMAKE_CXX_FLAGS="$(TESSERACT_DEFINES) -msimd128" \
  -DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR)

third_party/tesseract: third_party_versions.mk
	mkdir -p third_party/tesseract
	test -d $@/.git || git clone --depth 1 https://github.com/tesseract-ocr/tesseract.git $@
	cd $@ && git fetch origin $(TESSERACT_COMMIT) && git checkout $(TESSERACT_COMMIT)
	cd $@ && git stash && git apply ../../patches/tesseract.diff
	touch $@

third_party/tessdata_fast:
	mkdir -p third_party/tessdata_fast
	git clone --depth 1 https://github.com/tesseract-ocr/tessdata_fast.git $@

build/tesseract.uptodate: build/leptonica.uptodate third_party/tesseract
	mkdir -p build/tesseract
	(cd build/tesseract && $(EMSDK_DIR)/emcmake cmake -G Ninja ../../third_party/tesseract $(TESSERACT_FLAGS))
	(cd build/tesseract && $(EMSDK_DIR)/emmake ninja)
	(cd build/tesseract && $(EMSDK_DIR)/emmake ninja install)
	touch build/tesseract.uptodate

# emcc flags. `-Os` minifies the JS wrapper and optimises WASM code size.
# We also disable filesystem support to reduce the JS wrapper size.
# Enabling memory growth is important since loading document images may
# require large blocks of memory.
EMCC_FLAGS =\
  -O0\
  -g3\
  -sSTANDALONE_WASM\
  -sPURE_WASI\
  -sEXPORTED_FUNCTIONS="_malloc,_free,_pixReadMem"\
  --minify 0\
  $(EMCC_PORTS)\
  --no-entry\
  -sFILESYSTEM=0 \
  -sALLOW_MEMORY_GROWTH\
  -sMAXIMUM_MEMORY=1GB \
  -std=c++20 \
  -fexperimental-library

# Build main WASM binary for browsers that support WASM SIMD.
build/tesseract-core.js build/tesseract-core.wasm: src/lib.cpp src/tesseract-init.js build/tesseract.uptodate
	$(EMSDK_DIR)/emcc src/lib.cpp $(EMCC_FLAGS) \
		-I$(INSTALL_DIR)/include/ -L$(INSTALL_DIR)/lib/ -ltesseract -lleptonica -lembind \
		-o build/tesseract-core.js
	cp src/tesseract-core.d.ts build/

dist/tesseract-core.wasm: build/tesseract-core.wasm
	mkdir -p dist/
	cp $< $@
