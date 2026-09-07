// Reuse the established response, bass-intermodulation and compressor probes.
#define main original_test_main
#include "test_octaver.cpp"
#undef main
#include <lv2/core/lv2.h>
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
int main() {
 for(float fs : {44100.f,48000.f,96000.f}) {
  testBand(fs);
  for(auto split : {std::pair<float,float>{80,2500},{150,1200},{400,500},{500,400}}) {
   double worst=0, identity=0;
   for(float f : {31.f,60.f,134.33f,150.f,400.f,500.f,1200.f,2500.f,5000.f,10000.f}) {
    BandParams p; p.split1=split.first;p.split2=split.second;
    double ref=bandRespDb(p,fs,f);
    for(float blend : {0.f,.25f,.5f,.75f,1.f}) {
     p.blend=blend; double db=bandRespDb(p,fs,f);
     worst=std::max(worst,std::abs(db));identity=std::max(identity,std::abs(db-ref));
    }
   }
   check(worst<0.16 && identity<1e-5,"Band %.0f Hz splits %.0f/%.0f: all blends worst %.4f dB, spread %.2g",fs,split.first,split.second,worst,identity);
  }
  // Moving crossovers use the same filter state for clean/wet references.
  auto in=pluck(fs,41.2f,1.f,.3f);
  supr::BandDsp a,b; a.init(fs);b.init(fs);a.setBlend(0);b.setBlend(.5f);
  std::vector<float> x(in.size()),y(in.size());
  for(size_t off=0;off<in.size();) {
   unsigned n=std::min<size_t>(37,in.size()-off);
   float f=(off/1000)%2?500:40;
   a.setSplit1(f);b.setSplit1(f);a.setSplit2(540-f);b.setSplit2(540-f);
   a.process(in.data()+off,x.data()+off,n);b.process(in.data()+off,y.data()+off,n);off+=n;
  }
  check(x==y,"Band moving crossover neutral blend identity at %.0f",fs);
  // Exercise actual wrapper mappings and in-place execution, partitioned at
  // irregular boundaries; controls change at the same absolute sample.
  auto d=lv2_descriptor(0); auto h=d->instantiate(d,fs,"",nullptr);
  std::vector<float> out=in, want(in.size());float ports[22]={};
  ports[2]=150;ports[3]=1200;ports[4]=2;ports[7]=7;ports[10]=9;
  ports[5]=-25;ports[8]=-30;ports[11]=-15;ports[6]=-2;ports[9]=1;ports[12]=-4;ports[13]=.5f;ports[14]=-3;
  for(int p=2;p<22;p++)d->connect_port(h,p,&ports[p]);d->activate(h);
  supr::BandDsp ref;ref.init(fs);ref.setBlend(.5f);ref.setOutput(-3);
  for(int b=0;b<3;b++){ref.setDrive(b,ports[4+b*3]);ref.setComp(b,ports[5+b*3]);ref.setLevel(b,ports[6+b*3]);}
  for(size_t off=0;off<in.size();) {
   size_t end=std::min(in.size(),off+1000);float f=off?400:150;ports[2]=f;ref.setSplit1(f);
   ref.process(in.data()+off,want.data()+off,end-off);
   while(off<end){unsigned n=std::min<size_t>(end-off,off%73+1);d->connect_port(h,0,out.data()+off);d->connect_port(h,1,out.data()+off);d->run(h,n);off+=n;}
  }
  double err=0;for(size_t i=0;i<out.size();i++)err=std::max(err,double(std::abs(out[i]-want[i])));
  check(err<1e-6 && ports[18]==15,"Band wrapper/in-place/partition/automation %.0f: %.2g",fs,err);
  d->deactivate(h);d->cleanup(h);
 }
 return gFailures?1:0;
}
