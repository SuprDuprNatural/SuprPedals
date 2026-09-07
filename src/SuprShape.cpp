#include "ShapeDsp.h"
#include <lv2/core/lv2.h>
#include <new>
namespace {
enum PortIndex : uint32_t {
    PORT_IN = 0,
    PORT_OUT = 1,
    PORT_HPF = 2,
    PORT_LPF = 3,
    PORT_BASS = 4,
    PORT_TREBLE = 5,
    PORT_MID_FREQ = 6,
    PORT_MID_GAIN = 7,
    PORT_MID_Q = 8,
    PORT_LEVEL = 9,
    PORT_BOOM = 10,
    PORT_BOOM_FREQ = 11,
    PORT_HARSH = 12,
    PORT_HARSH_FREQ = 13,
    PORT_THRESHOLD = 14,
    PORT_RELEASE = 15,
    PORT_BOOM_GR = 16,
    PORT_HARSH_GR = 17
};
struct Effect { supr::ShapeDsp dsp; const float* in=nullptr; float* out=nullptr;
    const float* controls[14]={}; float* meters[2]={}; };
LV2_Handle instantiate(const LV2_Descriptor*,double rate,const char*,const LV2_Feature* const*) {
    auto* s=new(std::nothrow) Effect; if(s)s->dsp.init(rate); return s;
}
void connect(LV2_Handle h,uint32_t port,void* data) {
    auto* s=static_cast<Effect*>(h);
    if(port==PORT_IN)s->in=static_cast<const float*>(data);
    else if(port==PORT_OUT)s->out=static_cast<float*>(data);
    else if(port>=2 && port<16)s->controls[port-2]=static_cast<const float*>(data);
    else if(port>=16 && port<18)s->meters[port-16]=static_cast<float*>(data);
}
void params(Effect* s) {
    supr::ShapeDsp::Params p;
    float* fields[]={&p.hpf,&p.lpf,&p.bass,&p.treble,&p.mid_freq,&p.mid_gain,&p.mid_q,&p.level,&p.boom,&p.boom_freq,&p.harsh,&p.harsh_freq,&p.threshold,&p.release};
    for(unsigned i=0;i<14;++i)if(s->controls[i])*fields[i]=*s->controls[i];
    s->dsp.setParams(p);
}
void activate(LV2_Handle h){auto* s=static_cast<Effect*>(h);params(s);s->dsp.reset();}
void run(LV2_Handle h,uint32_t n){auto* s=static_cast<Effect*>(h);params(s);
    if(s->in&&s->out)s->dsp.process(s->in,s->out,n);
    for(unsigned j=0;j<2;++j)if(s->meters[j])*s->meters[j]=float(s->dsp.reductionDb(j));
}
void cleanup(LV2_Handle h){delete static_cast<Effect*>(h);}
const LV2_Descriptor desc={"https://suprduprnatural.github.io/supr-pedals/shape",instantiate,connect,activate,run,nullptr,cleanup,nullptr};
}
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i){return i==0?&desc:nullptr;}
