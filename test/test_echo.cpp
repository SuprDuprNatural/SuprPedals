#include "EchoDsp.h"
#include <lv2/core/lv2.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <limits>
#include <cstring>

static float percentToDb(double percent) { return percent>0?float(20*std::log10(percent/50)):-60.f; }
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
static int failures=0;
void check(bool ok,const char* what) { if(!ok) { ++failures; std::printf("FAIL %s\n",what); } }
double energy(const std::vector<float>& v,size_t a,size_t b) { double s=0; for(size_t i=a;i<std::min(b,v.size());++i) s+=double(v[i])*v[i]; return s; }
std::vector<float> render(double sr,supr::EchoDsp::Params p,const std::vector<float>& in,unsigned block=128) {
    supr::EchoDsp d; d.init(sr); d.setParams(p); d.reset(); std::vector<float> out(in.size());
    for(size_t i=0;i<in.size();i+=block) d.process(in.data()+i,out.data()+i,std::min(size_t(block),in.size()-i));
    return out;
}
int main() {
 for(double sr:{44100.,48000.,96000.}) {
    { supr::EchoDsp d;d.init(sr);supr::EchoDsp::Params p;p.dry=percentToDb(100);p.wet=percentToDb(0);p.send=0;d.setParams(p);d.reset();float x=32,y=0;d.process(&x,&y,1);check(y==x,"finite dry is transparent above nominal audio range"); }
    supr::EchoDsp::Params p; p.time=100; p.duck=0; p.dry=percentToDb(0);p.wet=percentToDb(100); p.feedback=.6;
    std::vector<float> in(size_t(sr*3)); in[0]=.25;
    auto y=render(sr,p,in); size_t t=size_t(sr*.1);
    check(energy(y,0,t)==0 && std::abs(y[t])>.2,"full wet removes dry and preserves first impulse timing");
    auto z=render(sr,p,in,1); check(y==z,"block partition exact");
    double e1=energy(y,t,t*2), e2=energy(y,t*2,t*3);
    double lp=0, dc=0, expectedEnergy=0, maxError=0;
    const double al=std::exp(-2*supr::timespace::pi*3500/sr), ad=std::exp(-2*supr::timespace::pi*8/sr);
    for(size_t i=0;i<t;++i) {
        lp=(1-al)*y[t+i]+al*lp; dc=(1-ad)*lp+ad*dc;
        const double expected=.6*(lp-dc); expectedEnergy+=expected*expected;
        maxError=std::max(maxError,std::abs(y[2*t+i]-expected));
    }
    check(maxError<1e-7 && std::abs(e2-expectedEnergy)<1e-8 && e2/e1<.36,"repeat matches independent filter/feedback recurrence");
    auto p0=p; p0.feedback=0; auto first=render(sr,p0,in);
    check(energy(first,2*t,3*t)<1e-12,"zero feedback has no second repeat");
    p0=p; p0.dry=percentToDb(100);p0.wet=percentToDb(0); for(size_t i=0;i<in.size();++i) in[i]=float(.1*std::sin(i*.23));
    check(render(sr,p0,in)==in,"exact dry mix zero");
    for(double hz:{31.,1000.}) {
        for(size_t i=0;i<in.size();++i) in[i]=float(.1*std::sin(2*supr::timespace::pi*hz*i/sr));
        p0=p; p0.feedback=0; auto a=render(sr,p0,in);
        double ratio=energy(a,size_t(sr),a.size())/energy(in,size_t(sr),in.size());
        check(hz==31 ? ratio<.003 : ratio>.8,"wet send rejects low B and passes mids");
        std::printf("Echo %.0f Hz lowcut test %.0f Hz: %.2f dB\n",sr,hz,10*std::log10(ratio));
    }
    // Return detector must recover without changing the stored loop.
    supr::EchoDsp a,b; a.init(sr); b.init(sr); a.setParams(p); a.reset(); p0=p; p0.duck=1; b.setParams(p0); b.reset();
    float x=.2,oa=0,ob=0; for(int i=0;i<int(sr*.1);++i) {a.process(&x,&oa,1);b.process(&x,&ob,1);}
    check(b.duckGain()<.06,"duck attack");
    x=0; for(int i=0;i<int(sr*.1);++i) {a.process(&x,&oa,1);b.process(&x,&ob,1);check(std::abs(ob-oa*b.duckGain())<1e-6,"duck outside feedback");}
    for(int i=0;i<int(sr*4);++i) b.process(&x,&ob,1);
    check(b.duckGain()>.999,"duck recovery");
    // Actual LV2 wiring, all control pointers, in-place and chunk independence.
    const auto* desc=lv2_descriptor(0); check(desc && !lv2_descriptor(1),"descriptor");
    float c[]={100,.6,-60,0,3500,150,300,0,1,0};
    auto h=desc->instantiate(desc,sr,nullptr,nullptr); check(h!=nullptr,"instantiate");
    for(int i=0;i<10;++i) desc->connect_port(h,2+i,&c[i]);
    float wet=percentToDb(100); desc->connect_port(h,13,&wet);
    float meter=0; desc->connect_port(h,12,&meter); desc->activate(h);
    in.assign(size_t(sr),0);in[0]=.25; auto ref=render(sr,p,in); auto actual=in;
    for(size_t i=0;i<in.size();i+=73) {desc->connect_port(h,0,actual.data()+i);desc->connect_port(h,1,actual.data()+i);desc->run(h,std::min(size_t(73),in.size()-i));}
    check(ref==actual,"wrapper in-place matches DSP"); desc->run(h,0); check(std::isfinite(meter),"zero run meter"); desc->cleanup(h);
    // Tails, bounded hold (including stuck switch timeout), rapid time and bad controls.
    supr::EchoDsp d; d.init(sr); p.feedback=.92; d.setParams(p); d.reset();
    std::vector<float> block(127,.2f),out(127); double tail=0,peak=0;
    for(int k=0;k<int(sr*15/127);++k) {
        if(k==int(sr/127)) {p.send=0;block.assign(127,0);}
        if(k==int(sr*2/127)) p.hold=1;
        if(k>int(sr*13/127)) p.hold=0;
        if(k%11==0) p.time=20+(k%1980);
        d.setParams(p);d.process(block.data(),out.data(),127);
        for(float v:out) { check(std::isfinite(v)&&std::abs(v)<9,"long-run hold/time stability");peak=std::max(peak,double(std::abs(v))); }
        if(k>int(sr/127)&&k<int(sr*1.2/127)) tail+=energy(out,0,127);
    }
    check(tail>1e-8,"send off retains tails");
    for(float bad:{0.f,-1e30f,1e30f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        p={bad,bad,bad,bad,bad,bad,bad,bad,bad,bad};d.setParams(p);block.assign(127,bad);d.process(block.data(),out.data(),127);
        for(float v:out) check(std::isfinite(v),"nonfinite controls and audio");
    }
    d.init(sr==96000?44100:96000);d.process(block.data(),out.data(),0);
    std::printf("Echo %.0f Hz: repeat energy ratio %.5f, stress peak %.3f, memory %zu bytes\n",sr,e2/e1,peak,d.memoryBytes());
 }
 std::printf("Echo: %d failures\n",failures); return failures?1:0;
}
