// SuprVowel: three moving formants over a protected bass foundation.
// No pitch detector, nonlinear excitation, allocation or transport latency.
#pragma once

#include "EnvFilterDsp.h" // Existing Simper SVF and envelope follower.
#include <array>
#include <limits>

namespace supr {
class VowelDsp {
public:
    struct Params {
        float vowel_a=0, vowel_b=2, mode=1, position=0, depth=1, rate=1;
        float sensitivity=6, release=180, throat=0, focus=.6f, mix=.8f, level=0;
    };
private:
    static constexpr double pi=3.14159265358979323846;
    // First three bass-voice centres from the Csound formant tables. Relative
    // levels/bandwidths are our broader instrument voicing, not a speech model.
    // OO, OH, AH, EH, EE. Interpolate in log frequency, never oscillator pitch.
    static constexpr double vowels[5][3] = {
        {350,600,2400}, {400,750,2400}, {600,1040,2250},
        {400,1620,2400}, {250,1750,2600}
    };
    static constexpr double weights[3]={1,.85,.35};
    Params target;
    double sr=48000, smooth=0, shapeSmooth=0, hpA=0, phase=0;
    double pos=0, envWeight=1, lfoWeight=0;
    // position, depth, rate, sensitivity gain, throat, focus, mix, output gain
    std::array<double,8> current{};
    std::array<double,3> logHz{}, g{}, gStep{};
    double k=1, kStep=0, hpX[2]{}, hpY[2]{};
    std::array<SvfMulti,3> filters;
    EnvFollower envelope;
    Biquad detector;
    float releaseMs=-1;
    unsigned tick=0;

    static double flush(double v) { return std::abs(v)<1e-24?0:v; }
    static float control(float v,float fallback,float lo,float hi) {
        return std::isfinite(v)?std::clamp(v,lo,hi):fallback;
    }
    static void approach(double& v,double goal,double amount) {
        v += amount*(goal-v);
        if(std::abs(goal-v)<1e-12) v=goal;
    }
    std::array<double,8> values() const {
        return {target.position,target.depth,target.rate,std::pow(10.,target.sensitivity/20.),
                target.throat,target.focus,target.mix,std::pow(10.,target.level/20.)};
    }
    double formant(unsigned j) const {
        const double a=vowels[unsigned(target.vowel_a)][j];
        const double b=vowels[unsigned(target.vowel_b)][j];
        return std::clamp(std::exp2(std::log2(a)+pos*std::log2(b/a)+current[4]/12.),
                          80.,std::min(6000.,sr*.4));
    }
    double protect(double x) {
        // Same explicit complementary protection as SuprPhase: H is two
        // 250 Hz highpasses, L=1-H. Output = x + Mix*H*(voice-x).
        // This is a voiced split, not an LR crossover or a phase-neutral bypass.
        for(unsigned j=0;j<2;++j) {
            const double y=hpA*(x-hpX[j])+(2*hpA-1)*hpY[j];
            hpX[j]=flush(x); hpY[j]=flush(y); x=y;
        }
        return x;
    }
public:
    void init(double sampleRate) {
        sr=std::isfinite(sampleRate)?std::clamp(sampleRate,8000.,192000.):48000.;
        smooth=1-std::exp(-1/(.020*sr));
        shapeSmooth=1-std::exp(-8/(.008*sr));
        hpA=1/(1+std::tan(pi*250/sr));
        detector.setHighpass(float(sr),30,.70710678f);
        for(auto& f:filters)f.init(float(sr));
        releaseMs=-1;
        setParams(target);
        reset();
    }
    void setParams(Params p) {
        target.vowel_a=std::round(control(p.vowel_a,0,0,4));
        target.vowel_b=std::round(control(p.vowel_b,2,0,4));
        target.mode=std::round(control(p.mode,1,0,2));
        target.position=control(p.position,0,0,1); target.depth=control(p.depth,1,0,1);
        target.rate=control(p.rate,1,.05f,8); target.sensitivity=control(p.sensitivity,6,-24,24);
        target.release=control(p.release,180,40,800); target.throat=control(p.throat,0,-6,6);
        target.focus=control(p.focus,.6f,0,1); target.mix=control(p.mix,.8f,0,1);
        target.level=control(p.level,0,-12,12);
        if(target.release!=releaseMs) {
            releaseMs=target.release;
            envelope.set(float(sr),5,releaseMs);
        }
    }
    void reset() {
        current=values(); phase=0; tick=0; pos=current[0];
        envWeight=target.mode==1?1:0; lfoWeight=target.mode==2?1:0;
        detector.reset(); envelope.reset();
        hpX[0]=hpX[1]=hpY[0]=hpY[1]=0;
        k=1/(2+8*current[5]); kStep=0;
        for(unsigned j=0;j<3;++j) {
            filters[j].reset(); logHz[j]=std::log2(formant(j));
            g[j]=std::tan(pi*std::exp2(logHz[j])/sr); gStep[j]=0;
        }
    }
    float position() const { return float(pos); }
    float frequency(unsigned j) const { return float(sr/pi*std::atan(g[j])); }

