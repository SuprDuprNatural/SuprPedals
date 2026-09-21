#include "VowelDsp.h"
#include <lv2/core/lv2.h>
#include <new>

namespace {
enum PortIndex : uint32_t {
    PORT_IN=0, PORT_OUT=1, PORT_VOWEL_A=2, PORT_VOWEL_B=3, PORT_MODE=4,
    PORT_POSITION=5, PORT_DEPTH=6, PORT_RATE=7, PORT_SENSITIVITY=8,
    PORT_RELEASE=9, PORT_THROAT=10, PORT_FOCUS=11, PORT_MIX=12, PORT_LEVEL=13,
    PORT_MORPH=14, PORT_F1=15, PORT_F2=16, PORT_F3=17
};
struct Effect {
    supr::VowelDsp dsp;
    const float* in=nullptr;
    float* out=nullptr;
    const float* controls[12]{};
    float* meters[4]{};
};
LV2_Handle instantiate(const LV2_Descriptor*,double rate,const char*,const LV2_Feature* const*) {
    auto* s=new(std::nothrow) Effect;
    if(s)s->dsp.init(rate);
    return s;
}
void connect(LV2_Handle h,uint32_t port,void* data) {
    auto* s=static_cast<Effect*>(h);
    if(port==PORT_IN)s->in=static_cast<const float*>(data);
    else if(port==PORT_OUT)s->out=static_cast<float*>(data);
    else if(port>=PORT_VOWEL_A&&port<=PORT_LEVEL)s->controls[port-PORT_VOWEL_A]=static_cast<const float*>(data);
    else if(port>=PORT_MORPH&&port<=PORT_F3)s->meters[port-PORT_MORPH]=static_cast<float*>(data);
}
void params(Effect* s) {
    supr::VowelDsp::Params p;
    float* fields[]={&p.vowel_a,&p.vowel_b,&p.mode,&p.position,&p.depth,&p.rate,
        &p.sensitivity,&p.release,&p.throat,&p.focus,&p.mix,&p.level};
    for(unsigned i=0;i<12;++i)if(s->controls[i])*fields[i]=*s->controls[i];
    s->dsp.setParams(p);
}
void activate(LV2_Handle h) { auto* s=static_cast<Effect*>(h);params(s);s->dsp.reset(); }
void run(LV2_Handle h,uint32_t n) {
    auto* s=static_cast<Effect*>(h);params(s);
    if(s->in&&s->out)s->dsp.process(s->in,s->out,n);
    if(s->meters[0])*s->meters[0]=s->dsp.position();
    for(unsigned j=0;j<3;++j)if(s->meters[j+1])*s->meters[j+1]=s->dsp.frequency(j);
}
void cleanup(LV2_Handle h) { delete static_cast<Effect*>(h); }
const LV2_Descriptor descriptor={"https://suprduprnatural.github.io/supr-pedals/vowel",instantiate,connect,activate,run,nullptr,cleanup,nullptr};
}
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index==0?&descriptor:nullptr; }
