// fuzz_probe.cpp — run the same measurements over FuzzDsp that nam_probe runs
// over a NAM capture, so the fit can be checked like against like.
//
// The analysis code here is deliberately the same shape as tools/nam_probe.cpp:
// integer-cycle windows, Goertzel per harmonic, no windowing to argue with. Any
// difference in the numbers is a difference in the DSP, not in the measurement.
//
//   fuzz_probe harmonics [sustain] [f0]
//   fuzz_probe response  [ampdb] [sustain] [tone]
//   fuzz_probe compress  [sustain] [f0]
//   fuzz_probe stats     <in.wav> [sustain] [blend]
//   fuzz_probe wav       <in.wav> <out.wav> [sustain] [tone] [octave] [blend]
//
// MIT license, (c) 2026 SuprPedals contributors.

#include "FuzzDsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int    kFs = 48000;

std::vector<float> readWav(const std::string& path, int& fsOut)
{
    std::vector<float> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw(size_t(len), 0);
    if (std::fread(raw.data(), 1, size_t(len), f) != size_t(len)) {
        std::fclose(f);
        return out;
    }
    std::fclose(f);

    auto rd32 = [&](size_t o) {
        return uint32_t(raw[o]) | (uint32_t(raw[o+1]) << 8)
             | (uint32_t(raw[o+2]) << 16) | (uint32_t(raw[o+3]) << 24);
    };
    auto rd16 = [&](size_t o) {
        return uint16_t(uint16_t(raw[o]) | (uint16_t(raw[o+1]) << 8));
    };
    if (raw.size() < 44 || std::memcmp(raw.data(), "RIFF", 4) != 0)
        return out;

    int channels = 1, bits = 16, format = 1;
    size_t pos = 12;
    while (pos + 8 <= raw.size()) {
        const char*    id = reinterpret_cast<const char*>(raw.data() + pos);
        const uint32_t sz = rd32(pos + 4);
        if (std::memcmp(id, "fmt ", 4) == 0) {
            format   = rd16(pos + 8);
            channels = rd16(pos + 10);
            fsOut    = int(rd32(pos + 12));
            bits     = rd16(pos + 22);
        } else if (std::memcmp(id, "data", 4) == 0) {
            const size_t bytes  = std::min(size_t(sz), raw.size() - pos - 8);
            const size_t stride = size_t(bits / 8) * size_t(channels);
            if (!stride)
                break;
            out.resize(bytes / stride);
            for (size_t i = 0; i < out.size(); ++i) {
                double acc = 0;
                for (int c = 0; c < channels; ++c) {
                    const size_t o = pos + 8 + i * stride + size_t(c * bits / 8);
                    if (format == 3 && bits == 32) {
                        float v; std::memcpy(&v, raw.data() + o, 4); acc += v;
                    } else if (bits == 16) {
                        acc += double(int16_t(rd16(o))) / 32768.0;
                    } else if (bits == 24) {
                        int32_t v = int32_t(uint32_t(raw[o]) << 8
                                          | uint32_t(raw[o+1]) << 16
                                          | uint32_t(raw[o+2]) << 24);
                        acc += double(v >> 8) / 8388608.0;
                    } else if (bits == 32) {
                        acc += double(int32_t(rd32(o))) / 2147483648.0;
                    }
                }
                out[i] = float(acc / channels);
            }
            break;
        }
        pos += 8 + sz + (sz & 1);
    }
    return out;
}

