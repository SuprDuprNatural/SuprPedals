#define main original_test_main
#include "test_octaver.cpp"
#undef main
#include <lv2/core/lv2.h>
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
int main(){
 for(float fs:{44100.f,48000.f,96000.f}) {
  supr::FuzzDsp d;d.init(fs);auto x=sine(fs,98,4,.1f);std::vector<float>y(x.size());
  d.process(x.data(),y.data(),fs);d.setLearn(true);d.process(x.data(),y.data(),2.1*fs);
  check(d.learnState()==2 && std::abs(d.learnedGainDb())<=18,"Fuzz %.0f learn phrase: %.2f dB",fs,d.learnedGainDb());
  float learned=d.learnedGainDb();d.setHeldGainDb(learned);d.setMatchMode(1);d.process(x.data(),y.data(),fs);
  float held=d.matchGainDb();for(auto&v:x)v*=.05f;d.process(x.data(),y.data(),x.size());
  check(d.matchGainDb()==held && std::abs(held-learned)<1e-4,"Fuzz held gain does not pump on quiet passage");
  d.setLearn(false);d.setLearn(true);std::fill(x.begin(),x.end(),0);d.process(x.data(),y.data(),x.size());
  check(d.learnState()==3 && d.learnedGainDb()==learned,"Fuzz silence rejects and preserves previous result");
  d.setLearn(false);d.setLearn(true);x[42]=std::numeric_limits<float>::quiet_NaN();d.process(x.data(),y.data(),x.size());
  check(d.learnState()==3 && allFinite(y),"Fuzz invalid input rejects without poisoning output");
  auto desc=lv2_descriptor(0);auto h=desc->instantiate(desc,fs,"",nullptr);float p[14]={};p[2]=7;p[3]=3;p[4]=-70;p[5]=.6;p[6]=.8;p[9]=1;p[10]=learned;
  for(int i=2;i<14;i++)desc->connect_port(h,i,&p[i]);desc->activate(h);
  auto input=sine(fs,98,1,.1f),out=input;std::vector<float>want(input.size());
  supr::FuzzDsp ref;ref.init(fs);ref.setSustain(7);ref.setTone(3);ref.setGate(-70);ref.setBlend(.6);ref.setLevel(.8);ref.setMatchMode(1);ref.setHeldGainDb(learned);ref.process(input.data(),want.data(),input.size());
  for(size_t off=0;off<out.size();){unsigned n=std::min<size_t>(off%67+1,out.size()-off);desc->connect_port(h,0,out.data()+off);desc->connect_port(h,1,out.data()+off);desc->run(h,n);off+=n;}
  check(out==want && p[8]==15,"Fuzz wrapper: restored held controls, exact in-place/partition, latency %.0f",fs);desc->cleanup(h);
 }
 return gFailures?1:0;
}
