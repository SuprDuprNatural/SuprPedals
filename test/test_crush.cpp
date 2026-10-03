#include "CrushDsp.h"
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
using Dsp=supr::CrushDsp;
constexpr double pi=3.14159265358979323846;
int failures=0;
void check(bool ok,const char* message) { if(!ok){std::printf("FAIL: %s\n",message);++failures;} }
std::vector<float> signal(unsigned n,double sr) {
    std::vector<float> x(n); unsigned rng=12345;
    for(unsigned i=0;i<n;++i){rng=1664525*rng+1013904223;x[i]=float(.15*std::sin(2*pi*30.8677*i/sr)+.1*std::sin(2*pi*739*i/sr)+.03*(double(rng)/4294967296.-.5));}
    return x;
}
Dsp::Params automated(unsigned event) {
    Dsp::Params p;
    p.bits=event%2?2:16; p.rate=event%2?200:48000; p.drive=event%2?24:0;
    p.env=event%2?4:-4;p.sensitivity=event%2?24:-24;p.release=event%2?30:1000;
    p.tone=event%2?400:18000;p.protect=event%2;p.dry=percentToDb(100*(1-(event%3?1:0)));p.wet=percentToDb(100*(event%3?1:0));if(p.wet>-60)p.wet+=(event%2?12:-24);return p;
}
std::vector<float> render(double sr,const std::vector<float>& input,bool wrapper,bool split,bool inplace) {
    std::vector<float> output(input.size());auto x=input;
    const auto* desc=lv2_descriptor(0);LV2_Handle h=nullptr;
    Dsp d;d.init(sr);float controls[10]{};
    auto set=[&](Dsp::Params p){
        d.setParams(p);const float v[]={p.bits,p.rate,p.drive,p.env,p.sensitivity,p.release,p.tone,p.protect,p.dry,p.wet};
        std::copy(v,v+10,controls);
    };
    set(automated(0));d.reset();
    if(wrapper){const LV2_Feature* features[]={nullptr};h=desc->instantiate(desc,sr,"",features);check(h!=nullptr,"LV2 instantiate");for(unsigned i=0;i<10;++i)desc->connect_port(h,i+2,controls+i);desc->activate(h);desc->run(h,0);}
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
        float controls[]={p.bits,p.rate,p.drive,p.env,p.sensitivity,p.release,p.tone,p.protect,p.dry,p.wet};
        for(unsigned i=0;i<10;++i)desc->connect_port(h,i+2,controls+i);
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
double rms(const std::vector<float>& x,unsigned start=0) {
    double sum=0;for(unsigned i=start;i<x.size();++i)sum+=double(x[i])*x[i];return std::sqrt(sum/(x.size()-start));
}
std::vector<float> sine(double sr,double hz,double amplitude=.2) {
    std::vector<float> x(unsigned(sr*2));for(unsigned i=0;i<x.size();++i)x[i]=float(amplitude*std::sin(2*pi*hz*i/sr));return x;
}
double spectral(const std::vector<float>& x,double sr,double hz) {
    std::complex<double> sum=0;for(unsigned i=unsigned(sr);i<x.size();++i)sum+=double(x[i])*std::polar(1.,-2*pi*hz*i/sr);return 2*std::abs(sum)/sr;
}
void suite(double sr) {
    std::printf("\n%.0f Hz\n",sr);
    auto x=signal(unsigned(sr),sr),reference=render(sr,x,false,false,false);
    for(bool wrapper:{false,true})for(bool split:{false,true})for(bool inplace:{false,true})
        check(reference==render(sr,x,wrapper,split,inplace),"automated DSP/wrapper/block/in-place bit identity");
    Dsp::Params p;p.protect=0;p.dry=percentToDb(100);p.wet=percentToDb(0);
    const float max=std::numeric_limits<float>::max(),nan=std::numeric_limits<float>::quiet_NaN();
    const std::vector<float> extreme={0,-0.f,32,-64,max,-max,.1f};
    auto bypass=fixedRender(sr,p,extreme,true);
    check(std::memcmp(bypass.data(),extreme.data(),extreme.size()*sizeof(float))==0,"unclipped bit-exact dry, signed zero and float limits");
    p={};auto normal=fixedRender(sr,p,x,true);
    p.bits=p.rate=p.drive=p.env=p.sensitivity=p.release=p.tone=p.protect=p.dry=p.wet=nan;
    check(normal==fixedRender(sr,p,x,true),"nonfinite controls default identically");
    p={};auto dirty=x,clean=x;dirty[5]=nan;dirty[12]=INFINITY;dirty[31]=-INFINITY;clean[5]=clean[12]=clean[31]=0;
    check(fixedRender(sr,p,dirty,true)==fixedRender(sr,p,clean,true),"nonfinite input cannot poison state");
    for(unsigned event=0;event<4;++event) {
        auto stress=extreme;stress.insert(stress.end(),x.begin(),x.end());
        const auto y=fixedRender(sr,automated(event),stress,true);
        check(std::all_of(y.begin(),y.end(),[](float v){return std::isfinite(v);}),"extreme finite audio/controls stay finite");
    }
    auto silence=render(sr,std::vector<float>(unsigned(sr)),true,true,true);
    check(std::all_of(silence.begin(),silence.end(),[](float v){return v==0;}),"silent automation produces no idle noise");
    p={};p.protect=0;p.dry=percentToDb(0);p.wet=percentToDb(100);p.drive=0;p.rate=48000;p.bits=3;p.tone=18000;
    for(double value:{-.76,-.3,-.1,0.,.1,.3,.76}) {
        auto y=fixedRender(sr,p,std::vector<float>(unsigned(sr),float(value)),true);
        check(std::abs(y.back()-std::round(value*4)/4)<1e-6,"three-bit symmetric quantizer levels incl zero");
    }
    p.bits=5.5;
    auto fractional=fixedRender(sr,p,std::vector<float>(unsigned(sr),.3f),true);
    check(std::abs(fractional.back()-std::round(.3*std::exp2(4.5))/std::exp2(4.5))<1e-6,"fractional effective bits");
    p.bits=16;p.drive=12;
    auto gain=fixedRender(sr,p,std::vector<float>(unsigned(sr),.05f),true);
    check(std::abs(gain.back()-.05)<1e-5,"drive compensated below clipping");
    // 15 kHz sampled at 8 kHz aliases to 1 kHz. Check the sound, not a private clock variable.
    p.drive=0;p.rate=8000;p.tone=18000;
    auto alias=fixedRender(sr,p,sine(sr,15000),true);
    const double folded=spectral(alias,sr,1000),original=spectral(alias,sr,15000);
    std::printf("  15 kHz / 8 kHz clock: 1 kHz alias %.4f, original %.4f\n",folded,original);
    check(folded>.07&&folded>original*3,"intentional sample-rate alias at predicted frequency");
    p.tone=400;auto dark=fixedRender(sr,p,sine(sr,15000),true);
    check(spectral(dark,sr,1000)<folded*.5,"tone filter reduces digital edges");
    double maxLow=0;
    for(float bits:{2.f,5.f,16.f})for(float rate:{200.f,1703.f,48000.f}) {
        p={};p.bits=bits;p.rate=rate;p.dry=percentToDb(0);p.wet=percentToDb(100);p.drive=12;
        const auto low=sine(sr,30.8677),y=fixedRender(sr,p,low,true);
        const double db=20*std::log10(rms(y,unsigned(sr))/rms(low,unsigned(sr)));
        maxLow=std::max(maxLow,std::abs(db));
        check(std::abs(db)<.5,"clean low B remains within 0.5 dB with protection");
    }
    std::printf("  worst protected low-B RMS change %.3f dB\n",maxLow);
    p={};p.protect=0;p.dry=percentToDb(0);p.wet=percentToDb(100);p.rate=1500;p.bits=10;
    const auto rich=signal(unsigned(sr),sr);const auto base=fixedRender(sr,p,rich,true);
    p.env=3;const auto up=fixedRender(sr,p,rich,true);p.env=-3;const auto down=fixedRender(sr,p,rich,true);
    double upDiff=0,downDiff=0;for(unsigned i=0;i<rich.size();++i){upDiff+=std::pow(up[i]-base[i],2);downDiff+=std::pow(down[i]-base[i],2);}
    check(std::sqrt(upDiff/sr)>.02&&std::sqrt(downDiff/sr)>.02,"both envelope directions audibly alter rate reduction");
    // Undo the known one-pole tone filter to count actual held-sample changes.
    // More captures means a higher clock, without reading private DSP state.
    auto captures=[&](const std::vector<float>& output) {
        const double a=std::exp(-2*pi*6000/sr);double last=0;unsigned count=0;
        for(unsigned i=unsigned(sr/2);i<output.size();++i) {
            const double held=(output[i]-a*output[i-1])/(1-a);
            if(std::abs(held-last)>1e-4)++count;
            last=held;
        }return count;
    };
    check(captures(up)>captures(base)*1.5&&captures(down)<captures(base)*.7,"positive sweep opens clock, negative closes it");
    p.env=3;p.sensitivity=-24;const auto insensitive=fixedRender(sr,p,rich,true);
    check(captures(insensitive)<captures(up),"sensitivity controls actual envelope clock travel");
    p={};p.dry=percentToDb(0);p.wet=percentToDb(100);p.protect=0;
    const auto untrimmed=fixedRender(sr,p,x,true);if(p.wet>-60)p.wet+=(-6);
    auto trim=fixedRender(sr,p,x,true);check(std::abs(rms(trim)/rms(untrimmed)-std::pow(10.,-.3))<1e-6,"effect level calibrated in dB");
    Dsp d;d.init(sr);d.setParams({});d.reset();auto y=x;d.process(x.data(),y.data(),unsigned(x.size()));
    d.reset();auto z=x;d.process(x.data(),z.data(),unsigned(x.size()));check(y==z,"reset deterministic");
    const auto* desc=lv2_descriptor(0);auto h=desc->instantiate(desc,sr,"",nullptr);
    desc->connect_port(h,0,x.data());desc->connect_port(h,1,z.data());desc->activate(h);desc->run(h,0);desc->run(h,unsigned(x.size()));
    check(y==z,"unconnected LV2 controls have DSP defaults");
    desc->activate(h);desc->run(h,unsigned(x.size()));check(y==z,"LV2 reactivation resets all history");desc->cleanup(h);
    d.init(96000);d.setParams(automated(1));d.reset();d.process(x.data(),z.data(),unsigned(x.size()));
    d.init(sr);d.setParams({});d.reset();d.process(x.data(),z.data(),unsigned(x.size()));check(y==z,"sample-rate reinit matches fresh instance");
    std::vector<float> tail(unsigned(sr*4));d.process(tail.data(),tail.data(),unsigned(tail.size()));check(rms(tail,unsigned(sr*3))<1e-12,"held audio and filters decay after silence");
    const auto start=std::chrono::steady_clock::now();
    for(unsigned k=0;k<10;++k)for(unsigned i=0;i<x.size();i+=64)d.process(x.data()+i,z.data()+i,std::min(64u,unsigned(x.size())-i));
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::printf("  64-frame CPU %.3f%% one core (checksum %.7g)\n",elapsed*10,z[123]);
}
int main() {
    check(lv2_descriptor(0)&&!lv2_descriptor(1),"single descriptor");
    check(std::strcmp(lv2_descriptor(0)->URI,"https://suprduprnatural.github.io/supr-pedals/crush")==0,"URI");
    for(double sr:{8000.,44100.,48000.,96000.,192000.}) {
        if(sr>=44100&&sr<=96000)suite(sr);
        else {auto x=signal(8000,sr);auto y=render(sr,x,true,true,true);check(std::all_of(y.begin(),y.end(),[](float v){return std::isfinite(v);}),"boundary sample rates finite");}
    }
    std::printf("\n%d failures\n",failures);return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