void writeWavF32(const std::string& path, const std::vector<float>& x, int fs)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t dataBytes = uint32_t(x.size() * 4);
    uint8_t hdr[44] = {0};
    auto put32 = [&](int o, uint32_t v) {
        hdr[o] = v & 0xff; hdr[o+1] = (v>>8)&0xff;
        hdr[o+2] = (v>>16)&0xff; hdr[o+3] = (v>>24)&0xff;
    };
    auto put16 = [&](int o, uint16_t v) { hdr[o] = v&0xff; hdr[o+1] = (v>>8)&0xff; };
    std::memcpy(hdr, "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(hdr + 8, "WAVEfmt ", 8);
    put32(16, 16); put16(20, 3); put16(22, 1);
    put32(24, uint32_t(fs)); put32(28, uint32_t(fs) * 4);
    put16(32, 4); put16(34, 32);
    std::memcpy(hdr + 36, "data", 4);
    put32(40, dataBytes);
    std::fwrite(hdr, 1, 44, f);
    std::fwrite(x.data(), 4, x.size(), f);
    std::fclose(f);
}

std::vector<float> resample(const std::vector<float>& in, int fsIn, int fsOut)
{
    if (fsIn == fsOut || in.empty()) return in;
    const double ratio = double(fsOut) / double(fsIn);
    const size_t n = size_t(double(in.size()) * ratio);
    const int taps = 32;
    std::vector<float> out(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / ratio;
        const long   c = long(std::floor(t));
        double acc = 0, wsum = 0;
        for (int k = -taps; k <= taps; ++k) {
            const long idx = c + k;
            if (idx < 0 || idx >= long(in.size())) continue;
            const double d = t - double(idx);
            const double s = (std::fabs(d) < 1e-9) ? 1.0
                             : std::sin(kPi * d) / (kPi * d);
            const double wn = (d + taps) / (2.0 * taps);
            if (wn < 0.0 || wn > 1.0) continue;
            const double w = 0.42 - 0.5*std::cos(2*kPi*wn) + 0.08*std::cos(4*kPi*wn);
            acc += in[size_t(idx)] * s * w;
            wsum += s * w;
        }
        out[i] = float(wsum > 1e-9 ? acc / wsum : 0.0);
    }
    return out;
}

double goertzel(const float* x, size_t n, double cycles)
{
    const double w = 2.0 * kPi * cycles / double(n);
    const double cw = std::cos(w), sw = std::sin(w), c = 2.0 * cw;
    double s0 = 0, s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) { s0 = double(x[i]) + c*s1 - s2; s2 = s1; s1 = s0; }
    const double re = s1 - s2*cw, im = s2*sw;
    return 2.0 * std::sqrt(re*re + im*im) / double(n);
}

double rmsOf(const std::vector<float>& x)
{
    double a = 0;
    for (float v : x) a += double(v)*double(v);
    return x.empty() ? 0.0 : std::sqrt(a / double(x.size()));
}
double peakOf(const std::vector<float>& x)
{
    double p = 0;
    for (float v : x) p = std::max(p, std::fabs(double(v)));
    return p;
}
double db(double v) { return 20.0 * std::log10(std::max(v, 1e-12)); }

struct Rig {
    supr::FuzzDsp dsp;
    void setup(float sustain, float tone, float octave, float blend)
    {
        dsp.init(kFs);
        dsp.setSustain(sustain);
        dsp.setTone(tone);
        dsp.setOctave(octave);
        dsp.setBlend(blend);
        dsp.setGate(-90.0f); // measurement: never gate
        dsp.setLevel(0.0f);
        dsp.reset();
    }
    std::vector<float> run(const std::vector<float>& in)
    {
        std::vector<float> out(in.size(), 0.0f);
        const uint32_t blk = 256;
        for (size_t i = 0; i < in.size(); i += blk) {
            const uint32_t n = uint32_t(std::min(size_t(blk), in.size() - i));
            dsp.process(in.data() + i, out.data() + i, n);
        }
        return out;
    }
};

std::vector<float> toneWindow(Rig& r, double wantHz, double amp, size_t n, double& fOut)
{
    const double cycles = std::round(wantHz * double(n) / kFs);
    const double freq   = cycles * double(kFs) / double(n);
    fOut = freq;
    const size_t total = n * 4;
    std::vector<float> in(total);
    for (size_t i = 0; i < total; ++i)
        in[i] = float(amp * std::sin(2.0 * kPi * freq * double(i) / kFs));
    std::vector<float> full = r.run(in);
    return std::vector<float>(full.end() - long(n), full.end());
}

