// PiOctave.cpp — LV2 wrapper around OctaverDsp.
#include "OctaverDsp.h"

#include <lv2/core/lv2.h>

#include <cstdint>
#include <cstdlib>
#include <new>

#define SUPROCTAVE_URI "https://suprduprnatural.github.io/supr-pedals/octave"

namespace {

enum PortIndex : uint32_t {
    PORT_IN     = 0,
    PORT_OUT    = 1,
    PORT_DIRECT = 2,
    PORT_OCT1   = 3,
    PORT_TONE   = 4,
    PORT_GATE   = 5,
};

struct SuprOctave {
    supr::OctaverDsp dsp;

    const float* in     = nullptr;
    float*       out    = nullptr;
    const float* direct = nullptr;
    const float* oct1   = nullptr;
    const float* tone   = nullptr;
    const float* gate   = nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprOctave* self = new (std::nothrow) SuprOctave();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprOctave* self = static_cast<SuprOctave*>(instance);
    switch (port) {
    case PORT_IN:     self->in     = static_cast<const float*>(data); break;
    case PORT_OUT:    self->out    = static_cast<float*>(data); break;
    case PORT_DIRECT: self->direct = static_cast<const float*>(data); break;
    case PORT_OCT1:   self->oct1   = static_cast<const float*>(data); break;
    case PORT_TONE:   self->tone   = static_cast<const float*>(data); break;
    case PORT_GATE:   self->gate   = static_cast<const float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprOctave*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprOctave* self = static_cast<SuprOctave*>(instance);
    if (!self->in || !self->out)
        return;

    if (self->direct)
        self->dsp.setDirect(*self->direct);
    if (self->oct1)
        self->dsp.setOct1(*self->oct1);
    if (self->tone)
        self->dsp.setTone(*self->tone);
    if (self->gate)
        self->dsp.setGateDb(*self->gate);

    self->dsp.process(self->in, self->out, nSamples);
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprOctave*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPROCTAVE_URI, instantiate, connect_port, activate,
    run,          deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
