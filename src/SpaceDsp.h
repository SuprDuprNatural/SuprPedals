#pragma once
#include "TimeSpaceDsp.h"
#include <array>
#include <cstdint>
namespace supr {
// Mono eight-delay feedback delay network (FDN). Householder scattering is
// orthogonal; damping is passive and every loop gain is strictly below one.
// Two Schroeder allpasses diffuse the send. Decay changes gain, never lengths.
class SpaceDsp {
public:
    struct Params { float mix=.18f,decay=1.4f,tone=4500,duck=.5f,predelay=15,lowcut=180,recovery=350,send=1; };
private:
    struct Line {
        std::vector<double> data; size_t head=0;
        void init(size_t n) { data.assign(n,0);head=0; }
        void reset() { std::fill(data.begin(),data.end(),0);head=0; }
        double read() const { return data[head]; }
        void write(double x) { data[head]=timespace::flush(x);if(++head==data.size())head=0; }
        double allpass(double x) { double d=read(),y=d-.5*x;write(x+.5*y);return y; }
    };
    Params p;
    double sr=48000,smoothing=0,attack=0,gain=1;
    std::array<Line,8> lines;
    std::array<Line,2> diffusion;
    std::array<timespace::Lowpass,8> damping;
    std::array<timespace::Smooth,8> loopGain;
    timespace::TapDelay pre;
    timespace::Highpass hp1,hp2;
    timespace::Duck detector;
    timespace::Smooth mix,duck,send,lpPole,hpPole,releasePole;
    static bool prime(size_t n) { for(size_t i=2;i*i<=n;++i)if(n%i==0)return false;return true; }
public:
    void init(double sampleRate) {
        sr=timespace::rate(sampleRate);smoothing=std::exp(-1/(.02*sr));attack=std::exp(-1/(.002*sr));
        const double ms[]={29.7,37.1,41.1,43.7,53.1,61.7,71.3,79.7};
        for(size_t i=0;i<8;++i) {size_t n=size_t(sr*ms[i]*.001);while(!prime(n))++n;lines[i].init(n);}
        diffusion[0].init(size_t(sr*.0047));diffusion[1].init(size_t(sr*.0127));
        pre.init(sr,.151);reset();
    }
    void setParams(Params v) {
        using timespace::finite;
        p.mix=finite(v.mix,.18,0,1);p.decay=finite(v.decay,1.4,.2,8);p.tone=finite(v.tone,4500,800,12000);
        p.duck=finite(v.duck,.5,0,1);p.predelay=finite(v.predelay,15,0,150);p.lowcut=finite(v.lowcut,180,40,600);
        p.recovery=finite(v.recovery,350,50,1500);p.send=finite(v.send,1,0,1);
        // One base sample is intentional even with the predelay control at zero.
        pre.setTime(1+sr*.001*p.predelay);
    }
    void reset() {
        for(auto& l:lines) l.reset();
        for(auto& l:diffusion) l.reset();
        damping={};hp1={};hp2={};detector={};gain=1;
        pre.reset(1+sr*.001*p.predelay);mix.value=p.mix;duck.value=p.duck;send.value=p.send;
        lpPole.value=timespace::pole(p.tone,sr);hpPole.value=timespace::pole(p.lowcut,sr);
        releasePole.value=std::exp(-1/(p.recovery*.001*sr));
        for(size_t j=0;j<8;++j)loopGain[j].value=std::pow(10.,-3.*lines[j].data.size()/(sr*p.decay));
    }
    void process(const float* in,float* out,uint32_t n) {
        using namespace timespace;
        constexpr double norm=.3535533905932737622; // 1/sqrt(8)
        std::array<double,8> targets;
        for(size_t j=0;j<8;++j)targets[j]=std::pow(10.,-3.*lines[j].data.size()/(sr*p.decay));
        const double lpTarget=pole(p.tone,sr),hpTarget=pole(p.lowcut,sr),relTarget=std::exp(-1/(p.recovery*.001*sr));
        for(uint32_t i=0;i<n;++i) {
            const float dry=in[i];const double x=audio(dry);
            const double a=lpPole.tick(lpTarget,smoothing),b=hpPole.tick(hpTarget,smoothing);
            const double filtered=hp2.tick(hp1.tick(x,b),b)*send.tick(p.send,smoothing);
            double excitation=pre.read();pre.write(filtered);
            for(auto& l:diffusion)excitation=l.allpass(excitation);
            std::array<double,8> v;double sum=0,wet=0;
            for(size_t j=0;j<8;++j) {v[j]=damping[j].tick(lines[j].read(),a);sum+=v[j];wet+=(j%2? -1:1)*v[j]*norm;}
            for(size_t j=0;j<8;++j) {
                // H=I-2uu': u=(1,...,1)/sqrt(8). Normalized alternating injection.
                double state=(v[j]-.25*sum)*loopGain[j].tick(targets[j],smoothing)+(j%2?-1:1)*excitation*norm;
                lines[j].write(std::clamp(state,-16.0,16.0));
            }
            gain=detector.tick(x,duck.tick(p.duck,smoothing),attack,releasePole.tick(relTarget,smoothing));
            const double m=mix.tick(p.mix,smoothing);
            out[i]=p.mix==0?(std::isfinite(dry)?dry:0.f):float((std::isfinite(dry)?double(dry):0.0)+m*wet*gain);
        }
    }
    double duckGain() const {return gain;}
    size_t memoryBytes() const {size_t b=pre.bytes();for(auto& l:lines)b+=l.data.size()*8;for(auto& l:diffusion)b+=l.data.size()*8;return b;}
};
}
