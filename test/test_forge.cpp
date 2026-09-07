// SuprForge musical and host contracts. No audio hardware required.
#include "ForgeDsp.h"
#include <lv2/core/lv2.h>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <random>
#include <cstring>

static int failures = 0;
static void check(bool ok, const char* fmt, ...) {
    std::printf(ok ? "  PASS " : "  FAIL ");
    va_list ap; va_start(ap,fmt); std::vprintf(fmt,ap); va_end(ap); puts("");
    if (!ok) ++failures;
}
using Params = supr::ForgeDsp::Params;
static Params neutral() {
    Params p;p.drive=0;p.comp=0;p.weight=0;p.bite=0;p.fizz=12000;p.level=0;p.gate=-90;return p;
}
static std::vector<float> tone(float fs, float f, float amp=.25f, float seconds=2) {
    std::vector<float> x(size_t(fs*seconds));
    for(size_t i=0;i<x.size();++i)x[i]=amp*std::sin(2*M_PI*f*double(i)/fs);
    return x;
}
static std::vector<float> render(const std::vector<float>& x,float fs,Params p,unsigned block=64) {
    supr::ForgeDsp d;d.init(fs);d.setParams(p);std::vector<float> y(x.size());
    for(size_t i=0;i<x.size();i+=block)d.process(x.data()+i,y.data()+i,std::min<size_t>(block,x.size()-i));
    return y;
}
static double mag(const std::vector<float>& x,float fs,float f) {
    const size_t a=x.size()-size_t(fs);double c=0,s=0;
    for(size_t i=a;i<x.size();++i){double ph=2*M_PI*f*double(i)/fs;c+=x[i]*std::cos(ph);s+=x[i]*std::sin(ph);}
    return 2*std::hypot(c,s)/fs;
}
static double db(double v){return 20*std::log10(std::max(v,1e-12));}
static double peak(const std::vector<float>& x){double p=0;for(float v:x)p=std::max(p,std::fabs(double(v)));return p;}
static bool finite(const std::vector<float>& x){for(float v:x)if(!std::isfinite(v))return false;return true;}

static void testRate(float fs) {
    printf("\nSuprForge @ %.0f Hz\n",fs);
    auto p=neutral();
    double worst=0;
    for(float tight:{80.f,180.f,500.f})for(float f:{31.f,41.f,80.f,150.f,180.f,400.f,1200.f,3000.f,6000.f}){
        p.tight=tight;auto x=tone(fs,f);auto y=render(x,fs,p);
        worst=std::max(worst,std::fabs(db(mag(y,fs,f)/mag(x,fs,f))));
    }
    check(worst<.12,"neutral crossover reconstructs, including low B (worst %.3f dB)",worst);
    p=neutral();p.tight=180;
    auto x=tone(fs,31,.3);
    auto hi=tone(fs,997,.07);for(size_t i=0;i<x.size();++i)x[i]+=hi[i];
    auto clean=render(x,fs,p);p.drive=10;auto dirty=render(x,fs,p);
    const double lowChange=db(mag(dirty,fs,31)/mag(clean,fs,31));
    const double harmonic=db(mag(dirty,fs,2991)/mag(dirty,fs,997));
    check(std::fabs(lowChange)<.4,"max drive preserves low B beside driven mids (%+.3f dB)",lowChange);
    check(harmonic>-40,"drive creates actual third-harmonic grind (H3 %.1f dB)",harmonic);
    p.weight=6;auto heavy=render(tone(fs,31),fs,p);
    auto unweighted=p;unweighted.weight=0;
    auto base=render(tone(fs,31),fs,unweighted);
    const double weight=db(mag(heavy,fs,31)/mag(base,fs,31));
    check(std::fabs(weight-6)<.25,"Weight +6 delivers +6 dB on low B (%.2f)",weight);
    p=neutral();p.bite=9;
    auto bite=render(tone(fs,1800),fs,p);auto biteBase=render(tone(fs,1800),fs,neutral());
    check(std::fabs(db(mag(bite,fs,1800)/mag(biteBase,fs,1800))-9)<.1,"Bite +9 is a calibrated 1.8 kHz bell");
    p=neutral();p.fizz=3000;
    auto dark=render(tone(fs,8000),fs,p);auto bright=render(tone(fs,8000),fs,neutral());
    const double topCut=db(mag(dark,fs,8000)/mag(bright,fs,8000));
    check(topCut<-30,"Fizz 3 kHz removes high hash (8 kHz %+.1f dB)",topCut);
    // Integer-second windows isolate non-harmonic foldback without leakage.
    p=Params{};p.drive=10;p.comp=0;p.gate=-90;p.level=0;
    const float probeHz=6107;
    auto aliasOut=render(tone(fs,probeHz,.4f),fs,p);
    const double fundamental=mag(aliasOut,fs,probeHz);
    double alias=0;
    for(int h=2;h<=80;++h){
        double f=std::fmod(h*probeHz,fs);if(f>fs/2)f=fs-f;
        if(h*probeHz>fs/2 && f>100 && f<10000)
            alias=std::max(alias,mag(aliasOut,fs,float(f)));
    }
    check(db(alias/fundamental)<-48,"max-drive non-harmonic foldback stays buried (%.1f dB)",db(alias/fundamental));
    p=neutral();p.comp=10;
    auto low=render(tone(fs,31,.6),fs,p);
    const double lowH3=db(mag(low,fs,93)/mag(low,fs,31));
    check(lowH3<-50 && db(mag(low,fs,31)/.6)<-6,"low compressor levels without waveform grind (H3 %.1f dB)",lowH3);
    std::vector<float> noise(size_t(fs*3));std::mt19937 rng(72);std::uniform_real_distribution<float> rnd(-1,1);
    for(float& v:noise)v=rnd(rng)*.00001f;
    p=Params{};p.drive=10;p.gate=-55;auto gated=render(noise,fs,p);
    p.gate=-90;auto open=render(noise,fs,p);
    const double rejection=db(mag(gated,fs,7137)/mag(open,fs,7137));
    check(rejection<-40,"gate rejects high-band hiss in gaps (%+.1f dB at 7.1 kHz)",rejection);
    // Fast bass pluck: compare the first 20 ms with gate open and enabled.
    auto pluck=tone(fs,82,.3f,2);auto partial=tone(fs,984,.12f,2);
    for(size_t i=0;i<pluck.size();++i){const double t=double(i)/fs;
        pluck[i]=(pluck[i]+partial[i])*std::min(1.,t/.002)*std::exp(-t*4);}
    p=Params{};p.gate=-90;auto ungated=render(pluck,fs,p);
    p.gate=-65;auto attacked=render(pluck,fs,p);
    double eOn=0,eOff=0;for(size_t i=0;i<size_t(fs*.02);++i){eOn+=attacked[i]*attacked[i];eOff+=ungated[i]*ungated[i];}
    check(std::fabs(10*std::log10(eOn/eOff))<.8,"gate preserves fast pluck onset (%+.2f dB)",10*std::log10(eOn/eOff));
    p=Params{};
    auto zero=render(std::vector<float>(size_t(fs)),fs,p);
    check(peak(zero)==0,"digital silence remains exact zero");
    auto signal=tone(fs,83,.4);for(size_t i=0;i<signal.size();++i)signal[i]+=.07f*rnd(rng);
    auto a=render(signal,fs,p,1),b=render(signal,fs,p,257);
    check(a==b && finite(a),"identical samples at block 1 and 257; finite");
    // Exercise every control at the same exact sample, with state already warm.
    supr::ForgeDsp dsp;dsp.init(fs);std::vector<float> out(signal.size());
    float maxExtra=0,prevIn=0,prevOut=0;
    for(size_t i=0;i<signal.size();++i){
        if(i%size_t(fs/4)==0){bool high=(i/size_t(fs/4))%2;Params q;
            q.drive=high?10:0;q.tight=high?500:80;q.weight=high?12:-12;q.bite=high?9:-9;
            q.fizz=high?12000:1500;q.level=-12;q.comp=high?10:0;q.gate=-90;dsp.setParams(q);}
        dsp.process(&signal[i],&out[i],1);
        maxExtra=std::max(maxExtra,std::fabs((out[i]-prevOut)-(signal[i]-prevIn)));prevOut=out[i];prevIn=signal[i];
    }
    check(finite(out)&&peak(out)<2 && maxExtra<.5,"extreme live control changes stay bounded (peak %.2f, step %.3f)",peak(out),maxExtra);
}

