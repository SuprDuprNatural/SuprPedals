#include <random>
#define main original_test_main
#include "test_octaver.cpp"
#undef main
#include <lv2/core/lv2.h>
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
int main(){
 for(float fs:{44100.f,48000.f,96000.f}) {
  testClack(fs);
  auto desc=lv2_descriptor(0);auto h=desc->instantiate(desc,fs,"",nullptr);float p[15]={};p[2]=65;p[3]=80;p[4]=5;p[5]=2200;p[6]=-90;p[7]=0;p[8]=200;
  for(int i=2;i<15;i++) desc->connect_port(h,i,&p[i]);
  desc->activate(h);
  auto input=steadyNote(fs,98,3,.15),out=input;
  for(size_t off=0;off<out.size();){unsigned n=std::min<size_t>(43,out.size()-off);desc->connect_port(h,0,out.data()+off);desc->connect_port(h,1,out.data()+off);desc->run(h,n);off+=n;}
  check(p[9]==std::round(fs*.002f) && allFinite(out),"Clack wrapper in-place / latency %.0f",fs);
  desc->cleanup(h);
 }
 return gFailures?1:0;
}
