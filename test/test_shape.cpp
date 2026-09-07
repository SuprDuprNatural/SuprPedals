#include "ShapeDsp.h"
#include <lv2/core/lv2.h>
#include <vector>
#include <cstdio>
#include <chrono>
#include <limits>
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
using D=supr::ShapeDsp;
int failures=0;
void check(bool b,const char* msg){if(!b){if(failures<20)std::printf("FAIL %s\n",msg);++failures;}}
std::vector<float> signal(double sr,double hz,double amp=.1,double seconds=1){std::vector<float>x(size_t(sr*seconds));for(size_t i=0;i<x.size();++i)x[i]=amp*std::sin(2*M_PI*hz*i/sr);return x;}
std::vector<float> render(double sr,D::Params p,const std::vector<float>& x,unsigned block=128){D d;d.init(sr);d.setParams(p);d.reset();std::vector<float>y(x.size());for(size_t i=0;i<x.size();i+=block)d.process(x.data()+i,y.data()+i,std::min(size_t(block),x.size()-i));return y;}
double rms(const std::vector<float>& x){double v=0;for(size_t i=x.size()/2;i<x.size();++i)v+=double(x[i])*x[i];return std::sqrt(v/(x.size()-x.size()/2));}
double gain(double sr,D::Params p,double hz){auto x=signal(sr,hz);return 20*std::log10(rms(render(sr,p,x))/rms(x));}
int main(){
 for(double sr:{44100.,48000.,96000.}){
  D::Params p;auto x=signal(sr,30.87);x[0]=32;check(render(sr,p,x)==x,"neutral bit identity, low B and headroom");
  D signedZero;signedZero.init(sr);float minusZero=-0.f,zeroOut=1;signedZero.process(&minusZero,&zeroOut,1);check(std::signbit(zeroOut),"neutral preserves signed zero");
  std::vector<float> dc(2048,.125f);check(render(sr,p,dc)==dc,"no unconditional DC blocker");
  std::vector<float> silence(2048);check(render(sr,p,silence)==silence,"exact silence");
  p.bass=6;double low=gain(sr,p,5),knee=gain(sr,p,90);check(std::abs(low-6)<.05&&std::abs(knee-3)<.04,"bass shelf calibrated gain/midpoint");
  p={};p.treble=-6;check(std::abs(gain(sr,p,3500)+3)<.04&&std::abs(gain(sr,p,sr*.45)+6)<.05,"treble shelf calibrated");
  p={};p.mid_gain=9;p.mid_freq=700;p.mid_q=2;check(std::abs(gain(sr,p,700)-9)<.02,"bell centre gain");
  // Definition of bell Q: bandwidth between half-gain points in the prewarped domain.
  double w=std::tan(M_PI*700/sr),r=(std::sqrt(4+1./4)+.5)/2;
  double hi=sr/M_PI*std::atan(w*r),lo=sr/M_PI*std::atan(w/r);
  check(std::abs(gain(sr,p,hi)-4.5)<.03&&std::abs(gain(sr,p,lo)-4.5)<.03,"bell Q half-gain bandwidth");
  p={};p.hpf=40;double corner=gain(sr,p,40),stop=gain(sr,p,10);
  check(std::abs(corner+3.0103)<.03&&stop<-23.9&&stop>-24.3,"HPF cutoff and 12dB/oct slope");
  p.hpf=15;double lowB=gain(sr,p,30.87);check(lowB>-.25,"cleanup preset low B under 0.25 dB loss");
  p={};p.lpf=3000;check(std::abs(gain(sr,p,3000)+3.0103)<.03,"LPF cutoff");
  p={};p.boom=12;p.threshold=-36;double boom=gain(sr,p,120),away=gain(sr,p,1000);
  check(boom<-10&&away>-.1,"boom cut and detector selectivity");
  D d;d.init(sr);d.setParams(p);d.reset();auto loud=signal(sr,120,.3);std::vector<float> y(loud.size());d.process(loud.data(),y.data(),loud.size());
  check(d.reductionDb(0)<-11&&d.reductionDb(1)==0,"actual selective reduction meters");
  auto quiet=signal(sr,120,.0001,3);y.resize(quiet.size());d.process(quiet.data(),y.data(),quiet.size());check(d.reductionDb(0)>-.05,"quiet note release");
  // Identical input detectors regardless of EQ gain, cut amount or output trim.
  D independent;independent.init(sr);auto boosted=p;boosted.bass=12;boosted.mid_gain=12;boosted.level=6;
  independent.setParams(boosted);independent.reset();d.reset();y.resize(loud.size());std::vector<float> other(loud.size());
  d.process(loud.data(),y.data(),loud.size());independent.process(loud.data(),other.data(),loud.size());
  check(d.reductionDb(0)==independent.reductionDb(0),"static EQ and output trim do not key detectors");
  p={};p.harsh=10;p.threshold=-36;check(gain(sr,p,2500)<-8&&gain(sr,p,120)>-.1,"upper-mid selective cut");
  p={};p.hpf=22;p.lpf=8000;p.bass=4;p.treble=-3;p.mid_gain=-5;p.mid_freq=550;p.mid_q=1.7;p.level=-2;p.boom=8;p.boom_freq=150;p.harsh=5;p.harsh_freq=3000;p.threshold=-32;p.release=170;
  x=signal(sr,120,.2);auto expected=render(sr,p,x);check(expected==render(sr,p,x,73),"exact block partition");
  const auto* desc=lv2_descriptor(0);check(desc&&!lv2_descriptor(1),"descriptor");auto h=desc->instantiate(desc,sr,nullptr,nullptr);check(h,"instantiate");
  float c[]={p.hpf,p.lpf,p.bass,p.treble,p.mid_freq,p.mid_gain,p.mid_q,p.level,p.boom,p.boom_freq,p.harsh,p.harsh_freq,p.threshold,p.release};
  for(unsigned i=0;i<14;++i)desc->connect_port(h,i+2,&c[i]);float meters[2];desc->connect_port(h,16,&meters[0]);desc->connect_port(h,17,&meters[1]);desc->activate(h);
  auto actual=x;for(size_t i=0;i<x.size();i+=73){desc->connect_port(h,0,actual.data()+i);desc->connect_port(h,1,actual.data()+i);desc->run(h,std::min(size_t(73),x.size()-i));}
  check(actual==expected,"LV2 all control mappings/in-place/default block cadence");check(meters[0]<=0&&meters[1]<=0,"LV2 meter contract");
  desc->run(h,0);desc->cleanup(h);h=desc->instantiate(desc,sr,nullptr,nullptr);desc->activate(h);desc->connect_port(h,0,x.data());actual.resize(x.size());desc->connect_port(h,1,actual.data());desc->run(h,x.size());check(actual==x,"disconnected controls use neutral defaults");desc->cleanup(h);
  d.init(sr);d.setParams(p);d.reset();float last=0,peak=0,maxStep=0;
  // Worst-range reversals on a bounded constant input isolate automation clicks.
  for(int i=0;i<int(sr);++i){if(i%97==0){p.hpf=p.hpf?0:100;p.lpf=p.lpf==1000?20000:1000;p.bass=-p.bass;p.treble=p.treble==12?-12:12;p.mid_freq=p.mid_freq==150?4000:150;p.mid_gain=-p.mid_gain;p.mid_q=p.mid_q==4?.3:4;p.boom=p.boom?0:12;p.harsh=p.harsh?0:12;d.setParams(p);}float input=.01f,output;d.process(&input,&output,1);check(std::isfinite(output),"automation finite");peak=std::max(peak,std::abs(output));maxStep=std::max(maxStep,std::abs(output-last));last=output;}
  check(peak<.2&&maxStep<.02,"automation bounded without large discontinuity");
  p.hpf=std::numeric_limits<float>::quiet_NaN();p.mid_gain=std::numeric_limits<float>::infinity();d.setParams(p);float bad[]={NAN,INFINITY,-INFINITY,0},out[4];d.process(bad,out,4);for(float v:out)check(std::isfinite(v),"nonfinite recovery");
  d.init(96000);d.setParams({});d.reset();float one=.2f,result;d.process(&one,&result,1);check(result==one,"sample-rate reinitialization resets");
  // Neutral impulse has no delay, ringing or phase rotation; HPF intentionally rotates phase.
  std::vector<float> impulse(8192);impulse[0]=1;check(render(sr,{},impulse)==impulse,"neutral impulse and zero latency");
  p={};p.hpf=40;auto imp=render(sr,p,impulse);double re=0,im=0;for(size_t i=0;i<imp.size();++i){double a=2*M_PI*40*i/sr;re+=imp[i]*std::cos(a);im-=imp[i]*std::sin(a);}double phase=std::atan2(im,re)*180/M_PI;check(std::abs(phase-90)<.1,"HPF corner phase");
  std::printf("Shape %.0f Hz: shelf %.3f/%.3f dB, HPF %.3f/%.3f dB, low B %.3f dB, boom %.3f/offband %.3f dB, automation peak/step %.5f/%.5f\n",sr,low,knee,corner,stop,lowB,boom,away,peak,maxStep);
 }
 D d;d.init(48000);D::Params p;p.boom=p.harsh=12;p.bass=6;p.mid_gain=-6;d.setParams(p);d.reset();auto x=signal(48000,120,.2,10);std::vector<float> y(x.size());auto start=std::chrono::steady_clock::now();for(size_t i=0;i<x.size();i+=64)d.process(x.data()+i,y.data()+i,std::min(size_t(64),x.size()-i));double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();std::printf("Shape CPU: 10s/48k/64 frames in %.6fs (%.3f%% one local core), checksum %.7f\n",seconds,seconds*10,rms(y));
 std::printf("Shape: %d failures\n",failures);return failures?1:0;
}
