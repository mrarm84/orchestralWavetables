#pragma once

#include "buffer.hh"
#include "persistent_storage.hh"

constexpr const int kScaleNr = 10;
constexpr const int kBankNr = 3;
constexpr const int kMaxScaleSize = 2 * kMaxNumOsc;
constexpr const f kScaleUnicityThreshold = 0.1_f;

struct PitchPair {
  f p1, p2, crossfade;
};

class Scale {
  // scale_[0] = 0, contains [size_] sorted elements
  f scale_[kMaxScaleSize];
  int size_ = 0;
  friend class PreScale;
public:
  Scale() {}
  Scale(std::initializer_list<f> scale) {
    size_ = scale.size();
    std::copy(scale.begin(), scale.end(), scale_);
  }

// #pragma GCC push_options
// #pragma GCC optimize ("O0")

  bool validate() const {
    if (!(size_ > 0 && size_ <= kMaxScaleSize)) return false;
    for (auto& note : scale_)
      if (!(note >= 0_f && note <= 150_f)) return false;
    return true;
  }

// #pragma GCC pop_options

  // pitch>0
  PitchPair Process(f const pitch) const {
    f max = scale_[size_-1];
    // quotient by the max
    f oct = (pitch / max).integral();
    f octaves = oct * max;
    f semitones = pitch - octaves;
    // semitones [0..max[
    int index = binary_search(semitones, scale_, size_);
    // index: [0..size_-2]
    f p1 = scale_[index];
    f p2 = scale_[index+1];
    f crossfade = (semitones - p1) / (p2 - p1);
    p1 += octaves;
    p2 += octaves;

    if ((index + (oct.floor() * (size_+1))) & 1) {
      crossfade = 1_f - crossfade;
      f tmp = p1;
      p1 = p2;
      p2 = tmp;
    }

    return {p1, p2, crossfade};
  }

  void copy_from(const Scale& g) {
    *this = g;
  }
};

class PreScale : Scale {
public:
  bool add(f x) {
    if (size_ < kMaxScaleSize-2) { // -2: if Octave wrap, will add one
      scale_[size_++] = x;
      return true;
    } else {
      return false;
    }
  }

  int size() const { return size_; }
  void clear() { size_ = 0; }
  f get(int i) const { return scale_[i]; }
  void set_last(f const x) { scale_[size_-1] = x; }
  bool remove_last() {
    if (size_ > 1) {
      size_--;
      return true;
    } else return false;
  }

  void quantize() {
    for (int i=0; i<size_; i++) {
        scale_[i] = scale_[i].integral();
    }
  }

  // do not call if size==0
  bool copy_to(Scale *g, bool wrap_octave) {
    // sort table
    std::sort(scale_, scale_+size_);
    // normalize from smallest element
    f base = scale_[0];
    for (int i=0; i<kMaxScaleSize; i++) {
      if (i<size_)
        scale_[i] -= base;
      else
        scale_[i] = 0_f;
    }

    if (wrap_octave) {
      f max = scale_[size_-1];
      scale_[size_++] = ((max / 12_f).integral() + 1_f) * 12_f;
    }

    // remove duplicate elements
    uniquify(scale_, size_, kScaleUnicityThreshold);

    if (size_ > 1) {
      // copy to real scale
      *g = *this;
      return true;
    } else {
      return false;
    }
  }
};

class Quantizer {
  struct ScaleTable : std::array<std::array<Scale, kScaleNr>, kBankNr> {
    bool validate() {
      for (const auto& bank : *this)
        for (const auto& scale : bank)
          if (!scale.validate()) return false;
      return true;
    }
  };

