// SuprNam.cpp — LV2 wrapper around the SuprNAM graph.
//
// Three model slots, seven fixed routings, one blend. The DSP lives in
// src/NamDsp.h and the threading in NamEngine.h; this file is the plumbing —
// ports, the three model file properties, loading off the audio thread, and
// state.
//
// LOADING MODELS OFF THE AUDIO THREAD. Building a model reads a file,
// allocates a few megabytes and prewarms a network — hundreds of milliseconds,
// none of it allowed anywhere near Run(). Loads go out to the LV2 worker,
// which hands back a finished NamModel; the audio thread does nothing but
// swap a pointer. The model being replaced goes back to the worker to be
// destroyed, because freeing it is just as forbidden as building it.
//
// The swap waits for a block where NamEngine::beginBlock() reports the worker
// idle. In threaded mode the coordinator may be mid-inference on the model
// about to be replaced, and swapping under it would be a use-after-free. A
// load already took a fraction of a second; waiting one more 1.3 ms block for
// a safe moment costs nothing and removes the whole hazard.
//
// MIT license, (c) 2026 SuprPedals contributors.

#include "NamEngine.h"
#include "NamModel.h"

#include "lv2_plugin/Lv2Plugin.hpp"
#include "lv2_plugin/Lv2Ports.hpp"

#include <cstring>
#include <memory>
#include <string>

#define SUPRNAM_URI "https://suprduprnatural.github.io/supr-pedals/nam"
#define SUPRNAM_PREFIX SUPRNAM_URI "#"

using namespace lv2c::lv2_plugin;

namespace supr {

namespace {

constexpr size_t kDefaultMaxBlock = 512;
constexpr size_t kMaxPathLength   = 1024;

// Layout of the model-info notification sent to the custom UI. Kept as a flat
// float vector because that is what PutPatchProperty can carry, and because a
// fixed layout is easier to keep in step with the TypeScript side than a
// nested atom object.
enum ModelInfoOffset {
    kInfoVersion = 0,   // bump when the layout changes
    kInfoOverload,      // 1 while the threaded workers are missing deadlines
    kInfoHostRate,
    kInfoSlotBase,      // five floats per slot, from here
};
constexpr int kInfoFloatsPerSlot = 7;
enum SlotInfoOffset {
    kSlotLoaded = 0,
    kSlotHasQuality,
    kSlotSampleRate,
    kSlotLoudnessDb,
    kSlotInputLevelDbu,
    // Whether the model actually said, or whether the figure above is a
    // stand-in. The UI needs this to mark a slot "uncalibrated", the way TooB
    // NAM greys out its Input Calibration control — otherwise a calibration
    // setting that is silently doing nothing looks exactly like one that is
    // working.
    kSlotHasLoudness,
    kSlotHasInputLevel,
};
constexpr size_t kInfoFloatCount = kInfoSlotBase + kNumSlots * kInfoFloatsPerSlot;
constexpr float  kInfoVersionValue = 1.0f;

enum class MessageType { LoadModel, ModelLoaded, FreeModel, SetQuality };

struct Message {
    MessageType type;
};

struct LoadModelMessage : Message {
    int  slot = 0;
    char path[kMaxPathLength] = {0};
};

// The worker hands ownership of the finished model back through this.
struct ModelLoadedMessage : Message {
    int       slot  = 0;
    NamModel* model = nullptr;
};

struct FreeModelMessage : Message {
    NamModel* model = nullptr;
};

// Changing Slim rebuilds the model's network at a different width — a full
// construction and a large allocation — so it goes to the worker like a load
// does, never to the audio thread. NAM Core stages the rebuilt model and
// swaps it in atomically inside its own process(), so calling this while
// audio is running is exactly what it is designed for.
struct SetQualityMessage : Message {
    NamModel* model   = nullptr;
    float     quality = 0.0f;
};

} // namespace

class SuprNam final : public Lv2PluginWithState {
public:
    static constexpr const char* URI = SUPRNAM_URI;

    SuprNam(double rate, const char* bundlePath, const LV2_Feature* const* features)
        : Lv2PluginWithState(rate, bundlePath, features)
    {
        urids_.map(this);

        size_t maxBlock = kDefaultMaxBlock;
        if (GetBuffSizeOptions().maxBlockLength != BufSizeOptions::INVALID_VALUE)
            maxBlock = GetBuffSizeOptions().maxBlockLength;
        maxBlock_ = std::max<size_t>(maxBlock, 64);

        engine_.init(rate, maxBlock_);

        for (int i = 0; i < kNumSlots; ++i)
            AddPatchProperty(urids_.model[i]);
    }

