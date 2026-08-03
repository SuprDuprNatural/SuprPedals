// SuprTuner.cpp — LV2 wrapper around the bass-first precision tuner.
#include "TunerDsp.h"

#include <lv2/core/lv2.h>

#include <cmath>
#include <cstdint>
#include <new>

#define SUPRTUNER_URI "https://suprduprnatural.github.io/supr-pedals/tuner"

namespace {

enum PortIndex : uint32_t {
    PORT_IN = 0,
    PORT_OUT = 1,
    PORT_REFERENCE = 2,
    PORT_GATE = 3,
    PORT_MUTE = 4,
    PORT_FREQUENCY = 5,
    PORT_NOTE = 6,
    PORT_CENTS = 7,
    PORT_CONFIDENCE = 8,
    PORT_LEVEL = 9,
    PORT_STROBE = 10,
};

struct SuprTuner {
    supr::TunerDsp dsp;

    const float* in = nullptr;
    float* out = nullptr;
    const float* reference = nullptr;
    const float* gate = nullptr;
    const float* mute = nullptr;
    float* frequency = nullptr;
    float* note = nullptr;
    float* cents = nullptr;
    float* confidence = nullptr;
    float* level = nullptr;
    float* strobe = nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprTuner* self = new (std::nothrow) SuprTuner();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprTuner* self = static_cast<SuprTuner*>(instance);
    switch (port) {
    case PORT_IN:         self->in = static_cast<const float*>(data); break;
    case PORT_OUT:        self->out = static_cast<float*>(data); break;
    case PORT_REFERENCE:  self->reference = static_cast<const float*>(data); break;
    case PORT_GATE:       self->gate = static_cast<const float*>(data); break;
    case PORT_MUTE:       self->mute = static_cast<const float*>(data); break;
    case PORT_FREQUENCY:  self->frequency = static_cast<float*>(data); break;
    case PORT_NOTE:       self->note = static_cast<float*>(data); break;
    case PORT_CENTS:      self->cents = static_cast<float*>(data); break;
    case PORT_CONFIDENCE: self->confidence = static_cast<float*>(data); break;
    case PORT_LEVEL:      self->level = static_cast<float*>(data); break;
    case PORT_STROBE:     self->strobe = static_cast<float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprTuner*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprTuner* self = static_cast<SuprTuner*>(instance);
    if (!self->in || !self->out)
        return;

    if (self->reference)
        self->dsp.setReference(*self->reference);
    if (self->gate)
        self->dsp.setGateDb(*self->gate);
    if (self->mute)
        self->dsp.setMute(*self->mute >= 0.5f);

    self->dsp.process(self->in, self->out, nSamples);

    if (self->frequency)
        *self->frequency = self->dsp.frequency();
    if (self->note)
        *self->note = float(self->dsp.note());
    if (self->cents)
        *self->cents = self->dsp.cents();
    if (self->confidence)
        *self->confidence = self->dsp.confidence();
    if (self->level)
        *self->level = self->dsp.levelDb();
    if (self->strobe)
        *self->strobe = self->dsp.strobePhase();
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprTuner*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPRTUNER_URI, instantiate, connect_port, activate,
    run,           deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
