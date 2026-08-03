// SuprSans.cpp — LV2 wrapper around SansDsp.
#include "SansDsp.h"

#include <lv2/core/lv2.h>

#include <cstdint>
#include <cstdlib>
#include <new>

#define SUPRSANS_URI "https://suprduprnatural.github.io/supr-pedals/sans"

namespace {

// Port order groups the controls into the UI sections declared in the ttl:
// Drive (2-3), Tone (4-9), Output (10).
enum PortIndex : uint32_t {
    PORT_IN      = 0,
    PORT_OUT     = 1,
    PORT_DRIVE   = 2,
    PORT_BLEND   = 3,
    PORT_BASS    = 4,
    PORT_MID     = 5,
    PORT_MIDFREQ = 6,
    PORT_TREBLE  = 7,
    PORT_AIR     = 8,
    PORT_RUMBLE  = 9,
    PORT_LEVEL   = 10,
    PORT_LATENCY = 11, // output: oversampler round trip, in frames
};

struct SuprSans {
    supr::SansDsp dsp;

    const float* in      = nullptr;
    float*       out     = nullptr;
    const float* drive   = nullptr;
    const float* blend   = nullptr;
    const float* bass    = nullptr;
    const float* mid     = nullptr;
    const float* midfreq = nullptr;
    const float* treble  = nullptr;
    const float* air     = nullptr;
    const float* rumble  = nullptr;
    const float* level   = nullptr;
    float*       latency = nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprSans* self = new (std::nothrow) SuprSans();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprSans* self = static_cast<SuprSans*>(instance);
    switch (port) {
    case PORT_IN:      self->in      = static_cast<const float*>(data); break;
    case PORT_OUT:     self->out     = static_cast<float*>(data); break;
    case PORT_DRIVE:   self->drive   = static_cast<const float*>(data); break;
    case PORT_BLEND:   self->blend   = static_cast<const float*>(data); break;
    case PORT_BASS:    self->bass    = static_cast<const float*>(data); break;
    case PORT_MID:     self->mid     = static_cast<const float*>(data); break;
    case PORT_MIDFREQ: self->midfreq = static_cast<const float*>(data); break;
    case PORT_TREBLE:  self->treble  = static_cast<const float*>(data); break;
    case PORT_AIR:     self->air     = static_cast<const float*>(data); break;
    case PORT_RUMBLE:  self->rumble  = static_cast<const float*>(data); break;
    case PORT_LEVEL:   self->level   = static_cast<const float*>(data); break;
    case PORT_LATENCY: self->latency = static_cast<float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprSans*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprSans* self = static_cast<SuprSans*>(instance);
    if (!self->in || !self->out)
        return;

    if (self->drive)
        self->dsp.setDrive(*self->drive);
    if (self->blend)
        self->dsp.setBlend(*self->blend);
    if (self->bass)
        self->dsp.setBass(*self->bass);
    if (self->mid)
        self->dsp.setMid(*self->mid);
    if (self->midfreq)
        self->dsp.setMidFreq(*self->midfreq);
    if (self->treble)
        self->dsp.setTreble(*self->treble);
    if (self->air)
        self->dsp.setAir(*self->air > 0.5f);
    if (self->rumble)
        self->dsp.setRumble(*self->rumble > 0.5f);
    if (self->level)
        self->dsp.setLevel(*self->level);

    self->dsp.process(self->in, self->out, nSamples);

    if (self->latency)
        *self->latency = float(supr::SansDsp::kLatency);
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprSans*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPRSANS_URI, instantiate, connect_port, activate,
    run,          deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
