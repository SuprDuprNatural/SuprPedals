#include "VowelDsp.h"
#include <lv2/core/lv2.h>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static float percentToDb(double percent) { return percent>0?float(20*std::log10(percent/50)):-60.f; }

using Dsp=supr::VowelDsp;
using Complex=std::complex<double>;
constexpr double pi=3.14159265358979323846;
int failures=0;
void check(bool ok,const char* message) {
    if(!ok){std::printf("FAIL: %s\n",message);++failures;}
}
std::array<float,13> controls(Dsp::Params p) {
    return {p.vowel_a,p.vowel_b,p.mode,p.position,p.depth,p.rate,
            p.sensitivity,p.release,p.throat,p.focus,p.dry,p.wet,p.protect};
}
std::vector<float> signal(unsigned n,double sr) {
    std::vector<float> x(n); unsigned rng=12345;
    for(unsigned i=0;i<n;++i) {
        rng=1664525*rng+1013904223;
        x[i]=float(.17*std::sin(2*pi*30.8677*i/sr)+.12*std::sin(2*pi*731*i/sr)
                   +.04*(double(rng)/4294967296.-.5));
    }
    return x;
}
std::vector<float> fixed(double sr,Dsp::Params p,std::vector<float> x,bool host=false) {
    if(host) {
        auto* desc=lv2_descriptor(0);
        auto h=desc->instantiate(desc,sr,"",nullptr); check(h!=nullptr,"instantiate");
        auto cv=controls(p); float meter[4]{};
        for(unsigned i=0;i<cv.size();++i)desc->connect_port(h,i==12?18:i+2,&cv[i]);
        for(unsigned i=0;i<4;++i)desc->connect_port(h,i+14,&meter[i]);
        desc->activate(h); desc->run(h,0);
        for(unsigned i=0;i<x.size();i+=73) {
            desc->connect_port(h,0,x.data()+i); desc->connect_port(h,1,x.data()+i);
            desc->run(h,std::min(73u,unsigned(x.size())-i));
        }
        check(meter[0]>=0 && meter[0]<=1,"host morph output bounded");
        for(unsigned j=1;j<4;++j)check(meter[j]>=80 && meter[j]<=6000,"host frequency outputs bounded");
        desc->cleanup(h);
    } else {
        Dsp d; d.init(sr); d.setParams(p); d.reset();
        d.process(x.data(),x.data(),unsigned(x.size()));
    }
    return x;
}
// Independent bilinear analogue model, not the SVF recurrence under test.
Complex theory(double sr,double hz,Dsp::Params p) {
    constexpr double table[5][3]={{350,600,2400},{400,750,2400},{600,1040,2250},
                                 {400,1620,2400},{250,1750,2600}};
    constexpr double weights[3]={1,.85,.35};
    Complex voice=0;
    for(unsigned j=0;j<3;++j) {
        double f=table[unsigned(p.vowel_a)][j]*std::pow(
            table[unsigned(p.vowel_b)][j]/table[unsigned(p.vowel_a)][j],p.position)*std::exp2(p.throat/12.);
        f=std::clamp(f,80.,std::min(6000.,sr*.4));
        const Complex s(0,std::tan(pi*hz/sr)/std::tan(pi*f/sr));
        const double k=1/(2+8*p.focus);
        voice+=1.75*weights[j]*k*s/(s*s+k*s+1.);
    }
    const Complex z=std::polar(1.,-2*pi*hz/sr);
    const double a=1/(1+std::tan(pi*250/sr));
    const Complex h=a*(1.-z)/(1.-(2*a-1)*z);
    const Complex s(0,std::tan(pi*hz/sr)/std::tan(pi*250/sr));
    const auto denominator=std::pow(s*s+std::sqrt(2.)*s+1.,2);
    const auto low=1./denominator, high=std::pow(s,4)/denominator;
    const double dryGain=supr::timespace::returnGain(p.dry),wetGain=supr::timespace::returnGain(p.wet);
    return p.protect?low+high*(dryGain+wetGain*voice):dryGain+wetGain*h*h*voice;
}
Complex measured(double sr,double hz,Dsp::Params p) {
    const unsigned n=unsigned(sr*.35);
    std::vector<float> x(n);
    for(unsigned i=0;i<n;++i)x[i]=float(.1*std::sin(2*pi*hz*i/sr));
    const auto y=fixed(sr,p,x);
    double ss=0,cc=0,sc=0,ys=0,yc=0;
    for(unsigned i=n/2;i<n;++i) {
        const double s=std::sin(2*pi*hz*i/sr),c=std::cos(2*pi*hz*i/sr);
        ss+=s*s; cc+=c*c; sc+=s*c; ys+=y[i]*s; yc+=y[i]*c;
    }
    return {10*(ys*cc-yc*sc)/(ss*cc-sc*sc),10*(yc*ss-ys*sc)/(ss*cc-sc*sc)};
}
Dsp::Params automated(unsigned event) {
    Dsp::Params p;
    p.vowel_a=float(event%5); p.vowel_b=float((event+2)%5); p.mode=float(event%3);
    p.position=event%2?1:0; p.depth=event%3?1:0; p.rate=event%2?8:.05f;
    p.sensitivity=event%2?24:-24; p.release=event%2?40:800;
    p.throat=event%2?6:-6; p.focus=float(event%2); p.dry=percentToDb(100*(1-(event%3?1:0)));p.wet=percentToDb(100*(event%3?1:0));
    if(p.wet>-60)p.wet+=(event%2?12:-12); p.protect=event%2; return p;
}
std::vector<float> automation(double sr,const std::vector<float>& input,bool host,bool split,bool inplace) {
    auto x=input; std::vector<float> y(x.size());
    Dsp d; d.init(sr); d.setParams(automated(0)); d.reset();
    auto* desc=lv2_descriptor(0); auto cv=controls(automated(0));
    LV2_Handle h=host?desc->instantiate(desc,sr,"",nullptr):nullptr;
    float meter[4]{};
    if(host) {
        for(unsigned j=0;j<cv.size();++j)desc->connect_port(h,j==12?18:j+2,&cv[j]);
        for(unsigned j=0;j<4;++j)desc->connect_port(h,j+14,&meter[j]);
        desc->activate(h);desc->run(h,0);
    }
    unsigned pos=0,event=0;
    while(pos<x.size()) {
        const auto p=automated(event++); d.setParams(p); cv=controls(p);
        const unsigned end=std::min(unsigned(x.size()),pos+997);
        while(pos<end) {
            unsigned n=std::min(end-pos,split?1+(pos*37)%257:end-pos);
            float* out=inplace?x.data()+pos:y.data()+pos;
            if(host) {
                desc->connect_port(h,0,x.data()+pos);desc->connect_port(h,1,out);desc->run(h,n);
            } else d.process(x.data()+pos,out,n);
            pos+=n;
        }
    }
    if(host)desc->cleanup(h);
    return inplace?x:y;
}
void suite(double sr) {
    std::printf("\nSuprVowel %.0f Hz\n",sr);
    Dsp::Params p; p.mode=0; p.dry=percentToDb(0);p.wet=percentToDb(100);
    double error=0,lowMin=100,lowMax=-100;
    for(float vowel:{0.f,1.f,2.f,3.f,4.f})for(float throat:{-6.f,0.f,6.f}) {
        p.vowel_a=vowel;p.throat=throat;p.focus=(vowel==4?1:0.6f);
        for(double hz:{30.8677,41.2034,55.,110.,350.,600.,1040.,1750.,2600.}) {
            if(hz>sr*.4)continue;
            const auto actual=measured(sr,hz,p),expected=theory(sr,hz,p);
            error=std::max(error,std::abs(actual-expected));
            if(hz<56) {
                const double db=20*std::log10(std::abs(actual));
                lowMin=std::min(lowMin,db);lowMax=std::max(lowMax,db);
                check(std::abs(db)<.1,"clean low B/E/A preserved at full wet, every vowel/throat");
            }
        }
    }
    std::printf("  independent response error %.3g; low B/E/A %.3f..%.3f dB\n",error,lowMin,lowMax);
    check(error<5e-5,"complex response agrees with independent bilinear model");
    // The independent dry/wet gain law and log-frequency midpoint, not only endpoints.
    p={};p.mode=0;p.vowel_b=4;p.position=.5f;p.focus=0;
    for(float mix:{0.f,.5f,1.f})for(float level:{-12.f,0.f,12.f}) {
        p.dry=percentToDb(100*(1-mix));p.wet=percentToDb(100*mix);if(p.wet>-60)p.wet+=(level);
        check(std::abs(measured(sr,1337,p)-theory(sr,1337,p))<5e-5,"dry/wet gains and geometric formant interpolation");
    }
    auto x=signal(unsigned(sr),sr);
    auto reference=automation(sr,x,false,false,false);
    for(bool host:{false,true})for(bool split:{false,true})for(bool inplace:{false,true}) {
        auto y=automation(sr,x,host,split,inplace);
        check(std::memcmp(reference.data(),y.data(),y.size()*sizeof(float))==0,"exact automation across DSP/LV2/block/in-place");
    }
    auto silent=automation(sr,std::vector<float>(unsigned(sr)),true,true,false);
    check(std::all_of(silent.begin(),silent.end(),[](float v){return v==0;}),"silent automation injects no signal");
    for(bool host:{false,true}) {
        p={};p.protect=0;p.dry=percentToDb(100);p.wet=percentToDb(0);
        const float max=std::numeric_limits<float>::max();
        std::vector<float> extremes={0.f,-0.f,32,-64,max,-max,.125f};
        const auto dry=fixed(sr,p,extremes,host);
        check(std::memcmp(dry.data(),extremes.data(),dry.size()*sizeof(float))==0,"unclipped bit-exact dry including signed zero and float limits");
        for(float mix:{0.f,.5f,1.f}) {
            p.dry=percentToDb(100*(1-mix));p.wet=percentToDb(100*mix);
            auto dirty=x,clean=x;
            dirty[17]=std::numeric_limits<float>::quiet_NaN();
            dirty[73]=std::numeric_limits<float>::infinity();dirty[151]=-dirty[73];
            clean[17]=clean[73]=clean[151]=0;
            check(fixed(sr,p,dirty,host)==fixed(sr,p,clean,host),"NaN/Inf audio replaced by zero without poisoning state");
            auto out=fixed(sr,p,extremes,host);
            check(std::all_of(out.begin(),out.end(),[](float v){return std::isfinite(v);}),"finite output for all finite float inputs");
        }
    }
    p={};const auto defaults=fixed(sr,p,x);
    p.vowel_a=p.vowel_b=p.mode=p.position=p.depth=p.rate=p.sensitivity=p.release=
        p.throat=p.focus=p.dry=p.wet=p.protect=std::numeric_limits<float>::quiet_NaN();
    check(defaults==fixed(sr,p,x,true),"all invalid host controls use documented defaults");
    // Defaults for absent optional connections, ignored unknown ports, and reset.
    auto* desc=lv2_descriptor(0);auto h=desc->instantiate(desc,sr,"",nullptr);
    std::vector<float> y(x.size());
    desc->connect_port(h,0,x.data());desc->connect_port(h,1,y.data());desc->connect_port(h,999,nullptr);
    for(unsigned run=0;run<2;++run) {
        desc->activate(h);desc->run(h,0);desc->run(h,unsigned(x.size()));
        check(y==defaults,"host unconnected defaults and repeated activation");
    }
    desc->cleanup(h);
    Dsp d; d.init(44100);d.setParams(automated(1));d.reset();d.process(x.data(),y.data(),unsigned(x.size()));
    d.init(sr);d.setParams({});d.reset();d.process(x.data(),y.data(),unsigned(x.size()));
    check(y==defaults,"sample-rate reinitialization clears every history");
    d.reset();d.process(x.data(),y.data(),unsigned(x.size()));check(y==defaults,"deterministic reset");
    // Decay and full-range, sample-fast hostile automation: no output limiter.
    double peak=0;
    for(unsigned e=0;e<300;++e) {
        d.setParams(automated(e));auto loud=signal(257,sr);for(auto& v:loud)v*=16;
        d.process(loud.data(),y.data(),257);
        for(unsigned i=0;i<257;++i){check(std::isfinite(y[i]),"finite stressed output");peak=std::max(peak,std::abs(double(y[i])));}
    }
    check(peak<80,"bounded resonances under full-range automation");
    std::vector<float> tail(unsigned(sr*4));d.process(tail.data(),tail.data(),unsigned(tail.size()));
    double tailPeak=0;for(unsigned i=unsigned(sr*3);i<tail.size();++i)tailPeak=std::max(tailPeak,std::abs(double(tail[i])));
    check(tailPeak<1e-12,"tails decay without denormal noise or self-oscillation");
    // Transitions to dry must eventually reach exact identity, not just approach it.
    p={};p.protect=0;p.dry=percentToDb(100);p.wet=percentToDb(0);d.setParams(p);d.process(x.data(),y.data(),unsigned(x.size()));
    check(std::memcmp(x.data()+x.size()/2,y.data()+y.size()/2,x.size()/2*sizeof(float))==0,"automated unity dry, zero wet settles to exact dry");
    std::printf("  hostile automation peak %.3f; tail %.3g\n",peak,tailPeak);
    // Output formants are the frequencies of the actual filters.
    for(float t:{0.f,.5f,1.f}) {
        p={};p.mode=0;p.position=t;p.vowel_a=0;p.vowel_b=4;
        d.setParams(p);d.reset();
        check(std::abs(d.frequency(0)-350*std::pow(250./350,t))<.001,"formant telemetry has true log-interpolated centre");
        check(std::abs(d.position()-t)<1e-6,"manual position telemetry");
    }
}
void motion() {
    constexpr double sr=48000;
    auto envelopePosition=[](float amplitude,float sensitivity,float release,unsigned silence) {
        Dsp d;d.init(sr);Dsp::Params p;p.sensitivity=sensitivity;p.release=release;
        d.setParams(p);d.reset();
        std::vector<float> x(static_cast<unsigned>(sr));
        for(unsigned i=0;i<x.size();++i)x[i]=float(amplitude*std::sin(2*pi*110*i/sr));
        d.process(x.data(),x.data(),unsigned(x.size()));
        x.assign(silence,0);d.process(x.data(),x.data(),unsigned(x.size()));
        return d.position();
    };
    const float soft=envelopePosition(.02f,0,180,0),hard=envelopePosition(.2f,0,180,0);
    const float boosted=envelopePosition(.02f,20,180,0);
    check(hard-soft>.35,"playing dynamics produce useful vowel travel");
    check(std::abs(hard-boosted)<1e-5,"sensitivity is calibrated in dB, independent of input gain");
    check(envelopePosition(.2f,0,800,12000)>envelopePosition(.2f,0,40,12000)+.25,
          "Release meaningfully controls vowel recovery");
    std::printf("\nMotion: soft %.4f, hard %.4f, +20 dB %.4f\n",soft,hard,boosted);
    for(float rate:{.5f,1.f,2.f}) {
        Dsp d;d.init(sr);Dsp::Params p;p.mode=2;p.rate=rate;p.position=.1f;p.depth=.7f;
        d.setParams(p);d.reset();
        std::vector<float> zero(unsigned(sr/rate*2)),out(zero.size());
        double lo=1,hi=0;unsigned peaks=0;double last=0;bool rising=true;
        for(unsigned i=0;i<zero.size();i+=48) {
            d.process(zero.data()+i,out.data()+i,std::min(48u,unsigned(zero.size())-i));
            const double pos=d.position();lo=std::min(lo,pos);hi=std::max(hi,pos);
            if(last>pos && rising){++peaks;rising=false;} else if(pos>last)rising=true;
            last=pos;
        }
        check(peaks==2,"LFO rate produces two cycles in two periods");
        check(lo>=.099 && hi>.77 && hi<=.801,"LFO floor/depth range with smoothing");
    }
    // A control jump on settled DC must not inject a click.
    Dsp d;d.init(sr);Dsp::Params p;p.mode=0;d.setParams(p);d.reset();
    std::vector<float> dc(unsigned(sr),.2f),y(dc.size());d.process(dc.data(),y.data(),unsigned(dc.size()));
    float last=y.back();double jump=0;
    for(unsigned e=0;e<100;++e) {
        auto q=automated(e);d.setParams(q);d.process(dc.data(),y.data(),257);
        for(unsigned i=0;i<257;++i){jump=std::max(jump,std::abs(double(y[i]-last)));last=y[i];}
    }
    check(jump<.001,"automated vowels/modes/focus/mix do not click on DC");
    // Existing SuprEnvelope -> Vowel remains finite and transient-bounded.
    supr::EnvFilterDsp env;env.init(sr);env.setRes(.8f);env.setBlend(.65f);
    auto x=signal(unsigned(sr*3),sr);env.process(x.data(),x.data(),unsigned(x.size()));
    p={};d.setParams(p);d.reset();d.process(x.data(),x.data(),unsigned(x.size()));
    check(std::all_of(x.begin(),x.end(),[](float v){return std::isfinite(v)&&std::abs(v)<4;}),"SuprEnvelope then SuprVowel integration");
    std::printf("  maximum DC automation step %.3g\n",jump);
    x=signal(unsigned(sr),sr);y.resize(x.size());
    const auto start=std::chrono::steady_clock::now();
    for(unsigned n=0;n<20;++n)for(unsigned i=0;i<x.size();i+=64)
        d.process(x.data()+i,y.data()+i,std::min(64u,unsigned(x.size())-i));
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::printf("  local CPU: 20 audio seconds in %.4f s = %.3f%% one core (%.7g)\n",elapsed,elapsed*5,y[123]);
}
int main() {
    check(lv2_descriptor(0)&&!lv2_descriptor(1),"descriptor enumeration");
    check(std::strcmp(lv2_descriptor(0)->URI,"https://suprduprnatural.github.io/supr-pedals/vowel")==0,"descriptor URI");
    for(double sr:{8000.,44100.,48000.,96000.,192000.})suite(sr);
    motion();std::printf("\n%d failures\n",failures);return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
