// OneGrade — semantic segmentation, for Magic Grade's region masks.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NOT PART OF THE GOLDEN RULE, for the same reasons as OneGradeAnalysis.h: CPU only, runs once
// per button press, produces region labels rather than pixels, and is never touched by a kernel.
//
// ---------------------------------------------------------------------------------------
// WHY A MODEL AT ALL
//
// Direction and permission are properties of what a thing IS. Cooling the dark half of a frame
// is right when it is water and wrong when it is skin in shadow, and no amount of luminance or
// chroma statistics recovers the difference. Two cheap region-finders were measured against
// eight frames and each failed exactly where the other worked: top/bottom bands found sky over
// water on a beach (db* +43) and nothing on a downward city view, while 2-means in (a*, b*) did
// the reverse. That is the case for segmentation -- not that it is nicer, but that nothing
// cheaper covers the range of shots.
//
// ---------------------------------------------------------------------------------------
// WHICH MODEL, AND WHY THAT ONE
//
// PP-MobileSeg-Base (PaddleSeg, Apache-2.0), converted to ncnn. See models/README.md.
//
// CHOSEN ON LICENCE FIRST AND IT TURNED OUT TO BE BETTER ANYWAY. NVIDIA's SegFormer-B0 is the
// obvious candidate and converts more easily, but its licence restricts use to "research or
// evaluation purposes only" -- and that lands on the END USER, not the distributor. A colourist
// grading a paid job is doing neither, so no amount of relicensing OneGrade could have fixed
// it; the restriction would simply have moved onto the people the plugin exists for.
//
// PP-MobileSeg is Apache-2.0 with no commercial restriction, and on measurement it beat the
// alternative on every axis that matters here: 41.57% mIoU against ~37.4%, 96 ms against 251,
// 4.77 GFLOPs against 17.2. It also removed the need for a distillation fallback, which would
// have cost a training set and baked in a teacher's own flaws.
//
// ---------------------------------------------------------------------------------------
// IT IS ALLOWED TO BE ABSENT
//
// If the model file is missing or fails to load, ready() stays false and the caller falls back
// to the heuristic stub. That is deliberate rather than defensive: Magic Grade is explicitly
// not a fail-safe tool, and a plugin that refuses to load because a resource is missing is
// worse than one that quietly does less. What it must NEVER do is silently degrade without
// saying so -- the panel reports which region source was used, because a missing resource that
// changes behaviour without announcing it is the single most repeated bug in this project
// (the CUDA CPU fallback, the Windows LUT directory, the LUT encode override).
#pragma once
#include "OneGradeAnalysis.h"

#include <algorithm>
#include <string>
#include <vector>

#include "net.h"

namespace og {
namespace seg {

// ---------------------------------------------------------------------------------------
// ADE20K's 150 classes -> the seven regions a grade treats differently.
//
// GENERATED, NOT HAND-WRITTEN. Emitted from the same region_of() the offline harness uses
// (experiments/segmentation/regions.py), so the plugin and the experiment cannot disagree about
// what a label means. Regenerate rather than edit -- the mapping is matched on whole words
// there, which is not a detail: substring matching once put a car seat in WATER because "seat"
// contains "sea", 36% of a frame in the wrong region with entirely plausible numbers.
using og::analysis::R_SKY;    using og::analysis::R_WATER; using og::analysis::R_SKIN;
using og::analysis::R_VEG;    using og::analysis::R_TERRAIN; using og::analysis::R_GROUND;
using og::analysis::R_BUILT;  using og::analysis::R_OTHER;

// 150 ADE20K classes -> SKY 1, WATER 7, SKIN 1, VEGETATION 6, TERRAIN 8, GROUND 9, BUILT 32, OTHER 86
static const unsigned char kAde20kToRegion[150] = {
    R_BUILT, R_BUILT, R_SKY, R_GROUND, R_VEG, R_GROUND,           //   0 wall, building, sky, floor, tree, ceiling
    R_GROUND, R_BUILT, R_BUILT, R_VEG, R_OTHER, R_GROUND,         //   6 road, bed , windowpane, grass, cabinet, sidewalk
    R_SKIN, R_TERRAIN, R_BUILT, R_BUILT, R_TERRAIN, R_VEG,        //  12 person, earth, door, table, mountain, plant
    R_BUILT, R_BUILT, R_BUILT, R_WATER, R_OTHER, R_BUILT,         //  18 curtain, chair, car, water, painting, sofa
    R_BUILT, R_BUILT, R_WATER, R_OTHER, R_OTHER, R_VEG,           //  24 shelf, house, sea, mirror, rug, field
    R_OTHER, R_BUILT, R_BUILT, R_OTHER, R_TERRAIN, R_BUILT,       //  30 armchair, seat, fence, desk, rock, wardrobe
    R_OTHER, R_OTHER, R_BUILT, R_BUILT, R_OTHER, R_BUILT,         //  36 lamp, bathtub, railing, cushion, base, box
    R_BUILT, R_BUILT, R_OTHER, R_OTHER, R_TERRAIN, R_OTHER,       //  42 column, signboard, chest of drawers, counter, sand, 
    R_BUILT, R_OTHER, R_OTHER, R_OTHER, R_TERRAIN, R_GROUND,      //  48 skyscraper, fireplace, refrigerator, grandstand, pat
    R_GROUND, R_OTHER, R_WATER, R_OTHER, R_BUILT, R_GROUND,       //  54 runway, case, pool table, pillow, screen door, stair
    R_WATER, R_GROUND, R_BUILT, R_OTHER, R_BUILT, R_OTHER,        //  60 river, bridge, bookcase, blind, coffee table, toilet
    R_VEG, R_OTHER, R_TERRAIN, R_OTHER, R_OTHER, R_OTHER,         //  66 flower, book, hill, bench, countertop, stove
    R_VEG, R_OTHER, R_OTHER, R_BUILT, R_OTHER, R_OTHER,           //  72 palm, kitchen island, computer, swivel chair, boat, 
    R_OTHER, R_OTHER, R_BUILT, R_OTHER, R_OTHER, R_BUILT,         //  78 arcade machine, hovel, bus, towel, light, truck
    R_BUILT, R_OTHER, R_OTHER, R_BUILT, R_OTHER, R_OTHER,         //  84 tower, chandelier, awning, streetlight, booth, telev
    R_OTHER, R_TERRAIN, R_OTHER, R_BUILT, R_TERRAIN, R_OTHER,     //  90 airplane, dirt track, apparel, pole, land, bannister
    R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER,         //  96 escalator, ottoman, bottle, buffet, poster, stage
    R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER,         // 102 van, ship, fountain, conveyer belt, canopy, washer
    R_OTHER, R_WATER, R_OTHER, R_OTHER, R_OTHER, R_WATER,         // 108 plaything, swimming pool, stool, barrel, basket, wat
    R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER,         // 114 tent, bag, minibike, cradle, oven, ball
    R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER,         // 120 food, step, tank, trade name, microwave, pot
    R_OTHER, R_OTHER, R_WATER, R_OTHER, R_BUILT, R_OTHER,         // 126 animal, bicycle, lake, dishwasher, screen, blanket
    R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER,         // 132 sculpture, hood, sconce, vase, traffic light, tray
    R_OTHER, R_OTHER, R_GROUND, R_BUILT, R_OTHER, R_OTHER,        // 138 ashcan, fan, pier, crt screen, plate, monitor
    R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_OTHER,         // 144 bulletin board, shower, radiator, glass, clock, flag
};

// ImageNet normalisation, which is what every ADE20K checkpoint expects. ncnn folds the mean
// subtraction and the scale into one substract_mean_normalize() call, so the scale is 1/(255*std).
static const float kMean[3] = { 0.485f * 255.f, 0.456f * 255.f, 0.406f * 255.f };
static const float kNorm[3] = { 1.f / (0.229f * 255.f), 1.f / (0.224f * 255.f), 1.f / (0.225f * 255.f) };

class Segmenter {
public:
    // Both files must be present and parse. ncnn returns non-zero on failure and leaves the Net
    // usable-but-empty, which would infer garbage rather than fail, so the return codes are
    // checked rather than assumed.
    bool load(const std::string& paramPath, const std::string& binPath, int inputSize = 512)
    {
        m_ready = false;
        m_net.clear();
        m_net.opt.use_vulkan_compute = false;
        // One thread on purpose. This runs on the UI thread during a button press, and taking
        // every core for a second to do it is worse for the host than taking one for two.
        m_net.opt.num_threads = 1;
        m_net.opt.lightmode = true;
        if (m_net.load_param(paramPath.c_str()) != 0) return false;
        if (m_net.load_model(binPath.c_str()) != 0)   return false;
        m_size = inputSize;
        m_ready = true;
        return true;
    }

