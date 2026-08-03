// SuprVu.cpp — LV2 wrapper around VuMeterDsp (stereo passthrough meter).
#include "VuMeterDsp.h"

#include <lv2/core/lv2.h>

#include <cstdint>
#include <cstdlib>
#include <new>

#define SUPRVU_URI "https://suprduprnatural.github.io/supr-pedals/vu-meter"

namespace {

enum PortIndex : uint32_t {
    PORT_IN_L   = 0,
    PORT_IN_R   = 1,
    PORT_OUT_L  = 2,
    PORT_OUT_R  = 3,
    PORT_VU_L   = 4, // output: VU level dB
    PORT_VU_R   = 5,
    PORT_PEAK_L = 6, // output: peak level dB
    PORT_PEAK_R = 7,
    PORT_CAL    = 8, // input: 0 VU reference level (dBFS)
};

struct SuprVu {
    supr::VuMeterDsp dsp;

    const float* inL   = nullptr;
    const float* inR   = nullptr;
    float*       outL  = nullptr;
    float*       outR  = nullptr;
    float*       vuL   = nullptr;
    float*       vuR   = nullptr;
    float*       peakL = nullptr;
    float*       peakR = nullptr;
    const float* cal   = nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprVu* self = new (std::nothrow) SuprVu();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprVu* self = static_cast<SuprVu*>(instance);
    switch (port) {
    case PORT_IN_L:   self->inL   = static_cast<const float*>(data); break;
    case PORT_IN_R:   self->inR   = static_cast<const float*>(data); break;
    case PORT_OUT_L:  self->outL  = static_cast<float*>(data); break;
    case PORT_OUT_R:  self->outR  = static_cast<float*>(data); break;
    case PORT_VU_L:   self->vuL   = static_cast<float*>(data); break;
    case PORT_VU_R:   self->vuR   = static_cast<float*>(data); break;
    case PORT_PEAK_L: self->peakL = static_cast<float*>(data); break;
    case PORT_PEAK_R: self->peakR = static_cast<float*>(data); break;
    case PORT_CAL:    self->cal   = static_cast<const float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprVu*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprVu* self = static_cast<SuprVu*>(instance);
    if (!self->inL || !self->outL)
        return;

    if (self->cal)
        self->dsp.setCalibration(*self->cal);

    const float* inR  = self->inR ? self->inR : self->inL;
    float*       outR = self->outR ? self->outR : self->outL;
    self->dsp.process(self->inL, inR, self->outL, outR, nSamples);

    if (self->vuL)
        *self->vuL = self->dsp.vuDb(0);
    if (self->vuR)
        *self->vuR = self->dsp.vuDb(1);
    if (self->peakL)
        *self->peakL = self->dsp.peakDb(0);
    if (self->peakR)
        *self->peakR = self->dsp.peakDb(1);
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprVu*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPRVU_URI, instantiate, connect_port, activate,
    run,        deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
