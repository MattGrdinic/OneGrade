# OneGrade — cross-platform OpenFX plugin build.
# SPDX-License-Identifier: BSD-3-Clause
UNAME := $(shell uname -s)

SDK      := third_party/openfx
BUILD    := build

# ncnn: the inference runtime for Magic Grade's region masks. Vendored (see
# third_party/ncnn/VENDORED.md) and built from source into $(BUILD), so there is no network
# step, no package manager, and nothing to install before `make` works.
#
# Built ONCE and then left alone -- the rule depends on ncnn's own CMakeLists, so a normal edit
# to OneGrade never re-enters it. A clean build pays 22 s for the universal library.
NCNN_SRC   := third_party/ncnn
NCNN_BUILD := $(BUILD)/ncnn
NCNN_LIB   := $(NCNN_BUILD)/src/libncnn.a
NCNN_OPTS  := -DCMAKE_BUILD_TYPE=Release -DNCNN_VULKAN=OFF -DNCNN_OPENMP=OFF \
              -DNCNN_SHARED_LIB=OFF -DNCNN_SIMPLEOCV=OFF -DNCNN_BUILD_TOOLS=OFF \
              -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF

INCLUDES := -I$(SDK)/include -I$(SDK)/Support/include -I$(SDK)/Support/Plugins/include -Isrc \
            -I$(NCNN_SRC)/src -I$(NCNN_BUILD)/src
# OFX_SUPPORTS_OPENCLRENDER guards the body of processImagesOpenCL() — undefined,
# the OpenCL render compiles to an empty function while still being advertised.
# It is NOT the same flag as the (unrelated) OpenGL one; the names are the trap.
# -MMD -MP emits a .d file per object listing the headers it included, pulled back in at the
# bottom of this file. WITHOUT IT, EDITING A HEADER REBUILDS NOTHING. That is not a theoretical
# tidiness point: a one-line fix in OneGradeAnalysis.h was made, `make` reported success, the
# bundle was installed, and the old object file was still in it -- so the feature shipped with
# the bug it had just been fixed for, and the symptom (a control that reported its decision and
# then did nothing) sent the hunt to the analysis code, which was correct all along.
CXXFLAGS := --std=c++20 -fvisibility=hidden $(INCLUDES) -MMD -MP -DOFX_SUPPORTS_OPENGLRENDER -DOFX_SUPPORTS_OPENCLRENDER
BUNDLE   := OneGrade.ofx.bundle

SUPPORT := ofxsCore ofxsImageEffect ofxsInteract ofxsLog ofxsMultiThread ofxsParams ofxsProperty ofxsPropertyValidation
SUPPORT_OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SUPPORT)))

ifeq ($(UNAME),Linux)
    CUDAPATH  ?= /usr/local/cuda
    NVCC      := $(CUDAPATH)/bin/nvcc
    CXXFLAGS  += -fPIC -DOFX_SUPPORTS_CUDARENDER
    LDFLAGS   := -shared -fvisibility=hidden -L$(CUDAPATH)/lib64 -lcuda -lcudart_static -lOpenCL
    NCNN_ARCH :=
    BUNDLE_ARCH := Linux-x86-64
    PLUGIN_OBJS := $(BUILD)/OneGrade.o $(BUILD)/OpenCLKernel.o $(BUILD)/CudaKernel.o
else
    ARCH      := -arch arm64 -arch x86_64
    CXXFLAGS  += $(ARCH)
    LDFLAGS   := -bundle -fvisibility=hidden -framework OpenCL -framework Metal -framework AppKit -framework OpenGL $(ARCH)
    # ncnn has to be fat too, or the link fails on whichever slice it is missing.
    NCNN_ARCH := -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
    BUNDLE_ARCH := MacOS
    PLUGIN_OBJS := $(BUILD)/OneGrade.o $(BUILD)/OpenCLKernel.o $(BUILD)/MetalKernel.o
endif

BINDIR := $(BUNDLE)/Contents/$(BUNDLE_ARCH)

.PHONY: all install clean test bundle-luts bundle-model
all: $(BINDIR)/OneGrade.ofx $(BUNDLE)/Contents/Info.plist bundle-luts bundle-model

# Built-in look LUTs ship inside the bundle (regenerate with luts/generate_luts.py).
bundle-luts:
	@mkdir -p "$(BUNDLE)/Contents/Resources/LUTs"
	@cp -f luts/*.cube "$(BUNDLE)/Contents/Resources/LUTs/"

# The test binary is header-only too, so it gets the same treatment.
# Magic Grade's region model, resolved at runtime from the plugin binary's own path so it
# works on a render machine that has never seen this checkout. See models/README.md.
bundle-model:
	@mkdir -p "$(BUNDLE)/Contents/Resources/Model"
	@cp -f models/ade20k.param models/ade20k.bin "$(BUNDLE)/Contents/Resources/Model/"
	@cp -f THIRD-PARTY-NOTICES.md "$(BUNDLE)/Contents/Resources/"
	@cp -f models/LICENSE-Apache-2.0.txt "$(BUNDLE)/Contents/Resources/"
	@cp -f third_party/ncnn/LICENSE.txt "$(BUNDLE)/Contents/Resources/LICENSE-ncnn.txt"

test: | $(BUILD)
	$(CXX) -std=c++17 -O2 -MMD -MP test/pipeline_test.cpp -o $(BUILD)/pipeline_test
	$(BUILD)/pipeline_test

$(BINDIR)/OneGrade.ofx: $(PLUGIN_OBJS) $(SUPPORT_OBJS) $(NCNN_LIB)
	@mkdir -p $(BINDIR)
	$(CXX) $(PLUGIN_OBJS) $(SUPPORT_OBJS) $(NCNN_LIB) -o $@ $(LDFLAGS)

$(NCNN_LIB): $(NCNN_SRC)/CMakeLists.txt
	cmake -S $(NCNN_SRC) -B $(NCNN_BUILD) $(NCNN_OPTS) $(NCNN_ARCH)
	cmake --build $(NCNN_BUILD) -j

$(BUNDLE)/Contents/Info.plist: src/Info.plist
	@mkdir -p $(BUNDLE)/Contents
	cp src/Info.plist $@

$(BUILD)/OneGrade.o: src/OneGrade.cpp $(NCNN_LIB) | $(BUILD)
	$(CXX) -c $< -o $@ $(CXXFLAGS)
$(BUILD)/OpenCLKernel.o: src/OpenCLKernel.cpp | $(BUILD)
	$(CXX) -c $< -o $@ $(CXXFLAGS)
$(BUILD)/MetalKernel.o: src/MetalKernel.mm | $(BUILD)
	$(CXX) -c $< -o $@ $(CXXFLAGS)
$(BUILD)/CudaKernel.o: src/CudaKernel.cu | $(BUILD)
	$(NVCC) -c $< -o $@ --compiler-options="-fPIC"
$(BUILD)/%.o: $(SDK)/Support/Library/%.cpp | $(BUILD)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

$(BUILD):
	mkdir -p $(BUILD)

# Header dependencies generated by -MMD above. The leading '-' keeps a clean tree working, where
# no .d files exist yet.
-include $(wildcard $(BUILD)/*.d)

install: all
	@mkdir -p /Library/OFX/Plugins
	cp -fr $(BUNDLE) /Library/OFX/Plugins/
	@echo "Installed -> /Library/OFX/Plugins/$(BUNDLE)"

clean:
	rm -rf $(BUILD) $(BUNDLE)
