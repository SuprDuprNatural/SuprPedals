#include "PhaseDsp.h"
#include <lv2/core/lv2.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

static float percentToDb(double percent) { return percent>0?float(20*std::log10(percent/50)):-60.f; }
using Dsp=supr::PhaseDsp;
constexpr double pi=3.14159265358979323846;
int failures=0;
void check(bool ok,const char* message) { if(!ok){std::printf("FAIL: %s\n",message);++failures;} }
std::vector<float> signal(unsigned n,double sr) {
    std::vector<float> x(n); unsigned rng=12345;
    for(unsigned i=0;i<n;++i){rng=1664525*rng+1013904223;x[i]=float(.15*std::sin(2*pi*30.8677*i/sr)+.1*std::sin(2*pi*739*i/sr)+.03*(double(rng)/4294967296.-.5));}
    return x;
}
std::complex<double> response(double sr,double hz,Dsp::Params p) {
    Dsp d;d.init(sr);d.setParams(p);d.reset();
    const unsigned n=unsigned(sr*2);std::vector<float> x(n),y(n);
    for(unsigned i=0;i<n;++i)x[i]=float(.1*std::sin(2*pi*hz*i/sr));
    d.process(x.data(),y.data(),n);
    // Least-squares quadrature, avoiding leakage from noninteger periods.
    double ss=0,cc=0,sc=0,ys=0,yc=0;
    for(unsigned i=n/2;i<n;++i){const double s=std::sin(2*pi*hz*i/sr),c=std::cos(2*pi*hz*i/sr);ss+=s*s;cc+=c*c;sc+=s*c;ys+=y[i]*s;yc+=y[i]*c;}
    return {10*(ys*cc-yc*sc)/(ss*cc-sc*sc),10*(yc*ss-ys*sc)/(ss*cc-sc*sc)};
}
std::complex<double> theory(double sr,double hz,Dsp::Params p) {
    const auto z=std::polar(1.,-2*pi*hz/sr);
    const double g=std::tan(pi*p.centre/sr),a=(g-1)/(g+1);
    const auto ap=std::pow((a+z)/(1.+a*z),4);
    const auto wet=(1-std::abs(double(p.feedback)))*ap/(1.-double(p.feedback)*z*ap);
    const std::complex<double> s(0,std::tan(pi*hz/sr)/std::tan(pi*250/sr));
    const auto denominator=std::pow(s*s+std::sqrt(2.)*s+1.,2);
    const auto l=1./denominator, h=std::pow(s,4)/denominator;
    const auto mix=supr::timespace::returnGain(p.dry)+supr::timespace::returnGain(p.wet)*wet;
    return p.protect?l+h*mix:mix;
}
Dsp::Params automated(unsigned event) {
    Dsp::Params p;
    p.rate=event%2?5:.03f;p.depth=event%3?1:0;p.centre=event%2?2500:100;
    p.feedback=event%2?.75f:-.75f;p.dry=percentToDb(100*(1-(event%3?1:0)));p.wet=percentToDb(100*(event%3?1:0));p.protect=float(event%2);
    p.mode=float((event/2)%2);p.sensitivity=event%2?24:-24;return p;
}
std::vector<float> render(double sr,const std::vector<float>& input,bool wrapper,bool split,bool inplace) {
    std::vector<float> output(input.size());auto x=input;
    const auto* desc=lv2_descriptor(0);LV2_Handle h=nullptr;
    Dsp d;d.init(sr);float controls[9]{};
    auto set=[&](Dsp::Params p){
        d.setParams(p);const float v[]={p.rate,p.depth,p.centre,p.feedback,p.dry,p.protect,p.mode,p.sensitivity,p.wet};
        std::copy(v,v+9,controls);
    };
    set(automated(0));d.reset();
    if(wrapper){const LV2_Feature* features[]={nullptr};h=desc->instantiate(desc,sr,"",features);check(h!=nullptr,"LV2 instantiate");for(unsigned i=0;i<9;++i)desc->connect_port(h,i+2,controls+i);desc->activate(h);desc->run(h,0);}
    unsigned pos=0,event=0;
    while(pos<input.size()){
        set(automated(event));const unsigned end=std::min(unsigned(input.size()),pos+997);
        while(pos<end){const unsigned n=std::min(end-pos,split?1+(pos*37)%131:end-pos);
            float* out=inplace?x.data()+pos:output.data()+pos;
            if(wrapper){desc->connect_port(h,0,x.data()+pos);desc->connect_port(h,1,out);desc->run(h,n);}else d.process(x.data()+pos,out,n);
            pos+=n;
        }++event;
    }
    if(wrapper)desc->cleanup(h);
    return inplace?x:output;
}
// Fixed controls through both the DSP and real host, with aliasing enabled.
std::vector<float> fixedRender(double sr,Dsp::Params p,std::vector<float> x,bool wrapper) {
    if(wrapper) {
        const auto* desc=lv2_descriptor(0);const LV2_Feature* features[]={nullptr};
        auto h=desc->instantiate(desc,sr,"",features);
        float controls[]={p.rate,p.depth,p.centre,p.feedback,p.dry,p.protect,p.mode,p.sensitivity,p.wet};
        for(unsigned i=0;i<9;++i)desc->connect_port(h,i+2,controls+i);
        desc->activate(h);
        for(unsigned i=0;i<x.size();i+=73) {
            desc->connect_port(h,0,x.data()+i);desc->connect_port(h,1,x.data()+i);
            desc->run(h,std::min(73u,unsigned(x.size())-i));
        }
        desc->cleanup(h);
    } else {
        Dsp d;d.init(sr);d.setParams(p);d.reset();d.process(x.data(),x.data(),unsigned(x.size()));
    }
    return x;
}
void inputAndReinit(double sr) {
    for(bool wrapper:{false,true}) {
        for(float protect:{0.f,1.f}) {
            Dsp::Params p;p.dry=percentToDb(100);p.wet=percentToDb(0);p.protect=protect;
            const float max=std::numeric_limits<float>::max();
            const std::vector<float> dry={32,-64,17,-17,0.f,-0.f,max,-max,.125f};
            const auto y=fixedRender(sr,p,dry,wrapper);
            check(protect ? std::all_of(y.begin(),y.end(),[](float v){return std::isfinite(v);}) :
                std::memcmp(dry.data(),y.data(),dry.size()*sizeof(float))==0,
                "finite clean crossover; protection-off dry is bit-exact incl float limits and signed zero");
        }
        for(float mix:{0.f,.5f,1.f})for(float level:{32.f,-64.f}) {
            Dsp::Params p;p.dry=percentToDb(100*(1-mix));p.wet=percentToDb(100*mix);p.protect=1;p.depth=0;p.feedback=.75f;
            auto y=fixedRender(sr,p,std::vector<float>(unsigned(sr),level),wrapper);
            check(std::all_of(y.begin()+unsigned(sr/2),y.end(),[&](float v){return v==level;}),"protected clean DC remains exact above nominal audio range");
        }
        for(float mix:{0.f,1.f})for(float protect:{0.f,1.f}) {
            Dsp::Params p;p.dry=percentToDb(100*(1-mix));p.wet=percentToDb(100*mix);p.protect=protect;p.mode=1;p.feedback=-.75f;
            auto dirty=signal(unsigned(sr),sr),sanitized=dirty;
            dirty[17]=std::numeric_limits<float>::quiet_NaN();
            dirty[73]=std::numeric_limits<float>::infinity();dirty[151]=-dirty[73];
            sanitized[17]=sanitized[73]=sanitized[151]=0;
            const auto y=fixedRender(sr,p,dirty,wrapper),expected=fixedRender(sr,p,sanitized,wrapper);
            check(y==expected,"NaN/Inf input equivalent to zero without poisoning subsequent audio");
            check(std::all_of(y.begin(),y.end(),[](float v){return std::isfinite(v);}),"nonfinite input gives finite output");
        }
    }
    for(double previousRate:{44100.,48000.,96000.}) {
        Dsp reused;reused.init(previousRate);reused.setParams(automated(1));reused.reset();
        auto history=signal(3001,previousRate);reused.process(history.data(),history.data(),unsigned(history.size()));
        auto p=automated(3);reused.init(sr);reused.setParams(p);reused.reset();
        Dsp fresh;fresh.init(sr);fresh.setParams(p);fresh.reset();
        auto x=signal(5003,sr),y=x;fresh.process(x.data(),x.data(),unsigned(x.size()));
        for(unsigned i=0;i<y.size();i+=97)reused.process(y.data()+i,y.data()+i,std::min(97u,unsigned(y.size())-i));
        check(x==y,"reused sample-rate init/setParams/reset exactly matches fresh instance");
    }
}
void suite(double sr) {
    inputAndReinit(sr);
    std::printf("\n%.0f Hz\n",sr);
    Dsp::Params p;p.depth=0;p.feedback=0;p.protect=0;
    for(double centre:{100.,700.,2500.}){
        p.centre=float(centre);
        for(double angle:{pi/8,3*pi/8}){
            const double f=sr/pi*std::atan(std::tan(pi*centre/sr)*std::tan(angle));
            const double db=20*std::log10(std::max(1e-12,std::abs(response(sr,f,p))));
            std::printf("  centre %.0f notch %.3f Hz: %.1f dB\n",centre,f,db);
            check(db < -90,"analytical static notch frequency/depth");
        }
    }
    double maxError=0,lowMin=100,lowMax=-100;
    for(float protect:{0.f,1.f})for(float feedback:{-.75f,0.f,.75f})for(float centre:{100.f,700.f,2500.f}){
        p.protect=protect;p.feedback=feedback;p.centre=centre;
        for(double f:{30.8677,100.,290.,700.,1700.,6000.}){
            const auto measured=response(sr,f,p);maxError=std::max(maxError,std::abs(measured-theory(sr,f,p)));
            if(protect&&f==30.8677){const double db=20*std::log10(std::abs(measured));lowMin=std::min(lowMin,db);lowMax=std::max(lowMax,db);check(std::abs(db)<.3,"protected low B within 0.3 dB");}
        }
    }
    std::printf("  complex response error %.3g; protected low B %.3f..%.3f dB\n",maxError,lowMin,lowMax);
    check(maxError<2e-6,"complex response incl polarity, mix, feedback delay and wet protection");
    p={};p.depth=0;p.feedback=0;p.protect=0;p.dry=percentToDb(0);p.wet=percentToDb(100);
    for(double f:{30.8677,100.,700.,2500.,10000.})check(std::abs(std::abs(response(sr,f,p))-1)<1e-6,"100% wet unity allpass magnitude");
    // Locate both LFO sweep extremes acoustically; this also verifies rate and
    // +/- two-octave depth, without exposing detector or oscillator test ports.
    for(float rate:{.3f,.6f})for(double phase:{.25,.75}) {
        Dsp sweep;sweep.init(sr);p={};p.rate=rate;p.depth=1;p.centre=700;p.feedback=0;p.protect=0;
        sweep.setParams(p);sweep.reset();
        const double centre=phase==.25?2800:175;
        const double hz=sr/pi*std::atan(std::tan(pi*centre/sr)*std::tan(pi/8));
        const unsigned midpoint=unsigned(sr*phase/rate),width=unsigned(sr*.005);
        std::vector<float> a(midpoint+width),b(a.size());
        for(unsigned i=0;i<a.size();++i)a[i]=float(.1*std::sin(2*pi*hz*i/sr));
        sweep.process(a.data(),b.data(),unsigned(a.size()));double power=0;
        for(unsigned i=midpoint-width;i<b.size();++i)power+=double(b[i])*b[i];
        check(std::sqrt(power/(2*width))<.002,"LFO rate/depth reach predicted sweep notches");
    }
    auto x=signal(unsigned(sr),sr);
    const auto reference=render(sr,x,false,false,false);
    for(bool wrapper:{false,true})for(bool split:{false,true})for(bool inplace:{false,true}){
        const auto y=render(sr,x,wrapper,split,inplace);
        check(std::memcmp(reference.data(),y.data(),y.size()*sizeof(float))==0,"exact automated DSP/wrapper/block/in-place equivalence");
    }
    Dsp d;d.init(sr);p={};p.protect=0;p.dry=percentToDb(100);p.wet=percentToDb(0);d.setParams(p);d.reset();std::vector<float> y(x.size());d.process(x.data(),y.data(),unsigned(x.size()));
    check(x==y,"unity dry, zero wet exact dry identity");
    // Silent automation must not inject transients; reset must restore all state.
    std::vector<float> zero(static_cast<unsigned>(sr));const auto silence=render(sr,zero,true,true,false);
    check(std::all_of(silence.begin(),silence.end(),[](float v){return v==0;}),"silent automation exact zero");
    p={};d.setParams(p);d.reset();d.process(x.data(),y.data(),unsigned(x.size()));auto first=y;d.reset();d.process(x.data(),y.data(),unsigned(x.size()));check(first==y,"deterministic reset");
    // Hard bounds, rapid frequency/mode/feedback changes and long tail decay.
    double peak=0;
    for(unsigned e=0;e<400;++e){p=automated(e);p.dry=percentToDb(0);p.wet=percentToDb(100);d.setParams(p);auto loud=signal(257,sr);for(auto& v:loud)v*=16;d.process(loud.data(),y.data(),257);for(unsigned i=0;i<257;++i){check(std::isfinite(y[i]),"finite stress output");peak=std::max(peak,std::abs(double(y[i])));}}
    check(peak<32,"bounded full-range automated feedback");
    std::vector<float> tail(unsigned(sr*4));d.process(tail.data(),tail.data(),unsigned(tail.size()));double tailPeak=0;for(unsigned i=unsigned(sr*3);i<tail.size();++i)tailPeak=std::max(tailPeak,std::abs(double(tail[i])));check(tailPeak<1e-12,"feedback decays to silence");
    // Parameter steps on DC isolate control discontinuities from input edges.
    std::vector<float> dc(unsigned(sr),.2f);p={};p.depth=0;p.protect=0;d.setParams(p);d.reset();d.process(dc.data(),y.data(),unsigned(dc.size()));
    double jump=0;float last=y.back();
    for(unsigned e=0;e<100;++e){d.setParams(automated(e));d.process(dc.data(),y.data(),257);for(unsigned i=0;i<257;++i){jump=std::max(jump,std::abs(double(y[i]-last)));last=y[i];}}
    check(jump<.01,"smoothed automation no discontinuity on DC");
    std::printf("  stress peak %.3f; tail %.2g; DC automation max step %.6f\n",peak,tailPeak,jump);
    // Envelope soft/hard notes: after settling, compare normalized waveforms.
    // +20 dB sensitivity on soft input must match a ten-times louder note.
    auto envRender=[&](float amplitude,float sensitivity){Dsp e;e.init(sr);auto q=Dsp::Params{};q.mode=1;q.sensitivity=sensitivity;q.depth=1;q.feedback=0;q.protect=0;e.setParams(q);e.reset();std::vector<float> a(unsigned(sr*2)),b(a.size());for(unsigned i=0;i<a.size();++i)a[i]=float(amplitude*std::sin(2*pi*700*i/sr));e.process(a.data(),b.data(),unsigned(a.size()));for(auto& v:b)v/=amplitude;return b;};
    const auto soft=envRender(.02f,0),hard=envRender(.2f,0),boost=envRender(.02f,20);
    double difference=0,equivalent=0;for(unsigned i=unsigned(sr);i<soft.size();++i){difference+=std::pow(soft[i]-hard[i],2);equivalent+=std::pow(boost[i]-hard[i],2);}
    difference=std::sqrt(difference/sr);equivalent=std::sqrt(equivalent/sr);
    check(difference>.1,"envelope soft/hard produce meaningful different movement");check(equivalent<1e-5,"sensitivity calibrated in dB");
    std::printf("  normalized soft/hard difference %.4f; +20 dB equivalence error %.3g\n",difference,equivalent);
    // Actual host lifecycle, optional/unconnected defaults and reactivation.
    const auto* desc=lv2_descriptor(0);const LV2_Feature* features[]={nullptr};
    auto h=desc->instantiate(desc,sr,"",features);
    desc->connect_port(h,0,x.data());desc->connect_port(h,1,y.data());desc->activate(h);
    desc->run(h,unsigned(x.size()));check(first==y,"wrapper unconnected control defaults");
    desc->activate(h);desc->run(h,unsigned(x.size()));check(first==y,"wrapper activation resets history");desc->cleanup(h);
    Dsp impulse;impulse.init(sr);p={};p.depth=0;p.feedback=0;p.protect=0;impulse.setParams(p);impulse.reset();
    float unit=1,instant=0;impulse.process(&unit,&instant,1);check(instant>.5f,"zero transport latency: first sample responds");
    // Invalid controls are sanitized identically to defaults, with no NaN state.
    p={};p.rate=p.depth=p.centre=p.feedback=p.dry=p.protect=p.mode=p.sensitivity=std::numeric_limits<float>::quiet_NaN();d.setParams(p);d.reset();d.process(x.data(),y.data(),unsigned(x.size()));check(first==y,"nonfinite controls use defaults");
    const auto start=std::chrono::steady_clock::now();
    for(unsigned k=0;k<30;++k)for(unsigned i=0;i<x.size();i+=64)d.process(x.data()+i,y.data()+i,std::min(64u,unsigned(x.size())-i));
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::printf("  CPU 30 audio seconds / 64 frames: %.4fs = %.3f%% one core (checksum %.7g)\n",elapsed,elapsed/30*100,y[123]);
}
int main(){check(lv2_descriptor(0)!=nullptr&&lv2_descriptor(1)==nullptr,"descriptor enumeration");check(std::strcmp(lv2_descriptor(0)->URI,"https://suprduprnatural.github.io/supr-pedals/phase")==0,"descriptor URI");for(double sr:{44100.,48000.,96000.})suite(sr);std::printf("\n%d failures\n",failures);return failures?EXIT_FAILURE:EXIT_SUCCESS;}