    ~SuprNam() override { engine_.stop(); }

    // -----------------------------------------------------------------------
    // Ports
    // -----------------------------------------------------------------------

    enum PortIndex : uint32_t {
        PORT_ROUTING = 0,
        PORT_BLEND,
        PORT_MIX_LAW,
        PORT_INPUT_GAIN,
        PORT_INPUT_LEVEL_OUT,
        PORT_OUTPUT_GAIN,
        PORT_GATE,
        PORT_GATE_OUT,
        PORT_THREADED,
        PORT_OVERLOAD_OUT,
        PORT_LATENCY_OUT,

        PORT_SLOT_BASE, // nine per slot, A then B then C
        PORT_AUDIO_IN = PORT_SLOT_BASE + kNumSlots * 9,
        PORT_AUDIO_OUT,
        PORT_CONTROL_IN,
        PORT_NOTIFY_OUT,
        PORT_COUNT,
    };

    enum SlotPortOffset {
        SLOT_DRIVE = 0,
        SLOT_LEVEL,
        SLOT_SLIM,
        SLOT_POLARITY,
        SLOT_DELAY,
        SLOT_IN_HP,
        SLOT_IN_LP,
        SLOT_OUT_HP,
        SLOT_OUT_LP,
        SLOT_PORT_COUNT,
    };

