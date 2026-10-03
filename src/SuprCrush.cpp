#include "CrushDsp.h"
#include <lv2/core/lv2.h>
#include <new>
namespace {
enum PortIndex : uint32_t {
    PORT_IN=0, PORT_OUT=1, PORT_BITS=2, PORT_RATE=3, PORT_DRIVE=4,
    PORT_ENV=5, PORT_SENSITIVITY=6, PORT_RELEASE=7, PORT_TONE=8,
    PORT_PROTECT=9, PORT_DRY=10, PORT_WET=11
};
struct Effect {
    supr::CrushDsp dsp;
    const float* in=nullptr;
    float* out=nullptr;
    const float* controls[10]{};
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
    else if(port>=PORT_BITS&&port<=PORT_WET)s->controls[port-PORT_BITS]=static_cast<const float*>(data);
}
void params(Effect* s) {
    supr::CrushDsp::Params p;
    float* fields[]={&p.bits,&p.rate,&p.drive,&p.env,&p.sensitivity,&p.release,&p.tone,&p.protect,&p.dry,&p.wet};
    for(unsigned i=0;i<10;++i)if(s->controls[i])*fields[i]=*s->controls[i];
    s->dsp.setParams(p);
}
void activate(LV2_Handle h) { auto* s=static_cast<Effect*>(h);params(s);s->dsp.reset(); }
void run(LV2_Handle h,uint32_t n) {
    auto* s=static_cast<Effect*>(h);params(s);
    if(s->in&&s->out)s->dsp.process(s->in,s->out,n);
}
void cleanup(LV2_Handle h) { delete static_cast<Effect*>(h); }
const LV2_Descriptor descriptor={"https://suprduprnatural.github.io/supr-pedals/crush",instantiate,connect,activate,run,nullptr,cleanup,nullptr};
}
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index==0?&descriptor:nullptr; }
