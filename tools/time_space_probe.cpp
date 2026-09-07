// Build with -DECHO for Echo; otherwise Space. Offline CPU and sustained-signal probe.
#ifdef ECHO
#include "EchoDsp.h"
using Effect=supr::EchoDsp;
#else
#include "SpaceDsp.h"
using Effect=supr::SpaceDsp;
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
int main(int argc,char** argv) {
 double sr=argc>1?std::atof(argv[1]):48000; Effect d;d.init(sr);Effect::Params p;d.setParams(p);d.reset();
 float in[64],out[64];double peak=0;auto start=std::chrono::steady_clock::now();
 for(size_t n=0;n<size_t(sr*10);n+=64){for(int i=0;i<64;++i)in[i]=float(.2*std::sin(2*supr::timespace::pi*110*(n+i)/sr));d.process(in,out,64);for(float y:out)peak=std::max(peak,double(std::abs(y)));}
 double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
 std::printf("%.0f Hz, 64 frames: 10 s audio in %.6f s, %.3f%% one core, %zu bytes, peak %.5f; dry latency 0 samples\n",sr,sec,sec*10,d.memoryBytes(),peak);
}
