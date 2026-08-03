// OneGrade — minimal .cube 3D LUT parser (host side).
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>

struct CubeLUT
{
    int size = 0;                       // 3D size N (0 = not loaded / unsupported)
    float domainMin[3] = {0.f,0.f,0.f};
    float domainMax[3] = {1.f,1.f,1.f};
    std::vector<float> data;            // N*N*N*3, red index varies fastest
    std::string loadedPath;

    bool valid() const { return size >= 2 && (int)data.size() == size*size*size*3; }

    // Returns true if the LUT at `path` is loaded (or already current). Only 3D LUTs supported.
    bool load(const std::string& path)
    {
        if (path == loadedPath && valid()) return true;
        size = 0; data.clear(); loadedPath.clear();
        domainMin[0]=domainMin[1]=domainMin[2]=0.f;
        domainMax[0]=domainMax[1]=domainMax[2]=1.f;
        if (path.empty()) return false;

        std::ifstream f(path.c_str());
        if (!f.is_open()) return false;

        int n = 0;
        std::vector<float> tmp;
        std::string line;
        while (std::getline(f, line))
        {
            // strip CR / leading spaces
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t p = line.find_first_not_of(" \t");
            if (p == std::string::npos) continue;
            if (line[p] == '#') continue;

            std::istringstream ss(line.substr(p));
            std::string tok; ss >> tok;
            if (tok == "LUT_3D_SIZE") { ss >> n; if (n >= 2) tmp.reserve((size_t)n*n*n*3); }
            else if (tok == "LUT_1D_SIZE") { return false; } // unsupported for now
            else if (tok == "DOMAIN_MIN") { ss >> domainMin[0] >> domainMin[1] >> domainMin[2]; }
            else if (tok == "DOMAIN_MAX") { ss >> domainMax[0] >> domainMax[1] >> domainMax[2]; }
            else if (tok == "TITLE" || tok == "LUT_3D_INPUT_RANGE") { /* ignore */ }
            else {
                // maybe three floats (a data row). tok is the first number.
                char* end = nullptr;
                float r = std::strtof(tok.c_str(), &end);
                if (end == tok.c_str()) continue; // not a number
                float g, b;
                if (!(ss >> g >> b)) continue;
                tmp.push_back(r); tmp.push_back(g); tmp.push_back(b);
            }
        }
        if (n >= 2 && (int)tmp.size() == n*n*n*3)
        {
            size = n; data.swap(tmp); loadedPath = path;
            return true;
        }
        return false;
    }
};