  // enosc3: every bank goes from minor / sad (Scale knob fully left)
  // to major / bright (fully right), for dub techno.
  ScaleTable const default_scales_ = {{{{{
          // 12TET (Scale switch up)
          {0_f, 3_f, 7_f, 12_f},                 // 1: minor triad
          {0_f, 3_f, 7_f, 10_f, 12_f},           // 2: minor 7th
          {0_f, 3_f, 7_f, 10_f, 14_f, 24_f},     // 3: minor 9th (2-octave voicing)
          {0_f, 5_f},                            // 4: stacked fourths (quartal, m11)
          {0_f, 1_f, 5_f, 7_f, 8_f, 12_f},       // 5: In scale (dark, phrygian)
          {0_f, 2_f, 3_f, 5_f, 7_f, 8_f, 10_f, 12_f}, // 6: natural minor
          {0_f, 3_f, 5_f, 7_f, 10_f, 12_f},      // 7: minor pentatonic
          {0_f, 2_f, 3_f, 5_f, 7_f, 9_f, 10_f, 12_f}, // 8: dorian
          {0_f, 7_f, 12_f},                      // 9: octave + fifth
          {0_f, 4_f, 7_f, 12_f},                 // 10: major triad
        }}, {{
          // OCTAVE: just intonation (Scale switch middle)

          // just minor triad: 1 6/5 3/2
          { 0_f, 3.15641287_f, 7.01955001_f, 12_f },

          // just minor 7th: 1 6/5 3/2 9/5
          { 0_f, 3.15641287_f, 7.01955001_f, 10.17596288_f, 12_f },

          // subharmonics 4-7 (utonal "minor" of harmonics 4-7): 1 7/6 7/5 7/4
          { 0_f, 2.66870906_f, 5.82512193_f, 9.68825906_f, 12_f },

          // just natural minor: 1 9/8 6/5 4/3 3/2 8/5 9/5
          { 0_f, 2.03910002_f, 3.15641287_f, 4.98044999_f, 7.01955001_f,
            8.13686286_f, 10.17596288_f, 12_f },

          // Pythagorean Do Fa So (sus4): 1 4/3 3/2
          { 0_f, 4.98044999_f, 7.01955001_f, 12_f },

          // harmonics 2-3 (perfect fifth)
          { 0_f, 7.0195500086_f, 12_f, },

          // 7-TET
          { 0_f, 1.71428571_f, 3.42857143_f, 5.14285714_f,
            6.85714286_f, 8.57142857_f, 10.28571429_f, 12_f },

          // 5-TET
          { 0_f ,  2.4_f,  4.8_f,  7.2_f,  9.6_f , 12_f},

          // harmonics 8-15
          {0_f, 2.03910002_f, 3.86313714_f, 5.51317942_f, 7.01955001_f,
           8.40527662_f, 9.68825906_f, 10.88268715_f, 12_f },

          // harmonics 4-7 (major, brightest)
          {0_f, 3.86313714_f, 7.01955001_f, 9.68825906_f, 12_f },

        }}, {{
          // FREE: no octave (Scale switch down)
          {0_f, 3_f},                // stacked minor thirds (diminished)

          // Bohlen-Pierce minor triad
          { 0_f, 5.85216923_f, 14.63042308_f, 19.01955001_f },

          {0_f, 1_f, 3_f},           // MLT #2 (octatonic)
          {0_f, 1_f, 2_f, 5_f, 6_f}, // MLT #4

          // Bohlen-Pierce
          { 0_f, 1.46304231_f, 2.92608462_f, 4.38912693_f,
            5.85216923_f, 7.31521154_f, 8.77825385_f, 10.24129616_f,
            11.70433847_f, 13.16738078_f, 14.63042308_f, 16.09346539_f,
            17.5565077_f, 19.01955001_f },

          {0_f, 2_f, 3_f, 4_f},      // MLT #3

          // Carlos alpha
          { 0_f, 0.78_f },

          {0_f, 2_f},                // whole tone = MLT#1
          {0_f, 7.0195500086_f},     // perfect fifths spiral

          // Bohlen-Pierce major triad
          { 0_f, 8.77825385_f, 14.63042308_f, 19.01955001_f },
        }}}}};

  ScaleTable scales_;
  // enosc3 keeps its scales in block 5 (stock firmware: block 2), so its
  // new default scale order is not replaced by scales saved by the stock
  // firmware
  Persistent<WearLevel<FlashBlock<5, ScaleTable>>> scales_storage_ {&scales_, default_scales_};

public:
  // runs once at boot
  COLD_CODE Quantizer() {}

  void reset_scale(Parameters::Scale scale) {
    int i=scale.mode;
    int j=scale.value;
    scales_[i][j].copy_from(default_scales_[i][j]);
  }

  Scale *get_scale(Parameters::Scale scale) {
    return &scales_[scale.mode][scale.value];
  }

  void Save() {
    scales_storage_.Save();
  }
};
