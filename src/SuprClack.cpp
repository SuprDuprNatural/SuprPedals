// SuprClack.cpp — LV2 wrapper around ClackDsp.
#include "ClackDsp.h"

#include <lv2/core/lv2.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>

#define SUPRCLACK_URI "https://suprduprnatural.github.io/supr-pedals/clack"

namespace {

enum PortIndex : uint32_t {
    PORT_IN      = 0,
    PORT_OUT     = 1,
    PORT_CLACK   = 2,
    PORT_SCRAPE  = 3,
    PORT_SENSE   = 4,
    PORT_FOCUS   = 5,
    PORT_THRESH  = 6,
    PORT_RANGE   = 7,
    PORT_RELEASE = 8,
    PORT_LATENCY = 9,  // output: the 2 ms lookahead, in samples
    PORT_CLACKGR = 10, // output: HF duck, dB <= 0 (feeds the UI meter)
    PORT_SCRGR   = 11, // output: sieve + squeak duck, dB <= 0
    PORT_EXPGR   = 12, // output: expander reduction, dB <= 0
    PORT_ENV     = 13, // output: key level, dB
    PORT_DELTA   = 14, // monitor the removed signal instead of the output
};

struct SuprClack {
    supr::ClackDsp dsp;

    const float* in      = nullptr;
    float*       out     = nullptr;
    const float* clack   = nullptr;
    const float* scrape  = nullptr;
    const float* sense   = nullptr;
    const float* focus   = nullptr;
    const float* thresh  = nullptr;
    const float* range   = nullptr;
    const float* release = nullptr;
    const float* delta   = nullptr;
    float*       latency = nullptr;
    float*       clackGr = nullptr;
    float*       scrGr   = nullptr;
    float*       expGr   = nullptr;
    float*       env     = nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprClack* self = new (std::nothrow) SuprClack();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprClack* self = static_cast<SuprClack*>(instance);
    switch (port) {
    case PORT_IN:      self->in      = static_cast<const float*>(data); break;
    case PORT_OUT:     self->out     = static_cast<float*>(data); break;
    case PORT_CLACK:   self->clack   = static_cast<const float*>(data); break;
    case PORT_SCRAPE:  self->scrape  = static_cast<const float*>(data); break;
    case PORT_SENSE:   self->sense   = static_cast<const float*>(data); break;
    case PORT_FOCUS:   self->focus   = static_cast<const float*>(data); break;
    case PORT_THRESH:  self->thresh  = static_cast<const float*>(data); break;
    case PORT_RANGE:   self->range   = static_cast<const float*>(data); break;
    case PORT_RELEASE: self->release = static_cast<const float*>(data); break;
    case PORT_DELTA:   self->delta   = static_cast<const float*>(data); break;
    case PORT_LATENCY: self->latency = static_cast<float*>(data); break;
    case PORT_CLACKGR: self->clackGr = static_cast<float*>(data); break;
    case PORT_SCRGR:   self->scrGr   = static_cast<float*>(data); break;
    case PORT_EXPGR:   self->expGr   = static_cast<float*>(data); break;
    case PORT_ENV:     self->env     = static_cast<float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprClack*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprClack* self = static_cast<SuprClack*>(instance);
    if (!self->in || !self->out)
        return;

    if (self->clack)
        self->dsp.setClack(*self->clack);
    if (self->scrape)
        self->dsp.setScrape(*self->scrape);
    if (self->sense)
        self->dsp.setSense(*self->sense);
    if (self->focus)
        self->dsp.setFocus(*self->focus);
    if (self->thresh)
        self->dsp.setThreshold(*self->thresh);
    if (self->range)
        self->dsp.setRange(*self->range);
    if (self->release)
        self->dsp.setRelease(*self->release);
    if (self->delta)
        self->dsp.setDelta(*self->delta > 0.5f);

    self->dsp.process(self->in, self->out, nSamples);

    if (self->latency)
        *self->latency = float(self->dsp.latencySamples());
    if (self->clackGr)
        *self->clackGr = self->dsp.clackGrDb();
    if (self->scrGr)
        *self->scrGr = self->dsp.scrapeGrDb();
    if (self->expGr)
        *self->expGr = self->dsp.expGrDb();
    if (self->env)
        *self->env = self->dsp.keyDb();
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprClack*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPRCLACK_URI, instantiate, connect_port, activate,
    run,           deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
