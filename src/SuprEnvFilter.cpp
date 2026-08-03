// SuprEnvFilter.cpp — LV2 wrapper around EnvFilterDsp.
#include "EnvFilterDsp.h"

#include <lv2/core/lv2.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>

#define SUPRENVFILTER_URI "https://suprduprnatural.github.io/supr-pedals/envelope-filter"

namespace {

enum PortIndex : uint32_t {
    PORT_IN      = 0,
    PORT_OUT     = 1,
    PORT_SENS    = 2,
    PORT_ATTACK  = 3,
    PORT_RELEASE = 4,
    PORT_MODE    = 5,
    PORT_DIR     = 6,
    PORT_CUTOFF  = 7,
    PORT_RANGE   = 8,
    PORT_RES     = 9,
    PORT_BLEND   = 10,
    PORT_LEVEL   = 11,
    PORT_FC      = 12, // output: live swept cutoff, Hz (feeds the UI plot)
};

struct SuprEnvFilter {
    supr::EnvFilterDsp dsp;

    const float* in      = nullptr;
    float*       out     = nullptr;
    const float* sens    = nullptr;
    const float* attack  = nullptr;
    const float* release = nullptr;
    const float* mode    = nullptr;
    const float* dir     = nullptr;
    const float* cutoff  = nullptr;
    const float* range   = nullptr;
    const float* res     = nullptr;
    const float* blend   = nullptr;
    const float* level   = nullptr;
    float*       fc      = nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprEnvFilter* self = new (std::nothrow) SuprEnvFilter();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprEnvFilter* self = static_cast<SuprEnvFilter*>(instance);
    switch (port) {
    case PORT_IN:      self->in      = static_cast<const float*>(data); break;
    case PORT_OUT:     self->out     = static_cast<float*>(data); break;
    case PORT_SENS:    self->sens    = static_cast<const float*>(data); break;
    case PORT_ATTACK:  self->attack  = static_cast<const float*>(data); break;
    case PORT_RELEASE: self->release = static_cast<const float*>(data); break;
    case PORT_MODE:    self->mode    = static_cast<const float*>(data); break;
    case PORT_DIR:     self->dir     = static_cast<const float*>(data); break;
    case PORT_CUTOFF:  self->cutoff  = static_cast<const float*>(data); break;
    case PORT_RANGE:   self->range   = static_cast<const float*>(data); break;
    case PORT_RES:     self->res     = static_cast<const float*>(data); break;
    case PORT_BLEND:   self->blend   = static_cast<const float*>(data); break;
    case PORT_LEVEL:   self->level   = static_cast<const float*>(data); break;
    case PORT_FC:      self->fc      = static_cast<float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprEnvFilter*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprEnvFilter* self = static_cast<SuprEnvFilter*>(instance);
    if (!self->in || !self->out)
        return;

    if (self->sens)
        self->dsp.setSens(*self->sens);
    if (self->attack)
        self->dsp.setAttack(*self->attack);
    if (self->release)
        self->dsp.setRelease(*self->release);
    if (self->mode)
        self->dsp.setMode(int(std::lround(*self->mode)));
    if (self->dir)
        self->dsp.setDir(int(std::lround(*self->dir)));
    if (self->cutoff)
        self->dsp.setCutoff(*self->cutoff);
    if (self->range)
        self->dsp.setRange(*self->range);
    if (self->res)
        self->dsp.setRes(*self->res);
    if (self->blend)
        self->dsp.setBlend(*self->blend);
    if (self->level)
        self->dsp.setLevel(*self->level);

    self->dsp.process(self->in, self->out, nSamples);

    if (self->fc)
        *self->fc = self->dsp.currentFc();
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprEnvFilter*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPRENVFILTER_URI, instantiate, connect_port, activate,
    run,               deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
