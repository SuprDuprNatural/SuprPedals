// SuprShape: explicit rumble control, neutral EQ and input-keyed dynamic cuts.
#pragma once
#include "SansDsp.h"
#include <array>
#include <algorithm>
#include <cmath>
namespace supr {
class ShapeDsp {
public:
    struct Params {
        float hpf=0, lpf=20000, bass=0, treble=0, mid_freq=700, mid_gain=0,
              mid_q=1, level=0, boom=0, boom_freq=120, harsh=0, harsh_freq=2500,
              threshold=-30, release=250;
    };
private:
    // Smooth physically meaningful SVF parameters, never interpolate direct-form poles.
    enum {HP,LP,BASS,TREBLE,MID,Q,GAIN,BOOM,HARSH,ONHP,ONLP,COUNT};
    std::array<double,COUNT> target{}, value{};
    Svf hp,lp,bass,treble,mid,detector[2],cut[2];
    Params p;
    double sr=48000, smooth=0, attack=0, recovery=0, env[2]={}, reduction[2]={};
    double amount[2]={}, threshold=0, cutA[2]={1,1}, cutTarget[2]={1,1};
    unsigned clock=0;
    static double valid(double x,double fallback,double lo,double hi) {
        return std::isfinite(x)?std::clamp(x,lo,hi):fallback;
    }
    static double flush(double x) { return std::abs(x)<1e-25?0:x; }
    static void tidy(Svf& f) {f.ic1=flush(f.ic1);f.ic2=flush(f.ic2);}
    static double warp(double f,double rate) {return Svf::prewarp(rate,f);}
    static void bell(Svf& f,double g,double q,double a) {
        f.setCoef(g,1/(q*a));f.m0=1;f.m1=f.k*(a*a-1);f.m2=0;
    }
public:
    void init(double rate) {
        sr=valid(rate,48000,8000,192000);smooth=std::exp(-1/(.02*sr));
        attack=std::exp(-1/(.005*sr));setParams(Params{});reset();
    }
    void setParams(const Params& in) {
        p=in;
        p.hpf=valid(p.hpf,0,0,100);p.lpf=valid(p.lpf,20000,1000,20000);
        p.bass=valid(p.bass,0,-12,12);p.treble=valid(p.treble,0,-12,12);
        p.mid_freq=valid(p.mid_freq,700,150,4000);p.mid_gain=valid(p.mid_gain,0,-12,12);
        p.mid_q=valid(p.mid_q,1,.3,4);p.level=valid(p.level,0,-18,6);
        p.boom=valid(p.boom,0,0,12);p.harsh=valid(p.harsh,0,0,12);
        p.boom_freq=valid(p.boom_freq,120,50,350);p.harsh_freq=valid(p.harsh_freq,2500,800,6000);
        p.threshold=valid(p.threshold,-30,-60,0);p.release=valid(p.release,250,50,1000);
        target={warp(std::max(10.f,p.hpf),sr),warp(p.lpf,sr),std::pow(10,p.bass/40.),
            std::pow(10,p.treble/40.),warp(p.mid_freq,sr),p.mid_q,std::pow(10,p.level/20.),
            warp(p.boom_freq,sr),warp(p.harsh_freq,sr),p.hpf>0?1.:0.,p.lpf<20000?1.:0.};
        // Amount and detector threshold are independent of the static EQ and output trim.
        amount[0]=p.boom;amount[1]=p.harsh;threshold=std::pow(10,p.threshold/20.);
        recovery=std::exp(-1/(.001*p.release*sr));
    }
    void reset() {
        value=target;midGain=std::pow(10,p.mid_gain/40.);hp.reset();lp.reset();bass.reset();treble.reset();mid.reset();
        for(int j=0;j<2;++j){detector[j].reset();cut[j].reset();env[j]=reduction[j]=0;cutA[j]=cutTarget[j]=1;}
        clock=0;
    }
    double reductionDb(unsigned band) const {return band<2?40*std::log10(cutA[band]):0;}
    void process(const float* in,float* out,unsigned n) {
        const double gb=warp(90,sr),gt=warp(3500,sr);
        const double midA=std::pow(10,p.mid_gain/40.);
        for(unsigned i=0;i<n;++i){
            // Smooth the mid gain separately, including reset's exact neutral endpoint.
            for(unsigned j=0;j<COUNT;++j){value[j]=target[j]+smooth*(value[j]-target[j]);
                if(std::abs(value[j]-target[j])<1e-12)value[j]=target[j];}
            double x=std::isfinite(in[i])?in[i]:0;
            for(int j=0;j<2;++j){
                auto& d=detector[j];d.setCoef(value[j?HARSH:BOOM],1./3);d.m0=0;d.m1=d.k;d.m2=0;
                double a=std::abs(d.process(float(x)));
                env[j]=flush(a+(a>env[j]?attack:recovery)*(env[j]-a));
                if(clock==0){
                    reduction[j]=amount[j]*std::clamp(20*std::log10(std::max(1e-20,env[j])/threshold)/12.,0.,1.);
                    cutTarget[j]=std::pow(10,-reduction[j]/40.);
                }
                cutA[j]=cutTarget[j]+smooth*(cutA[j]-cutTarget[j]);
                if(std::abs(cutA[j]-cutTarget[j])<1e-12)cutA[j]=cutTarget[j];
                bell(cut[j],value[j?HARSH:BOOM],3,cutA[j]);tidy(d);
            }
            clock=(clock+1)%16;
            hp.setCoef(value[HP],std::sqrt(2.));hp.m0=1;hp.m1=-hp.k;hp.m2=-1;
            lp.setCoef(value[LP],std::sqrt(2.));lp.m0=lp.m1=0;lp.m2=1;
            double y=x+value[ONHP]*(hp.process(float(x))-x);
            y+=value[ONLP]*(lp.process(float(y))-y);
            double a=value[BASS];bass.setCoef(gb/std::sqrt(a),std::sqrt(2.));
            bass.m0=1;bass.m1=bass.k*(a-1);bass.m2=a*a-1;
            a=value[TREBLE];treble.setCoef(gt*std::sqrt(a),std::sqrt(2.));
            treble.m0=a*a;treble.m1=treble.k*(1-a)*a;treble.m2=1-a*a;
            // Integrator topology permits smooth live gain changes without state resets.
            midGain=midA+smooth*(midGain-midA);
            if(std::abs(midGain-midA)<1e-12)midGain=midA;
            bell(mid,value[MID],value[Q],midGain);
            y=bass.process(float(y));y=mid.process(float(y));y=treble.process(float(y));
            y=cut[0].process(float(y));y=cut[1].process(float(y));
            const bool neutral=value[ONHP]==0 && value[ONLP]==0 && value[BASS]==1 &&
                value[TREBLE]==1 && midGain==1 && cutA[0]==1 && cutA[1]==1 && value[GAIN]==1;
            out[i]=neutral?float(x):float(y*value[GAIN]);
            tidy(hp);tidy(lp);tidy(bass);tidy(mid);tidy(treble);tidy(cut[0]);tidy(cut[1]);
        }
    }
private:
    double midGain=1;
};
}
