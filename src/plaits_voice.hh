// Plaits engines for the Ensemble Oscillator ("enosc3").
//
// The oscillators below are ported from Mutable Instruments Plaits
// (Copyright 2016 Emilie Gillet, MIT License): variable shape / variable
// saw (virtual analog), slope + waveshaper + wavefolder (waveshaping),
// 2-operator FM, grainlet + Z oscillators (formant/grain), clocked noise
// + state variable filters (filtered noise).
//
// They are plain float code with no dependency on stmlib, rendered one
// voice at a time on small blocks. Each enosc voice (16 voices x 2 for
// the scale crossfade) owns one VoiceState.

#pragma once

#include <cstdint>
#include "plaits_tables.hh"

namespace pl {

constexpr float kMaxFrequency = 0.25f;
constexpr float kMinFrequency = 0.000001f;
constexpr float kPi = 3.14159265358979f;

inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : x > hi ? hi : x;
}
inline float maxf(float a, float b) { return a > b ? a : b; }
inline float minf(float a, float b) { return a < b ? a : b; }
inline float absf(float a) { return a < 0.0f ? -a : a; }

// 2^x, about 0.01% error (0.2 cents): enough for intervals and filters
inline float Exp2(float x) {
  x = clampf(x, -60.0f, 60.0f);
  int i = static_cast<int>(x);
  if (x < static_cast<float>(i)) i--;
  float fr = x - static_cast<float>(i);
  float p = 1.0f + fr * (0.69606564f + fr * (0.22449434f + fr * 0.07944023f));
  union { float f; int32_t i; } u;
  u.i = (i + 127) << 23;
  return p * u.f;
}

inline float SemitonesToRatio(float s) { return Exp2(s * (1.0f / 12.0f)); }

