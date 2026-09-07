#include "SpaceDsp.h"
#include <lv2/core/lv2.h>
#include <cstdio>
#include <vector>
#include <limits>
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
int failures=0;
void check(bool ok,const char* msg) {if(!ok){if(failures<20)std::printf("FAIL %s\n",msg);++failures;}}
double energy(const std::vector<float>& v,size_t a,size_t b){double e=0;for(size_t i=a;i<std::min(b,v.size());++i)e+=double(v[i])*v[i];return e;}
std::vector<float> render(double sr,supr::SpaceDsp::Params p,const std::vector<float>& in,unsigned block=128){
 supr::SpaceDsp d;d.init(sr);d.setParams(p);d.reset();std::vector<float> out(in.size());
 for(size_t i=0;i<in.size();i+=block)d.process(in.data()+i,out.data()+i,std::min(size_t(block),in.size()-i));
 return out;
}
int main(){
 for(double sr:{44100.,48000.,96000.}){
    { supr::SpaceDsp d;d.init(sr);supr::SpaceDsp::Params p;p.mix=1;p.send=0;d.setParams(p);d.reset();float x=32,y=0;d.process(&x,&y,1);check(y==x,"finite dry is transparent above nominal audio range"); }
  supr::SpaceDsp::Params p;p.mix=1;p.duck=0;p.decay=1.4;
  std::vector<float> in(size_t(sr*6));in[0]=.25;auto y=render(sr,p,in);
  check(y[0]==in[0]&&energy(y,1,size_t(sr*.044))==0,"transparent dry and predelay onset");
  check(energy(y,size_t(sr*.044),size_t(sr*.1))>1e-5,"room onset present");
  check(render(sr,p,in,73)==y,"block partition exact");
  double early=energy(y,size_t(sr*.1),size_t(sr*.4)), late=energy(y,size_t(sr*1.5),size_t(sr*1.8));
  double db=10*std::log10(late/early);check(db<-45&&db>-100,"impulse decay consistent with RT60 and damping");
  check(energy(y,size_t(sr*5),y.size())<1e-15,"impulse tail dies away");
  auto p0=p;p0.mix=0;check(render(sr,p0,in)==in,"exact dry");
  p0=p;p0.decay=.65;auto room=render(sr,p0,in);p0.decay=2.8;auto plate=render(sr,p0,in);
  check(energy(plate,size_t(sr),size_t(sr*2))>100*energy(room,size_t(sr),size_t(sr*2)),"room/plate decay differs meaningfully");
  // LV2 contract, in-place processing and disconnected/default controls.
  const auto* desc=lv2_descriptor(0);check(desc&&!lv2_descriptor(1),"descriptor");auto h=desc->instantiate(desc,sr,nullptr,nullptr);
  float c[]={1,1.4,4500,0,15,180,350,1};for(int i=0;i<8;++i)desc->connect_port(h,i+2,&c[i]);
  float meter=0;desc->connect_port(h,10,&meter);desc->activate(h);auto actual=in;
  for(size_t i=0;i<actual.size();i+=127){desc->connect_port(h,0,actual.data()+i);desc->connect_port(h,1,actual.data()+i);desc->run(h,std::min(size_t(127),actual.size()-i));}
  check(y==actual,"wrapper in-place equals DSP");desc->run(h,0);check(std::isfinite(meter),"zero run");desc->cleanup(h);
  supr::SpaceDsp d;d.init(sr);p.decay=8;d.setParams(p);d.reset();
  float x=0,o=0;double peak=0,mean=0,tail=0;
  for(size_t i=0;i<size_t(sr*20);++i){
   x=i<size_t(sr*10)?float(.3*std::sin(2*supr::timespace::pi*30.87*i/sr)):0;
   if(i==size_t(sr*10)){p.send=0;d.setParams(p);}
   d.process(&x,&o,1);check(std::isfinite(o)&&std::abs(o)<1,"sustained low B max decay bounded");peak=std::max(peak,double(std::abs(o)));
   if(i>size_t(sr*19))mean+=o;
   if(i>size_t(sr*10)&&i<size_t(sr*11))tail+=double(o)*o;
  }
  check(std::abs(mean/sr)<1e-5&&tail>1e-8,"trails and no accumulating DC");
  // Detector is outside the FDN and recovers after a clean-input attack.
  supr::SpaceDsp a,b;a.init(sr);b.init(sr);p={};p.mix=1;p.duck=0;a.setParams(p);a.reset();p.duck=1;b.setParams(p);b.reset();
  float oa=0,ob=0;x=.2;
  for(int i=0;i<int(sr*.1);++i){a.process(&x,&oa,1);b.process(&x,&ob,1);}check(b.duckGain()<.06,"duck attack");x=0;
  for(int i=0;i<int(sr);++i){a.process(&x,&oa,1);b.process(&x,&ob,1);check(std::abs(ob-oa*b.duckGain())<1e-6,"duck leaves FDN unchanged");}
  for(int i=0;i<int(sr*4);++i) b.process(&x,&ob,1);
  check(b.duckGain()>.999,"duck recovery");
  std::vector<float> buf(113,.1),out(113);for(int i=0;i<2000;++i){p.decay=i%2?8:.2;p.predelay=i%151;p.tone=i%2?800:12000;d.setParams(p);d.process(buf.data(),out.data(),113);for(float v:out)check(std::isfinite(v)&&std::abs(v)<3,"rapid maximum control transitions");}
  for(float bad:{0.f,-1e30f,1e30f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
   p={bad,bad,bad,bad,bad,bad,bad,bad};d.setParams(p);buf.assign(113,bad);d.process(buf.data(),out.data(),113);for(float v:out)check(std::isfinite(v),"nonfinite controls and audio");
  }
  std::printf("Space %.0f Hz: 1.4 s decay-window drop %.2f dB; low-B peak %.3f; memory %zu bytes\n",sr,db,peak,d.memoryBytes());
  d.init(48000);d.process(buf.data(),out.data(),0);
 }
 std::printf("Space: %d failures\n",failures);return failures?1:0;
}
