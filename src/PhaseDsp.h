#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace supr {
// Four identical first-order allpasses, positive linear wet/dry mix: two
// notches at atan(tan(pi*centre/sr)*tan(pi/8 or 3*pi/8))*sr/pi.
// Normalized lattice sections preserve energy even during coefficient motion.
class PhaseDsp {
public:
    struct Params {
        float rate=.3f, depth=.65f, centre=700, feedback=.25f;
        float mix=.5f, protect=1, mode=0, sensitivity=0;
    };
private:
    static constexpr double pi=3.14159265358979323846;
    Params target;
    double sr=48000, smooth=0, attack=0, release=0, hpA=0;
    std::array<double,8> current{};
    std::array<double,4> state{};
    double phase=0, envelope=0, previousWet=0, coefficient=0, coefficientStep=0;
    double hpX[2]{}, hpY[2]{};
    unsigned tick=0;
    static double clean(double v) { return std::abs(v)<1e-24?0:v; }
    static float control(float x,float fallback,float lo,float hi) {
        return std::isfinite(x)?std::clamp(x,lo,hi):fallback;
    }
    std::array<double,8> values() const {
        return {target.rate,target.depth,target.centre,target.feedback,
                target.mix,target.protect,target.mode,target.sensitivity};
    }
    double allpassCoefficient(double modulation) const {
        // +/- two octaves at full depth, limited below Nyquist at every rate.
        const double f=std::clamp(current[2]*std::exp2(2*current[1]*modulation),25.,std::min(10000.,sr*.22));
        const double g=std::tan(pi*f/sr);
        return (g-1)/(g+1);
    }
    double highpass(double x) {
        for(unsigned j=0;j<2;++j) {
            const double y=hpA*(x-hpX[j])+(2*hpA-1)*hpY[j];
            hpX[j]=clean(x); hpY[j]=clean(y); x=y;
        }
        return x;
    }
public:
    void init(double sampleRate) {
        sr=std::isfinite(sampleRate)&&sampleRate>=8000?sampleRate:48000;
        smooth=1-std::exp(-1/(.020*sr));
        attack=1-std::exp(-1/(.008*sr)); release=1-std::exp(-1/(.180*sr));
        hpA=1/(1+std::tan(pi*250/sr));
        reset();
    }
    void setParams(Params p) {
        target.rate=control(p.rate,.3f,.03f,5); target.depth=control(p.depth,.65f,0,1);
        target.centre=control(p.centre,700,100,2500); target.feedback=control(p.feedback,.25f,-.75f,.75f);
        target.mix=control(p.mix,.5f,0,1); target.protect=control(p.protect,1,0,1)>=.5f?1:0;
        target.mode=control(p.mode,0,0,1)>=.5f?1:0; target.sensitivity=control(p.sensitivity,0,-24,24);
    }
    void reset() {
        current=values(); state={}; phase=0; envelope=0; previousWet=0;
        hpX[0]=hpX[1]=hpY[0]=hpY[1]=0; tick=0; coefficientStep=0;
        coefficient=allpassCoefficient(-current[6]);
    }
    void process(const float* in,float* out,unsigned n) {
        const auto goal=values();
        for(unsigned i=0;i<n;++i) {
            const float dry=in[i];
            // Keep the finite dry reference intact, including above nominal
            // audio levels. Only the detector/allpass excitation is bounded.
            const float finiteDry=std::isfinite(dry)?dry:0.f;
            const double x=finiteDry, excitation=std::clamp(x,-16.,16.);
            for(unsigned j=0;j<8;++j) current[j]+=smooth*(goal[j]-current[j]);
            envelope=clean(envelope+(std::abs(excitation)>envelope?attack:release)*(std::abs(excitation)-envelope));
            // Fixed sample cadence, independent of host blocks. Interpolate the
            // lattice coefficient over 16 samples; expensive transcendentals run
            // at sr/16. Envelope compression gives quiet notes useful travel.
            if(tick==0) {
                const double e=envelope*std::exp2(current[7]/6.020599913279624);
                const double env=2*e/(e+.1)-1;
                const double lfo=std::sin(2*pi*phase);
                coefficientStep=(allpassCoefficient(lfo+current[6]*(env-lfo))-coefficient)/16;
            }
            tick=(tick+1)&15; coefficient+=coefficientStep;
            phase+=current[0]/sr; if(phase>=1)phase-=1;
            const double b=std::sqrt(std::max(0.,1-coefficient*coefficient));
            // |feedback| <= .75; injection scales by 1-|feedback| so fixed
            // coefficients have closed-loop gain <=1. The emergency +/-8 bound
            // acts only on the feedback sample, never on the normal dry path.
            double wet=(1-std::abs(current[3]))*excitation+current[3]*previousWet;
            for(auto& z:state) {
                const double y=coefficient*wet+b*z;
                z=clean(b*wet-coefficient*z); wet=y;
            }
            previousWet=std::clamp(wet,-8.,8.);
            const double difference=wet-x, high=highpass(difference);
            // H = two 250 Hz highpasses; L=1-H exactly. This is an explicitly
            // voiced complementary split, not LR: L*x + H*wet = x+H*(wet-x).
            // Protect morphs that correction smoothly, with no unmatched dry.
            const double correction=(1-current[5])*difference+current[5]*high;
            // Preserve signed zero and every finite float exactly at settled
            // zero mix; an automated move to zero still follows the smoother.
            out[i]=current[4]==0?finiteDry:float(x+current[4]*correction);
        }
    }
};
} // namespace supr