// log2(x) for x > 0, about 0.005 error
inline float Log2(float x) {
  union { float f; int32_t i; } u;
  u.f = x;
  float e = static_cast<float>(((u.i >> 23) & 255) - 127);
  u.i = (u.i & 0x007FFFFF) | 0x3F800000;  // mantissa in [1, 2)
  float m = u.f;
  return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

inline float ThisBlepSample(float t) { return 0.5f * t * t; }
inline float NextBlepSample(float t) { t = 1.0f - t; return -0.5f * t * t; }
inline float NextIntegratedBlepSample(float t) {
  const float t1 = 0.5f * t;
  const float t2 = t1 * t1;
  const float t4 = t2 * t2;
  return 0.1875f - t1 + 1.5f * t2 - t4;
}
inline float ThisIntegratedBlepSample(float t) {
  return NextIntegratedBlepSample(1.0f - t);
}

// stmlib::ParameterInterpolator
struct Interp {
  float* state_;
  float value_;
  float increment_;
  Interp(float* state, float target, int size) :
    state_(state), value_(*state),
    increment_((target - *state) / static_cast<float>(size)) {}
  ~Interp() { *state_ = value_; }
  float Next() { value_ += increment_; return value_; }
  float subsample(float t) const { return value_ + increment_ * t; }
};

// Sine from the Plaits table, safe for phase >= 0 (wraps)
inline float Sine(float phase) {
  phase -= static_cast<float>(static_cast<int>(phase));
  float x = phase * 512.0f;
  int i = static_cast<int>(x);
  float fr = x - static_cast<float>(i);
  float a = plaits::lut_sine[i];
  float b = plaits::lut_sine[i + 1];
  return a + (b - a) * fr;
}

// Sine with phase modulation up to an index of 32
inline float SinePM(uint32_t phase, float pm) {
  const float max_uint32 = 4294967296.0f;
  const int max_index = 32;
  const float offset = float(max_index);
  const float scale = max_uint32 / float(max_index * 2);
  phase += static_cast<uint32_t>((pm + offset) * scale) * max_index * 2;
  uint32_t integral = phase >> (32 - 9);
  float fractional = static_cast<float>(phase << 9) / max_uint32;
  float a = plaits::lut_sine[integral];
  float b = plaits::lut_sine[integral + 1];
  return a + (b - a) * fractional;
}

inline float Interpolate(const float* table, float index, float size) {
  index *= size;
  int i = static_cast<int>(index);
  float fr = index - static_cast<float>(i);
  float a = table[i];
  float b = table[i + 1];
  return a + (b - a) * fr;
}

// on the int16 (x 32000) wavefolder tables
inline float InterpolateHermite16(const int16_t* table, float index, float size) {
  index *= size;
  int i = static_cast<int>(index);
  float fr = index - static_cast<float>(i);
  constexpr float k = 1.0f / 32000.0f;
  const float xm1 = static_cast<float>(table[i - 1]) * k;
  const float x0 = static_cast<float>(table[i + 0]) * k;
  const float x1 = static_cast<float>(table[i + 1]) * k;
  const float x2 = static_cast<float>(table[i + 2]) * k;
  const float c = (x1 - xm1) * 0.5f;
  const float v = x0 - x1;
  const float w = c + v;
  const float a = w + v + (x2 - x0) * 0.5f;
  const float b_neg = w + a;
  return (((a * fr) - b_neg) * fr + c) * fr + x0;
}

// tan(pi * f) approximation (stmlib FREQUENCY_ACCURATE)
inline float TanPi(float f) {
  f = clampf(f, 0.0f, 0.24f);
  const float a = 3.260e-01f * kPi * kPi * kPi;
  const float b = 1.823e-01f * kPi * kPi * kPi * kPi * kPi;
  float f2 = f * f;
  return f * (kPi + f2 * (a + b * f2));
}

// State variable filter (stmlib::Svf), coefficients set once per block
struct Svf {
  float g_, r_, h_;
  float s1_, s2_;
  void Init() { s1_ = s2_ = 0.0f; set_f_q(0.01f, 0.5f); }
  void set_f_q(float f, float q) {
    g_ = TanPi(f);
    r_ = 1.0f / q;
    h_ = 1.0f / (1.0f + r_ * g_ + g_ * g_);
  }
  // returns lp, writes bp and hp
  inline float Process(float in, float* bp_out, float* hp_out) {
    float hp = (in - r_ * s1_ - g_ * s1_ - s2_) * h_;
    float bp = g_ * hp + s1_;
    s1_ = g_ * hp + bp;
    float lp = g_ * bp + s2_;
    s2_ = g_ * bp + lp;
    *bp_out = bp;
    *hp_out = hp;
    return lp;
  }
  inline float ProcessLP(float in) {
    float hp = (in - r_ * s1_ - g_ * s1_ - s2_) * h_;
    float bp = g_ * hp + s1_;
    s1_ = g_ * hp + bp;
    float lp = g_ * bp + s2_;
    s2_ = g_ * bp + lp;
    return lp;
  }
  inline float ProcessBP(float in) {
    float hp = (in - r_ * s1_ - g_ * s1_ - s2_) * h_;
    float bp = g_ * hp + s1_;
    s1_ = g_ * hp + bp;
    float lp = g_ * bp + s2_;
    s2_ = g_ * bp + lp;
    return bp;
  }
};

struct OnePoleHp {
  float lp_;
  void Init() { lp_ = 0.0f; }
  inline float Process(float x, float coef) {
    lp_ += coef * (x - lp_);
    return x - lp_;
  }
};

// ---------------------------------------------------------------------
// Oscillators

// plaits::VariableShapeOscillator (triangle - saw - square, PW, hard sync)
struct VariableShapeOscillator {
  float master_phase_, slave_phase_, next_sample_, previous_pw_;
  bool high_;
  float master_frequency_, slave_frequency_, pw_, waveshape_;

  void Init() {
    master_phase_ = 0.0f;
    slave_phase_ = 0.0f;
    next_sample_ = 0.0f;
    previous_pw_ = 0.5f;
    high_ = false;
    master_frequency_ = 0.0f;
    slave_frequency_ = 0.01f;
    pw_ = 0.5f;
    waveshape_ = 0.0f;
  }

  void set_master_phase(float p) { master_phase_ = p; }

  template<bool enable_sync>
  void Render(float master_frequency, float frequency, float pw,
              float waveshape, float* out, int size) {
    if (master_frequency >= kMaxFrequency) master_frequency = kMaxFrequency;
    if (frequency >= kMaxFrequency) frequency = kMaxFrequency;
    if (frequency >= 0.25f) {
      pw = 0.5f;
    } else {
      pw = clampf(pw, frequency * 2.0f, 1.0f - 2.0f * frequency);
    }

    Interp master_fm(&master_frequency_, master_frequency, size);
    Interp fm(&slave_frequency_, frequency, size);
    Interp pwm(&pw_, pw, size);
    Interp waveshape_modulation(&waveshape_, waveshape, size);

    float next_sample = next_sample_;

    while (size--) {
      bool reset = false;
      bool transition_during_reset = false;
      float reset_time = 0.0f;

      float this_sample = next_sample;
      next_sample = 0.0f;

      const float master_freq = master_fm.Next();
      const float slave_frequency = fm.Next();
      const float pw = pwm.Next();
      const float waveshape = waveshape_modulation.Next();

      const float square_amount = maxf(waveshape - 0.5f, 0.0f) * 2.0f;
      const float triangle_amount = maxf(1.0f - waveshape * 2.0f, 0.0f);

      const float slope_up = 1.0f / (pw);
      const float slope_down = 1.0f / (1.0f - pw);

      if (enable_sync) {
        master_phase_ += master_freq;
        if (master_phase_ >= 1.0f) {
          master_phase_ -= 1.0f;
          reset_time = master_phase_ / master_freq;

          float slave_phase_at_reset = slave_phase_ +
            (1.0f - reset_time) * slave_frequency;
          reset = true;
          if (slave_phase_at_reset >= 1.0f) {
            slave_phase_at_reset -= 1.0f;
            transition_during_reset = true;
          }
          if (!high_ && slave_phase_at_reset >= pw) {
            transition_during_reset = true;
          }
          float value = ComputeNaiveSample(slave_phase_at_reset, pw,
                                           slope_up, slope_down,
                                           triangle_amount, square_amount);
          this_sample -= value * ThisBlepSample(reset_time);
          next_sample -= value * NextBlepSample(reset_time);
        }
      }

      slave_phase_ += slave_frequency;
      while (transition_during_reset || !reset) {
        if (!high_) {
          if (slave_phase_ < pw) break;
          float t = (slave_phase_ - pw) / (previous_pw_ - pw + slave_frequency);
          float triangle_step = (slope_up + slope_down) * slave_frequency;
          triangle_step *= triangle_amount;

          this_sample += square_amount * ThisBlepSample(t);
          next_sample += square_amount * NextBlepSample(t);
          this_sample -= triangle_step * ThisIntegratedBlepSample(t);
          next_sample -= triangle_step * NextIntegratedBlepSample(t);
          high_ = true;
        }

        if (high_) {
          if (slave_phase_ < 1.0f) break;
          slave_phase_ -= 1.0f;
          float t = slave_phase_ / slave_frequency;
          float triangle_step = (slope_up + slope_down) * slave_frequency;
          triangle_step *= triangle_amount;

          this_sample -= (1.0f - triangle_amount) * ThisBlepSample(t);
          next_sample -= (1.0f - triangle_amount) * NextBlepSample(t);
          this_sample += triangle_step * ThisIntegratedBlepSample(t);
          next_sample += triangle_step * NextIntegratedBlepSample(t);
          high_ = false;
        }
      }

      if (enable_sync && reset) {
        slave_phase_ = reset_time * slave_frequency;
        high_ = false;
      }

      next_sample += ComputeNaiveSample(slave_phase_, pw, slope_up, slope_down,
                                        triangle_amount, square_amount);
      previous_pw_ = pw;
      *out++ = (2.0f * this_sample - 1.0f);
    }

    next_sample_ = next_sample;
  }

  inline float ComputeNaiveSample(float phase, float pw, float slope_up,
                                  float slope_down, float triangle_amount,
                                  float square_amount) const {
    float saw = phase;
    float square = phase < pw ? 0.0f : 1.0f;
    float triangle = phase < pw
      ? phase * slope_up
      : 1.0f - (phase - pw) * slope_down;
    saw += (square - saw) * square_amount;
    saw += (triangle - saw) * triangle_amount;
    return saw;
  }
};

// plaits::VariableSawOscillator (notched saw - triangle)
struct VariableSawOscillator {
  static constexpr float kNotchDepth = 0.2f;
  float phase_, next_sample_, previous_pw_;
  bool high_;
  float frequency_, pw_, waveshape_;

  void Init() {
    phase_ = 0.0f;
    next_sample_ = 0.0f;
    previous_pw_ = 0.5f;
    high_ = false;
    frequency_ = 0.01f;
    pw_ = 0.5f;
    waveshape_ = 0.0f;
  }

  void Render(float frequency, float pw, float waveshape, float* out, int size) {
    if (frequency >= kMaxFrequency) frequency = kMaxFrequency;
    if (frequency >= 0.25f) {
      pw = 0.5f;
    } else {
      pw = clampf(pw, frequency * 2.0f, 1.0f - 2.0f * frequency);
    }

    Interp fm(&frequency_, frequency, size);
    Interp pwm(&pw_, pw, size);
    Interp waveshape_modulation(&waveshape_, waveshape, size);

    float next_sample = next_sample_;

    while (size--) {
      float this_sample = next_sample;
      next_sample = 0.0f;

      const float freq = fm.Next();
      const float pw = pwm.Next();
      const float waveshape = waveshape_modulation.Next();
      const float triangle_amount = waveshape;
      const float notch_amount = 1.0f - waveshape;
      const float slope_up = 1.0f / (pw);
      const float slope_down = 1.0f / (1.0f - pw);

      phase_ += freq;

      if (!high_ && phase_ >= pw) {
        const float triangle_step = (slope_up + slope_down) * freq * triangle_amount;
        const float notch = (kNotchDepth + 1.0f - pw) * notch_amount;
        const float t = (phase_ - pw) / (previous_pw_ - pw + freq);
        this_sample += notch * ThisBlepSample(t);
        next_sample += notch * NextBlepSample(t);
        this_sample -= triangle_step * ThisIntegratedBlepSample(t);
        next_sample -= triangle_step * NextIntegratedBlepSample(t);
        high_ = true;
      } else if (phase_ >= 1.0f) {
        phase_ -= 1.0f;
        const float triangle_step = (slope_up + slope_down) * freq * triangle_amount;
        const float notch = (kNotchDepth + 1.0f) * notch_amount;
        const float t = phase_ / freq;
        this_sample -= notch * ThisBlepSample(t);
        next_sample -= notch * NextBlepSample(t);
        this_sample += triangle_step * ThisIntegratedBlepSample(t);
        next_sample += triangle_step * NextIntegratedBlepSample(t);
        high_ = false;
      }

      float notch_saw = phase_ < pw ? phase_ : 1.0f + kNotchDepth;
      float triangle = phase_ < pw
        ? phase_ * slope_up
        : 1.0f - (phase_ - pw) * slope_down;
      next_sample += notch_saw * notch_amount + triangle * triangle_amount;
      previous_pw_ = pw;

      *out++ = (2.0f * this_sample - 1.0f) / (1.0f + kNotchDepth);
    }

    next_sample_ = next_sample;
  }
};

// plaits::Oscillator, OSCILLATOR_SHAPE_SLOPE only
struct SlopeOscillator {
  float phase_, next_sample_;
  bool high_;
  float frequency_, pw_;

  void Init() {
    phase_ = 0.5f;
    next_sample_ = 0.0f;
    high_ = true;
    frequency_ = 0.001f;
    pw_ = 0.5f;
  }

  void Render(float frequency, float pw, float* out, int size) {
    frequency = clampf(frequency, kMinFrequency, kMaxFrequency);
    pw = clampf(pw, frequency * 2.0f, 1.0f - 2.0f * frequency);

    Interp fm(&frequency_, frequency, size);
    Interp pwm(&pw_, pw, size);
    float next_sample = next_sample_;

    while (size--) {
      float this_sample = next_sample;
      next_sample = 0.0f;
      const float freq = fm.Next();
      const float pw = pwm.Next();
      phase_ += freq;

      const float slope_up = 1.0f / (pw);
      const float slope_down = 1.0f / (1.0f - pw);
      if (high_ ^ (phase_ < pw)) {
        float t = (phase_ - pw) / freq;
        float discontinuity = (slope_up + slope_down) * freq;
        this_sample -= ThisIntegratedBlepSample(t) * discontinuity;
        next_sample -= NextIntegratedBlepSample(t) * discontinuity;
        high_ = phase_ < pw;
      }
      if (phase_ >= 1.0f) {
        phase_ -= 1.0f;
        float t = phase_ / freq;
        float discontinuity = (slope_up + slope_down) * freq;
        this_sample += ThisIntegratedBlepSample(t) * discontinuity;
        next_sample += NextIntegratedBlepSample(t) * discontinuity;
        high_ = true;
      }
      next_sample += high_
        ? phase_ * slope_up
        : 1.0f - (phase_ - pw) * slope_down;
      *out++ = 2.0f * this_sample - 1.0f;
    }
    next_sample_ = next_sample;
  }
};

// plaits::GrainletOscillator
struct GrainletOscillator {
  float carrier_phase_, formant_phase_, next_sample_;
  float carrier_frequency_, formant_frequency_, carrier_shape_, carrier_bleed_;

  void Init() {
    carrier_phase_ = 0.0f;
    formant_phase_ = 0.0f;
    next_sample_ = 0.0f;
    carrier_frequency_ = 0.0f;
    formant_frequency_ = 0.0f;
    carrier_shape_ = 0.0f;
    carrier_bleed_ = 0.0f;
  }

  void Render(float carrier_frequency, float formant_frequency,
              float carrier_shape, float carrier_bleed, float* out, int size) {
    if (carrier_frequency >= kMaxFrequency * 0.5f)
      carrier_frequency = kMaxFrequency * 0.5f;
    if (formant_frequency >= kMaxFrequency)
      formant_frequency = kMaxFrequency;

    Interp carrier_frequency_modulation(&carrier_frequency_, carrier_frequency, size);
    Interp formant_frequency_modulation(&formant_frequency_, formant_frequency, size);
    Interp carrier_shape_modulation(&carrier_shape_, carrier_shape, size);
    Interp carrier_bleed_modulation(&carrier_bleed_, carrier_bleed, size);

    float next_sample = next_sample_;

    while (size--) {
      float this_sample = next_sample;
      next_sample = 0.0f;

      const float f0 = carrier_frequency_modulation.Next();
      const float f1 = formant_frequency_modulation.Next();

      carrier_phase_ += f0;
      if (carrier_phase_ >= 1.0f) {
        carrier_phase_ -= 1.0f;
        float reset_time = carrier_phase_ / f0;
        float before = Grainlet(
            1.0f,
            formant_phase_ + (1.0f - reset_time) * f1,
            carrier_shape_modulation.subsample(1.0f - reset_time),
            carrier_bleed_modulation.subsample(1.0f - reset_time));
        float after = Grainlet(
            0.0f, 0.0f,
            carrier_shape_modulation.subsample(1.0f),
            carrier_bleed_modulation.subsample(1.0f));
        float discontinuity = after - before;
        this_sample += discontinuity * ThisBlepSample(reset_time);
        next_sample += discontinuity * NextBlepSample(reset_time);
        formant_phase_ = reset_time * f1;
      } else {
        formant_phase_ += f1;
        if (formant_phase_ >= 1.0f) formant_phase_ -= 1.0f;
      }

      next_sample += Grainlet(carrier_phase_, formant_phase_,
                              carrier_shape_modulation.Next(),
                              carrier_bleed_modulation.Next());
      *out++ = this_sample;
    }
    next_sample_ = next_sample;
  }

  static inline float Carrier(float phase, float shape) {
    shape *= 3.0f;
    int shape_integral = static_cast<int>(shape);
    float shape_fractional = shape - static_cast<float>(shape_integral);
    float t = 1.0f - shape_fractional;

    if (shape_integral == 0) {
      phase = phase * (1.0f + t * t * t * 15.0f);
      if (phase >= 1.0f) phase = 1.0f;
      phase += 0.75f;
    } else if (shape_integral == 1) {
      float breakpoint = 0.001f + 0.499f * t * t * t;
      if (phase < breakpoint) {
        phase *= (0.5f / breakpoint);
      } else {
        phase = 0.5f + (phase - breakpoint) * 0.5f / (1.0f - breakpoint);
      }
      phase += 0.75f;
    } else {
      t = 1.0f - t;
      phase = 0.25f + phase * (0.5f + t * t * t * 14.5f);
      if (phase >= 0.75f) phase = 0.75f;
    }
    return (Sine(phase) + 1.0f) * 0.25f;
  }

  static inline float Grainlet(float carrier_phase, float formant_phase,
                               float shape, float bleed) {
    float carrier = Carrier(carrier_phase, shape);
    float formant = Sine(formant_phase);
    return carrier * (formant + bleed) / (1.0f + bleed);
  }
};

// plaits::ZOscillator
struct ZOscillator {
  float carrier_phase_, discontinuity_phase_, formant_phase_, next_sample_;
  float carrier_frequency_, formant_frequency_, carrier_shape_, mode_;

  void Init() {
    carrier_phase_ = 0.0f;
    discontinuity_phase_ = 0.0f;
    formant_phase_ = 0.0f;
    next_sample_ = 0.0f;
    carrier_frequency_ = 0.0f;
    formant_frequency_ = 0.0f;
    carrier_shape_ = 0.0f;
    mode_ = 0.0f;
  }

  void Render(float carrier_frequency, float formant_frequency,
              float carrier_shape, float mode, float* out, int size) {
    if (carrier_frequency >= kMaxFrequency * 0.5f)
      carrier_frequency = kMaxFrequency * 0.5f;
    if (formant_frequency >= kMaxFrequency)
      formant_frequency = kMaxFrequency;

    Interp carrier_frequency_modulation(&carrier_frequency_, carrier_frequency, size);
    Interp formant_frequency_modulation(&formant_frequency_, formant_frequency, size);
    Interp carrier_shape_modulation(&carrier_shape_, carrier_shape, size);
    Interp mode_modulation(&mode_, mode, size);

    float next_sample = next_sample_;

    while (size--) {
      float this_sample = next_sample;
      next_sample = 0.0f;

      const float f0 = carrier_frequency_modulation.Next();
      const float f1 = formant_frequency_modulation.Next();

      discontinuity_phase_ += 2.0f * f0;
      carrier_phase_ += f0;

      if (discontinuity_phase_ >= 1.0f) {
        discontinuity_phase_ -= 1.0f;
        float reset_time = discontinuity_phase_ / (2.0f * f0);

        float carrier_phase_before = carrier_phase_ >= 1.0f ? 1.0f : 0.5f;
        float carrier_phase_after = carrier_phase_ >= 1.0f ? 0.0f : 0.5f;
        float before = Z(carrier_phase_before, 1.0f,
                         formant_phase_ + (1.0f - reset_time) * f1,
                         carrier_shape_modulation.subsample(1.0f - reset_time),
                         mode_modulation.subsample(1.0f - reset_time));
        float after = Z(carrier_phase_after, 0.0f, 0.0f,
                        carrier_shape_modulation.subsample(1.0f),
                        mode_modulation.subsample(1.0f));
        float discontinuity = after - before;
        this_sample += discontinuity * ThisBlepSample(reset_time);
        next_sample += discontinuity * NextBlepSample(reset_time);
        formant_phase_ = reset_time * f1;

        if (carrier_phase_ > 1.0f) {
          carrier_phase_ = discontinuity_phase_ * 0.5f;
        }
      } else {
        formant_phase_ += f1;
        if (formant_phase_ >= 1.0f) formant_phase_ -= 1.0f;
      }

      if (carrier_phase_ >= 1.0f) carrier_phase_ -= 1.0f;

      next_sample += Z(carrier_phase_, discontinuity_phase_, formant_phase_,
                       carrier_shape_modulation.Next(), mode_modulation.Next());
      *out++ = this_sample;
    }
    next_sample_ = next_sample;
  }

  static inline float Z(float c, float d, float f, float shape, float mode) {
    float ramp_down = 0.5f * (1.0f + Sine(0.5f * d + 0.25f));

    float offset;
    float phase_shift;
    if (mode < 0.333f) {
      offset = 1.0f;
      phase_shift = 0.25f + mode * 1.50f;
    } else if (mode < 0.666f) {
      phase_shift = 0.7495f - (mode - 0.33f) * 0.75f;
      offset = -Sine(phase_shift);
    } else {
      phase_shift = 0.7495f - (mode - 0.33f) * 0.75f;
      offset = 0.001f;
    }

    float discontinuity = Sine(f + phase_shift);
    float contour;
    if (shape < 0.5f) {
      shape *= 2.0f;
      if (c >= 0.5f) ramp_down *= shape;
      contour = 1.0f + (Sine(c + 0.25f) - 1.0f) * shape;
    } else {
      contour = Sine(c + shape * 0.5f);
    }
    return (ramp_down * (offset + discontinuity) - offset) * contour;
  }
};

// plaits::ClockedNoise, with its own random generator (one per voice)
struct ClockedNoise {
  float phase_, sample_, next_sample_, frequency_;
  uint32_t rng_;

  void Init(uint32_t seed) {
    phase_ = 0.0f;
    sample_ = 0.0f;
    next_sample_ = 0.0f;
    frequency_ = 0.001f;
    rng_ = seed * 2654435761u + 12345u;
  }

  inline float Random() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
  }

  void Render(float frequency, float* out, int size) {
    frequency = clampf(frequency, 0.0f, 1.0f);
    Interp fm(&frequency_, frequency, size);
    float next_sample = next_sample_;
    float sample = sample_;

    while (size--) {
      float this_sample = next_sample;
      next_sample = 0.0f;

      const float freq = fm.Next();
      const float raw_sample = Random() * 2.0f - 1.0f;
      float raw_amount = clampf(4.0f * (freq - 0.25f), 0.0f, 1.0f);

      phase_ += freq;
      if (phase_ >= 1.0f) {
        phase_ -= 1.0f;
        float t = phase_ / freq;
        float discontinuity = raw_sample - sample;
        this_sample += discontinuity * ThisBlepSample(t);
        next_sample += discontinuity * NextBlepSample(t);
        sample = raw_sample;
      }
      next_sample += sample;
      *out++ = this_sample + raw_amount * (raw_sample - this_sample);
    }
    next_sample_ = next_sample;
    sample_ = sample;
  }
};