void cmdHarmonics(float sustain, double f0)
{
    const size_t n = 32768;
    std::printf("# SuprFuzz harmonics, Sustain %.1f, f0 ~= %.1f Hz (dB rel H1)\n",
                sustain, f0);
    std::printf("# inDb   outRms   gain    H2     H3     H4     H5     H6     H7     H8     H9    THD%%\n");
    for (double inDb = -42.0; inDb <= 0.01; inDb += 6.0) {
        Rig r; r.setup(sustain, 5.0f, 0.0f, 1.0f);
        const double amp = std::pow(10.0, inDb / 20.0);
        double f;
        std::vector<float> y = toneWindow(r, f0, amp, n, f);
        const double c1 = f * double(n) / kFs;
        double h[10];
        for (int k = 1; k <= 9; ++k) h[k] = goertzel(y.data(), n, c1 * k);
        double hs = 0;
        for (int k = 2; k <= 9; ++k) hs += h[k]*h[k];
        std::printf("%6.1f %8.5f %7.2f", inDb, rmsOf(y), db(rmsOf(y)/(amp*0.70710678)));
        for (int k = 2; k <= 9; ++k)
            std::printf(" %6.1f", db(h[k] / std::max(h[1], 1e-12)));
        std::printf(" %7.2f\n", 100.0 * std::sqrt(hs) / std::max(h[1], 1e-12));
    }
}

void cmdResponse(double ampDb, float sustain, float tone)
{
    const size_t n = 32768;
    const double amp = std::pow(10.0, ampDb / 20.0);
    std::printf("# SuprFuzz fundamental gain vs frequency, %.0f dBFS in, Sustain %.1f Tone %.1f\n",
                ampDb, sustain, tone);
    std::printf("# freq     gainDb   h3Db   outRmsDb\n");
    static const double freqs[] = {20,25,31.5,40,50,63,80,100,125,160,200,250,
        315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,
        10000,12500,16000};
    for (double want : freqs) {
        Rig r; r.setup(sustain, tone, 0.0f, 1.0f);
        double f;
        std::vector<float> y = toneWindow(r, want, amp, n, f);
        const double c1 = f * double(n) / kFs;
        const double h1 = goertzel(y.data(), n, c1);
        const double h3 = (c1*3 < 0.45*n) ? goertzel(y.data(), n, c1*3) : 0;
        std::printf("%8.1f %8.2f %7.1f %9.2f\n", f, db(h1/amp),
                    db(h3/std::max(h1,1e-12)), db(rmsOf(y)));
    }
}

void cmdCompress(float sustain, double f0)
{
    const size_t n = 32768;
    std::printf("# SuprFuzz output vs input, Sustain %.1f at %.1f Hz\n", sustain, f0);
    std::printf("# inDb    outRmsDb  outPeakDb  gainDb   crestDb\n");
    for (double inDb = -48.0; inDb <= 0.01; inDb += 3.0) {
        Rig r; r.setup(sustain, 5.0f, 0.0f, 1.0f);
        const double amp = std::pow(10.0, inDb/20.0);
        double f;
        std::vector<float> y = toneWindow(r, f0, amp, n, f);
        const double rr = rmsOf(y), pp = peakOf(y);
        std::printf("%6.1f %10.2f %10.2f %8.2f %8.2f\n", inDb, db(rr), db(pp),
                    db(rr/(amp*0.70710678)), db(pp/std::max(rr,1e-12)));
    }
}

