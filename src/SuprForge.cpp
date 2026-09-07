// SuprForge LV2 port contract. Keep indices in sync with ttl/suprforge.ttl.
#include "ForgeDsp.h"
#include <lv2/core/lv2.h>
#include <new>

namespace {
enum PortIndex : uint32_t {
    PORT_IN = 0, PORT_OUT = 1, PORT_DRIVE = 2, PORT_WEIGHT = 3,
    PORT_TIGHT = 4, PORT_BITE = 5, PORT_FIZZ = 6, PORT_LEVEL = 7,
    PORT_COMP = 8, PORT_GATE = 9, PORT_LOW_GR = 10, PORT_GATE_GR = 11,
    PORT_PEAK = 12, PORT_LATENCY = 13,
};
struct Forge {
    supr::ForgeDsp dsp;
    const float* in = nullptr;
    float* out = nullptr;
    const float* controls[8] = {};
    float* meters[4] = {};
};
LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*) {
    auto* self = new (std::nothrow) Forge;
    if (self) self->dsp.init(rate);
    return self;
}
void connect(LV2_Handle instance, uint32_t port, void* data) {
    auto* self = static_cast<Forge*>(instance);
    if (port == PORT_IN) self->in = static_cast<const float*>(data);
    else if (port == PORT_OUT) self->out = static_cast<float*>(data);
    else if (port >= PORT_DRIVE && port <= PORT_GATE)
        self->controls[port - PORT_DRIVE] = static_cast<const float*>(data);
    else if (port >= PORT_LOW_GR && port <= PORT_LATENCY)
        self->meters[port - PORT_LOW_GR] = static_cast<float*>(data);
}
void activate(LV2_Handle instance) { static_cast<Forge*>(instance)->dsp.reset(); }
void run(LV2_Handle instance, uint32_t n) {
    auto* self = static_cast<Forge*>(instance);
    if (!self->in || !self->out) return;
    supr::ForgeDsp::Params p;
    float* values[] = {&p.drive, &p.weight, &p.tight, &p.bite, &p.fizz, &p.level, &p.comp, &p.gate};
    for (int i = 0; i < 8; ++i) if (self->controls[i]) *values[i] = *self->controls[i];
    self->dsp.setParams(p);
    self->dsp.process(self->in, self->out, n);
    const float meters[] = {self->dsp.lowReductionDb(), -self->dsp.gateReductionDb(),
                            self->dsp.outputPeakDb(), float(supr::ForgeDsp::kLatency)};
    for (int i = 0; i < 4; ++i) if (self->meters[i]) *self->meters[i] = meters[i];
}
void cleanup(LV2_Handle instance) { delete static_cast<Forge*>(instance); }
const void* extension(const char*) { return nullptr; }
const LV2_Descriptor descriptor = {
    "https://suprduprnatural.github.io/supr-pedals/forge", instantiate, connect,
    activate, run, nullptr, cleanup, extension
};
}
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : nullptr;
}