// ---------------------------------------------------------------------
// Engines. Same controls as Plaits: HARMONICS, TIMBRE, MORPH (0..1).
// Each renders a main OUT and an AUX signal; only the ones asked for
// are computed (CPU).

struct EngineParams {
  float f0;          // normalized frequency (Hz / 48000)
  float harmonics;
  float timbre;
  float morph;
  bool want_out;
  bool want_aux;
};

constexpr int kMaxBlock = 8;

// Virtual analog (Plaits VA, variant 2).
// OUT: variable square (TIMBRE: width, then sync) + variable saw (MORPH),
//      the saw detuned by HARMONICS (unison - fifth - octave).
// AUX: "monster sync" of two detuned variable waveshapes.
struct VAEngine {
  VariableShapeOscillator sync_, primary_, auxiliary_;
  VariableSawOscillator saw_;
  float square_gain_, saw_gain_;

  void Init() {
    sync_.Init();
    primary_.Init();
    auxiliary_.Init();
    auxiliary_.set_master_phase(0.25f);
    saw_.Init();
    square_gain_ = 0.0f;
    saw_gain_ = 0.0f;
  }

  static inline float Squash(float x) { return x * x * (3.0f - 2.0f * x); }

  static float ComputeDetuning(float detune) {
    static constexpr float intervals[5] = { 0.0f, 7.01f, 12.01f, 19.01f, 24.01f };
    detune = clampf(2.05f * detune - 1.025f, -1.0f, 1.0f);
    float sign = detune < 0.0f ? -1.0f : 1.0f;
    detune = detune * sign * 3.9999f;
    int di = static_cast<int>(detune);
    float df = detune - static_cast<float>(di);
    float a = intervals[di];
    float b = intervals[di + 1];
    return (a + (b - a) * Squash(Squash(df))) * sign;
  }

