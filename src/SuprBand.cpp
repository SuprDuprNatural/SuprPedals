// SuprBand.cpp — LV2 wrapper around BandDsp.
#include "BandDsp.h"

#include <lv2/core/lv2.h>

#include <cstdint>
#include <cstdlib>
#include <new>

#define SUPRBAND_URI "https://suprduprnatural.github.io/supr-pedals/multiband"

namespace {

// Always three bands, no polarity switches and no solo: BandDsp still has
// all three (the test harness drives them to prove the crossover sums and
// that the solos re-sum to the whole), but they are not worth a control on
// the front panel of a bass pedal.
enum PortIndex : uint32_t {
    PORT_IN      = 0,
    PORT_OUT     = 1,
    PORT_SPLIT1  = 2,
    PORT_SPLIT2  = 3,
    // three identical band strips, in the same order as BandDsp's LOW/MID/HIGH
    PORT_LO_DRIVE = 4,
    PORT_LO_COMP  = 5,
    PORT_LO_LEVEL = 6,
    PORT_MD_DRIVE = 7,
    PORT_MD_COMP  = 8,
    PORT_MD_LEVEL = 9,
    PORT_HI_DRIVE = 10,
    PORT_HI_COMP  = 11,
    PORT_HI_LEVEL = 12,
    PORT_BLEND   = 13,
    PORT_LEVEL   = 14,
    PORT_GR_LO   = 15, // output: per-band gain reduction, for the meters
    PORT_GR_MD   = 16,
    PORT_GR_HI   = 17,
    PORT_LATENCY = 18,
    PORT_DRV_LO  = 19, // output: per-band drive action, for the meters
    PORT_DRV_MD  = 20,
    PORT_DRV_HI  = 21,
};

// The band strips are contiguous and identical, so the three controls are
// addressed by (band, offset) rather than by nine named pointers.
constexpr uint32_t kStripBase   = PORT_LO_DRIVE;
constexpr uint32_t kStripStride = 3;

struct SuprBand {
    supr::BandDsp dsp;

    const float* in    = nullptr;
    float*       out   = nullptr;
    const float* split1 = nullptr;
    const float* split2 = nullptr;
    const float* strip[supr::BandDsp::kBands][kStripStride] = {{nullptr}};
    const float* blend = nullptr;
    const float* level = nullptr;
    float*       gr[supr::BandDsp::kBands] = {nullptr};
    float*       drv[supr::BandDsp::kBands] = {nullptr};
    float*       latency = nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprBand* self = new (std::nothrow) SuprBand();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprBand* self = static_cast<SuprBand*>(instance);

    if (port >= kStripBase
        && port < kStripBase + kStripStride * supr::BandDsp::kBands) {
        const uint32_t i = port - kStripBase;
        self->strip[i / kStripStride][i % kStripStride] =
            static_cast<const float*>(data);
        return;
    }

    switch (port) {
    case PORT_IN:      self->in      = static_cast<const float*>(data); break;
    case PORT_OUT:     self->out     = static_cast<float*>(data); break;
    case PORT_SPLIT1:  self->split1  = static_cast<const float*>(data); break;
    case PORT_SPLIT2:  self->split2  = static_cast<const float*>(data); break;
    case PORT_BLEND:   self->blend   = static_cast<const float*>(data); break;
    case PORT_LEVEL:   self->level   = static_cast<const float*>(data); break;
    case PORT_GR_LO:   self->gr[0]   = static_cast<float*>(data); break;
    case PORT_GR_MD:   self->gr[1]   = static_cast<float*>(data); break;
    case PORT_GR_HI:   self->gr[2]   = static_cast<float*>(data); break;
    case PORT_DRV_LO:  self->drv[0]  = static_cast<float*>(data); break;
    case PORT_DRV_MD:  self->drv[1]  = static_cast<float*>(data); break;
    case PORT_DRV_HI:  self->drv[2]  = static_cast<float*>(data); break;
    case PORT_LATENCY: self->latency = static_cast<float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprBand*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprBand* self = static_cast<SuprBand*>(instance);
    if (!self->in || !self->out)
        return;

    if (self->split1)
        self->dsp.setSplit1(*self->split1);
    if (self->split2)
        self->dsp.setSplit2(*self->split2);

    for (int b = 0; b < supr::BandDsp::kBands; ++b) {
        if (self->strip[b][0])
            self->dsp.setDrive(b, *self->strip[b][0]);
        if (self->strip[b][1])
            self->dsp.setComp(b, *self->strip[b][1]);
        if (self->strip[b][2])
            self->dsp.setLevel(b, *self->strip[b][2]);
    }

    if (self->blend)
        self->dsp.setBlend(*self->blend);
    if (self->level)
        self->dsp.setOutput(*self->level);

    self->dsp.process(self->in, self->out, nSamples);

    for (int b = 0; b < supr::BandDsp::kBands; ++b) {
        if (self->gr[b])
            *self->gr[b] = self->dsp.grDb(b);
        if (self->drv[b])
            *self->drv[b] = self->dsp.driveDb(b);
    }
    if (self->latency)
        *self->latency = float(supr::BandDsp::kLatency);
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprBand*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPRBAND_URI, instantiate, connect_port, activate,
    run,          deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
