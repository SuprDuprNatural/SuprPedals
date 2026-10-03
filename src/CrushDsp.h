#pragma once
#include "TimeSpaceDsp.h"
#include "CleanLowsDsp.h"
#include <array>
#include <limits>

namespace supr {
// Deliberate aliasing, no oversampling: a fractional sample/hold clock feeds a
// symmetric mid-tread quantizer. Zero maps to zero (no dither/no idle noise).
class CrushDsp {
public:
    struct Params {
        float bits=8, rate=8000, drive=6, env=0, sensitivity=0, release=180;
        float tone=6000, protect=1, dry=0.0f, wet=0.0f;
    };
private:
    Params target;
    std::array<double,10> current{};
    double sr=48000, smoothing=0, attack=0;
    double envelope=0, phase=0, previous=0, held=0;
    double clock=8000, gain=1, quant=128, tonePole=0, releasePole=0;
    unsigned tick=0;
    bool first=true;
    timespace::Lowpass toneFilter;
    CleanLowsDsp cleanLows;
    std::array<double,10> values() const {
        return {target.bits,target.rate,target.drive,target.env,target.sensitivity,
                target.release,target.tone,target.protect,timespace::returnGain(target.dry),timespace::returnGain(target.wet)};
    }
    void coefficients() {
        gain=std::exp2(current[2]/6.020599913279624);
        quant=std::exp2(current[0]-1);
        tonePole=timespace::pole(current[6],sr);
        releasePole=std::exp(-1/(current[5]*.001*sr));
        const double e=envelope*std::exp2(current[4]/6.020599913279624);
        // +Env opens the clock on the attack; -Env crushes harder on attack.
        clock=std::clamp(current[1]*std::exp2(current[3]*e/(e+.1)),200.,sr);
    }
public:
    void init(double sampleRate) {
        sr=timespace::rate(sampleRate);
        smoothing=std::exp(-1/(.020*sr)); attack=std::exp(-1/(.008*sr));
        cleanLows.init(sr);
        reset();
    }
    void setParams(Params p) {
        using timespace::finite;
        target.bits=finite(p.bits,8,2,16); target.rate=finite(p.rate,8000,200,48000);
        target.drive=finite(p.drive,6,0,24); target.env=finite(p.env,0,-4,4);
        target.sensitivity=finite(p.sensitivity,0,-24,24); target.release=finite(p.release,180,30,1000);
        target.tone=finite(p.tone,6000,400,18000); target.protect=finite(p.protect,1,0,1)>=.5?1:0;
        target.dry=finite(p.dry,0.0f,-60,24); target.wet=finite(p.wet,0.0f,-60,24);
    }
    void reset() {
        current=values(); envelope=phase=previous=held=0; tick=0; first=true;
        toneFilter={}; cleanLows.reset(); coefficients();
    }
    void process(const float* in,float* out,unsigned n) {
        const auto goal=values();
        for(unsigned i=0;i<n;++i) {
            const float dry=std::isfinite(in[i])?in[i]:0.f;
            const double x=dry, excitation=timespace::audio(x);
            for(unsigned j=0;j<current.size();++j) {
                current[j]=goal[j]+smoothing*(current[j]-goal[j]);
                if(std::abs(current[j]-goal[j])<1e-12)current[j]=goal[j];
            }
            const double amplitude=std::abs(excitation);
            envelope=timespace::flush(amplitude+(amplitude>envelope?attack:releasePole)*(envelope-amplitude));
            // Fixed sample cadence, never tied to host blocks. The only clock
            // steps are intentional sample/hold edges; its phase never resets
            // during a knob or envelope sweep.
            if(tick==0)coefficients();
            tick=(tick+1)&15;
            const double step=clock/sr;
            phase+=step;
            if(first || phase>=1) {
                const bool initial=first;
                if(phase>=1)phase-=1;
                const double sample=initial?excitation:excitation+(previous-excitation)*(phase/step);
                held=std::round(std::clamp(sample*gain,-1.,1.)*quant)/quant/gain;
                first=false;
            }
            previous=excitation;
            const double wet=toneFilter.tick(held,tonePole);
            const double protectedMix=cleanLows.process(x,wet,current[8],current[9]);
            const double fullMix=current[8]*x+current[9]*wet;
            const double result=(1-current[7])*fullMix+current[7]*protectedMix;
            // Exact bypass with protection off, Dry +6.0206 dB and Wet muted.
            // Bound only pathological overflow; normal audio isn't limited.
            out[i]=current[7]==0&&current[8]==1&&current[9]==0?dry:
                float(std::clamp(result,-double(std::numeric_limits<float>::max()),double(std::numeric_limits<float>::max())));
        }
    }
};
}