  void Render(const EngineParams& p, float* out, float* aux, int size) {
    float tmp[kMaxBlock];
    const float detune = ComputeDetuning(p.harmonics);
    const float primary_f = p.f0;
    const float auxiliary_f = p.f0 * SemitonesToRatio(detune);

    if (p.want_aux) {
      const float sync_amount = p.timbre * p.timbre;
      const float sync_ratio = SemitonesToRatio(sync_amount * 48.0f);
      float shape = clampf(p.morph * 1.5f, 0.0f, 1.0f);
      float pw = clampf(0.5f + (p.morph - 0.66f) * 1.46f, 0.5f, 0.995f);
      primary_.Render<true>(primary_f, primary_f * sync_ratio, pw, shape, tmp, size);
      auxiliary_.Render<true>(auxiliary_f, auxiliary_f * sync_ratio, pw, shape, aux, size);
      for (int i = 0; i < size; ++i) aux[i] = (aux[i] - tmp[i]) * 0.5f;
    }

    if (p.want_out) {
      float square_pw = clampf(1.3f * p.timbre - 0.15f, 0.005f, 0.5f);
      const float square_sync_ratio = p.timbre < 0.5f
        ? 0.0f
        : (p.timbre - 0.5f) * (p.timbre - 0.5f) * 4.0f * 48.0f;
      const float square_gain = minf(p.timbre * 8.0f, 1.0f);

      float saw_pw = p.morph < 0.5f
        ? p.morph + 0.5f
        : 1.0f - (p.morph - 0.5f) * 2.0f;
      saw_pw = clampf(saw_pw * 1.1f, 0.005f, 1.0f);
      float saw_shape = clampf(10.0f - 21.0f * p.morph, 0.0f, 1.0f);
      float saw_gain = clampf(8.0f * (1.0f - p.morph), 0.02f, 1.0f);

      const float square_sync_f = primary_f * SemitonesToRatio(square_sync_ratio);
      sync_.Render<true>(primary_f, square_sync_f, square_pw, 1.0f, tmp, size);
      saw_.Render(auxiliary_f, saw_pw, saw_shape, out, size);

      float norm = 1.0f / maxf(square_gain, saw_gain);
      Interp square_gain_modulation(&square_gain_, square_gain * 0.3f * norm, size);
      Interp saw_gain_modulation(&saw_gain_, saw_gain * 0.5f * norm, size);
      for (int i = 0; i < size; ++i) {
        out[i] = out[i] * saw_gain_modulation.Next() +
          square_gain_modulation.Next() * tmp[i];
      }
    }
  }
};

