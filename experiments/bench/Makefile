# OneGrade offline bench. Links the plugin's own headers plus ncnn, so it runs the same grade
# solve, the same decision and the same segmentation the plugin does.
ROOT   := ../..
NCNN   := $(ROOT)/build/ncnn
CXXFLAGS := -std=c++17 -O2 -I$(ROOT)/src -I$(ROOT)/third_party/ncnn/src -I$(NCNN)/src -I.

# EVERY plugin header the bench includes, not just the three the grade solve lives in.
# OneGradePipeline.h was missing, which is the colour pipeline itself -- so a change to
# decode_log left a stale binary and the bench silently graded with the old curve. It cost
# a run that showed a new camera changing nothing, which read exactly like the camera being
# ignored. Same failure the run.sh wrapper already guards against ("always make, never make
# if missing"), reached by a different route: the rule ran, it just did not think it had to.
DEPS := $(wildcard $(ROOT)/src/*.h) $(wildcard *.h)

bench: bench.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) bench.cpp $(NCNN)/src/libncnn.a -o bench

clean:
	rm -f bench