void cmdStats(const std::string& path, float sustain, float blend)
{
    int fsIn = 0;
    std::vector<float> in = readWav(path, fsIn);
    if (in.empty()) return;
    if (fsIn != kFs) in = resample(in, fsIn, kFs);
    Rig r; r.setup(sustain, 5.0f, 0.0f, blend);
    std::vector<float> out = r.run(in);

    auto bandRms = [&](const std::vector<float>& x, double lo, double hi) {
        const size_t N = 4096;
        double acc = 0; size_t frames = 0;
        for (size_t off = 0; off + N <= x.size(); off += N) {
            const size_t k0 = size_t(lo*N/kFs), k1 = size_t(hi*N/kFs);
            for (size_t k = k0; k <= k1 && k < N/2; ++k) {
                const double m = goertzel(x.data()+off, N, double(k));
                acc += m*m;
            }
            ++frames;
        }
        return frames ? std::sqrt(acc/double(frames)) : 0.0;
    };

    std::printf("# SuprFuzz on %s  (Sustain %.1f, Blend %.2f)\n", path.c_str(),
                sustain, blend);
    std::printf("             peak      rms     crest\n");
    std::printf("  dry   %9.2f %8.2f %8.2f\n", db(peakOf(in)), db(rmsOf(in)),
                db(peakOf(in)/std::max(rmsOf(in),1e-12)));
    std::printf("  wet   %9.2f %8.2f %8.2f\n", db(peakOf(out)), db(rmsOf(out)),
                db(peakOf(out)/std::max(rmsOf(out),1e-12)));
    std::printf("\n  band          dry      wet    delta\n");
    static const double edges[][2] = {{30,60},{60,120},{120,250},{250,500},
        {500,1000},{1000,2000},{2000,4000},{4000,8000},{8000,16000}};
    for (auto& e : edges) {
        const double d = bandRms(in, e[0], e[1]);
        const double w = bandRms(out, e[0], e[1]);
        std::printf("  %5.0f-%-5.0f %8.2f %8.2f %8.2f\n", e[0], e[1], db(d), db(w),
                    db(w)-db(d));
    }
}

void cmdWav(const std::string& inPath, const std::string& outPath, float sustain,
            float tone, float octave, float blend)
{
    int fsIn = 0;
    std::vector<float> in = readWav(inPath, fsIn);
    if (in.empty()) return;
    if (fsIn != kFs) in = resample(in, fsIn, kFs);
    Rig r; r.setup(sustain, tone, octave, blend);
    r.dsp.setGate(-70.0f);
    std::vector<float> out = r.run(in);
    writeWavF32(outPath, out, kFs);
    std::printf("wrote %s (%.2f s, peak %.2f dBFS, rms %.2f dBFS)\n",
                outPath.c_str(), double(out.size())/kFs, db(peakOf(out)),
                db(rmsOf(out)));
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: fuzz_probe <command> [args]\n"
            "  harmonics [sustain] [f0] | response [ampdb] [sustain] [tone] |\n"
            "  compress [sustain] [f0] | stats <in.wav> [sustain] [blend] |\n"
            "  wav <in> <out> [sustain] [tone] [octave] [blend]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    auto arg = [&](int i, double d) { return argc > i ? std::atof(argv[i]) : d; };

    if (cmd == "harmonics")
        cmdHarmonics(float(arg(2, 5.0)), arg(3, 82.41));
    else if (cmd == "response")
        cmdResponse(arg(2, -20.0), float(arg(3, 5.0)), float(arg(4, 5.0)));
    else if (cmd == "compress")
        cmdCompress(float(arg(2, 5.0)), arg(3, 82.41));
    else if (cmd == "stats" && argc >= 3)
        cmdStats(argv[2], float(arg(3, 5.0)), float(arg(4, 1.0)));
    else if (cmd == "wav" && argc >= 4)
        cmdWav(argv[2], argv[3], float(arg(4, 5.0)), float(arg(5, 5.0)),
               float(arg(6, 0.0)), float(arg(7, 1.0)));
    else {
        std::fprintf(stderr, "unknown or incomplete command: %s\n", cmd.c_str());
        return 2;
    }
    return 0;
}