inline float Tame(float f0, float harmonics, float order) {
  f0 *= harmonics;
  float max_f = 0.5f / order;
  float max_amount = clampf(1.0f - (f0 - max_f) / (0.5f - max_f), 0.0f, 1.0f);
  return max_amount * max_amount * max_amount;
}

// Waveshaping (Plaits). HARMONICS: waveshaper curve, TIMBRE: wavefolder
// amount, MORPH: slope asymmetry (triangle - saw).
// AUX: sine folded by a second (asymmetric) wavefolder.
struct WaveshapingEngine {
  SlopeOscillator slope_, triangle_;
  float previous_shape_, previous_wavefolder_gain_, previous_overtone_gain_;

  void Init() {
    slope_.Init();
    triangle_.Init();
    previous_shape_ = 0.0f;
    previous_wavefolder_gain_ = 0.0f;
    previous_overtone_gain_ = 0.0f;
  }

  void Render(const EngineParams& p, float* out, float* aux, int size) {
    float shaped[kMaxBlock];
    float tri[kMaxBlock];
    const float f0 = p.f0;
    const float pw = p.morph * 0.45f + 0.5f;

    slope_.Render(f0, pw, shaped, size);
    if (p.want_aux) triangle_.Render(f0, 0.5f, tri, size);

    const float slope = 3.0f + absf(p.morph - 0.5f) * 5.0f;
    const float shape_amount = absf(p.harmonics - 0.5f) * 2.0f;
    const float shape_amount_attenuation = Tame(f0, slope, 16.0f);
    const float wavefolder_gain = p.timbre;
    const float wavefolder_gain_attenuation = Tame(
        f0, slope * (3.0f + shape_amount * shape_amount_attenuation * 5.0f), 12.0f);

    Interp shape_modulation(&previous_shape_,
        0.5f + (p.harmonics - 0.5f) * shape_amount_attenuation, size);
    Interp wf_gain_modulation(&previous_wavefolder_gain_,
        0.03f + 0.46f * wavefolder_gain * wavefolder_gain_attenuation, size);
    const float overtone_gain = p.timbre * (2.0f - p.timbre);
    Interp overtone_gain_modulation(&previous_overtone_gain_,
        overtone_gain * (2.0f - overtone_gain), size);

    for (int i = 0; i < size; ++i) {
      float shape = shape_modulation.Next() * 3.9999f;
      int shape_integral = static_cast<int>(shape);
      float shape_fractional = shape - static_cast<float>(shape_integral);

      const int16_t* shape_1 = plaits::lookup_table_i16_table[shape_integral];
      const int16_t* shape_2 = plaits::lookup_table_i16_table[shape_integral + 1];

      float ws_index = 127.0f * shaped[i] + 128.0f;
      int ws_index_integral = static_cast<int>(ws_index);
      float ws_index_fractional = ws_index - static_cast<float>(ws_index_integral);
      ws_index_integral &= 255;

      float x0 = static_cast<float>(shape_1[ws_index_integral]) / 32768.0f;
      float x1 = static_cast<float>(shape_1[ws_index_integral + 1]) / 32768.0f;
      float x = x0 + (x1 - x0) * ws_index_fractional;

      float y0 = static_cast<float>(shape_2[ws_index_integral]) / 32768.0f;
      float y1 = static_cast<float>(shape_2[ws_index_integral + 1]) / 32768.0f;
      float y = y0 + (y1 - y0) * ws_index_fractional;

      float mix = x + (y - x) * shape_fractional;
      float index = clampf(mix * wf_gain_modulation.Next() + 0.5f, 0.0f, 1.0f);
      float og = overtone_gain_modulation.Next();
      if (p.want_out) out[i] = InterpolateHermite16(plaits::lut_fold + 1, index, 512.0f);
      if (p.want_aux) {
        float fold_2 = -InterpolateHermite16(plaits::lut_fold_2 + 1, index, 512.0f);
        float sine = Sine(tri[i] * 0.25f + 0.5f);
        aux[i] = sine + (fold_2 - sine) * og;
      }
    }
  }
};

