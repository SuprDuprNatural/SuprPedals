#define main original_test_main
#include "test_octaver.cpp"
#undef main
#include <lv2/core/lv2.h>
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
int main(){
 for(float fs:{44100.f,48000.f,96000.f}) {
  for(float f:{15.5f,31.f,41.f,200.f}) {
   auto x=sine(fs,f,2,.1f);std::vector<float>a(x.size()),b(x.size()),c(x.size());
   supr::OctaverDsp oct;oct.init(fs);oct.setDirect(1);oct.setOct1(0);oct.process(x.data(),a.data(),a.size());
   supr::EnvFilterDsp env;env.init(fs);env.setBlend(0);env.process(a.data(),b.data(),b.size());
   supr::CompressorDsp comp;comp.init(fs);comp.setBlend(0);comp.process(b.data(),c.data(),c.size());
   check(x==a && x==b && x==c,"clean bass %.0f/%.1f Hz: exact dry chain",fs,f);
  }
  auto d=lv2_descriptor(0);auto h=d->instantiate(d,fs,"",nullptr);float p[10]={};
  p[2]=-32;p[3]=6;p[4]=9;p[5]=75;p[6]=90;p[7]=3;p[8]=.7;
  for(int i=2;i<10;i++) d->connect_port(h,i,&p[i]);
  d->activate(h);
  auto x=pluck(fs,31,1,.3f),y=x;std::vector<float>want(x.size());
  supr::CompressorDsp ref;ref.init(fs);ref.setThreshold(p[2]);ref.setRatio(p[3]);ref.setAttack(p[4]);ref.setRelease(p[5]);ref.setScHpf(p[6]);ref.setMakeup(p[7]);ref.setBlend(p[8]);
  ref.process(x.data(),want.data(),x.size());
  for(size_t off=0;off<x.size();){unsigned n=std::min<size_t>(off%71+1,x.size()-off);d->connect_port(h,0,y.data()+off);d->connect_port(h,1,y.data()+off);d->run(h,n);off+=n;}
  double err=0;for(size_t i=0;i<y.size();i++)err=std::max(err,double(std::abs(y[i]-want[i])));
  check(err<1e-6,"legacy compressor LV2 all controls, in-place, partition %.0f: %.2g",fs,err);d->cleanup(h);
 }
 return gFailures?1:0;
}
