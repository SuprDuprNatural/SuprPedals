// NamModel.cpp — see NamModel.h.
//
// MIT license, (c) 2026 SuprPedals contributors.

#include "NamModel.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "NeuralAudio/NeuralModel.h"
#pragma GCC diagnostic pop

#include <filesystem>
#include <mutex>

namespace supr {

namespace {

// NeuralAudio's load-time knobs are process-wide statics, so two plugin
// instances loading at once can interleave inside CreateFromFile. PiPedal runs
// every plugin's worker on its own thread, so this is a real race, not a
// theoretical one.
std::mutex g_loadMutex;

// The sentinel we pin audioInputLevelDBu to. See the header: with this at 0,
// GetRecommendedInputDBAdjustment() returns exactly -modelInputLevelDBu.
constexpr float kInputLevelSentinel = 0.0f;

// NeuralAudio's stand-ins for metadata a model file does not carry. Matching
// one of these exactly is the only evidence available that the field was
// missing rather than genuinely this value.
constexpr float kDefaultInputLevelDbu = 12.0f;
constexpr float kDefaultLoudnessDb    = -18.0f;

} // namespace

NamModel::NamModel()  = default;
NamModel::~NamModel() = default;

bool NamModel::load(const std::string& path, size_t maxBlock, std::string& errorOut)
{
    model_.reset();
    info_ = ModelInfo();
    path_.clear();

    if (path.empty() || path == "null")
        return false;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        errorOut = "Model file not found: " + path;
        return false;
    }

    NeuralAudio::NeuralModel* raw = nullptr;
    try {
        std::lock_guard lock{g_loadMutex};
        NeuralAudio::NeuralModel::SetDefaultMaxAudioBufferSize(static_cast<int>(maxBlock));
        NeuralAudio::NeuralModel::SetAudioInputLevelDBu(kInputLevelSentinel);
        raw = NeuralAudio::NeuralModel::CreateFromFile(path);
    } catch (const std::exception& e) {
        errorOut = std::string("Failed to load model: ") + e.what();
        return false;
    }

    if (!raw) {
        errorOut = "Not a usable model file: " + path;
        return false;
    }

    model_.reset(raw);
    path_ = path;

    // Recover the model's own metadata by inverting the two derived figures
    // NeuralAudio exposes.
    info_.loaded          = true;
    info_.inputLevelDbu   = -model_->GetRecommendedInputDBAdjustment();
    info_.hasInputLevel   = info_.inputLevelDbu != kDefaultInputLevelDbu;
    info_.loudnessDb      = -18.0f - model_->GetRecommendedOutputDBAdjustment();
    info_.hasLoudness     = info_.loudnessDb != kDefaultLoudnessDb;
    info_.sampleRate      = model_->GetSampleRate();
    info_.receptiveField  = model_->GetReceptiveFieldSize();
    info_.hasQualityScale = model_->HasQualityScaling();
    info_.version         = model_->GetModelVersion();

    model_->SetMaxAudioBufferSize(static_cast<int>(maxBlock));
    if (info_.hasQualityScale)
        model_->SetQualityScaleFactor(quality_);

    recomputeGains();
    return true;
}

void NamModel::setQuality(float scale)
{
    quality_ = std::clamp(scale, 0.0f, 1.0f);
    if (model_ && info_.hasQualityScale)
        model_->SetQualityScaleFactor(quality_);
}

void NamModel::setCalibration(const CalibrationSettings& settings)
{
    calibration_ = settings;
    recomputeGains();
}

// Both trims are pure gain, so applying them around the inference call costs
// two multiplies per sample and — crucially for parallel paths — adds no
// delay and no filtering. Nothing here can push two slots out of alignment.
void NamModel::recomputeGains()
{
    if (!model_) {
        inputGain_  = 1.0f;
        outputGain_ = 1.0f;
        return;
    }

    // Scale the incoming signal so the model sees the level it was trained at.
    // Only when the model actually said what that level was — guessing here is
    // worse than not trying, because the guess is wrong by 18 dB on a typical
    // capture and it is wrong in the same direction every time.
    inputGain_ = (calibration_.input == InputCalibration::Calibrated && info_.hasInputLevel)
                     ? dbToLin(calibration_.instrumentDbu - info_.inputLevelDbu)
                     : 1.0f;

    // Bring every model to the same -18 LUFS reference so Blend crossfades
    // rather than jumping between two volumes.
    outputGain_ = (calibration_.output == OutputCalibration::Normalized && info_.hasLoudness)
                      ? dbToLin(-18.0f - info_.loudnessDb)
                      : 1.0f;
}

void NamModel::process(const float* in, float* out, size_t n)
{
    if (!model_) {
        std::fill(out, out + n, 0.0f);
        return;
    }
    // NeuralAudio takes a non-const input pointer but does not write to it.
    model_->Process(const_cast<float*>(in), out, n);
}

void NamModel::reset()
{
    if (model_)
        model_->Prewarm();
}

} // namespace supr