// 2-operator FM (Plaits), 2x oversampled. HARMONICS: frequency ratio
// (quantized to musical ratios), TIMBRE: modulation index, MORPH:
// feedback (left: phase feedback, right: modulator feedback).
// AUX: sub-octave operator modulated by the carrier.
struct FMEngine {
  uint32_t carrier_phase_, modulator_phase_, sub_phase_;
  float previous_carrier_frequency_, previous_modulator_frequency_;
  float previous_amount_, previous_feedback_, previous_sample_;
  float lp_out_, lp_aux_;

  void Init() {
    carrier_phase_ = 0;
    modulator_phase_ = 0;
    sub_phase_ = 0;
    previous_carrier_frequency_ = 0.001f;
    previous_modulator_frequency_ = 0.001f;
    previous_amount_ = 0.0f;
    previous_feedback_ = 0.0f;
    previous_sample_ = 0.0f;
    lp_out_ = lp_aux_ = 0.0f;
  }

  void Render(const EngineParams& p, float* out, float* aux, int size) {
    constexpr int kOversampling = 2;
    const float carrier_f = p.f0 * (1.0f / kOversampling);
    const float ratio = Interpolate(plaits::lut_fm_frequency_quantizer,
                                    p.harmonics, 128.0f);
    float target_modulator_frequency = clampf(
        carrier_f * SemitonesToRatio(ratio), 0.0f, 0.5f);

    // Reduce the FM index for high modulator notes, to limit aliasing.
    // Plaits (4x oversampled) starts at MIDI 96; at 2x we start at 84.
    float modulator_note = 69.0f + 12.0f * Log2(
        target_modulator_frequency * (48000.0f * kOversampling / 440.0f));
    float hf_taming = clampf(1.0f - (modulator_note - 84.0f) * 0.025f, 0.0f, 1.0f);
    hf_taming *= hf_taming;

    Interp carrier_frequency(&previous_carrier_frequency_, carrier_f, size);
    Interp modulator_frequency(&previous_modulator_frequency_,
                               target_modulator_frequency, size);
    Interp amount_modulation(&previous_amount_,
                             2.0f * p.timbre * p.timbre * hf_taming, size);
    Interp feedback_modulation(&previous_feedback_, 2.0f * p.morph - 1.0f, size);

    const float max_uint32 = 4294967296.0f;
    for (int i = 0; i < size; ++i) {
      const float amount = amount_modulation.Next();
      const float feedback = feedback_modulation.Next();
      const float phase_feedback = feedback < 0.0f ? 0.5f * feedback * feedback : 0.0f;
      const float modulator_fb = feedback > 0.0f ? 0.25f * feedback * feedback : 0.0f;
      const uint32_t carrier_increment =
        static_cast<uint32_t>(max_uint32 * carrier_frequency.Next());
      const float mod_f = modulator_frequency.Next();

      float c_acc = 0.0f, s_acc = 0.0f;
      for (int j = 0; j < kOversampling; ++j) {
        modulator_phase_ += static_cast<uint32_t>(
            max_uint32 * mod_f * (1.0f + previous_sample_ * phase_feedback));
        carrier_phase_ += carrier_increment;
        float modulator = SinePM(modulator_phase_, modulator_fb * previous_sample_);
        float carrier = SinePM(carrier_phase_, amount * modulator);
        previous_sample_ += 0.05f * (carrier - previous_sample_);
        c_acc += carrier;
        if (p.want_aux) {
          sub_phase_ += carrier_increment >> 1;
          s_acc += SinePM(sub_phase_, amount * carrier * 0.25f);
        }
      }
      // decimation: average of the 2 sub-samples + one pole smoothing
      float c = c_acc * 0.5f;
      lp_out_ += 0.7f * (c - lp_out_);
      out[i] = lp_out_;
      if (p.want_aux) {
        float s = s_acc * 0.5f;
        lp_aux_ += 0.7f * (s - lp_aux_);
        aux[i] = lp_aux_;
      }
    }
  }
};

