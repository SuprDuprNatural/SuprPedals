// SuprCompressor.cpp — LV2 wrapper around CompressorDsp.
#include "CompressorDsp.h"

#include <lv2/core/lv2.h>

#include <cstdint>
#include <cstdlib>
#include <new>

#define SUPRCOMPRESSOR_URI "https://suprduprnatural.github.io/supr-pedals/compressor"

namespace {

// Port order groups the controls into the UI sections declared in the ttl:
// Compression (2-6), Output (7-8).
enum PortIndex : uint32_t {
    PORT_IN        = 0,
    PORT_OUT       = 1,
    PORT_THRESHOLD = 2,
    PORT_RATIO     = 3,
    PORT_ATTACK    = 4,
    PORT_RELEASE   = 5,
    PORT_SCHPF     = 6,
    PORT_MAKEUP    = 7,
    PORT_BLEND     = 8,
    PORT_GR        = 9, // output: gain reduction in dB (<= 0)
};

struct SuprCompressor {
    supr::CompressorDsp dsp;

    const float* in        = nullptr;
    float*       out       = nullptr;
    const float* threshold = nullptr;
    const float* ratio     = nullptr;
    const float* attack    = nullptr;
    const float* release   = nullptr;
    const float* makeup    = nullptr;
    const float* blend     = nullptr;
    const float* schpf     = nullptr;
    float*       gr        = nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprCompressor* self = new (std::nothrow) SuprCompressor();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprCompressor* self = static_cast<SuprCompressor*>(instance);
    switch (port) {
    case PORT_IN:        self->in        = static_cast<const float*>(data); break;
    case PORT_OUT:       self->out       = static_cast<float*>(data); break;
    case PORT_THRESHOLD: self->threshold = static_cast<const float*>(data); break;
    case PORT_RATIO:     self->ratio     = static_cast<const float*>(data); break;
    case PORT_ATTACK:    self->attack    = static_cast<const float*>(data); break;
    case PORT_RELEASE:   self->release   = static_cast<const float*>(data); break;
    case PORT_MAKEUP:    self->makeup    = static_cast<const float*>(data); break;
    case PORT_BLEND:     self->blend     = static_cast<const float*>(data); break;
    case PORT_SCHPF:     self->schpf     = static_cast<const float*>(data); break;
    case PORT_GR:        self->gr        = static_cast<float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprCompressor*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprCompressor* self = static_cast<SuprCompressor*>(instance);
    if (!self->in || !self->out)
        return;

    if (self->threshold)
        self->dsp.setThreshold(*self->threshold);
    if (self->ratio)
        self->dsp.setRatio(*self->ratio);
    if (self->attack)
        self->dsp.setAttack(*self->attack);
    if (self->release)
        self->dsp.setRelease(*self->release);
    if (self->makeup)
        self->dsp.setMakeup(*self->makeup);
    if (self->blend)
        self->dsp.setBlend(*self->blend);
    if (self->schpf)
        self->dsp.setScHpf(*self->schpf);

    self->dsp.process(self->in, self->out, nSamples);

    if (self->gr)
        *self->gr = self->dsp.grDb();
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprCompressor*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPRCOMPRESSOR_URI, instantiate, connect_port, activate,
    run,                deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
