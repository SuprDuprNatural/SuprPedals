#include "PhaseDsp.h"
#include <lv2/core/lv2.h>
#include <new>
namespace {
enum PortIndex : uint32_t {
    PORT_IN=0, PORT_OUT=1, PORT_RATE=2, PORT_DEPTH=3, PORT_CENTRE=4,
    PORT_FEEDBACK=5, PORT_MIX=6, PORT_PROTECT=7, PORT_MODE=8, PORT_SENSITIVITY=9
};
struct Effect {
    supr::PhaseDsp dsp;
    const float* in=nullptr;
    float* out=nullptr;
    const float* controls[8]{};
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
    else if(port>=PORT_RATE&&port<=PORT_SENSITIVITY)s->controls[port-PORT_RATE]=static_cast<const float*>(data);
}
void params(Effect* s) {
    supr::PhaseDsp::Params p;
    float* fields[]={&p.rate,&p.depth,&p.centre,&p.feedback,&p.mix,&p.protect,&p.mode,&p.sensitivity};
    for(unsigned i=0;i<8;++i)if(s->controls[i])*fields[i]=*s->controls[i];
    s->dsp.setParams(p);
}
void activate(LV2_Handle h) { auto* s=static_cast<Effect*>(h);params(s);s->dsp.reset(); }
void run(LV2_Handle h,uint32_t n) {
    auto* s=static_cast<Effect*>(h);params(s);
    if(s->in&&s->out)s->dsp.process(s->in,s->out,n);
}
void cleanup(LV2_Handle h) { delete static_cast<Effect*>(h); }
const LV2_Descriptor descriptor={"https://suprduprnatural.github.io/supr-pedals/phase",instantiate,connect,activate,run,nullptr,cleanup,nullptr};
}
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index==0?&descriptor:nullptr; }
