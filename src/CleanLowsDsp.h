// Shared 250 Hz LR4 crossover for Phase, Crush and Vowel.
#pragma once
#include "SansDsp.h"

namespace supr {
class CleanLowsDsp {
    Svf low[2], dryHigh[2], wetHigh[2];

    static double cascade(Svf (&filters)[2], double x) {
        for(auto& f:filters) {
            x=f.tick(x);
            if(std::abs(f.ic1)<1e-30) f.ic1=0;
            if(std::abs(f.ic2)<1e-30) f.ic2=0;
        }
        return x;
    }
public:
    void init(double sr) {
        constexpr double q=.7071067811865475244;
        for(auto& f:low) f.setLowpass(sr,250,q);
        for(auto& f:dryHigh) f.setHighpass(sr,250,q);
        for(auto& f:wetHigh) f.setHighpass(sr,250,q);
        reset();
    }
    void reset() {
        for(auto& f:low) f.reset();
        for(auto& f:dryHigh) f.reset();
        for(auto& f:wetHigh) f.reset();
    }
    double process(double x,double wet,double dryGain,double wetGain) {
        // Filter the ungained sources, then mix: gain automation cannot pump
        // the low reference or leave gain-dependent crossover history.
        // L4 and H4 share phase; L4+H4 is a unity-magnitude AP2, not 1-H4.
        // No full-band dry signal is summed against a phase-rotated low band.
        const double l=cascade(low,x), h=cascade(dryHigh,x);
        const double w=cascade(wetHigh,wet);
        return l+dryGain*h+wetGain*w;
    }
};
}
