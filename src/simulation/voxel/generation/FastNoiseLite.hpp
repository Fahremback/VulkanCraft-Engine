// MIT License
//
// Copyright(c) 2023 Jordan Peck (jordan.me2@gmail.com)
// Copyright(c) 2023 Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>

class FastNoiseLite
{
public:
    enum NoiseType { NoiseType_OpenSimplex2, NoiseType_OpenSimplex2S, NoiseType_Cellular, NoiseType_Perlin, NoiseType_ValueCubic, NoiseType_Value };
    enum RotationType3D { RotationType3D_None, RotationType3D_ImproveXYPlanes, RotationType3D_ImproveXZPlanes };
    enum FractalType { FractalType_None, FractalType_FBm, FractalType_Ridged, FractalType_PingPong, FractalType_DomainWarpProgressive, FractalType_DomainWarpIndependent };
    enum CellularDistanceFunction { CellularDistanceFunction_Euclidean, CellularDistanceFunction_EuclideanSq, CellularDistanceFunction_Manhattan, CellularDistanceFunction_Natural };
    enum CellularReturnType { CellularReturnType_CellValue, CellularReturnType_Distance, CellularReturnType_Distance2, CellularReturnType_Distance2Add, CellularReturnType_Distance2Sub, CellularReturnType_Distance2Mul, CellularReturnType_Distance2Div };
    enum DomainWarpType { DomainWarpType_OpenSimplex2, DomainWarpType_OpenSimplex2Reduced, DomainWarpType_BasicGrid };

    FastNoiseLite(int seed = 1337) { SetSeed(seed); }

    void SetSeed(int seed) { mSeed = seed; }
    void SetFrequency(float frequency) { mFrequency = frequency; }
    void SetNoiseType(NoiseType noiseType) { mNoiseType = noiseType; }
    void SetRotationType3D(RotationType3D rotationType3D) { mRotationType3D = rotationType3D; }

    void SetFractalType(FractalType fractalType) { mFractalType = fractalType; }
    void SetFractalOctaves(int octaves) { mOctaves = octaves; }
    void SetFractalLacunarity(float lacunarity) { mLacunarity = lacunarity; }
    void SetFractalGain(float gain) { mGain = gain; }
    void SetFractalWeightedStrength(float weightedStrength) { mWeightedStrength = weightedStrength; }
    void SetFractalPingPongStrength(float pingPongStrength) { mPingPongStrength = pingPongStrength; }

    float GetNoise(float x, float y) const {
        x *= mFrequency; y *= mFrequency;
        return SinglePerlin2D(mSeed, x, y);
    }

    float GetNoise(float x, float y, float z) const {
        x *= mFrequency; y *= mFrequency; z *= mFrequency;
        return SinglePerlin3D(mSeed, x, y, z);
    }

private:
    static inline int FastFloor(float f) { return (f >= 0 ? (int)f : (int)f - 1); }
    static inline float GradCoord2D(int seed, int x, int y, float xd, float yd) {
        int hash = seed ^ x ^ (y * 57);
        hash = hash * hash * 60493;
        hash = (hash >> 13) ^ hash;
        float gradX = (hash & 1) ? 1.0f : -1.0f;
        float gradY = (hash & 2) ? 1.0f : -1.0f;
        return xd * gradX + yd * gradY;
    }
    static inline float GradCoord3D(int seed, int x, int y, int z, float xd, float yd, float zd) {
        int hash = seed ^ x ^ (y * 57) ^ (z * 131);
        hash = hash * hash * 60493;
        hash = (hash >> 13) ^ hash;
        float gradX = (hash & 1) ? 1.0f : -1.0f;
        float gradY = (hash & 2) ? 1.0f : -1.0f;
        float gradZ = (hash & 4) ? 1.0f : -1.0f;
        return xd * gradX + yd * gradY + zd * gradZ;
    }
    static inline float InterpQuintic(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }

    float SinglePerlin2D(int seed, float x, float y) const {
        int x0 = FastFloor(x);
        int y0 = FastFloor(y);
        float xd0 = x - x0;
        float yd0 = y - y0;
        float xd1 = xd0 - 1;
        float yd1 = yd0 - 1;

        float xs = InterpQuintic(xd0);
        float ys = InterpQuintic(yd0);

        float xf0 = std::lerp(GradCoord2D(seed, x0, y0, xd0, yd0), GradCoord2D(seed, x0 + 1, y0, xd1, yd0), xs);
        float xf1 = std::lerp(GradCoord2D(seed, x0, y0 + 1, xd0, yd1), GradCoord2D(seed, x0 + 1, y0 + 1, xd1, yd1), xs);

        return std::lerp(xf0, xf1, ys);
    }

    float SinglePerlin3D(int seed, float x, float y, float z) const {
        int x0 = FastFloor(x);
        int y0 = FastFloor(y);
        int z0 = FastFloor(z);
        float xd0 = x - x0;
        float yd0 = y - y0;
        float zd0 = z - z0;
        float xd1 = xd0 - 1;
        float yd1 = yd0 - 1;
        float zd1 = zd0 - 1;

        float xs = InterpQuintic(xd0);
        float ys = InterpQuintic(yd0);
        float zs = InterpQuintic(zd0);

        float xf00 = std::lerp(GradCoord3D(seed, x0, y0, z0, xd0, yd0, zd0), GradCoord3D(seed, x0 + 1, y0, z0, xd1, yd0, zd0), xs);
        float xf10 = std::lerp(GradCoord3D(seed, x0, y0 + 1, z0, xd0, yd1, zd0), GradCoord3D(seed, x0 + 1, y0 + 1, z0, xd1, yd1, zd0), xs);
        float xf01 = std::lerp(GradCoord3D(seed, x0, y0, z0 + 1, xd0, yd0, zd1), GradCoord3D(seed, x0 + 1, y0, z0 + 1, xd1, yd0, zd1), xs);
        float xf11 = std::lerp(GradCoord3D(seed, x0, y0 + 1, z0 + 1, xd0, yd1, zd1), GradCoord3D(seed, x0 + 1, y0 + 1, z0 + 1, xd1, yd1, zd1), xs);

        float yf0 = std::lerp(xf00, xf10, ys);
        float yf1 = std::lerp(xf01, xf11, ys);

        return std::lerp(yf0, yf1, zs);
    }

    int mSeed = 1337;
    float mFrequency = 0.01f;
    NoiseType mNoiseType = NoiseType_OpenSimplex2;
    RotationType3D mRotationType3D = RotationType3D_None;
    FractalType mFractalType = FractalType_None;
    int mOctaves = 3;
    float mLacunarity = 2.0f;
    float mGain = 0.5f;
    float mWeightedStrength = 0.0f;
    float mPingPongStrength = 2.0f;
};
