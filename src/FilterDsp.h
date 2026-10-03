// Shared filter and oversampling primitives.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

namespace supr {

// Topology-preserving state-variable filter (Simper's trapezoidal SVF).
//
// Chosen over a direct-form biquad for two reasons that both bite in this
// plugin: it stays well conditioned when fc/fs is tiny (a 90 Hz shelf is
// 0.0009 of the sample rate here, and 0.0005 at 2x), and its state is a pair
// of integrators, so coefficients can be re-derived every block as knobs move
// without the state discontinuities that make direct forms click.
struct Svf {
    double g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
    double m0 = 1, m1 = 0, m2 = 0;
    double ic1 = 0, ic2 = 0;

    void reset() { ic1 = ic2 = 0; }

    // Prewarped corner, clamped below Nyquist so a section specified near or
    // above fs/2 (the top shelf at 17.6 kHz, at a 44.1 kHz session) stays
    // finite and stable instead of running tan() off to infinity.
    static double prewarp(double fs, double fc)
    {
        return std::tan(M_PI * std::min(fc, 0.45 * fs) / fs);
    }

    void setCoef(double gg, double kk)
    {
        g  = gg;
        k  = kk;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void setLowpass(double fs, double fc, double q)
    {
        setCoef(prewarp(fs, fc), 1.0 / q);
        m0 = 0;
        m1 = 0;
        m2 = 1;
    }

    void setHighpass(double fs, double fc, double q)
    {
        setCoef(prewarp(fs, fc), 1.0 / q);
        m0 = 1;
        m1 = -k;
        m2 = -1;
    }

    // Peaking / bell. `dB` is the gain at fc.
    void setBell(double fs, double fc, double q, double dB)
    {
        const double A = std::pow(10.0, dB / 40.0);
        setCoef(prewarp(fs, fc), 1.0 / (q * A));
        m0 = 1;
        m1 = k * (A * A - 1.0);
        m2 = 0;
    }

    // Low shelf. `dB` is the gain at DC; unity at Nyquist.
    void setLowShelf(double fs, double fc, double q, double dB)
    {
        const double A = std::pow(10.0, dB / 40.0);
        setCoef(prewarp(fs, fc) / std::sqrt(A), 1.0 / q);
        m0 = 1;
        m1 = k * (A - 1.0);
        m2 = A * A - 1.0;
    }

    // Allpass: flat magnitude, phase rotating through 360 degrees across fc.
    // An LR4 crossover's two halves sum to exactly this section at the same
    // fc and Q, which is what a three-band split's low band needs in order to
    // stay phase-matched to the pair above it. See BandDsp.h.
    void setAllpass(double fs, double fc, double q)
    {
        setCoef(prewarp(fs, fc), 1.0 / q);
        m0 = 1;
        m1 = -2.0 * k;
        m2 = 0;
    }

    // High shelf. `dB` is the gain at Nyquist; unity at DC.
    void setHighShelf(double fs, double fc, double q, double dB)
    {
        const double A = std::pow(10.0, dB / 40.0);
        setCoef(prewarp(fs, fc) * std::sqrt(A), 1.0 / q);
        m0 = A * A;
        m1 = k * (1.0 - A) * A;
        m2 = 1.0 - A * A;
    }

    double tick(double x)
    {
        const double v0 = x;
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1             = 2.0 * v1 - ic1;
        ic2             = 2.0 * v2 - ic2;
        return m0 * v0 + m1 * v1 + m2 * v2;
    }

    float process(float x) { return float(tick(x)); }
};

// ---------------------------------------------------------------------------
// Halfband2x — 2x oversampling with a 31-tap windowed-sinc halfband.
//
// Half the taps of a halfband filter are zero and the centre tap is exactly
// 0.5, so one polyphase branch collapses to a plain delay: interpolation
// costs 16 multiplies per input sample and decimation the same. The round
// trip is exactly 15 samples at the base rate — an integer, which is what
// lets the clean path be delay-matched with a plain buffer.
// ---------------------------------------------------------------------------
struct Halfband2x {
    static constexpr int kTaps  = 31;
    static constexpr int kHalf  = 16; // nonzero taps (the even indices)
    static constexpr int kDelay = 15; // filter delay at the 2x rate
    static constexpr int kMask  = 31; // ring size 32

    void init()
    {
        // Ideal halfband impulse response, Blackman-windowed.
        double h[kTaps];
        double sum = 0;
        for (int n = 0; n < kTaps; ++n) {
            const int    m  = n - kTaps / 2;
            const double id = (m == 0) ? 0.5
                              : (m % 2 == 0)
                                  ? 0.0
                                  : std::sin(M_PI * m * 0.5) / (M_PI * m);
            const double t  = double(n) / double(kTaps - 1);
            const double w  = 0.42 - 0.5 * std::cos(2.0 * M_PI * t)
                             + 0.08 * std::cos(4.0 * M_PI * t);
            h[n] = id * w;
            if (m != 0)
                sum += h[n];
        }
        // Renormalise the windowed taps so the branches still sum to 1 and
        // the centre tap stays exactly 0.5 — both halfband properties have to
        // hold or the "pure delay" branch stops being a pure delay.
        const double s = (sum != 0.0) ? 0.5 / sum : 1.0;
        for (int i = 0; i < kHalf; ++i)
            c[i] = float(h[2 * i] * s);
        reset();
    }

    void reset()
    {
        std::memset(upHist, 0, sizeof(upHist));
        std::memset(dnHist, 0, sizeof(dnHist));
        std::memset(dnOdd, 0, sizeof(dnOdd));
        upPos = dnPos = dnOddPos = 0;
    }

    // One base-rate sample in, two 2x-rate samples out.
    void up(float x, float* y)
    {
        upHist[upPos] = x;
        float acc     = 0;
        for (int i = 0; i < kHalf; ++i)
            acc += c[i] * upHist[(upPos - i) & kMask];
        upPos = (upPos + 1) & kMask;

        y[0] = 2.0f * acc;                        // filtered branch
        y[1] = upHist[(upPos - 1 - 7) & kMask];   // pure-delay branch
    }

    // Two 2x-rate samples in, one base-rate sample out.
    float down(const float* x)
    {
        dnHist[dnPos] = x[0];
        float acc     = 0;
        for (int i = 0; i < kHalf; ++i)
            acc += c[i] * dnHist[(dnPos - i) & kMask];
        dnPos = (dnPos + 1) & kMask;

        dnOdd[dnOddPos] = x[1];
        const float odd = dnOdd[(dnOddPos - 8) & kMask];
        dnOddPos        = (dnOddPos + 1) & kMask;

        return acc + 0.5f * odd;
    }

    float c[kHalf] = {0};
    float upHist[32] = {0}, dnHist[32] = {0}, dnOdd[32] = {0};
    int upPos = 0, dnPos = 0, dnOddPos = 0;
};

} // namespace supr
