# enosc3: Plaits engines for the Ensemble Oscillator

Alternative firmware for the Ensemble Oscillator Eurorack module, aimed at dub techno chords.
Each of the 16 voices can run a Plaits synthesis engine instead of the original sine engine.
The chord/scale system and the controls stay as on the original module.

This is an unofficial community firmware.
It is built from the open source Ensemble Oscillator code that 4ms Company and Matthias Puech publish ([github.com/4ms/enosc](https://github.com/4ms/enosc)).
The synthesis code is ported from the open source Mutable Instruments Plaits code by Emilie Gillet ([github.com/pichenettes/eurorack](https://github.com/pichenettes/eurorack)).
The project is not affiliated with or endorsed by 4ms Company or Mutable Instruments.
See [License](#license).

> **Status: not yet tested on hardware.**
> It builds and has been checked in a PC simulation.
> If the module shows no LEDs after flashing, re-flash any other firmware with the audio bootloader (the bootloader is not changed).

## Install

1. Get `release/enosc3.wav` from this repo (or build it, see [Build](#build)).
2. Put the module in bootloader mode: hold **Learn** and **Freeze** while powering on.
3. Play the wav file into the module's audio input, as for any official firmware update.

## Engines (tap Learn)

Tap **Learn** (shorter than 1 s) to step to the next engine.
The Learn LED color shows which engine is on:

| # | LED | Engine | Mod knob = HARMONICS | Twist knob = TIMBRE | Warp knob = MORPH |
|---|-----|--------|----------------------|---------------------|-------------------|
| 1 | red | Virtual analog | detune of the 2nd oscillator (unison, 5th, octave) | square width, then sync | saw shape |
| 2 | green | Waveshaper | waveshaper curve | wavefolder amount | slope asymmetry |
| 3 | blue | 2-operator FM | frequency ratio | FM index | feedback |
| 4 | magenta | Formant/grain | formant ratio | formant frequency | carrier shape |
| 5 | cyan | Filtered noise, tuned to the chord notes | filter LP, BP, HP | noise clock | resonance |
| 0 | yellow | Original sine engine | as original | as original | as original |

The first boot starts on engine 1 (virtual analog).
The chosen engine is remembered after power-off.

In the Plaits engines (1 to 5) the switches change meaning:

- **Twist switch**: main output / main and second output mixed / second output of each engine.
- **Warp switch**: voice color: open / warm low-pass / dark resonant low-pass. The cutoff follows the pitch of each voice. Every voice also goes through a soft saturation.
- **Mod switch**: HARMONICS is the same on all voices / spread across the chord / alternated per voice.

These work as on the original module: Root, Pitch, Spread, Balance, Detune, Scale, Freeze, the stereo split and the freeze split.

## Learn (kept)

- Hold Learn for 1 s and release to enter Learn mode. The LED turns dark red when you can release.
- Tap Learn again to leave Learn mode.
- Hold Learn and turn Root for manual learn.
- Hold Learn for 4 s to reset the current scale.
- Hold Learn and Freeze for CV calibration.

## Scales: minor and sad first, major and bright last

In every bank, the Scale knob fully left is the darkest scale and fully right the brightest:

- **12-TET** (switch up): minor triad, minor 7th, minor 9th (two octaves), stacked 4ths, In scale, natural minor, minor pentatonic, dorian, octave + 5th, major triad.
- **Just intonation** (switch middle): just minor triad, just minor 7th, subharmonics 4 to 7, just natural minor, Do-Fa-So, 5th, 7-TET, 5-TET, harmonics 8 to 15, harmonics 4 to 7.
- **Free** (switch down): stacked minor 3rds, Bohlen-Pierce minor, Messiaen modes 2 and 4, Bohlen-Pierce, Messiaen mode 3, Carlos alpha, whole tone, spiral of 5ths, Bohlen-Pierce major.

Settings and learned scales are kept apart from the original firmware's settings, so switching between firmwares does not mix them up.
CV and LED calibration are shared.

## CPU

The Plaits engines use more CPU than the sine engine.
In the PC simulation FM costs about 3.5 times as much and virtual analog about 1.6 times.
If the CPU gets close to full, a watchdog drops the top voices, then brings them back when there is room.
After an engine switch the engine starts with 8 voices and grows back to 16.
Chords of 3 to 5 notes should not be affected.

## Sound demos

`demo/` holds PC renders of each engine: a 4-voice minor 7th chord with the knobs sweeping.

## Build

Requirements: `arm-none-eabi-gcc` (v8 or later), GNU make, and Python 3.

```
make          # main.bin
make wav      # main.wav, the audio bootloader file
make size     # sizes of the RAM image and of the flash
```

On Windows the makefile calls Python as `py -3`. Elsewhere use `make PYTHON=python3`.

The original firmware already fills most of the 48 KB of application flash, so the Plaits code cannot fit as it is. The build works around this:

1. The application is linked to run from RAM (`lib/CMSIS/enosc3_ram.ld`, about 64 KB).
2. `tools/pack_image.py` compresses it with raw deflate.
3. `loader/` is a small loader stored at the start of the application flash (0x08004000). At boot it unpacks the application into SRAM1 at 0x20010000, checks a hash and jumps to it. If the image is corrupt it halts rather than running garbage.

About 47.3 KB of the 48 KB of flash is used.

Source layout of the new code:

- `src/plaits_voice.hh`: ported Plaits oscillators and engines (plain float, no stmlib).
- `src/plaits_tables.cc/.hh`: lookup tables taken from Plaits.
- `src/polyptic_oscillator.hh`: the voice bank, with the Plaits voice pairs, color filter and engine crossfade.
- `src/ui.hh`: engine selection on Learn, plus the CPU watchdog.
- `src/quantizer.hh`: the reordered scale banks.

## License

This firmware is a derivative work of two open source projects. Their copyright and license notices are kept in [LICENSE](LICENSE), as both licenses require:

- Ensemble Oscillator source code, Copyright (c) 2020 Matthias Puech and Dan Green, BSD 2-clause license.
- Mutable Instruments Plaits DSP code, Copyright 2016 Emilie Gillet, MIT license.

Third-party libraries are under their own licenses, given in their files: the ST HAL/CMSIS in `lib/` and the audio bootloader and stmlib in `bootloader/`.

"Ensemble Oscillator" and "4ms" are names of 4ms Company and are used here only to say which hardware this firmware runs on.
