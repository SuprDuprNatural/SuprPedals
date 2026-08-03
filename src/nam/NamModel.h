// NamModel.h — a loaded neural amp model, plus the level calibration that
// makes two of them comparable enough to blend.
//
// This is the only file in SuprNAM that includes NeuralAudio, which keeps
// src/NamDsp.h buildable (and testable) anywhere.
//
// CALIBRATION IS WHAT MAKES BLEND MEAN ANYTHING. Models arrive at wildly
// different levels — a cranked Plexi capture and a clean DI capture can be 20
// dB apart at the same input. Blend one against the other raw and the knob is
// really just a "which model do I hear" switch with a loud end and a quiet
// end. Normalising both to -18 LUFS first is what turns it into an actual
// crossfade, so Normalized is the default output mode and the one the mix law
// in NamDsp.h assumes.
//
// GETTING AT THE MODEL'S METADATA. NeuralAudio parses loudness and
// input_level_dbu out of the model file but keeps them protected, exposing
// only two derived numbers:
//
//     GetRecommendedInputDBAdjustment()  == audioInputLevelDBu - modelInputLevelDBu
//     GetRecommendedOutputDBAdjustment() == -18 - modelLoudnessDB
//
// Pin the static audioInputLevelDBu at 0 and both invert cleanly, so we can
// recover the model's own figures and then do our own arithmetic per instance.
// That matters because audioInputLevelDBu is a *static* — one process-wide
// value shared by every model in every plugin instance — so reading it live
// would make one SuprNAM's calibration setting leak into another's. We touch
// it once, at load, and never rely on it again.
//
// The one figure that is genuinely unreachable this way is output_level_dbu,
// which is why Output Calibration offers Normalized and Raw but not the
// absolute-dBu Calibrated mode TooB NAM has. Nothing that affects blending
// depends on it.
//
// TELLING "ABSENT" FROM "PRESENT". This matters more than it sounds. Most
// models in the wild carry `loudness` but not `input_level_dbu` — every model
// tested here was like that — and NeuralAudio silently substitutes defaults
// (+12 dBu in, -18 dB loudness) rather than saying so. Take those at face
// value and Calibrated input mode quietly applies an 18 dB cut derived from a
// number that was never in the file, which drives every model far below where
// it was trained and leaves the blend hopelessly lopsided.
//
// TooB NAM sidesteps this with HasModelInputLevelDBu() accessors from a fork
// of NeuralAudio that was never published. Without them the only signal
// available is that the value comes back *exactly* equal to the default, to
// the bit — so that is what is used. A model whose real input level is
// precisely 12.00 dBu would be misread as having none, and would get unity
// gain instead; that is a rare coincidence with a mild consequence, and it is
// much better than the alternative of trusting a default that is wrong most of
// the time.
//
// MIT license, (c) 2026 SuprPedals contributors.
//
// The calibration arithmetic follows TooB Neural Amp Modeler's
// CalculateNamVolumeAdjustments, (c) Robin E. R. Davies, MIT.

#pragma once

#include "../NamDsp.h"

#include <memory>
#include <string>

namespace NeuralAudio {
class NeuralModel;
}

namespace supr {

enum class InputCalibration : int {
    Raw        = 0, // trust the incoming level as-is
    Calibrated = 1, // scale so the model sees the level it was trained at
};

enum class OutputCalibration : int {
    Normalized = 0, // every model lands at -18 LUFS — required for sane blending
    Raw        = 1, // whatever the model puts out
};

struct CalibrationSettings {
    InputCalibration  input        = InputCalibration::Calibrated;
    OutputCalibration output       = OutputCalibration::Normalized;
    float             instrumentDbu = -6.0f; // measured instrument level
};

// What the UI needs to know about a slot after a load: whether there is a
// model at all, whether the Slim control applies to it, and whether its
// sample rate matches the host's.
struct ModelInfo {
    bool        loaded          = false;
    bool        hasQualityScale = false; // A2 "slimmable" model
    float       sampleRate      = 0.0f;
    float       loudnessDb      = 0.0f;
    bool        hasLoudness     = false; // false when the file did not say
    float       inputLevelDbu   = 0.0f;
    bool        hasInputLevel   = false;
    int         receptiveField  = -1;
    std::string version;
};

// A model plus its calibration trims, presented to the graph as a NamModelRef.
class NamModel final : public NamModelRef {
public:
    NamModel();
    ~NamModel() override;

    // Loads a .nam or .aidax file. Never call from the audio thread — this
    // allocates, reads from disk and prewarms the network. Returns false and
    // leaves the object empty on failure.
    bool load(const std::string& path, size_t maxBlock, std::string& errorOut);

    bool loaded() const { return model_ != nullptr; }
    const ModelInfo& info() const { return info_; }
    const std::string& path() const { return path_; }

    // A2 slimmable models trade quality against CPU. Ignored by models that
    // do not support it.
    void setQuality(float scale);
    float quality() const { return quality_; }

    void setCalibration(const CalibrationSettings& settings);

    // NamModelRef
    void process(const float* in, float* out, size_t n) override;
    float inputGain() const override { return inputGain_; }
    float outputGain() const override { return outputGain_; }
    void reset() override;

private:
    void recomputeGains();

    std::unique_ptr<NeuralAudio::NeuralModel> model_;
    std::string         path_;
    ModelInfo           info_;
    CalibrationSettings calibration_;
    float               quality_    = 1.0f;
    float               inputGain_  = 1.0f;
    float               outputGain_ = 1.0f;
};

} // namespace supr
