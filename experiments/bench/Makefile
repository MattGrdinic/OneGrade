# OneGrade offline bench. Links the plugin's own headers plus ncnn, so it runs the same grade
# solve, the same decision and the same segmentation the plugin does.
ROOT   := ../..
NCNN   := $(ROOT)/build/ncnn
CXXFLAGS := -std=c++17 -O2 -I$(ROOT)/src -I$(ROOT)/third_party/ncnn/src -I$(NCNN)/src -I.

bench: bench.cpp $(ROOT)/src/OneGradeCreative.h $(ROOT)/src/OneGradeAnalysis.h $(ROOT)/src/OneGradeSegment.h
	$(CXX) $(CXXFLAGS) bench.cpp $(NCNN)/src/libncnn.a -o bench

clean:
	rm -f bench
