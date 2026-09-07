#include <random>
#define main original_test_main
#include "test_octaver.cpp"
#undef main
#include <lv2/core/lv2.h>
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
int main(){
 for(float fs:{44100.f,48000.f,96000.f}) {
  testClack(fs);
  auto noise=sine(fs,173,4,0);std::mt19937 rng(18);std::uniform_real_distribution<float> dist(-.001,.001);for(auto&x:noise)x=dist(rng);
  supr::ClackDsp d;d.init(fs);std::vector<float>y(noise.size());d.process(noise.data(),y.data(),fs);
  d.setLearn(true);d.process(noise.data(),y.data(),2.1*fs);
  check(d.learnState()==2 && d.learnedThreshold()>=-80 && d.learnedThreshold()<=-44,"Clack %.0f learns only quiet idle noise: state %d, %.1f dB",fs,d.learnState(),d.learnedThreshold());
  float saved=d.learnedThreshold();d.setLearn(false);
  auto note=steadyNote(fs,98,3,.15);d.process(note.data(),y.data(),fs);
  d.setLearn(true);d.process(note.data(),y.data(),2.1*fs);
  check(d.learnState()==3 && d.learnedThreshold()==saved,"Clack rejects sustained harmonics, preserves last recommendation");
  d.reset();std::fill(noise.begin(),noise.end(),0);d.setLearn(true);d.process(noise.data(),y.data(),noise.size());check(d.learnState()==3,"Clack silence cannot teach arbitrary threshold");
  auto desc=lv2_descriptor(0);auto h=desc->instantiate(desc,fs,"",nullptr);float p[19]={};p[2]=65;p[3]=80;p[4]=5;p[5]=2200;p[6]=-90;p[7]=0;p[8]=200;p[15]=1;
  for(int i=2;i<19;i++)desc->connect_port(h,i,&p[i]);desc->activate(h);
  auto input=steadyNote(fs,98,3,.15),out=input;
  for(size_t off=0;off<out.size();){unsigned n=std::min<size_t>(43,out.size()-off);desc->connect_port(h,0,out.data()+off);desc->connect_port(h,1,out.data()+off);desc->run(h,n);p[15]=0;off+=n;}
  check(p[17]==3 && p[18]>=0 && p[18]<=7 && p[9]==std::round(fs*.002f) && allFinite(out),"Clack wrapper in-place / learning / state / latency %.0f",fs);
  desc->cleanup(h);
 }
 return gFailures?1:0;
}