// Formant / grain (Plaits "grain" engine). HARMONICS: ratio between the
// two formants, TIMBRE: formant frequency, MORPH: carrier shape.
// AUX: Z oscillator (filtered-waveform formant synthesis).
struct FormantEngine {
  GrainletOscillator grainlet_[2];
  ZOscillator z_oscillator_;
  OnePoleHp dc_blocker_[2];

  void Init() {
    grainlet_[0].Init();
    grainlet_[1].Init();
    z_oscillator_.Init();
    dc_blocker_[0].Init();
    dc_blocker_[1].Init();
  }

  void Render(const EngineParams& p, float* out, float* aux, int size) {
    const float f0 = p.f0;
    const float hp_coef = clampf(2.0f * kPi * 0.3f * f0, 0.0f, 0.5f);

    if (p.want_out) {
      float tmp[kMaxBlock];
      // formant frequency: MIDI note 24..108
      const float f1 = 0.000680f * SemitonesToRatio(84.0f * p.timbre);
      const float ratio = SemitonesToRatio(-24.0f + 48.0f * p.harmonics);
      const float carrier_bleed = p.harmonics < 0.5f ? 1.0f - 2.0f * p.harmonics : 0.0f;
      const float carrier_bleed_fixed = carrier_bleed * (2.0f - carrier_bleed);
      const float carrier_shape = 0.33f + (p.morph - 0.33f) * maxf(1.0f - f0 * 24.0f, 0.0f);

      grainlet_[0].Render(f0, f1, carrier_shape, carrier_bleed_fixed, out, size);
      grainlet_[1].Render(f0, f1 * ratio, carrier_shape, carrier_bleed_fixed, tmp, size);
      for (int i = 0; i < size; ++i)
        out[i] = dc_blocker_[0].Process(out[i] + tmp[i], hp_coef);
    }

    if (p.want_aux) {
      const float cutoff = f0 * SemitonesToRatio(96.0f * p.timbre);
      z_oscillator_.Render(f0, cutoff, p.morph, p.harmonics, aux, size);
      for (int i = 0; i < size; ++i)
        aux[i] = dc_blocker_[1].Process(aux[i], hp_coef);
    }
  }
};