    bool ready() const { return m_ready; }
    int  size()  const { return m_size; }

    // rgb: display-referred 8-bit RGB, w*h*3. Display-referred and NOT camera log, because the
    // model was trained on ordinary photographs and log footage is far outside that
    // distribution -- the same trap as measuring percentiles in Cineon.
    //
    // Returns a region label per output cell, at outW x outH, row 0 = TOP of the frame (image
    // convention, not OFX's bottom-up one; the caller flips).
    bool run(const unsigned char* rgb, int w, int h,
             std::vector<unsigned char>& regions, int& outW, int& outH) const
    {
        if (!m_ready || !rgb || w <= 0 || h <= 0) return false;
        ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb, ncnn::Mat::PIXEL_RGB, w, h, m_size, m_size);
        in.substract_mean_normalize(kMean, kNorm);

        ncnn::Extractor ex = m_net.create_extractor();
        if (ex.input("in0", in) != 0) return false;
        ncnn::Mat out;
        if (ex.extract("out0", out) != 0) return false;
        if (out.w <= 0 || out.h <= 0 || out.c < 2) return false;

        // Channel-wise argmax, SUBSAMPLED. ncnn hands back logits as c planes of w*h, so the
        // winning class for a cell is a stride walk rather than a contiguous read -- 150 reads
        // per cell, which at a full 512x512 output is 39 million of them for a mask that then
        // gets sampled at roughly forty thousand points and reduced to seven percentages.
        //
        // PP-MobileSeg upsamples to the input resolution internally, so the fine detail is
        // interpolated rather than earned; taking every Nth cell discards nothing that was
        // really there. Capped at 256 on the long side, which is already finer than the 128x128
        // the previous model produced natively.
        const int cap = 256;
        const int step = std::max(1, (out.w > out.h ? out.w : out.h) / cap);
        outW = (out.w + step - 1) / step;
        outH = (out.h + step - 1) / step;
        regions.assign((size_t)outW * outH, (unsigned char)R_OTHER);
        const int classes = out.c < 150 ? out.c : 150;
        for (int y = 0; y < outH; ++y) {
            const int sy = y * step;
            for (int x = 0; x < outW; ++x) {
                const int sx = x * step;
                int best = 0; float bv = -1e30f;
                for (int c = 0; c < classes; ++c) {
                    const float v = out.channel(c).row(sy)[sx];
                    if (v > bv) { bv = v; best = c; }
                }
                regions[(size_t)y * outW + x] = kAde20kToRegion[best];
            }
        }
        return true;
    }

private:
    mutable ncnn::Net m_net;
    bool m_ready = false;
    int  m_size  = 512;
};

} // namespace seg
} // namespace og
