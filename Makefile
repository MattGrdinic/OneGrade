# OneGrade — cross-platform OpenFX plugin build.
# SPDX-License-Identifier: BSD-3-Clause
UNAME := $(shell uname -s)

SDK      := third_party/openfx
INCLUDES := -I$(SDK)/include -I$(SDK)/Support/include -I$(SDK)/Support/Plugins/include -Isrc
# OFX_SUPPORTS_OPENCLRENDER guards the body of processImagesOpenCL() — undefined,
# the OpenCL render compiles to an empty function while still being advertised.
# It is NOT the same flag as the (unrelated) OpenGL one; the names are the trap.
CXXFLAGS := --std=c++20 -fvisibility=hidden $(INCLUDES) -DOFX_SUPPORTS_OPENGLRENDER -DOFX_SUPPORTS_OPENCLRENDER
BUILD    := build
BUNDLE   := OneGrade.ofx.bundle

SUPPORT := ofxsCore ofxsImageEffect ofxsInteract ofxsLog ofxsMultiThread ofxsParams ofxsProperty ofxsPropertyValidation
SUPPORT_OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SUPPORT)))

ifeq ($(UNAME),Linux)
    CUDAPATH  ?= /usr/local/cuda
    NVCC      := $(CUDAPATH)/bin/nvcc
    CXXFLAGS  += -fPIC -DOFX_SUPPORTS_CUDARENDER
    LDFLAGS   := -shared -fvisibility=hidden -L$(CUDAPATH)/lib64 -lcuda -lcudart_static -lOpenCL
    BUNDLE_ARCH := Linux-x86-64
    PLUGIN_OBJS := $(BUILD)/OneGrade.o $(BUILD)/OpenCLKernel.o $(BUILD)/CudaKernel.o
else
    ARCH      := -arch arm64 -arch x86_64
    CXXFLAGS  += $(ARCH)
    LDFLAGS   := -bundle -fvisibility=hidden -framework OpenCL -framework Metal -framework AppKit $(ARCH)
    BUNDLE_ARCH := MacOS
    PLUGIN_OBJS := $(BUILD)/OneGrade.o $(BUILD)/OpenCLKernel.o $(BUILD)/MetalKernel.o
endif

BINDIR := $(BUNDLE)/Contents/$(BUNDLE_ARCH)

.PHONY: all install clean test bundle-luts
all: $(BINDIR)/OneGrade.ofx $(BUNDLE)/Contents/Info.plist bundle-luts

# Built-in look LUTs ship inside the bundle (regenerate with luts/generate_luts.py).
bundle-luts:
	@mkdir -p "$(BUNDLE)/Contents/Resources/LUTs"
	@cp -f luts/*.cube "$(BUNDLE)/Contents/Resources/LUTs/"

test: | $(BUILD)
	$(CXX) -std=c++17 -O2 test/pipeline_test.cpp -o $(BUILD)/pipeline_test
	$(BUILD)/pipeline_test

$(BINDIR)/OneGrade.ofx: $(PLUGIN_OBJS) $(SUPPORT_OBJS)
	@mkdir -p $(BINDIR)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(BUNDLE)/Contents/Info.plist: src/Info.plist
	@mkdir -p $(BUNDLE)/Contents
	cp src/Info.plist $@

$(BUILD)/OneGrade.o: src/OneGrade.cpp | $(BUILD)
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

install: all
	@mkdir -p /Library/OFX/Plugins
	cp -fr $(BUNDLE) /Library/OFX/Plugins/
	@echo "Installed -> /Library/OFX/Plugins/$(BUNDLE)"

clean:
	rm -rf $(BUILD) $(BUNDLE)
