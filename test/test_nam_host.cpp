// A minimal LV2 host that loads the built suprnam.so and drives it.
//
// The graph tests in test_nam.cpp exercise the DSP directly and never see the
// LV2 layer, which means they cannot catch the one class of bug that will
// silently ruin the plugin in a real host: the port indices in ttl/suprnam.ttl
// drifting out of step with the PortIndex enum in src/nam/SuprNam.cpp. Nothing
// crashes when that happens — the Blend knob just turns out to be Output Gain.
//
// So this harness reads the port table out of the ttl and connects everything
// by symbol, then checks that each control does what its symbol says. If the
// two sides disagree, these fail.
//
// Also worth having: it is the only test that runs the worker, the state
// extension and the atom ports at all.

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <dlfcn.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;

static void check(bool ok, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::printf(ok ? "  PASS  " : "  FAIL  ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    if (!ok)
        ++failures;
}

// ---------------------------------------------------------------------------
// Host features
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> g_urids{""};

LV2_URID uridMap(LV2_URID_Map_Handle, const char* uri)
{
    for (size_t i = 1; i < g_urids.size(); ++i)
        if (g_urids[i] == uri)
            return static_cast<LV2_URID>(i);
    g_urids.emplace_back(uri);
    return static_cast<LV2_URID>(g_urids.size() - 1);
}

const char* uridUnmap(LV2_URID_Unmap_Handle, LV2_URID urid)
{
    return urid < g_urids.size() ? g_urids[urid].c_str() : nullptr;
}

// The plugin schedules work; we run it inline and feed the response straight
// back, which is a legal (if unusually prompt) host.
const LV2_Worker_Interface* g_workerInterface = nullptr;
LV2_Handle                  g_instance        = nullptr;

LV2_Worker_Status workerRespond(LV2_Worker_Respond_Handle, uint32_t size, const void* data)
{
    if (g_workerInterface && g_workerInterface->work_response)
        g_workerInterface->work_response(g_instance, size, data);
    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status scheduleWork(LV2_Worker_Schedule_Handle, uint32_t size, const void* data)
{
    if (g_workerInterface && g_workerInterface->work)
        g_workerInterface->work(g_instance, workerRespond, nullptr, size, data);
    return LV2_WORKER_SUCCESS;
}

int logPrintf(LV2_Log_Handle, LV2_URID, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::printf("         [plugin] ");
    const int result = std::vprintf(fmt, ap);
    va_end(ap);
    return result;
}

int logVprintf(LV2_Log_Handle, LV2_URID, const char* fmt, va_list ap)
{
    std::printf("         [plugin] ");
    return std::vprintf(fmt, ap);
}

// ---------------------------------------------------------------------------
// A very small ttl reader
// ---------------------------------------------------------------------------

// Enough Turtle to pull out "index N ... symbol S" pairs. Not a parser; it
// does not need to be, and a real one would be a dependency for no gain.
std::map<std::string, uint32_t> readPortTable(const std::string& ttlPath, bool& ok)
{
    std::map<std::string, uint32_t> ports;
    std::ifstream file(ttlPath);
    if (!file) {
        ok = false;
        return ports;
    }

    // Custom delimiter: the symbol pattern contains )" and would otherwise
    // close the raw string early.
    const std::regex indexRe(R"RX(lv2:index\s+(\d+)\s*;)RX");
    const std::regex symbolRe(R"RX(lv2:symbol\s+"([^"]+)"\s*;)RX");

    std::string line;
    bool        haveIndex = false;
    uint32_t    index     = 0;
    while (std::getline(file, line)) {
        std::smatch match;
        // A port block gives index then symbol, but the file properties also
        // carry lv2:index, so only pair an index with a symbol that follows it
        // before the next index.
        if (std::regex_search(line, match, indexRe)) {
            index     = static_cast<uint32_t>(std::stoul(match[1]));
            haveIndex = true;
        }
        if (haveIndex && std::regex_search(line, match, symbolRe)) {
            ports[match[1]] = index;
            haveIndex       = false;
        }
    }
    ok = true;
    return ports;
}

// The port contract, in the order the plugin's PortIndex enum declares it.
// Keeping the expected order here means a reshuffle on either side is caught,
// not just a missing symbol.
const char* const kExpectedPorts[] = {
    "routing", "blend", "mixLaw", "inputGain", "inputLevelOut", "outputGain",
    "gate", "gateOut", "threaded", "overloadOut", "latency",
    "driveA", "levelA", "slimA", "polarityA", "delayA", "inHpA", "inLpA", "outHpA", "outLpA",
    "driveB", "levelB", "slimB", "polarityB", "delayB", "inHpB", "inLpB", "outHpB", "outLpB",
    "driveC", "levelC", "slimC", "polarityC", "delayC", "inHpC", "inLpC", "outHpC", "outLpC",
    "in", "out", "control", "notify",
};
constexpr uint32_t kPortCount = sizeof(kExpectedPorts) / sizeof(kExpectedPorts[0]);

} // namespace

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    const std::string binary = argc > 1 ? argv[1] : "build/nam/suprnam.so";
    const std::string ttl    = argc > 2 ? argv[2] : "ttl/suprnam.ttl";

    std::printf("SuprNAM LV2 host checks\n  binary: %s\n  ttl:    %s\n",
                binary.c_str(), ttl.c_str());

    // --- the port contract --------------------------------------------------
    std::printf("\nPort table\n");
    bool ttlOk = false;
    const auto ports = readPortTable(ttl, ttlOk);
    check(ttlOk, "ttl is readable");
    if (!ttlOk)
        return 1;

    check(ports.size() == kPortCount, "ttl declares %u ports (found %zu)", kPortCount,
          ports.size());

    bool orderOk = true;
    for (uint32_t i = 0; i < kPortCount; ++i) {
        auto it = ports.find(kExpectedPorts[i]);
        if (it == ports.end()) {
            check(false, "ttl is missing port '%s'", kExpectedPorts[i]);
            orderOk = false;
        } else if (it->second != i) {
            check(false, "port '%s' is at index %u, expected %u", kExpectedPorts[i],
                  it->second, i);
            orderOk = false;
        }
    }
    if (orderOk)
        check(true, "all %u ports are where the C++ enum expects them", kPortCount);

    // --- load ---------------------------------------------------------------
    std::printf("\nPlugin loading\n");
    void* handle = dlopen(binary.c_str(), RTLD_NOW);
    check(handle != nullptr, "dlopen: %s", handle ? "ok" : dlerror());
    if (!handle)
        return 1;

    using DescriptorFn = const LV2_Descriptor* (*)(uint32_t);
    auto descriptorFn  = reinterpret_cast<DescriptorFn>(dlsym(handle, "lv2_descriptor"));
    check(descriptorFn != nullptr, "lv2_descriptor is exported");
    if (!descriptorFn)
        return 1;

    const LV2_Descriptor* descriptor = descriptorFn(0);
    check(descriptor != nullptr, "descriptor 0 exists");
    if (!descriptor)
        return 1;
    check(std::strcmp(descriptor->URI,
                      "https://suprduprnatural.github.io/supr-pedals/nam") == 0,
          "URI is %s", descriptor->URI);

    // --- instantiate --------------------------------------------------------
    LV2_URID_Map      map{nullptr, uridMap};
    LV2_URID_Unmap    unmap{nullptr, uridUnmap};
    LV2_Worker_Schedule schedule{nullptr, scheduleWork};
    LV2_Log_Log       log{nullptr, logPrintf, logVprintf};

    constexpr uint32_t kBlock = 64;
    int32_t            maxBlock = kBlock;
    LV2_Options_Option options[] = {
        {LV2_OPTIONS_INSTANCE, 0, uridMap(nullptr, LV2_BUF_SIZE__maxBlockLength),
         sizeof(int32_t), uridMap(nullptr, LV2_ATOM__Int), &maxBlock},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };

    LV2_Feature mapFeature{LV2_URID__map, &map};
    LV2_Feature unmapFeature{LV2_URID__unmap, &unmap};
    LV2_Feature scheduleFeature{LV2_WORKER__schedule, &schedule};
    LV2_Feature logFeature{LV2_LOG__log, &log};
    LV2_Feature optionsFeature{LV2_OPTIONS__options, options};
    const LV2_Feature* features[] = {&mapFeature,  &unmapFeature, &scheduleFeature,
                                     &logFeature,  &optionsFeature, nullptr};

    LV2_Handle instance = descriptor->instantiate(descriptor, 48000.0, "./", features);
    check(instance != nullptr, "instantiate");
    if (!instance)
        return 1;
    g_instance = instance;
    g_workerInterface =
        static_cast<const LV2_Worker_Interface*>(descriptor->extension_data(LV2_WORKER__interface));
    check(g_workerInterface != nullptr, "worker interface is available");
    check(descriptor->extension_data(LV2_STATE__interface) != nullptr,
          "state interface is available");

    // --- connect ------------------------------------------------------------
    std::vector<float> controls(kPortCount, 0.0f);
    std::vector<float> audioIn(kBlock, 0.0f);
    std::vector<float> audioOut(kBlock, 0.0f);
    std::vector<uint8_t> controlAtom(4096, 0);
    std::vector<uint8_t> notifyAtom(4096, 0);

    auto set = [&](const char* symbol, float value) { controls[ports.at(symbol)] = value; };
    auto get = [&](const char* symbol) { return controls[ports.at(symbol)]; };

    // Defaults that matter for the checks below.
    set("routing", 0.0f);
    set("blend", 0.5f);
    set("mixLaw", 0.0f);
    set("inputGain", 0.0f);
    set("outputGain", 0.0f);
    set("gate", -120.0f);
    set("threaded", 0.0f);
    for (const char* slot : {"A", "B", "C"}) {
        set(("drive" + std::string(slot)).c_str(), 0.0f);
        set(("level" + std::string(slot)).c_str(), 0.0f);
        set(("slim" + std::string(slot)).c_str(), 1.0f);
        set(("polarity" + std::string(slot)).c_str(), 0.0f);
        set(("delay" + std::string(slot)).c_str(), 0.0f);
        set(("inHp" + std::string(slot)).c_str(), 10.0f);
        set(("inLp" + std::string(slot)).c_str(), 20000.0f);
        set(("outHp" + std::string(slot)).c_str(), 10.0f);
        set(("outLp" + std::string(slot)).c_str(), 20000.0f);
    }

    for (uint32_t i = 0; i < kPortCount; ++i) {
        const std::string symbol = kExpectedPorts[i];
        if (symbol == "in")
            descriptor->connect_port(instance, i, audioIn.data());
        else if (symbol == "out")
            descriptor->connect_port(instance, i, audioOut.data());
        else if (symbol == "control")
            descriptor->connect_port(instance, i, controlAtom.data());
        else if (symbol == "notify")
            descriptor->connect_port(instance, i, notifyAtom.data());
        else
            descriptor->connect_port(instance, i, &controls[i]);
    }

    // Empty atom sequences.
    auto resetSequence = [&](std::vector<uint8_t>& buffer) {
        auto* sequence = reinterpret_cast<LV2_Atom_Sequence*>(buffer.data());
        sequence->atom.size = sizeof(LV2_Atom_Sequence_Body);
        sequence->atom.type = uridMap(nullptr, LV2_ATOM__Sequence);
        sequence->body.unit = 0;
        sequence->body.pad  = 0;
    };

    descriptor->activate(instance);

    // `measureFrom` is a fraction of the run: gate and gain checks want the
    // settled tail, not the ramp.
    auto runBlocks = [&](int blocks, float amplitude, float measureFrom = 0.5f) {
        // A real host leaves a block period between calls. Without that the
        // threaded workers never get any wall clock at all and the plugin
        // rightly reports an overload on every block, so pace this loop the
        // way a Pi would.
        const bool paced = controls[ports.at("threaded")] != 0.0f;
        const auto period = std::chrono::microseconds(
            static_cast<long>(1e6 * double(kBlock) / 48000.0));

        float peak = 0.0f;
        const int measureAfter = static_cast<int>(float(blocks) * measureFrom);
        for (int b = 0; b < blocks; ++b) {
            if (paced)
                std::this_thread::sleep_for(period);
            for (uint32_t i = 0; i < kBlock; ++i)
                audioIn[i] = amplitude
                           * std::sin(2.0f * float(M_PI) * 220.0f
                                      * float(b * int(kBlock) + int(i)) / 48000.0f);
            resetSequence(controlAtom);
            notifyAtom.assign(notifyAtom.size(), 0);
            reinterpret_cast<LV2_Atom_Sequence*>(notifyAtom.data())->atom.size =
                static_cast<uint32_t>(notifyAtom.size() - sizeof(LV2_Atom));
            descriptor->run(instance, kBlock);
            if (b >= measureAfter)
                for (uint32_t i = 0; i < kBlock; ++i)
                    peak = std::max(peak, std::fabs(audioOut[i]));
        }
        return peak;
    };

    // --- behaviour ----------------------------------------------------------
    std::printf("\nBehaviour with no models loaded\n");

    const float dryPeak = runBlocks(20, 0.25f);
    check(std::fabs(dryPeak - 0.25f) < 0.01f,
          "with nothing loaded the plugin is a wire (peak %.3f)", double(dryPeak));

    // Output Gain has to be Output Gain. If the ttl and the enum disagreed,
    // this would move something else.
    set("outputGain", -12.0f);
    const float quietPeak = runBlocks(40, 0.25f);
    check(std::fabs(20.0f * std::log10(quietPeak / dryPeak) + 12.0f) < 0.3f,
          "outputGain of -12 dB gives %.2f dB", double(20.0f * std::log10(quietPeak / dryPeak)));
    set("outputGain", 0.0f);

    set("inputGain", 6.0f);
    const float loudPeak = runBlocks(40, 0.25f);
    check(std::fabs(20.0f * std::log10(loudPeak / dryPeak) - 6.0f) < 0.3f,
          "inputGain of +6 dB gives %.2f dB", double(20.0f * std::log10(loudPeak / dryPeak)));
    set("inputGain", 0.0f);

    // The gate watches the input, so a loud signal must get through and a
    // quiet one must not. Give it long enough to get through the 50 ms hold
    // and the release ramp before measuring — an exponential release is still
    // audibly closing well after it is nominally "shut".
    set("gate", -20.0f);
    const float openPeak = runBlocks(60, 0.25f);
    check(openPeak > 0.2f, "gate open on a loud signal (peak %.3f)", double(openPeak));
    const float shutPeak = runBlocks(600, 0.001f, 0.9f);
    check(shutPeak < 1e-5f, "gate shut on a quiet signal (peak %.2e)", double(shutPeak));
    set("gate", -120.0f);
    runBlocks(20, 0.25f);

    std::printf("\nThreaded mode\n");
    check(get("latency") == 0.0f, "latency reports 0 when inline (%.0f)", double(get("latency")));
    set("threaded", 1.0f);
    const float threadedPeak = runBlocks(60, 0.25f);
    check(std::fabs(threadedPeak - 0.25f) < 0.01f,
          "still a wire when threaded (peak %.3f)", double(threadedPeak));
    check(get("latency") == float(kBlock), "latency reports one block (%.0f)",
          double(get("latency")));
    check(get("overloadOut") == 0.0f, "no overload reported");
    set("threaded", 0.0f);
    runBlocks(10, 0.25f);
    check(get("latency") == 0.0f, "latency back to 0 inline (%.0f)", double(get("latency")));

    // --- every routing survives, with and without threading -----------------
    std::printf("\nAll routings\n");
    for (int threaded = 0; threaded <= 1; ++threaded) {
        set("threaded", float(threaded));
        bool allFinite = true;
        for (int routing = 0; routing <= 6; ++routing) {
            set("routing", float(routing));
            runBlocks(20, 0.25f);
            for (uint32_t i = 0; i < kBlock; ++i)
                if (!std::isfinite(audioOut[i]))
                    allFinite = false;
        }
        check(allFinite, "%s: all seven routings produce finite output",
              threaded ? "threaded" : "inline");
    }
    set("threaded", 0.0f);
    set("routing", 0.0f);

    // --- state --------------------------------------------------------------
    std::printf("\nState\n");
    const auto* state =
        static_cast<const LV2_State_Interface*>(descriptor->extension_data(LV2_STATE__interface));
    std::map<LV2_URID, std::string> saved;
    auto store = [](LV2_State_Handle handle, uint32_t key, const void* value, size_t size,
                    uint32_t, uint32_t) {
        auto* target = static_cast<std::map<LV2_URID, std::string>*>(handle);
        (*target)[key] = std::string(static_cast<const char*>(value), size);
        return LV2_STATE_SUCCESS;
    };
    const LV2_State_Status saveStatus =
        state->save(instance, store, &saved, 0, features);
    check(saveStatus == LV2_STATE_SUCCESS, "state saves");
    check(saved.size() == 3, "three model paths stored (%zu)", saved.size());

    descriptor->deactivate(instance);
    descriptor->cleanup(instance);
    dlclose(handle);

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
