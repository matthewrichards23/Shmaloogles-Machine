SMILING BUILDUP  -  by DJ MJ   (v3 source)
Buildup machine with an RMX-style Roll. Stereo effect, VST3 + AU.
Universal binary: one build runs on Intel Macs and Apple Silicon (M4 mini).

BUILD (on the Mac you want to use it on)
  1. Install Xcode command line tools:   xcode-select --install
  2. Install CMake (3.22+): cmake.org or "brew install cmake"
  3. In Terminal, cd into this folder, then:
       cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
       cmake --build build --config Release -j 8
  The first build downloads JUCE 8.0.6 (needs internet) and takes a few minutes.
  The plugins are copied automatically to:
       ~/Library/Audio/Plug-Ins/VST3/Smiling Buildup.vst3
       ~/Library/Audio/Plug-Ins/Components/Smiling Buildup.component
  Rescan plugins in Ableton (Preferences > Plug-ins). If you build on the Intel Mac
  and copy the .vst3 to the M4, remove the quarantine flag first:
       xattr -cr "Smiling Buildup.vst3"
  If Ableton on the M4 refuses an unsigned plugin:
       codesign --force --deep -s - "Smiling Buildup.vst3"

USE
  Put it on a stereo track or group.
  - Pick a character tile (Pim, Charlie, Allan, Glep, Mr. Boss, Mr. Frog). Each one
    has its own mix of effects and its own roll behaviour.
  - INTENSITY is the build macro. Every effect follows it. AUTO BUILD ramps it over
    1-32 bars from when you hit play (Curve changes the shape). It holds at 100%
    until you press DROP.
  - DROP (hold): intensity snaps to 0, the noise, reverb and delay ring out over the
    TAIL time (knob at the end of the bottom row), then everything is silent.
  - Effect knobs (Filter, Flanger, Pitch, Noise, Drive, Delay, Reverb, Pan, Gate)
    set how much of each effect the build can reach.
  - ROLL (hold, or turn on Latch): loops a slice of audio at the chosen rate (1/4 down
    to 1/64, triplets, dotted). LINK makes the rate speed up as intensity rises.
      Roll source = Live input : freezes and loops the last slice of your incoming
                                 audio, like a beat-repeat. The input is replaced
                                 while rolling and still goes through the effects.
      Roll source = Sample     : retriggers YOUR sample on the grid, layered on top
                                 of the track, after the effects.
    Pitch / Gate (how much of each hit plays) / Fade (each repeat gets quieter) /
    Level shape the roll.
  - SAMPLES: drag audio files onto the waveform, or click "Load sample". There are 8
    slots (pick one with the Slot menu); dropping several files fills slots in order.
    wav / aiff / mp3 / flac, up to 30 seconds each. Samples are stored inside your
    Ableton set (files up to 12 MB), so you don't need to keep the originals.

NOTES
  - Not compiled or tested yet. The DSP building blocks were unit tested in a
    sandbox; the plugin itself could not be built there. If the build prints an
    error, send me the first error lines and I'll fix it.
  - The output has a transparent limiter at -0.5 dB, not clipping.
  - Rename: PRODUCT_NAME in CMakeLists.txt, getName() in Source/Plugin.cpp, and the
    "BUILDUP MACHINE" header text in SmilingEditor::paint().
  - Artwork and logo are Smiling Friends property: fine for personal use, but you'd
    need permission to distribute the plugin with them.