    void ConnectPort(uint32_t port, void* data) override
    {
        if (port >= PORT_SLOT_BASE && port < PORT_AUDIO_IN) {
            const uint32_t rel  = port - PORT_SLOT_BASE;
            SlotPorts&     slot = slotPorts_[rel / SLOT_PORT_COUNT];
            switch (rel % SLOT_PORT_COUNT) {
            case SLOT_DRIVE:    slot.drive.SetData(data); break;
            case SLOT_LEVEL:    slot.level.SetData(data); break;
            case SLOT_SLIM:     slot.slim.SetData(data); break;
            case SLOT_POLARITY: slot.polarity.SetData(data); break;
            case SLOT_DELAY:    slot.delay.SetData(data); break;
            case SLOT_IN_HP:    slot.inHp.SetData(data); break;
            case SLOT_IN_LP:    slot.inLp.SetData(data); break;
            case SLOT_OUT_HP:   slot.outHp.SetData(data); break;
            case SLOT_OUT_LP:   slot.outLp.SetData(data); break;
            }
            return;
        }

        switch (port) {
        case PORT_ROUTING:        cRouting_.SetData(data); break;
        case PORT_BLEND:          cBlend_.SetData(data); break;
        case PORT_MIX_LAW:        cMixLaw_.SetData(data); break;
        case PORT_INPUT_GAIN:     cInputGain_.SetData(data); break;
        case PORT_INPUT_LEVEL_OUT: cInputLevelOut_.SetData(data); break;
        case PORT_OUTPUT_GAIN:    cOutputGain_.SetData(data); break;
        case PORT_GATE:           cGate_.SetData(data); break;
        case PORT_GATE_OUT:       cGateOut_.SetData(data); break;
        case PORT_THREADED:       cThreaded_.SetData(data); break;
        case PORT_OVERLOAD_OUT:   cOverloadOut_.SetData(data); break;
        case PORT_LATENCY_OUT:    cLatencyOut_.SetData(data); break;
        case PORT_AUDIO_IN:   audioIn_  = static_cast<const float*>(data); break;
        case PORT_AUDIO_OUT:  audioOut_ = static_cast<float*>(data); break;
        case PORT_CONTROL_IN:
            controlIn_ = static_cast<LV2_Atom_Sequence*>(data);
            SetAtomPortBuffers(controlIn_, notifyOut_);
            break;
        case PORT_NOTIFY_OUT:
            notifyOut_ = static_cast<LV2_Atom_Sequence*>(data);
            SetAtomPortBuffers(controlIn_, notifyOut_);
            break;
        }
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void Activate() override
    {
        engine_.start();
        engine_.reset();
        engine_.setThreaded(cThreaded_.GetValue() != 0.0f);
        readAllPorts(true);
        active_ = true;
        sendInfo_ = true;
    }

    void Deactivate() override
    {
        active_ = false;
        engine_.stop();
    }

    // The framework's RunOuter has already opened the notify sequence and
    // dispatched incoming patch messages by the time we get here.
    void Run(uint32_t n_samples) override
    {
        // Threaded is read every block, before anything else, and deliberately
        // outside the beginBlock() guard below. Everything else can wait for a
        // block where the workers are idle, but this one cannot: when they are
        // running late the graph is never reported safe to touch, and that is
        // precisely the moment the user is reaching for this switch to get out
        // of trouble. Gating it there made the switch stop responding exactly
        // when it was needed. setThreaded() drains any job in flight itself.
        if (cThreaded_.HasChanged())
            engine_.setThreaded(cThreaded_.GetValue() != 0.0f);

        // The engine tells us whether the workers are between jobs. Parameters
        // and model swaps may only be touched when they are; on the odd block
        // where they are not, everything simply waits one block.
        const bool mutable_ = engine_.beginBlock();
        if (mutable_) {
            applyPendingSwaps();
            readAllPorts(false);
        }

        engine_.process(audioIn_, audioOut_, n_samples);

        updateMeters(n_samples);
        if (sendInfo_) {
            sendInfo_ = false;
            sendModelInfo();
        }
    }

    // -----------------------------------------------------------------------
    // Patch properties: the three model files
    // -----------------------------------------------------------------------

    bool OnPatchPathSet(LV2_URID propertyUrid, const char* value) override
    {
        for (int i = 0; i < kNumSlots; ++i) {
            if (propertyUrid == urids_.model[i]) {
                requestLoad(i, value ? value : "");
                return true;
            }
        }
        return false;
    }

    const char* OnGetPatchPropertyValue(LV2_URID propertyUrid) override
    {
        for (int i = 0; i < kNumSlots; ++i)
            if (propertyUrid == urids_.model[i])
                return slotPaths_[i].c_str();
        return nullptr;
    }

    void OnPatchGetAll() override
    {
        for (int i = 0; i < kNumSlots; ++i)
            PutPatchPropertyPath(0, urids_.model[i], slotPaths_[i].c_str());
        sendInfo_ = true;
    }

    // -----------------------------------------------------------------------
    // Worker: model loading and freeing
    // -----------------------------------------------------------------------

    LV2_Worker_Status OnWork(LV2_Worker_Respond_Function respond,
                             LV2_Worker_Respond_Handle handle, uint32_t size,
                             const void* data) override
    {
        (void)size;
        const Message* message = static_cast<const Message*>(data);
        switch (message->type) {
        case MessageType::LoadModel: {
            const auto* load = static_cast<const LoadModelMessage*>(message);
            ModelLoadedMessage reply;
            reply.type = MessageType::ModelLoaded;
            reply.slot = load->slot;

            if (load->path[0] != '\0') {
                auto model = std::make_unique<NamModel>();
                std::string error;
                if (model->load(load->path, maxBlock_, error)) {
                    if (model->info().sampleRate > 0
                        && std::fabs(model->info().sampleRate - static_cast<float>(getRate())) > 1.0f) {
                        // Not fatal — TooB NAM ignores this too, and PiPedal
                        // runs at 48 kHz — but a model running at the wrong
                        // rate is transposed and worth saying out loud.
                        LogWarning("SuprNAM: model is %.0f Hz but the host is running at %.0f Hz.\n",
                                   double(model->info().sampleRate), getRate());
                    }
                    reply.model = model.release();
                } else {
                    LogError("SuprNAM: %s\n", error.c_str());
                }
            }
            respond(handle, sizeof(reply), &reply);
            break;
        }
        case MessageType::FreeModel: {
            const auto* free = static_cast<const FreeModelMessage*>(message);
            delete free->model;
            break;
        }
        case MessageType::SetQuality: {
            const auto* quality = static_cast<const SetQualityMessage*>(message);
            if (quality->model)
                quality->model->setQuality(quality->quality);
            break;
        }
        case MessageType::ModelLoaded:
            break;
        }
        return LV2_WORKER_SUCCESS;
    }

    // Called on the audio thread. Park the result; Run() installs it at the
    // next safe moment.
    LV2_Worker_Status OnWorkResponse(uint32_t size, const void* data) override
    {
        (void)size;
        const Message* message = static_cast<const Message*>(data);
        if (message->type != MessageType::ModelLoaded)
            return LV2_WORKER_SUCCESS;

        const auto* loaded = static_cast<const ModelLoadedMessage*>(message);
        if (loaded->slot < 0 || loaded->slot >= kNumSlots)
            return LV2_WORKER_SUCCESS;

        // A second load for the same slot can land before the first has been
        // installed. Push the superseded one straight to the free queue rather
        // than leaking it.
        if (pendingModels_[loaded->slot])
            scheduleFree(pendingModels_[loaded->slot]);
        pendingModels_[loaded->slot] = loaded->model;
        // A null model is a real outcome, not "nothing to do": it is how the
        // slot gets emptied, either because the user cleared it or because the
        // file would not load. Flag it so applyPendingSwaps does not skip it.
        pendingClear_[loaded->slot] = loaded->model == nullptr;
        hasPendingSwap_ = true;
        return LV2_WORKER_SUCCESS;
    }

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    LV2_State_Status OnSaveLv2State(LV2_State_Store_Function store, LV2_State_Handle handle,
                                    uint32_t flags, const LV2_Feature* const* features) override
    {
        (void)flags;
        for (int i = 0; i < kNumSlots; ++i) {
            const std::string path = mapPath(features, slotPaths_[i]);
            store(handle, urids_.model[i], path.c_str(), path.length() + 1,
                  urids_.atom__Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
        }
        return LV2_STATE_SUCCESS;
    }

    LV2_State_Status OnRestoreLv2State(LV2_State_Retrieve_Function retrieve,
                                       LV2_State_Handle handle, uint32_t flags,
                                       const LV2_Feature* const* features) override
    {
        (void)flags;
        for (int i = 0; i < kNumSlots; ++i) {
            size_t      size  = 0;
            uint32_t    type  = 0;
            uint32_t    vflags = 0;
            const void* data  = retrieve(handle, urids_.model[i], &size, &type, &vflags);
            if (!data)
                continue;
            if (type != urids_.atom__Path && type != urids_.atom__String)
                return LV2_STATE_ERR_BAD_TYPE;
            requestLoad(i, unmapPath(features, static_cast<const char*>(data)));
        }
        return LV2_STATE_SUCCESS;
    }

private:
    // -----------------------------------------------------------------------
    // Port reading
    // -----------------------------------------------------------------------

    void readAllPorts(bool force)
    {
        GraphParams p = engine_.graph().params();

        if (force || cRouting_.HasChanged())
            p.routing = static_cast<Routing>(
                std::clamp(static_cast<int>(cRouting_.GetValue()), 0, kNumRouting - 1));
        if (force || cBlend_.HasChanged())
            p.blend = cBlend_.GetValue();
        if (force || cMixLaw_.HasChanged())
            p.mixLaw = static_cast<MixLaw>(cMixLaw_.GetValue());
        if (force || cInputGain_.HasChanged())
            p.inputGainDb = cInputGain_.GetDb();
        if (force || cOutputGain_.HasChanged())
            p.outputGainDb = cOutputGain_.GetDb();
        if (force || cGate_.HasChanged())
            p.gateDb = cGate_.GetValue();

        for (int i = 0; i < kNumSlots; ++i) {
            SlotPorts&  ports = slotPorts_[i];
            SlotParams& sp    = p.slots[i];
            if (force || ports.drive.HasChanged())
                sp.driveDb = ports.drive.GetValue();
            if (force || ports.level.HasChanged())
                sp.levelDb = ports.level.GetValue();
            if (force || ports.polarity.HasChanged())
                sp.invert = ports.polarity.GetValue() != 0.0f;
            if (force || ports.delay.HasChanged())
                sp.delayMs = ports.delay.GetValue();
            if (force || ports.inHp.HasChanged())
                sp.inHpHz = ports.inHp.GetValue();
            if (force || ports.inLp.HasChanged())
                sp.inLpHz = ports.inLp.GetValue();
            if (force || ports.outHp.HasChanged())
                sp.outHpHz = ports.outHp.GetValue();
            if (force || ports.outLp.HasChanged())
                sp.outLpHz = ports.outLp.GetValue();

            if (force || ports.slim.HasChanged()) {
                const float quality = ports.slim.GetValue();
                slotQuality_[i] = quality;
                if (models_[i])
                    requestQuality(models_[i], quality);
            }
        }

        engine_.graph().setParams(p);

    }

    void updateMeters(uint32_t n_samples)
    {
        cGateOut_.SetValue(engine_.graph().gateReduction());
        cLatencyOut_.SetValue(static_cast<float>(engine_.latencySamples()));

        // Peak of the raw input, in dB, for the level meter.
        float peak = 0.0f;
        if (audioIn_)
            for (uint32_t i = 0; i < n_samples; ++i)
                peak = std::max(peak, std::fabs(audioIn_[i]));
        const float peakDb = linToDb(peak);
        inputLevelDb_ = std::max(peakDb, inputLevelDb_ - 0.5f * static_cast<float>(n_samples) / 48.0f);
        cInputLevelOut_.SetValue(std::clamp(inputLevelDb_, -35.0f, 20.0f));

        const bool overload = engine_.dropouts() != lastDropouts_;
        lastDropouts_ = engine_.dropouts();
        cOverloadOut_.SetValue(overload ? 1.0f : 0.0f);
        if (overload != lastOverload_) {
            lastOverload_ = overload;
            sendInfo_     = true;
        }
    }

    // -----------------------------------------------------------------------
    // Model swapping
    // -----------------------------------------------------------------------

    void requestLoad(int slot, const std::string& path)
    {
        slotPaths_[slot] = path;

        LoadModelMessage message;
        message.type = MessageType::LoadModel;
        message.slot = slot;
        std::strncpy(message.path, path.c_str(), kMaxPathLength - 1);
        message.path[kMaxPathLength - 1] = '\0';

        const LV2_Worker_Schedule* schedule = GetLv2WorkerSchedule();
        if (schedule)
            schedule->schedule_work(schedule->handle, sizeof(message), &message);
    }

    void applyPendingSwaps()
    {
        if (!hasPendingSwap_)
            return;
        hasPendingSwap_ = false;

        for (int i = 0; i < kNumSlots; ++i) {
            NamModel* incoming = pendingModels_[i];
            if (!incoming && !pendingClear_[i])
                continue;
            pendingModels_[i] = nullptr;
            pendingClear_[i]  = false;

            NamModel* outgoing = models_[i];
            if (incoming) {
                // Calibration is pure arithmetic and safe here. Quality is
                // not, so it goes back out to the worker.
                incoming->setCalibration(calibration_);
                requestQuality(incoming, slotQuality_[i]);
            }
            models_[i] = incoming;
            engine_.graph().setModel(i, incoming);

            if (outgoing)
                scheduleFree(outgoing);
        }
        sendInfo_ = true;
    }

    // Ordering matters and the worker preserves it: a model is only ever
    // freed by a message scheduled after any quality change already queued
    // for it, so the pointer here cannot be dangling by the time it is used.
    void requestQuality(NamModel* model, float quality)
    {
        SetQualityMessage message;
        message.type    = MessageType::SetQuality;
        message.model   = model;
        message.quality = quality;
        const LV2_Worker_Schedule* schedule = GetLv2WorkerSchedule();
        if (schedule)
            schedule->schedule_work(schedule->handle, sizeof(message), &message);
    }

    void scheduleFree(NamModel* model)
    {
        if (!model)
            return;
        FreeModelMessage message;
        message.type  = MessageType::FreeModel;
        message.model = model;
        const LV2_Worker_Schedule* schedule = GetLv2WorkerSchedule();
        if (schedule) {
            schedule->schedule_work(schedule->handle, sizeof(message), &message);
        } else {
            delete model; // no worker: only reachable outside a live host
        }
    }

    // -----------------------------------------------------------------------
    // UI notification
    // -----------------------------------------------------------------------

    void sendModelInfo()
    {
        float values[kInfoFloatCount] = {0.0f};
        values[kInfoVersion]  = kInfoVersionValue;
        values[kInfoOverload] = lastOverload_ ? 1.0f : 0.0f;
        values[kInfoHostRate] = static_cast<float>(getRate());

        for (int i = 0; i < kNumSlots; ++i) {
            float* slot = values + kInfoSlotBase + i * kInfoFloatsPerSlot;
            if (!models_[i])
                continue;
            const ModelInfo& info = models_[i]->info();
            slot[kSlotLoaded]        = 1.0f;
            slot[kSlotHasQuality]    = info.hasQualityScale ? 1.0f : 0.0f;
            slot[kSlotSampleRate]    = info.sampleRate;
            slot[kSlotLoudnessDb]    = info.loudnessDb;
            slot[kSlotInputLevelDbu] = info.inputLevelDbu;
            slot[kSlotHasLoudness]   = info.hasLoudness ? 1.0f : 0.0f;
            slot[kSlotHasInputLevel] = info.hasInputLevel ? 1.0f : 0.0f;
        }
        PutPatchProperty(0, urids_.modelInfo, kInfoFloatCount, values);
    }

    // -----------------------------------------------------------------------
    // Path mapping (abstract/absolute) for state
    // -----------------------------------------------------------------------

    std::string mapPath(const LV2_Feature* const* features, const std::string& path)
    {
        if (path.empty())
            return path;
        const auto* mapPathFeature =
            GetFeature<LV2_State_Map_Path>(features, LV2_STATE__mapPath);
        if (!mapPathFeature)
            return path;
        char* abstract = mapPathFeature->abstract_path(mapPathFeature->handle, path.c_str());
        if (!abstract)
            return path;
        std::string result{abstract};
        std::free(abstract);
        return result;
    }

    std::string unmapPath(const LV2_Feature* const* features, const char* path)
    {
        if (!path || path[0] == '\0')
            return std::string();
        const auto* mapPathFeature =
            GetFeature<LV2_State_Map_Path>(features, LV2_STATE__mapPath);
        if (!mapPathFeature)
            return std::string(path);
        char* absolute = mapPathFeature->absolute_path(mapPathFeature->handle, path);
        if (!absolute)
            return std::string(path);
        std::string result{absolute};
        std::free(absolute);
        return result;
    }

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    struct Urids {
        LV2_URID model[kNumSlots];
        LV2_URID modelInfo;
        LV2_URID atom__Path;
        LV2_URID atom__String;

        void map(SuprNam* plugin)
        {
            model[0]  = plugin->MapURI(SUPRNAM_PREFIX "modelA");
            model[1]  = plugin->MapURI(SUPRNAM_PREFIX "modelB");
            model[2]  = plugin->MapURI(SUPRNAM_PREFIX "modelC");
            modelInfo = plugin->MapURI(SUPRNAM_PREFIX "modelInfo");
            atom__Path   = plugin->MapURI(LV2_ATOM__Path);
            atom__String = plugin->MapURI(LV2_ATOM__String);
        }
    };

    struct SlotPorts {
        RangedInputPort drive{-30.0f, 20.0f};
        RangedInputPort level{-60.0f, 12.0f};
        RangedInputPort slim{0.0f, 1.0f};
        InputPort       polarity;
        RangedInputPort delay{0.0f, NamSlot::kMaxDelayMs};
        RangedInputPort inHp{10.0f, 1000.0f};
        RangedInputPort inLp{1000.0f, 20000.0f};
        RangedInputPort outHp{10.0f, 1000.0f};
        RangedInputPort outLp{1000.0f, 20000.0f};
    };

    Urids     urids_;
    NamEngine engine_;
    size_t    maxBlock_ = kDefaultMaxBlock;
    bool      active_   = false;

    EnumeratedInputPort cRouting_{kNumRouting};
    RangedInputPort     cBlend_{0.0f, 1.0f};
    EnumeratedInputPort cMixLaw_{2};
    RangedDbInputPort   cInputGain_{-40.0f, 40.0f};
    OutputPort          cInputLevelOut_{-35.0f};
    RangedDbInputPort   cOutputGain_{-40.0f, 40.0f};
    RangedInputPort     cGate_{-120.0f, 0.0f};
    OutputPort          cGateOut_;
    InputPort           cThreaded_;
    OutputPort          cOverloadOut_;
    OutputPort          cLatencyOut_;

    SlotPorts slotPorts_[kNumSlots];

    const float*       audioIn_   = nullptr;
    float*             audioOut_  = nullptr;
    LV2_Atom_Sequence* controlIn_ = nullptr;
    LV2_Atom_Sequence* notifyOut_ = nullptr;

    // Fixed, not exposed. Output normalisation to -18 LUFS is what makes Blend
    // a crossfade rather than a jump between two volumes, and there is no sane
    // reason to switch it off. Input calibration is pinned to Raw because it
    // only does anything for models that recorded the level they were trained
    // at — not one of the captures this was developed against does — so the
    // control was an invisible fudge factor at best. Drive, per slot, is the
    // honest replacement.
    const CalibrationSettings calibration_{InputCalibration::Raw,
                                           OutputCalibration::Normalized, -6.0f};
    NamModel*   models_[kNumSlots]        = {nullptr, nullptr, nullptr};
    NamModel*   pendingModels_[kNumSlots] = {nullptr, nullptr, nullptr};
    bool        pendingClear_[kNumSlots]  = {false, false, false};
    float       slotQuality_[kNumSlots]   = {0.0f, 0.0f, 0.0f};
    std::string slotPaths_[kNumSlots];
    bool        hasPendingSwap_ = false;
    bool        sendInfo_       = false;

    float    inputLevelDb_ = -35.0f;
    uint64_t lastDropouts_ = 0;
    bool     lastOverload_ = false;
};

static PluginRegistration<SuprNam> registration{SUPRNAM_URI};

} // namespace supr
