// SuprChorus.cpp — LV2 wrapper around ChorusDsp.
#include "ChorusDsp.h"

#include <lv2/core/lv2.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>

#define SUPRCHORUS_URI "https://suprduprnatural.github.io/supr-pedals/chorus"

namespace {

enum PortIndex : uint32_t {
    PORT_IN     = 0,
    PORT_OUT    = 1,
    PORT_RATE   = 2,
    PORT_DEPTH  = 3,
    PORT_VOICES = 4,
    PORT_LOW    = 5,
    PORT_TONE   = 6,
    PORT_MIX    = 7,
    PORT_LFO    = 8, // output: LFO phase 0..1 (feeds the UI display)
};

struct SuprChorus {
    supr::ChorusDsp dsp;

    const float* in     = nullptr;
    float*       out    = nullptr;
    const float* rate   = nullptr;
    const float* depth  = nullptr;
    const float* voices = nullptr;
    const float* low    = nullptr;
    const float* tone   = nullptr;
    const float* mix    = nullptr;
    float*       lfo    = nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprChorus* self = new (std::nothrow) SuprChorus();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprChorus* self = static_cast<SuprChorus*>(instance);
    switch (port) {
    case PORT_IN:     self->in     = static_cast<const float*>(data); break;
    case PORT_OUT:    self->out    = static_cast<float*>(data); break;
    case PORT_RATE:   self->rate   = static_cast<const float*>(data); break;
    case PORT_DEPTH:  self->depth  = static_cast<const float*>(data); break;
    case PORT_VOICES: self->voices = static_cast<const float*>(data); break;
    case PORT_LOW:    self->low    = static_cast<const float*>(data); break;
    case PORT_TONE:   self->tone   = static_cast<const float*>(data); break;
    case PORT_MIX:    self->mix    = static_cast<const float*>(data); break;
    case PORT_LFO:    self->lfo    = static_cast<float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprChorus*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprChorus* self = static_cast<SuprChorus*>(instance);
    if (!self->in || !self->out)
        return;

    if (self->rate)
        self->dsp.setRate(*self->rate);
    if (self->depth)
        self->dsp.setDepth(*self->depth);
    if (self->voices)
        self->dsp.setVoices(int(std::lround(*self->voices)));
    if (self->low)
        self->dsp.setLow(*self->low);
    if (self->tone)
        self->dsp.setTone(*self->tone);
    if (self->mix)
        self->dsp.setMix(*self->mix);

    self->dsp.process(self->in, self->out, nSamples);

    if (self->lfo)
        *self->lfo = self->dsp.lfoPhase();
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprChorus*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPRCHORUS_URI, instantiate, connect_port, activate,
    run,            deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
