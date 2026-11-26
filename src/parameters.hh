#pragma once

#include "numtypes.hh"

constexpr struct Frame {
  s9_23 l = 0._s9_23;
  s9_23 r = 0._s9_23;
} zero;

constexpr int kUiUpdateRate = 200; // Hz
constexpr int kSampleRate = 48000; // Hz
constexpr int kBlockSize = 8;
// Code that never runs in the audio interrupt (UI events, constructors):
// optimized for size. Never use it on code reachable from the audio
// interrupt (Ui::Poll, PolypticOscillator::Process): -Os there overloads
// the CPU.
#define COLD_CODE [[gnu::optimize("Os")]]

constexpr int kMaxNumOsc = 16;
constexpr int kNumEngines = 6;   // see pl::Engine in plaits_voice.hh

enum TwistMode { FEEDBACK, PULSAR, CRUSH };
enum WarpMode { FOLD, CHEBY, SEGMENT };
enum ScaleMode { TWELVE, OCTAVE, FREE };
enum ModulationMode { ONE, TWO, THREE };

enum SplitMode { ALTERNATE, LOW_HIGH, LOWEST_REST };

struct SavedDualPotState {
  enum : uint32_t{ MainMode, CatchUpMode = 0x1234ABCD } restore_catchup_mode = MainMode;
  f restore_main_val = 0.5_f;
  f restore_alt_val = 0_f;
  bool validate() { 
    return restore_main_val >= 0_f && restore_main_val <= 1_f &&
           restore_alt_val  >= 0_f && restore_alt_val  <= 1_f;
  }
};

struct Parameters {
  f balance;                        // -1..1
  f root;                        // semitones
  f pitch;                       // midi note
  f spread;                      // semitones
  f detune;                      // semitones

  struct Modulation {
    ModulationMode mode;
    f value;                 // 0..1
  } modulation;

  struct Scale {
    ScaleMode mode;
    int value;                     // 0..9
  } scale;

  struct Twist {
    TwistMode mode;
    f value;                    // 0..1
  } twist;

  struct Warp {
    WarpMode mode;
    f value;                    // 0..1
  } warp;

  struct AltParameters {
    int numOsc;// = kMaxNumOsc;      // 0..kMaxNumOsc
    SplitMode stereo_mode;// = ALTERNATE;
    SplitMode freeze_mode;// = LOW_HIGH;
    f crossfade_factor;// = 0.125_f;

	SavedDualPotState pitch_pot_state;

    int engine;    // sound engine (pl::Engine), changed by tapping Learn

    bool validate() {
      return
        numOsc <= kMaxNumOsc &&
        numOsc > 0 &&
        ( stereo_mode == ALTERNATE ||
          stereo_mode == LOW_HIGH ||
          stereo_mode == LOWEST_REST ) &&
        ( freeze_mode == ALTERNATE ||
          freeze_mode == LOW_HIGH ||
          freeze_mode == LOWEST_REST ) &&
        crossfade_factor <= 1_f &&
        crossfade_factor >= 0_f &&
        engine >= 0 && engine < kNumEngines;
    }
  };
  AltParameters alt;
  // first boot: virtual analog engine (1)
  AltParameters default_alt = {kMaxNumOsc, ALTERNATE, LOW_HIGH, 0.125_f, {}, 1};

  // Plaits engines: raw knob+CV values, 0..1
  struct PlaitsControls {
    f harmonics = 0.5_f;         // Mod knob
    f timbre = 0.5_f;            // Twist knob
    f morph = 0.5_f;             // Warp knob
  } plaits;

  // lowered automatically if the CPU overloads (Plaits engines)
  int voice_cap = kMaxNumOsc;

  f new_note, fine_tune;
};

enum EventType {
  ButtonPush,
  ButtonRelease,
  ButtonTimeout,
  SwitchScale,
  SwitchMod,
  SwitchTwist,
  SwitchWarp,
  PotMove,
  NewNote,
  NewNoteAfterDelay,
  ScaleChange,
  AltParamChange,
  StartCatchup,
  EndOfCatchup,
  CalibrationFailed,
  CalibrationStepDone,
  LearnHold,           // Learn held for 1 s: release to enter Learn mode
};

struct Event {
  EventType type;
  int data;
};

enum CalibratorState {
  NOT_CALIBRATING,
  CALIBRATING_PITCH_UNPATCHED,
  CALIBRATING_ROOT_UNPATCHED,
  CALIBRATING_PITCH_OFFSET,
  CALIBRATING_PITCH_SLOPE,
  CALIBRATING_ROOT_OFFSET,
  CALIBRATING_ROOT_SLOPE,
};

