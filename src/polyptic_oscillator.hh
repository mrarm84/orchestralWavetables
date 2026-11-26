#pragma once

#include <algorithm>

#include "dsp.hh"
#include "oscillator.hh"
#include "quantizer.hh"
#include "plaits_voice.hh"

static_assert(pl::kNumEngines == kNumEngines);

// Soft clipper (tanh-like, unity gain at 0, saturates at +-1)
inline float SoftClip(float x) {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// per-engine output levels, to sit close to the sine engine
constexpr float kPlaitsOutGain[kNumEngines] = {1.0f, 1.3f, 1.0f, 0.8f, 2.2f, 1.0f};
constexpr float kPlaitsAuxGain[kNumEngines] = {1.0f, 1.2f, 1.0f, 0.8f, 1.2f, 0.8f};

// One enosc voice pair (two notes of the scale, crossfaded) rendered by
// a Plaits engine. Mirrors OscillatorPair: same frequency smoothing,
// freeze and crossfade; adds a key-tracked color low-pass filter.
template<int block_size>
class PlaitsPair : Nocopy {
  pl::VoiceState voice_[2];
  FrequencyState freq_;
  OnePoleLp crossfade_lp_;
  pl::Svf filter_;
  float fade_[2] = {0.0f, 0.0f};
  float amp_ = 0.0f;

public:
  void Init(int engine, uint32_t seed) {
    voice_[0].Init(engine, seed * 2u + 1u);
    voice_[1].Init(engine, seed * 2u + 2u);
    filter_.Init();
  }

  void Mute() { amp_ = 0.0f; fade_[0] = fade_[1] = 0.0f; }

  // out_mode: 0 = OUT, 1 = OUT+AUX, 2 = AUX (Twist switch)
  // color: 0 = open, 1 = warm, 2 = dark+resonant (Warp switch)
  void Process(int engine, FrequencyPair freq, bool frozen,
               f crossfade_factor, pl::EngineParams p,
               int out_mode, int color, f amplitude,
               Buffer<f, block_size>& sum_output) {
    f coef = frozen ? 0_f : 1_f;
    auto [freq1, freq2, crossfade] = freq_.Process(coef, freq);
    crossfade = crossfade_lp_.Process(0.1_f, Signal::crop(crossfade_factor, crossfade));

    const float cf = crossfade.repr();
    const float target[2] = {1.0f - cf, cf};
    const float fr[2] = {freq1.repr(), freq2.repr()};
    const float out_gain = kPlaitsOutGain[engine];
    const float aux_gain = kPlaitsAuxGain[engine];

    p.want_out = out_mode != 2;
    p.want_aux = out_mode != 0;

    float acc[block_size];
    for (int i = 0; i < block_size; ++i) acc[i] = 0.0f;

    for (int k = 0; k < 2; ++k) {
      // a silent voice of the pair costs nothing
      if (target[k] < 0.0001f && fade_[k] < 0.0001f) {
        fade_[k] = 0.0f;
        continue;
      }
      float o[block_size], a[block_size];
      p.f0 = fr[k];
      voice_[k].Render(engine, p, o, a, block_size);
      float g = fade_[k];
      const float dg = (target[k] - g) * (1.0f / block_size);
      for (int i = 0; i < block_size; ++i) {
        g += dg;
        float x =
          out_mode == 0 ? o[i] * out_gain :
          out_mode == 2 ? a[i] * aux_gain :
          (o[i] * out_gain + a[i] * aux_gain) * 0.7f;
        acc[i] += x * g;
      }
      fade_[k] = target[k];
    }

    if (color != 0) {
      // cutoff follows the pitch of the voice
      const float key = fr[0] * (1.0f - cf) + fr[1] * cf;
      if (color == 1) {
        filter_.set_f_q(pl::clampf(key * 5.0f, 0.004f, 0.2f), 0.7f);
      } else {
        filter_.set_f_q(pl::clampf(key * 2.2f, 0.0025f, 0.2f), 2.2f);
      }
      for (int i = 0; i < block_size; ++i) acc[i] = filter_.ProcessLP(acc[i]);
    }

    float am = amp_;
    const float da = (amplitude.repr() - am) * (1.0f / block_size);
    for (int i = 0; i < block_size; ++i) {
      am += da;
      sum_output[i] += f(SoftClip(acc[i]) * am);
    }
    amp_ = amplitude.repr();
  }
};

class AmplitudeAccumulator {
  f amplitude = 1_f;
  f amplitudes = 0_f;
  f balance;
  f numOsc;

public:
  AmplitudeAccumulator(f t, f n) : balance(t), numOsc(n) {}
  f next() {
    f r =
      numOsc > 1_f ? amplitude :
      numOsc > 0_f ? amplitude * numOsc :
      0_f;
    amplitudes += r;
    amplitude *= balance;
    numOsc--;
    return r;
  }

  f sum() { return amplitudes; }
};

template<int block_size>
class PreListenOscillators : Nocopy {
  Oscillator oscs_[kMaxScaleSize];
  Buffer<u0_16, block_size> dummy_block_;
  OnePoleLp amp_lp_;

public:
  void Process(Parameters const &params, PreScale &scale,
               Buffer<f, block_size>& out1, Buffer<f, block_size>& out2) {
    out1.fill(0_f);
    out2.fill(0_f);

    f twist = params.twist.value;
    f warp = params.warp.value;
    f modulation = 0_f;    // no modulation in pre-listen

    f scale_size = amp_lp_.Process(0.05_f, f(scale.size()));
    AmplitudeAccumulator amplitudes { 1_f, scale_size };

    typename OscillatorPair<block_size>::processor_t process =
      OscillatorPair<block_size>::pick_processor(params.twist.mode, params.warp.mode);

    for (int i=0; i<kMaxScaleSize; ++i) {
      f freq = Freq::of_pitch(scale.get(i)).repr();
      (oscs_[i].*process)(freq, twist, warp,
                          modulation, 1_f, amplitudes.next(),
                          dummy_block_, dummy_block_, out1);
    }

    f sum = amplitudes.sum();
    f atten = 0.5_f / sum;
    atten *= 1_f + Data::normalization_factors.interpolate_from_index(sum.max(f(kMaxNumOsc)));

    for (auto [o1, o2] : zip(out1, out2)) {
      o2 = o1 = o1 * atten;
    }
  }
};

template<int block_size>
class Oscillators : Nocopy {
  OscillatorPair<block_size> oscs_[kMaxNumOsc];
  PlaitsPair<block_size> plaits_[kMaxNumOsc];
  int engine_ = pl::ENGINE_ENOSC;
  Buffer<u0_16, block_size> modulation_blocks_[kMaxNumOsc+1];
  Buffer<u0_16, block_size> dummy_block_;
  bool frozen_ = false;
  bool temp_frozen_ = false;
  f lowest_pitch_;

  TwistMode previous_twist_mode_;
  WarpMode previous_warp_mode_;
  ModulationMode previous_modulation_mode_;

  static inline bool pick_split(SplitMode mode, int i, int numOsc) {
    return
      mode == ALTERNATE ? !(i&1) :
      mode == LOW_HIGH ? i<numOsc/2 :
      i == 0;
  }

  inline std::tuple<Buffer<u0_16, block_size>&, Buffer<u0_16, block_size>&>
  pick_modulation_blocks(ModulationMode mode, int i, int numOsc) {
    if(mode == ONE) { //Up
      if (i==0) {
        return std::forward_as_tuple(dummy_block_, modulation_blocks_[0]);
      } else {
        return std::forward_as_tuple(modulation_blocks_[0], dummy_block_);
      }
    } else if (mode == TWO) { //All
      if (i==0) {
        return std::forward_as_tuple(dummy_block_, modulation_blocks_[i+1]);
      } else {
        return std::forward_as_tuple(modulation_blocks_[i], modulation_blocks_[i+1]);
      }
    } else { // mode == THREE //Down
      if (i==numOsc-1) {
        return std::forward_as_tuple(dummy_block_, modulation_blocks_[0]);
      } else {
        return std::forward_as_tuple(modulation_blocks_[0], dummy_block_);
      }
    }
  }

  class FrequencyAccumulator {
    f root;
    f pitch;
    f spread;
    f detune;
    f detune_accum = 0_f;
    Scale const &scale;
  public:
    FrequencyAccumulator(Scale const &g, f r, f p, f s, f d) :
      scale(g), root(r), pitch(p), spread(s), detune(d) {}

    f next_pitch() {
      PitchPair p = scale.Process(root);
      f lowest = p.crossfade < 0.5_f ? p.p1 : p.p2;
      return lowest + pitch + detune_accum;
    }

    FrequencyPair next() {

      // root > 0
      PitchPair p = scale.Process(root); // 2%

      p.p1 += pitch + detune_accum;
      p.p2 += pitch + detune_accum;

      f freq1 = Freq::of_pitch(p.p1).repr();
      f freq2 = Freq::of_pitch(p.p2).repr();

      root += spread;
      detune *= -1.2_f;
      detune_accum += detune;

      return {freq1, freq2, p.crossfade};
    }
  };

  // Harmonics of voice i of n (Mod switch): same / spread / alternate
  static float voice_harmonics(float h, int i, int n, ModulationMode mode) {
    if (mode == TWO) {
      float pos = n > 1 ? float(i) / float(n - 1) - 0.5f : 0.0f;
      return pl::clampf(h + 0.3f * pos, 0.0f, 1.0f);
    } else if (mode == THREE) {
      return (i & 1) ? 1.0f - h : h;
    }
    return h;
  }

  void ProcessPlaits(Parameters const &params, Scale const &scale,
                     Buffer<f, block_size>& out1, Buffer<f, block_size>& out2) {
    int numOsc = std::min(params.alt.numOsc, params.voice_cap);

    AmplitudeAccumulator amplitude {params.balance, f(numOsc)};
    FrequencyAccumulator frequency {scale, params.root, params.pitch,
                                    params.spread, params.detune};

    lowest_pitch_ = frequency.next_pitch();

    const int out_mode = params.twist.mode;     // FEEDBACK/PULSAR/CRUSH = 0/1/2
    const int color = params.warp.mode;         // FOLD/CHEBY/SEGMENT = 0/1/2
    const ModulationMode harm_mode = params.modulation.mode;
    const float h = pl::clampf(params.plaits.harmonics.repr(), 0.0f, 1.0f);

    pl::EngineParams p;
    p.f0 = 0.0f;
    p.harmonics = h;
    p.timbre = pl::clampf(params.plaits.timbre.repr(), 0.0f, 1.0f);
    p.morph = pl::clampf(params.plaits.morph.repr(), 0.0f, 1.0f);
    p.want_out = p.want_aux = true;

    f crossfade_factor = params.alt.crossfade_factor;
    SplitMode stereo_mode = params.alt.stereo_mode;
    SplitMode freeze_mode = params.alt.freeze_mode;

    for (int i=0; i<numOsc; ++i) {
      FrequencyPair fp = frequency.next();
      f amp = amplitude.next();
      Buffer<f, block_size>& out = pick_split(stereo_mode, i, numOsc) ? out1 : out2;
      bool frozen = (pick_split(freeze_mode, i, numOsc) && frozen_) || temp_frozen_;
      p.harmonics = voice_harmonics(h, i, numOsc, harm_mode);
      plaits_[i].Process(engine_, fp, frozen, crossfade_factor, p,
                         out_mode, color, amp, out);
    }
    for (int i=numOsc; i<kMaxNumOsc; ++i) plaits_[i].Mute();

    temp_frozen_ = false;

    f atten = 1_f / amplitude.sum();
    f balance = params.balance <= 1_f ? params.balance : 1_f / params.balance;
    atten *= 0.5_f + (0.5_f + Data::normalization_factors[numOsc]) * balance;

    f atten1 = atten, atten2 = atten;
    if (stereo_mode == LOWEST_REST) {
      atten1 = 0.7_f;
      atten2 = 0.6_f * atten;
    }

    for (auto [o1, o2] : zip(out1, out2)) {
      o1 *= atten1;
      o2 *= atten2;
    }
  }

public:
  // Switch engine (called while the output is faded out)
  void set_engine(int engine) {
    engine_ = engine;
    for (int i=0; i<kMaxNumOsc; ++i) {
      plaits_[i].Init(engine, i);
      plaits_[i].Mute();
    }
  }
  int engine() const { return engine_; }

  void Process(Parameters const &params, Scale const &scale,
               Buffer<f, block_size>& out1, Buffer<f, block_size>& out2) {
    out1.fill(0_f);
    out2.fill(0_f);

    if (engine_ != pl::ENGINE_ENOSC) {
      ProcessPlaits(params, scale, out1, out2);
      return;
    }

    int numOsc = params.alt.numOsc;

    AmplitudeAccumulator amplitude {params.balance, f(numOsc)};
    FrequencyAccumulator frequency {scale, params.root, params.pitch,
                                    params.spread, params.detune};

    lowest_pitch_ = frequency.next_pitch();

    TwistMode twist_mode = params.twist.mode;
    WarpMode warp_mode = params.warp.mode;
    f twist = params.twist.value;
    f warp = params.warp.value;
    f modulation = params.modulation.value;

    f crossfade_factor = params.alt.crossfade_factor;
    SplitMode stereo_mode = params.alt.stereo_mode;
    SplitMode freeze_mode = params.alt.freeze_mode;
    ModulationMode modulation_mode = params.modulation.mode;

    bool twist_needs_jump = false;
    if (twist_mode != previous_twist_mode_) {
      previous_twist_mode_ = twist_mode;
      twist_needs_jump = true;
    }

    bool warp_needs_jump = false;
    if (warp_mode != previous_warp_mode_) {
      previous_warp_mode_ = warp_mode;
      warp_needs_jump = true;
    }

    bool modulation_needs_jump = false;
    if (modulation_mode != previous_modulation_mode_) {
      previous_modulation_mode_ = modulation_mode;
      modulation_needs_jump = true;
    }

    for (int i=0; i<kMaxNumOsc; ++i) {
      FrequencyPair p = frequency.next(); // 3%
      f amp = amplitude.next();
      Buffer<f, block_size>& out = pick_split(stereo_mode, i, numOsc) ? out1 : out2;
      auto [mod_in, mod_out] = pick_modulation_blocks(modulation_mode, i, numOsc);
      dummy_block_.fill(0._u0_16);
      bool frozen = (pick_split(freeze_mode, i, numOsc) && frozen_) || temp_frozen_;
      oscs_[i].Process(twist_mode, twist_needs_jump, warp_mode, warp_needs_jump,
                       p, frozen, crossfade_factor,
                       twist, warp, modulation, modulation_needs_jump, amp,
                       mod_in, mod_out, out);
    }

    temp_frozen_ = false;

    f atten1, atten2;

    f atten = 1_f / amplitude.sum();
    f balance = params.balance <= 1_f ? params.balance : 1_f / params.balance;
    atten *= 0.5_f + (0.5_f + Data::normalization_factors[numOsc]) * balance;

    atten1 = atten2 = atten;

    if (stereo_mode == LOWEST_REST) {
      // adjust attenuation to not clip the output
      atten1 = 0.7_f;
      atten2 = 0.6_f * atten;
    }

    for (auto [o1, o2] : zip(out1, out2)) {
      o1 *= atten1;
      o2 *= atten2;
    }
  }

  void set_freeze (bool frozen) { frozen_ = frozen; }
  void set_temporary_freeze() { temp_frozen_ = true; }
  bool frozen() { return frozen_; }
  f lowest_pitch() { return lowest_pitch_; }
};

template<int block_size>
class PolypticOscillator : public Oscillators<block_size>, PreListenOscillators<block_size> {
  Parameters& params_;
  Quantizer quantizer_;
  PreScale pre_scale_;
  Scale *current_scale_;
  DCBlocker dc_blocker1_, dc_blocker2_;

  bool learn_ = false;
  bool pre_listen_ = false;
  bool follow_new_note_ = false;
  f manual_learn_offset_ = 0_f;

  int previous_scale_index;

  // engine changes fade the output out and back in (about 10 ms)
  float switch_gain_ = 0.0f;
  static constexpr float kSwitchStep = 1.0f / 64.0f;

public:
  PolypticOscillator(Parameters& params) : params_(params) {}

  void enable_pre_listen() { pre_listen_ = true; }
  void disable_pre_listen() { pre_listen_ = false; }
  void enable_follow_new_note() { follow_new_note_ = true; }
  void disable_follow_new_note() { follow_new_note_ = false; }

  void enable_learn() {
    learn_ = true;
    pre_scale_.clear();
    manual_learn_offset_ = this->lowest_pitch() - params_.root;
  }

  COLD_CODE bool disable_learn() {
    learn_ = false;
    disable_pre_listen();
    current_scale_ = quantizer_.get_scale(params_.scale);
    if (params_.scale.mode == TWELVE)
      pre_scale_.quantize();
    bool wrap_octave = params_.scale.mode == OCTAVE;
    bool success = pre_scale_.copy_to(current_scale_, wrap_octave);
    if (success) quantizer_.Save();
    return success;
  }

  bool learn_mode() { return learn_; }

  bool new_note(f x) {
    if (params_.scale.mode == TWELVE)
      x = x.integral();
    return pre_scale_.add(x);
  }

  bool empty_pre_scale() {
    return pre_scale_.size() == 0;
  }

  void change_last_note(f coarse, f fine) {
    if (params_.scale.mode == TWELVE)
      coarse = coarse.integral();
    pre_scale_.set_last(coarse + fine);
  }

  bool remove_last_note() {
    return pre_scale_.remove_last();
  }

  void reset_current_scale() {
    quantizer_.reset_scale(params_.scale);
    quantizer_.Save();
  }

  void Process(Buffer<Frame, block_size>& out) {
    Buffer<f, block_size> out1;
    Buffer<f, block_size> out2;

    float gain_start = switch_gain_;
    int requested = params_.alt.engine;
    if (requested != this->engine()) {
      switch_gain_ -= kSwitchStep;
      if (switch_gain_ <= 0.0f) {
        switch_gain_ = 0.0f;
        Oscillators<block_size>::set_engine(requested);
      }
    } else if (switch_gain_ < 1.0f) {
      switch_gain_ = std::min(switch_gain_ + kSwitchStep, 1.0f);
    }
    const float gain_step = (switch_gain_ - gain_start) * (1.0f / block_size);

    if (pre_listen_) {
      if (follow_new_note_)
        change_last_note(params_.new_note + manual_learn_offset_, params_.fine_tune);
      PreListenOscillators<block_size>::Process(params_, pre_scale_, out1, out2);
    } else {
      if (previous_scale_index != params_.scale.value) {
        Oscillators<block_size>::set_temporary_freeze();
        previous_scale_index = params_.scale.value;
      }
      current_scale_ = quantizer_.get_scale(params_.scale);
      Oscillators<block_size>::Process(params_, *current_scale_, out1, out2);
    }

    float g = gain_start;
    for (auto [o1, o2, o] : zip(out1, out2, out)) {
      g += gain_step;
      o1 = dc_blocker1_.process(o1) * f(g);
      o2 = dc_blocker2_.process(o2) * f(g);
      o.l = s9_23::inclusive(o1.clip());
      o.r = s9_23::inclusive(o2.clip());
    }
  }
};
