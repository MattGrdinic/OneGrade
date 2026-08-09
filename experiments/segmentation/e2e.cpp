// OneGrade — end-to-end check of the Magic Grade region path, outside Resolve.
//
// Loads a real frame, runs the converted model, assigns regions to a sample set exactly as the
// plugin does, and reports what Magic Grade would decide. This is the last thing that can be
// verified without the host: everything after it is Resolve handing over pixels.
#include <cmath>          // stb_image.h uses powf/pow without including it
#include <cstring>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "OneGradeAnalysis.h"
#include "OneGradeSegment.h"
#include <chrono>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: e2e MODEL_DIR FRAME.png [...]\n"); return 2; }
    og::seg::Segmenter seg;
    char pp[512], bp[512];
    snprintf(pp, sizeof pp, "%s/ade20k.param", argv[1]);
    snprintf(bp, sizeof bp, "%s/ade20k.bin", argv[1]);
    if (!seg.load(pp, bp)) { fprintf(stderr, "model failed to load from %s\n", argv[1]); return 2; }
    printf("model loaded (input %d)\n", seg.size());

    for (int f = 2; f < argc; ++f) {
        int w = 0, h = 0, ch = 0;
        unsigned char* img = stbi_load(argv[f], &w, &h, &ch, 3);
        if (!img) { fprintf(stderr, "cannot read %s\n", argv[f]); continue; }

        // The frames are already display-referred (exported after Creative Grade), which is
        // what the model wants -- in the plugin this is the thumbnail og::process() produces.
        std::vector<unsigned char> thumb((size_t)512 * 512 * 3);
        for (int y = 0; y < 512; ++y)
            for (int x = 0; x < 512; ++x) {
                const unsigned char* s = img + (((size_t)(y * h / 512) * w) + (x * w / 512)) * 3;
                unsigned char* d = &thumb[((size_t)y * 512 + x) * 3];
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            }

        auto t0 = std::chrono::steady_clock::now();
        std::vector<unsigned char> mask; int mw = 0, mh = 0;
        const bool ok = seg.run(thumb.data(), 512, 512, mask, mw, mh);
        auto t1 = std::chrono::steady_clock::now();
        if (!ok) { fprintf(stderr, "inference failed\n"); stbi_image_free(img); continue; }

        // A sample set on the same grid the plugin uses, carrying normalised positions. v is
        // OFX's bottom-up convention, so it is flipped relative to the image row.
        oga::SampleSet S;
        const int step = 8;
        for (int y = 0; y < h; y += step)
            for (int x = 0; x < w; x += step) {
                const unsigned char* s = img + ((size_t)y * w + x) * 3;
                S.rgb.push_back(s[0]/255.f); S.rgb.push_back(s[1]/255.f); S.rgb.push_back(s[2]/255.f);
                S.band.push_back((uint8_t)std::min(2, (int)((long long)(h - 1 - y) * 3 / h)));
                S.u.push_back((float)x / (float)w);
                S.v.push_back(1.0f - (float)y / (float)h);
            }
        if (!oga::assign_regions(S, mask, mw, mh)) { fprintf(stderr, "assign failed\n"); stbi_image_free(img); continue; }

        // enc 1 = gamma 2.2, cam 1 = DWG/DI: the frames are display-referred already, so this
        // is close to a pass-through and the Lab numbers land where the harness put them.
        oga::RegionStat st[oga::kRegionN];
        float P[oga::kParamN] = {0,0,0, 0,1,1, 0,0, 0,1, 0,6500, 0};
        oga::region_stats(S, 1, 1, P, st);

        const char* name = strrchr(argv[f], '/'); name = name ? name + 1 : argv[f];
        printf("\n=== %s  (%dx%d, %zu samples, mask %dx%d, %.0f ms) ===\n",
               name, w, h, S.size(), mw, mh,
               std::chrono::duration<double, std::milli>(t1 - t0).count());
        for (int r = 0; r < oga::kRegionN; ++r)
            if (st[r].cover >= 1.f)
                printf("   %-8s %5.1f%%  L*%6.1f b*%+6.1f\n",
                       oga::region_name(r), st[r].cover, st[r].L, st[r].b);
        for (int c = 0; c < 2; ++c) {
            oga::MagicChoice m = oga::magic_decide(st, c);
            if (!m.ok) { printf("   press %d: NO MOVE\n", c + 1); break; }
            printf("   press %d: %-8s -> %-11s %s\n", c + 1, oga::region_name(m.subject),
                   (m.param == 6) ? "Offset Temp" : "Gain Temp", m.sign > 0 ? "+ve" : "-ve");
        }
        stbi_image_free(img);
    }
    return 0;
}