static void testHost() {
    const auto* desc=lv2_descriptor(0);check(desc && !lv2_descriptor(1),"LV2 has exactly one descriptor");
    auto instance=desc->instantiate(desc,48000,nullptr,nullptr);check(instance!=nullptr,"LV2 instantiates");
    if(!instance)return;
    float controls[]={0,0,180,0,12000,0,0,-90};float meters[4]={};
    auto input=tone(48000,997,.1f);std::vector<float> out(input.size());
    desc->connect_port(instance,0,input.data());desc->connect_port(instance,1,out.data());
    for(int i=0;i<8;++i)desc->connect_port(instance,i+2,&controls[i]);
    for(int i=0;i<4;++i)desc->connect_port(instance,i+10,&meters[i]);
    desc->activate(instance);desc->run(instance,input.size());
    auto expected=render(input,48000,neutral());
    check(out==expected,"LV2 controls agree with DSP port contract");
    check(meters[3]==23 && meters[0]==0 && std::fabs(meters[1])<.001 && meters[2]>-25,"LV2 latency and meters are meaningful");
    // Hosts may reconnect the same buffer for input and output.
    desc->activate(instance);auto inplace=input;
    desc->connect_port(instance,0,inplace.data());desc->connect_port(instance,1,inplace.data());
    desc->run(instance,inplace.size());check(inplace==expected,"LV2 supports in-place audio");
    desc->run(instance,0);desc->cleanup(instance);
}
int main(int argc,char** argv){
    if(argc>1 && std::strcmp(argv[1],"--bench")==0){
        auto in=tone(48000,83,.4,10);std::vector<float> out(in.size());supr::ForgeDsp d;d.init(48000);
        auto start=std::chrono::steady_clock::now();
        for(size_t i=0;i<in.size();i+=64)d.process(in.data()+i,out.data()+i,std::min<size_t>(64,in.size()-i));
        double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        printf("Forge: 10 s audio in %.3f s, %.2f%% of one core; peak %.3f\n",sec,sec*10,peak(out));return 0;
    }
    for(float fs:{44100.f,48000.f,96000.f})testRate(fs);
    testHost();printf("%s (%d failures)\n",failures?"FAILED":"OK",failures);return failures?1:0;
}