// Filtered noise (Plaits). HARMONICS: filter type LP - BP - HP (OUT) /
// spacing of the two band-pass filters (AUX), TIMBRE: noise clock
// (grainy - smooth - white), MORPH: resonance (up to self-whistling).
// The filters are tuned to the voice pitch, so the scale and chords
// work on noise too.
struct NoiseEngine {
  ClockedNoise clocked_noise_[2];
  Svf lp_hp_filter_;
  Svf bp_filter_[2];

  void Init(uint32_t seed) {
    clocked_noise_[0].Init(seed);
    clocked_noise_[1].Init(seed + 7919u);
    lp_hp_filter_.Init();
    bp_filter_[0].Init();
    bp_filter_[1].Init();
  }

  void Render(const EngineParams& p, float* out, float* aux, int size) {
    float n1[kMaxBlock];
    float n2[kMaxBlock];
    const float f0 = clampf(p.f0, 0.0001f, 0.2f);
    const float f1 = clampf(p.f0 * SemitonesToRatio(p.harmonics * 48.0f - 24.0f), 0.0001f, 0.2f);
    // clock: MIDI 0..128, as an unpatched Plaits
    const float clock_f = 0.000170f * SemitonesToRatio(p.timbre * 128.0f);
    const float q = 0.5f * SemitonesToRatio(p.morph * 120.0f);
    const float gain = 1.0f / __builtin_sqrtf((0.5f + q) * 40.0f * f0);

    clocked_noise_[0].Render(clock_f, n1, size);
    if (p.want_aux) clocked_noise_[1].Render(clock_f * f1 / f0, n2, size);

    if (p.want_out) {
      lp_hp_filter_.set_f_q(f0, q);
      const float mode = p.harmonics;
      // LP -> BP -> HP
      const float lp_gain = maxf(1.0f - mode * 2.0f, 0.0f);
      const float bp_gain = 1.0f - absf(mode * 2.0f - 1.0f);
      const float hp_gain = maxf(mode * 2.0f - 1.0f, 0.0f);
      for (int i = 0; i < size; ++i) {
        float bp, hp;
        float lp = lp_hp_filter_.Process(n1[i] * gain, &bp, &hp);
        out[i] = lp * lp_gain + bp * bp_gain + hp * hp_gain;
      }
    }
    if (p.want_aux) {
      bp_filter_[0].set_f_q(f0, q);
      bp_filter_[1].set_f_q(f1, q);
      for (int i = 0; i < size; ++i) {
        aux[i] = bp_filter_[0].ProcessBP(n1[i] * gain) +
          bp_filter_[1].ProcessBP(n2[i] * gain);
      }
    }
  }
};

enum Engine {
  ENGINE_ENOSC = 0,     // original Ensemble Oscillator engine
  ENGINE_VA,
  ENGINE_WAVESHAPE,
  ENGINE_FM,
  ENGINE_FORMANT,
  ENGINE_NOISE,
  kNumEngines,
};

// Output level of each engine, so they all sit at about the level of
// the original sine engine
constexpr float kEngineGain[kNumEngines] = {
  1.0f,   // ENOSC (unused)
  1.6f,   // VA
  1.0f,   // WAVESHAPE
  1.0f,   // FM
  1.5f,   // FORMANT
  1.4f,   // NOISE
};

union VoiceState {
  VAEngine va;
  WaveshapingEngine ws;
  FMEngine fm;
  FormantEngine formant;
  NoiseEngine noise;

  void Init(int engine, uint32_t seed) {
    switch (engine) {
    case ENGINE_VA: va.Init(); break;
    case ENGINE_WAVESHAPE: ws.Init(); break;
    case ENGINE_FM: fm.Init(); break;
    case ENGINE_FORMANT: formant.Init(); break;
    case ENGINE_NOISE: noise.Init(seed); break;
    default: break;
    }
  }

  void Render(int engine, const EngineParams& p, float* out, float* aux, int size) {
    switch (engine) {
    case ENGINE_VA: va.Render(p, out, aux, size); break;
    case ENGINE_WAVESHAPE: ws.Render(p, out, aux, size); break;
    case ENGINE_FM: fm.Render(p, out, aux, size); break;
    case ENGINE_FORMANT: formant.Render(p, out, aux, size); break;
    case ENGINE_NOISE: noise.Render(p, out, aux, size); break;
    default: break;
    }
  }
};

}  // namespace pl
