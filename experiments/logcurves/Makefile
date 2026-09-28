# Ground-truth log-curve extractor. Builds two small tools; the Resolve half is extract.py.
ROOT := ../..
CXXFLAGS := -std=c++17 -O2 -I$(ROOT)/src -I$(ROOT)/third_party/ncnn/src -I.

all: makeramp genheader

makeramp: makeramp.cpp png16.h
	$(CXX) $(CXXFLAGS) makeramp.cpp -lz -o makeramp

genheader: genheader.cpp
	$(CXX) $(CXXFLAGS) genheader.cpp -o genheader

clean:
	rm -f makeramp genheader
