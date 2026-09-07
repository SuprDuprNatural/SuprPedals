#define main original_test_main
#include "test_octaver.cpp"
#undef main
#include <lv2/core/lv2.h>
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
static std::vector<float> heldRender(float fs,int mode,float threshold,float release=20) {
 auto x=sine(fs,31,4,.31622777f);std::vector<float> y(x.size());
 supr::CompressorDsp d;d.init(fs);d.setThreshold(threshold);d.setRatio(4);d.setAttack(3);d.setRelease(release);d.setScHpf(20);d.setDetector(mode);d.process(x.data(),y.data(),y.size());return y;
}
static double thd31(const std::vector<float>& y,float fs) {double h=goertzel(y,2*fs,4*fs,fs,31),sum=0;for(int i=2;i<=10;i++){double a=goertzel(y,2*fs,4*fs,fs,31*i);sum+=a*a;}return 100*sqrt(sum)/h;}
int main(){
 for(float fs:{44100.f,48000.f,96000.f}) {
  for(float f:{15.5f,31.f,41.f,200.f}) {
   auto x=sine(fs,f,2,.1f);std::vector<float>a(x.size()),b(x.size()),c(x.size());
   supr::OctaverDsp oct;oct.init(fs);oct.setDirect(1);oct.setOct1(0);oct.process(x.data(),a.data(),a.size());
   supr::EnvFilterDsp env;env.init(fs);env.setBlend(0);env.process(a.data(),b.data(),b.size());
   supr::CompressorDsp comp;comp.init(fs);comp.setBlend(0);comp.process(b.data(),c.data(),c.size());
   check(x==a && x==b && x==c,"clean bass %.0f/%.1f Hz: exact dry chain",fs,f);
  }
  auto peak=heldRender(fs,0,-30);double target=rms(peak,2*fs,4*fs);
  float lo=-40,hi=-20;
  for(int i=0;i<15;i++){float mid=(lo+hi)*.5f;auto y=heldRender(fs,1,mid);if(rms(y,2*fs,4*fs)<target)lo=mid;else hi=mid;}
  auto held=heldRender(fs,1,(lo+hi)*.5f);double db=20*log10(rms(held,2*fs,4*fs)/target);
  check(std::abs(db)<.01 && thd31(held,fs)<.3,"compressor %.0f matched reduction %.3f dB: Peak %.3f%% THD, Held %.3f%% (threshold %.3f)",fs,db,thd31(peak,fs),thd31(held,fs),(lo+hi)*.5f);
  // Identical step envelopes establish attack and the deliberate extra hold.
  for(int mode:{0,1}) {
   supr::CompressorDsp d;d.init(fs);d.setThreshold(-30);d.setRatio(4);d.setScHpf(20);d.setDetector(mode);d.setRelease(200);
   auto x=sine(fs,1000,2,.3f);for(size_t i=fs;i<x.size();i++)x[i]=0;
   std::vector<float>y(x.size());d.process(x.data(),y.data(),fs);
   check(d.grDb() < -10,"detector %d reaches useful reduction",mode);
   d.process(x.data()+size_t(fs),y.data()+size_t(fs),fs);
   check(d.grDb()>-1 && peakAbs(y,fs,y.size())==0,"detector %d quiet release and no lookahead tail",mode);
  }
  auto d=lv2_descriptor(0);auto h=d->instantiate(d,fs,"",nullptr);float p[11]={};
  p[2]=-32;p[3]=6;p[4]=9;p[5]=75;p[6]=90;p[7]=3;p[8]=.7;p[10]=1;
  for(int i=2;i<11;i++)d->connect_port(h,i,&p[i]);d->activate(h);
  auto x=pluck(fs,31,1,.3f),y=x;std::vector<float>want(x.size());
  supr::CompressorDsp ref;ref.init(fs);ref.setThreshold(p[2]);ref.setRatio(p[3]);ref.setAttack(p[4]);ref.setRelease(p[5]);ref.setScHpf(p[6]);ref.setMakeup(p[7]);ref.setBlend(p[8]);ref.setDetector(1);
  ref.process(x.data(),want.data(),x.size());
  for(size_t off=0;off<x.size();){unsigned n=std::min<size_t>(off%71+1,x.size()-off);d->connect_port(h,0,y.data()+off);d->connect_port(h,1,y.data()+off);d->run(h,n);off+=n;}
  double err=0;for(size_t i=0;i<y.size();i++)err=std::max(err,double(std::abs(y[i]-want[i])));
  check(err<1e-6,"held detector LV2 all controls, in-place, partition %.0f: %.2g",fs,err);d->cleanup(h);
 }
 return gFailures?1:0;
}
