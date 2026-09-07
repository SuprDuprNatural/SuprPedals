// Utilities owned by SuprEcho and SuprSpace. No allocation after init().
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
namespace supr { namespace timespace {
constexpr double pi = 3.14159265358979323846;
inline double finite(double v, double fallback, double lo, double hi) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}
inline double audio(double v) { return std::isfinite(v) ? std::clamp(v, -16.0, 16.0) : 0.0; }
inline double rate(double r) { return finite(r, 48000, 8000, 192000); }
inline double pole(double hz, double sr) { return std::exp(-2*pi*std::min(hz, sr*.45)/sr); }
inline double flush(double x) { return std::abs(x) < 1e-30 ? 0 : x; }
struct Lowpass {
    double z = 0;
    double tick(double x, double a) { z = flush((1-a)*x + a*z); return z; }
};
struct Highpass {
    Lowpass lp;
    double tick(double x, double a) { return x-lp.tick(x,a); }
};
struct Smooth {
    double value = 0;
    double tick(double target, double a) { value = target + a*(value-target); return value; }
};
struct Duck {
    double envelope = 0;
    double tick(double x, double amount, double attack, double release) {
        const double level = std::abs(x);
        envelope = flush(level + (level > envelope ? attack : release)*(envelope-level));
        // Amount=1 gives 20 dB attenuation at input amplitude 0.1 (-20 dBFS).
        return 1.0/(1.0+90.0*amount*envelope);
    }
};
class TapDelay {
    std::vector<double> data;
    size_t head = 0;
    double current = 1, next = 1, requested = 1;
    unsigned fade = 0, fadeLength = 1;
public:
    void init(double sr, double seconds) {
        data.assign(size_t(std::ceil(sr*seconds))+4, 0);
        fadeLength = std::max(1u, unsigned(sr*.02));
        reset(1);
    }
    void reset(double samples) {
        std::fill(data.begin(),data.end(),0); head=0; fade=0;
        current=next=requested=std::clamp(samples,1.0,double(data.size()-2));
    }
    void setTime(double samples) { requested=std::clamp(samples,1.0,double(data.size()-2)); }
    double readAt(double samples) const {
        double pos=double(head)-samples;
        if(pos<0) pos+=data.size();
        size_t i=size_t(pos); double f=pos-i;
        return data[i]*(1-f)+data[(i+1)%data.size()]*f;
    }
    double read() {
        // Finish each stationary-tap crossfade before taking the newest request.
        // Fast automation is coalesced, never restarts/discontinues an active fade.
        if(!fade && std::abs(requested-current)>1e-6) { next=requested; fade=fadeLength; }
        if(!fade) return readAt(current);
        double t=double(fadeLength-fade+1)/fadeLength;
        double y=(1-t)*readAt(current)+t*readAt(next);
        if(--fade==0) current=next;
        return y;
    }
    void write(double x) { data[head]=flush(x); if(++head==data.size()) head=0; }
    size_t bytes() const { return data.size()*sizeof(double); }
};
}}
