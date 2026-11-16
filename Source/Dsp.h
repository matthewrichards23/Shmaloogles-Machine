#pragma once
// Plain C++ DSP building blocks. No JUCE dependency on purpose.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sb
{
constexpr float kPi    = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717958f;

//==============================================================================
// Circular delay line with linear interpolation. read(0) = newest sample.
struct DelayLine
{
    std::vector<float> buf;
    int mask = 0, w = 0;

    void prepare (int maxSamples)
    {
        int n = 16;
        while (n < maxSamples + 4) n <<= 1;
        buf.assign ((size_t) n, 0.0f);
        mask = n - 1;
        w = 0;
    }

    void clear() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }

    void push (float x)
    {
        buf[(size_t) w] = x;
        w = (w + 1) & mask;
    }

    float read (float d) const
    {
        d = std::max (0.0f, std::min (d, (float) (mask - 2)));
        const float pos = (float) (w - 1) - d;
        const int   i0  = (int) std::floor (pos);
        const float fr  = pos - (float) i0;
        const float a   = buf[(size_t) (i0 & mask)];
        const float b   = buf[(size_t) ((i0 + 1) & mask)];
        return a + (b - a) * fr;
    }
};

//==============================================================================
// Zavalishin TPT state-variable filter (stable under fast modulation).
struct SVF
{
    float ic1 = 0, ic2 = 0, g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;

    void set (float fc, float q, float sr)
    {
        fc = std::max (10.0f, std::min (fc, sr * 0.45f));
        g  = std::tan (kPi * fc / sr);
        k  = 1.0f / std::max (0.1f, q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void reset() { ic1 = ic2 = 0; }

    inline void run (float v0, float& low, float& band, float& high)
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        low  = v2;
        band = v1;
        high = v0 - k * v1 - v2;
    }
};

//==============================================================================
struct Noise
{
    uint32_t s = 22222u;
    inline float next()
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float) s * (2.0f / 4294967295.0f) - 1.0f;
    }
};

//==============================================================================
// 8-line feedback delay network reverb (Householder mixing, damped, modulated).
struct FDNReverb
{
    static constexpr int N = 8;
    DelayLine line[N], pre;
    float len[N] {}, gain[N] {}, lp[N] {}, ph[N] {}, inc[N] {};
    float modDepth = 0, damp = 0.42f, preSamples = 0;
    double sr = 44100.0;

    void prepare (double rate)
    {
        sr = rate;
        const float ms[N] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f };
        const float lf[N] = { 0.13f, 0.17f, 0.23f, 0.29f, 0.31f, 0.37f, 0.19f, 0.27f };
        modDepth = (float) (0.0003 * sr);
        for (int i = 0; i < N; ++i)
        {
            len[i] = ms[i] * 0.001f * (float) sr;
            line[i].prepare ((int) (len[i] + modDepth) + 8);
            ph[i]  = (float) i / (float) N;
            inc[i] = lf[i] / (float) sr;
        }
        pre.prepare ((int) (0.1 * sr));
        preSamples = (float) (0.02 * sr);
        reset();
        setDecay (2.0f);
    }

    void reset()
    {
        for (int i = 0; i < N; ++i) { line[i].clear(); lp[i] = 0; }
        pre.clear();
    }

    void setDecay (float rt60)
    {
        rt60 = std::max (0.1f, rt60);
        for (int i = 0; i < N; ++i)
            gain[i] = std::pow (10.0f, -3.0f * len[i] / (rt60 * (float) sr));
    }

    void process (float inL, float inR, float& outL, float& outR)
    {
        static const float sgn[N] = { 1, -1, 1, -1, -1, 1, -1, 1 };
        pre.push (0.5f * (inL + inR));
        const float x = pre.read (preSamples);

        float y[N];
        float sum = 0;
        for (int i = 0; i < N; ++i)
        {
            ph[i] += inc[i];
            if (ph[i] >= 1.0f) ph[i] -= 1.0f;
            const float m = modDepth * std::sin (kTwoPi * ph[i]);
            const float v = line[i].read (len[i] + m);
            lp[i] += damp * (v - lp[i]);
            y[i] = lp[i];
            sum += y[i];
        }

        const float h = sum * (2.0f / (float) N);
        for (int i = 0; i < N; ++i)
            line[i].push ((y[i] - h) * gain[i] + x * 0.35f * sgn[i]);

        outL = 0.4f * (y[0] - y[2] + y[4] - y[6]);
        outR = 0.4f * (y[1] - y[3] + y[5] - y[7]);
    }
};

//==============================================================================
// Two-head crossfading delay-line pitch shifter (good enough for risers).
struct PitchShifter
{
    DelayLine dl[2];
    float p = 0, inc = 0;
    int W = 1;

    void prepare (double sr)
    {
        W = std::max (64, (int) (0.06 * sr));
        dl[0].prepare (W + 8);
        dl[1].prepare (W + 8);
        p = 0; inc = 0;
    }

    void reset() { dl[0].clear(); dl[1].clear(); p = 0; }

    void setRatio (float r) { inc = (1.0f - r) / (float) W; }

    void process (float l, float r, float& oL, float& oR)
    {
        dl[0].push (l);
        dl[1].push (r);

        float p2 = p + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;

        const float w1 = 0.5f - 0.5f * std::cos (kTwoPi * p);
        const float w2 = 1.0f - w1;
        const float d1 = p * (float) W;
        const float d2 = p2 * (float) W;

        oL = dl[0].read (d1) * w1 + dl[0].read (d2) * w2;
        oR = dl[1].read (d1) * w1 + dl[1].read (d2) * w2;

        p += inc;
        if (p >= 1.0f) p -= 1.0f;
        if (p < 0.0f)  p += 1.0f;
    }
};
} // namespace sb
