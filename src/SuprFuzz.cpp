// SuprFuzz.cpp — LV2 wrapper around FuzzDsp.
#include "FuzzDsp.h"

#include <lv2/core/lv2.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>

#define SUPRFUZZ_URI "https://suprduprnatural.github.io/supr-pedals/fuzz"

namespace {

enum PortIndex : uint32_t {
    PORT_IN      = 0,
    PORT_OUT     = 1,
    PORT_SUSTAIN = 2,
    PORT_TONE    = 3,
    PORT_GATE    = 4,
    PORT_BLEND   = 5,
    PORT_LEVEL   = 6,
    PORT_NOTE    = 7, // output: tracked pitch, Hz (feeds the UI)
    PORT_MATCH_MODE = 9,
    PORT_HELD_GAIN = 10,
    PORT_LEARN = 11,
    PORT_LEARNED_GAIN = 12,
    PORT_LEARN_STATE = 13,
    PORT_LATENCY = 8, // output: halfband round trip, samples
};

struct SuprFuzz {
    supr::FuzzDsp dsp;

    const float* in      = nullptr;
    float*       out     = nullptr;
    const float* sustain = nullptr;
    const float* tone    = nullptr;
    const float* gate    = nullptr;
    const float* blend   = nullptr;
    const float* level   = nullptr;
    float*       note    = nullptr;
    float*       latency = nullptr;
    const float* matchMode=nullptr;
    const float* heldGain=nullptr;
    const float* learn=nullptr;
    float* learnedGain=nullptr;
    float* learnState=nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprFuzz* self = new (std::nothrow) SuprFuzz();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprFuzz* self = static_cast<SuprFuzz*>(instance);
    switch (port) {
    case PORT_IN:      self->in      = static_cast<const float*>(data); break;
    case PORT_OUT:     self->out     = static_cast<float*>(data); break;
    case PORT_SUSTAIN: self->sustain = static_cast<const float*>(data); break;
    case PORT_TONE:    self->tone    = static_cast<const float*>(data); break;
    case PORT_GATE:    self->gate    = static_cast<const float*>(data); break;
    case PORT_BLEND:   self->blend   = static_cast<const float*>(data); break;
    case PORT_LEVEL:   self->level   = static_cast<const float*>(data); break;
    case PORT_NOTE:    self->note    = static_cast<float*>(data); break;
    case PORT_MATCH_MODE:self->matchMode=static_cast<const float*>(data);break;
    case PORT_HELD_GAIN:self->heldGain=static_cast<const float*>(data);break;
    case PORT_LEARN:self->learn=static_cast<const float*>(data);break;
    case PORT_LEARNED_GAIN:self->learnedGain=static_cast<float*>(data);break;
    case PORT_LEARN_STATE:self->learnState=static_cast<float*>(data);break;
    case PORT_LATENCY: self->latency = static_cast<float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprFuzz*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprFuzz* self = static_cast<SuprFuzz*>(instance);
    if (!self->in || !self->out)
        return;

    if (self->sustain)
        self->dsp.setSustain(*self->sustain);
    if (self->tone)
        self->dsp.setTone(*self->tone);
    if (self->gate)
        self->dsp.setGate(*self->gate);
    if (self->blend)
        self->dsp.setBlend(*self->blend);
    if (self->level)
        self->dsp.setLevel(*self->level);

    self->dsp.setMatchMode(self->matchMode && *self->matchMode>.5f ? 1:0);
    self->dsp.setHeldGainDb(self->heldGain?*self->heldGain:0);
    self->dsp.setLearn(self->learn && *self->learn>.5f);
    self->dsp.process(self->in, self->out, nSamples);

    if(self->learnedGain)*self->learnedGain=self->dsp.learnedGainDb();
    if(self->learnState)*self->learnState=float(self->dsp.learnState());
    if (self->note)
        *self->note = self->dsp.currentNote();
    if (self->latency)
        *self->latency = float(supr::FuzzDsp::kLatency);
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprFuzz*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPRFUZZ_URI, instantiate, connect_port, activate,
    run,          deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
