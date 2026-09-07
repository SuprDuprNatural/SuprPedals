// New Space LV2 contract; indices must agree with ttl/suprspace.ttl.
#include "SpaceDsp.h"
#include <lv2/core/lv2.h>
#include <new>
namespace {
enum PortIndex : uint32_t {
    PORT_IN = 0,
    PORT_OUT = 1,
    PORT_MIX = 2,
    PORT_DECAY = 3,
    PORT_TONE = 4,
    PORT_DUCK = 5,
    PORT_PREDELAY = 6,
    PORT_LOWCUT = 7,
    PORT_RECOVERY = 8,
    PORT_SEND = 9,
    PORT_DUCK_GR = 10
};
struct Effect {
    supr::SpaceDsp dsp;
    const float* in=nullptr;
    float* out=nullptr;
    const float* controls[8]={};
    float* reduction=nullptr;
};
LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*) {
    // Buffer allocation is confined to instantiate; report allocation failure to host.
    try { auto* s=new Effect; try { s->dsp.init(rate); } catch(...) { delete s; throw; } return s; }
    catch(...) { return nullptr; }
}
void connect(LV2_Handle h,uint32_t port,void* data) {
    auto* s=static_cast<Effect*>(h);
    if(port==PORT_IN) s->in=static_cast<const float*>(data);
    else if(port==PORT_OUT) s->out=static_cast<float*>(data);
    else if(port>=2 && port<10) s->controls[port-2]=static_cast<const float*>(data);
    else if(port==PORT_DUCK_GR) s->reduction=static_cast<float*>(data);
}
void params(Effect* s) {
    supr::SpaceDsp::Params p;
    float* v[]={&p.mix,&p.decay,&p.tone,&p.duck,&p.predelay,&p.lowcut,&p.recovery,&p.send};
    for(int i=0;i<8;++i) if(s->controls[i]) *v[i]=*s->controls[i];
    s->dsp.setParams(p);
}
void activate(LV2_Handle h) { auto* s=static_cast<Effect*>(h); params(s); s->dsp.reset(); }
void run(LV2_Handle h,uint32_t n) {
    auto* s=static_cast<Effect*>(h); params(s);
    if(s->in && s->out) s->dsp.process(s->in,s->out,n);
    if(s->reduction) *s->reduction=float(20*std::log10(std::max(1e-6,s->dsp.duckGain())));
}
void cleanup(LV2_Handle h) { delete static_cast<Effect*>(h); }
const LV2_Descriptor descriptor={"https://suprduprnatural.github.io/supr-pedals/space",instantiate,connect,activate,run,nullptr,cleanup,nullptr};
}
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index==0?&descriptor:nullptr; }
