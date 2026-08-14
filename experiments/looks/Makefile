# OneGrade look-reference measurement. Links the plugin's own headers plus ncnn, so it picks its
# percentiles with the same function the solve does and segments with the same model.
ROOT   := ../..
NCNN   := $(ROOT)/build/ncnn
# stb_image.h comes from ncnn's vendored copy, same as the bench gets it -- there is no separate
# checkout of stb anywhere in the tree.
CXXFLAGS := -std=c++17 -O2 -I$(ROOT)/src -I$(ROOT)/third_party/ncnn/src -I$(NCNN)/src -I.

all: looks variants

variants: variants.cpp $(ROOT)/src/OneGradeCreative.h $(ROOT)/src/OneGradeAnalysis.h $(ROOT)/src/OneGradeSegment.h
	$(CXX) $(CXXFLAGS) variants.cpp $(NCNN)/src/libncnn.a -o variants

looks: looks.cpp $(ROOT)/src/OneGradeCreative.h $(ROOT)/src/OneGradeAnalysis.h $(ROOT)/src/OneGradeSegment.h
	$(CXX) $(CXXFLAGS) looks.cpp $(NCNN)/src/libncnn.a -o looks

clean:
	rm -f looks variants
