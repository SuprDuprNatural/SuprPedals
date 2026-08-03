// SuprOctavePlus.cpp — LV2 wrapper around OctaverPlusDsp.
#include "OctaverPlusDsp.h"

#include <lv2/core/lv2.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>

#define SUPROCTAVEPLUS_URI "https://suprduprnatural.github.io/supr-pedals/octave-plus"

namespace {

// Port order groups the controls into the UI sections declared in the ttl:
// Octaves (2-6), Synth (7-11), Filter (12-17), Envelope (18-22).
enum PortIndex : uint32_t {
    PORT_IN       = 0,
    PORT_OUT      = 1,
    // Mixer
    PORT_DIRECT   = 2,
    PORT_GATE     = 3,
    PORT_OCT1     = 4,
    PORT_TONE     = 5,
    // Synth: two independent oscillators, then the shared pitch controls
    PORT_O1LEVEL  = 6,
    PORT_O1OCT    = 7,
    PORT_O1WAVE   = 8,
    PORT_O2LEVEL  = 9,
    PORT_O2OCT    = 10,
    PORT_O2WAVE   = 11,
    PORT_DETUNE   = 12,
    PORT_GLIDE    = 13,
    // Filter
    PORT_CUTOFF   = 14,
    PORT_RES      = 15,
    PORT_ENVMOD   = 16,
    PORT_KEYTRACK = 17,
    PORT_FATTACK  = 18,
    PORT_FDECAY   = 19,

    PORT_FC       = 20, // live synth filter cutoff, Hz
    PORT_NOTE     = 21, // tracked note fundamental, Hz
    PORT_SAMP     = 22, // synth amp envelope level, 0..1
};

struct SuprOctavePlus {
    supr::OctaverPlusDsp dsp;

    const float* in       = nullptr;
    float*       out      = nullptr;
    const float* direct   = nullptr;
    const float* oct1     = nullptr;
    const float* tone     = nullptr;
    const float* gate     = nullptr;
    const float* o1level  = nullptr;
    const float* o1wave   = nullptr;
    const float* o1oct    = nullptr;
    const float* o2level  = nullptr;
    const float* o2wave   = nullptr;
    const float* o2oct    = nullptr;
    const float* cutoff   = nullptr;
    const float* res      = nullptr;
    const float* envmod   = nullptr;
    const float* glide    = nullptr;
    const float* fattack  = nullptr;
    const float* fdecay   = nullptr;
    const float* detune   = nullptr;
    const float* keytrack = nullptr;
    float*       fc       = nullptr;
    float*       note     = nullptr;
    float*       samp     = nullptr;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    SuprOctavePlus* self = new (std::nothrow) SuprOctavePlus();
    if (!self)
        return nullptr;
    self->dsp.init(rate);
    return static_cast<LV2_Handle>(self);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    SuprOctavePlus* self = static_cast<SuprOctavePlus*>(instance);
    switch (port) {
    case PORT_IN:     self->in     = static_cast<const float*>(data); break;
    case PORT_OUT:    self->out    = static_cast<float*>(data); break;
    case PORT_DIRECT: self->direct = static_cast<const float*>(data); break;
    case PORT_OCT1:   self->oct1   = static_cast<const float*>(data); break;
    case PORT_TONE:   self->tone   = static_cast<const float*>(data); break;
    case PORT_GATE:   self->gate   = static_cast<const float*>(data); break;
    case PORT_O1LEVEL: self->o1level = static_cast<const float*>(data); break;
    case PORT_O1WAVE:  self->o1wave  = static_cast<const float*>(data); break;
    case PORT_O1OCT:   self->o1oct   = static_cast<const float*>(data); break;
    case PORT_O2LEVEL: self->o2level = static_cast<const float*>(data); break;
    case PORT_O2WAVE:  self->o2wave  = static_cast<const float*>(data); break;
    case PORT_O2OCT:   self->o2oct   = static_cast<const float*>(data); break;
    case PORT_CUTOFF: self->cutoff = static_cast<const float*>(data); break;
    case PORT_RES:    self->res    = static_cast<const float*>(data); break;
    case PORT_ENVMOD: self->envmod = static_cast<const float*>(data); break;
    case PORT_GLIDE:    self->glide    = static_cast<const float*>(data); break;
    case PORT_FATTACK:  self->fattack  = static_cast<const float*>(data); break;
    case PORT_FDECAY:   self->fdecay   = static_cast<const float*>(data); break;
    case PORT_DETUNE:   self->detune   = static_cast<const float*>(data); break;
    case PORT_KEYTRACK: self->keytrack = static_cast<const float*>(data); break;
    case PORT_FC:       self->fc       = static_cast<float*>(data); break;
    case PORT_NOTE:     self->note     = static_cast<float*>(data); break;
    case PORT_SAMP:     self->samp     = static_cast<float*>(data); break;
    }
}

void activate(LV2_Handle instance)
{
    static_cast<SuprOctavePlus*>(instance)->dsp.reset();
}

void run(LV2_Handle instance, uint32_t nSamples)
{
    SuprOctavePlus* self = static_cast<SuprOctavePlus*>(instance);
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
    if (self->o1level)
        self->dsp.setOsc1Level(*self->o1level);
    if (self->o1wave)
        self->dsp.setOsc1Wave(int(std::lround(*self->o1wave)));
    if (self->o1oct)
        self->dsp.setOsc1Oct(int(std::lround(*self->o1oct)));
    if (self->o2level)
        self->dsp.setOsc2Level(*self->o2level);
    if (self->o2wave)
        self->dsp.setOsc2Wave(int(std::lround(*self->o2wave)));
    if (self->o2oct)
        self->dsp.setOsc2Oct(int(std::lround(*self->o2oct)));
    if (self->cutoff)
        self->dsp.setCutoff(*self->cutoff);
    if (self->res)
        self->dsp.setRes(*self->res);
    if (self->envmod)
        self->dsp.setEnvMod(*self->envmod);
    if (self->glide)
        self->dsp.setGlide(*self->glide);
    if (self->fattack)
        self->dsp.setFAttack(*self->fattack);
    if (self->fdecay)
        self->dsp.setFDecay(*self->fdecay);
    if (self->detune)
        self->dsp.setDetune(*self->detune);
    if (self->keytrack)
        self->dsp.setKeytrack(*self->keytrack);

    self->dsp.process(self->in, self->out, nSamples);

    if (self->fc)
        *self->fc = self->dsp.currentFc();
    if (self->note)
        *self->note = self->dsp.currentNoteHz();
    if (self->samp)
        *self->samp = self->dsp.currentSynthAmp();
}

void deactivate(LV2_Handle) {}

void cleanup(LV2_Handle instance)
{
    delete static_cast<SuprOctavePlus*>(instance);
}

const void* extension_data(const char*)
{
    return nullptr;
}

const LV2_Descriptor descriptor = {
    SUPROCTAVEPLUS_URI, instantiate, connect_port, activate,
    run,              deactivate,  cleanup,      extension_data,
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
