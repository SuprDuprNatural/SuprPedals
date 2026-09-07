#pragma once
#include "TimeSpaceDsp.h"
#include <cstdint>
namespace supr {
class EchoDsp {
public:
    struct Params {
        float time=375, feedback=.4f, mix=.2f, duck=.5f, tone=3500;
        float lowcut=150, recovery=300, division=0, send=1, hold=0;
    };
private:
    Params p;
    double sr=48000, smoothing=0, attack=0;
    timespace::TapDelay delay;
    timespace::Highpass hp1,hp2,dc;
    timespace::Lowpass colour;
    timespace::Duck detector;
    timespace::Smooth fb,mix,duck,send,hold,lpPole,hpPole,releasePole;
    uint64_t holdSamples=0;
    double gain=1;
    double samples() const {
        const double divisions[]={1,.5,.75};
        return sr*.001*p.time*divisions[int(p.division)];
    }
public:
    void init(double sampleRate) {
        sr=timespace::rate(sampleRate); smoothing=std::exp(-1/(.01*sr));
        attack=std::exp(-1/(.002*sr)); delay.init(sr,2.0); reset();
    }
    void setParams(Params v) {
        using timespace::finite;
        p.time=finite(v.time,375,20,2000); p.feedback=finite(v.feedback,.4,0,.92);
        p.mix=finite(v.mix,.2,0,1); p.duck=finite(v.duck,.5,0,1);
        p.tone=finite(v.tone,3500,800,12000); p.lowcut=finite(v.lowcut,150,40,600);
        p.recovery=finite(v.recovery,300,50,1500);
        p.division=std::round(finite(v.division,0,0,2));
        p.send=finite(v.send,1,0,1); p.hold=finite(v.hold,0,0,1);
        delay.setTime(samples());
    }
    void reset() {
        delay.reset(samples()); hp1={}; hp2={}; dc={}; colour={}; detector={};
        fb.value=p.feedback; mix.value=p.mix; duck.value=p.duck; send.value=p.send;
        hold.value=0; holdSamples=0; gain=1;
        lpPole.value=timespace::pole(p.tone,sr); hpPole.value=timespace::pole(p.lowcut,sr);
        releasePole.value=std::exp(-1/(p.recovery*.001*sr));
    }
    void process(const float* in,float* out,uint32_t n) {
        using namespace timespace;
        const double lpTarget=pole(p.tone,sr), hpTarget=pole(p.lowcut,sr);
        const double dcPole=pole(8,sr);
        const double releaseTarget=std::exp(-1/(p.recovery*.001*sr));
        for(uint32_t i=0;i<n;++i) {
            const float dry=in[i]; const double x=audio(dry);
            if(p.hold>=.5f) { if(holdSamples<uint64_t(sr*10)+1) ++holdSamples; } else holdSamples=0;
            // A held switch times out after ten seconds, and must be released to rearm.
            const double h=hold.tick(p.hold>=.5f && holdSamples<=uint64_t(sr*10)?1:0,smoothing);
            const double s=send.tick(p.send,smoothing)*(1-h);
            const double feedback=fb.tick(p.feedback,smoothing)*(1-h)+.995*h;
            const double a=lpPole.tick(lpTarget,smoothing), b=hpPole.tick(hpTarget,smoothing);
            const double wet=delay.read();
            const double filtered=hp2.tick(hp1.tick(x,b),b);
            // Return ducking is deliberately absent from the feedback path.
            const double repeat=dc.tick(colour.tick(wet,a),dcPole);
            delay.write(std::clamp(filtered*s+feedback*repeat,-8.0,8.0));
            gain=detector.tick(x,duck.tick(p.duck,smoothing),attack,releasePole.tick(releaseTarget,smoothing));
            const double m=mix.tick(p.mix,smoothing);
            out[i]=p.mix==0 ? (std::isfinite(dry)?dry:0.f) : float((std::isfinite(dry)?double(dry):0.0)+m*wet*gain);
        }
    }
    double duckGain() const { return gain; }
    size_t memoryBytes() const { return delay.bytes(); }
};
}