    void process(const float* in,float* out,uint32_t n) {
        const auto goal=values();
        for(uint32_t i=0;i<n;++i) {
            const float dry=std::isfinite(in[i])?in[i]:0.f;
            // Preserve every finite dry value at zero mix; protect only the
            // filter excitation from hostile inputs. Normal audio stays linear.
            const double x=dry, excitation=std::clamp(x,-16.,16.);
            for(unsigned j=0;j<8;++j)approach(current[j],goal[j],smooth);
            approach(envWeight,target.mode==1?1:0,smooth);
            approach(lfoWeight,target.mode==2?1:0,smooth);
            const double e=envelope.process(detector.process(float(excitation)))*current[3];
            envelope.env=float(flush(envelope.env));
            detector.z1=float(flush(detector.z1)); detector.z2=float(flush(detector.z2));
            const double lfo=.5-.5*std::cos(2*pi*phase);
            const double motion=envWeight*e/(e+.1)+lfoWeight*lfo;
            approach(pos,std::clamp(current[0]+current[1]*motion,0.,1.),smooth);
            phase+=current[2]/sr; if(phase>=1)phase-=1;
            if(tick==0) {
                for(unsigned j=0;j<3;++j) {
                    approach(logHz[j],std::log2(formant(j)),shapeSmooth);
                    gStep[j]=(std::tan(pi*std::exp2(logHz[j])/sr)-g[j])/8;
                }
                kStep=(1/(2+8*current[5])-k)/8;
            }
            tick=(tick+1)&7; k+=kStep;
            double voice=0;
            for(unsigned j=0;j<3;++j) {
                // Reuse SuprEnvelope's SVF; interpolate its integrator/damping
                // parameters per sample, not arbitrary biquad coefficients.
                // Peak-normalized BP (k*bp) keeps Focus from becoming gain.
                auto& f=filters[j]; g[j]+=gStep[j];
                f.k=float(k); f.a1=float(1/(1+g[j]*(g[j]+k)));
                f.a2=float(g[j]*f.a1); f.a3=float(g[j]*f.a2);
                f.process(float(excitation));
                f.ic1=float(flush(f.ic1)); f.ic2=float(flush(f.ic2));
                voice+=weights[j]*k*f.bp;
            }
            const double correction=protect(1.75*voice-x);
            const double mixed=current[6]==0?x:x+current[6]*correction;
            // Unity dry bypass is bit-exact, including signed zero. Saturation
            // here is only float overflow protection, not an audible limiter.
            out[i]=current[6]==0 && current[7]==1 ? dry : float(std::clamp(
                current[7]*mixed,-double(std::numeric_limits<float>::max()),
                double(std::numeric_limits<float>::max())));
        }
    }
};
} // namespace supr
