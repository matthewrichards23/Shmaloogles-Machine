// SMILING BUILDUP - buildup machine with an RMX-style roll. By DJ MJ.
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <BinaryData.h>
#include <array>
#include <cmath>
#include "Dsp.h"

using namespace juce;
using APVTS = AudioProcessorValueTreeState;

//==============================================================================
namespace
{
constexpr int    kSlots           = 8;
constexpr double kMaxSampleSecs   = 30.0;
constexpr size_t kMaxEmbedBytes   = 12u * 1024u * 1024u;   // bigger files are referenced by path only

enum Fx { fxFilter, fxFlanger, fxPitch, fxNoise, fxDrive, fxDelay, fxReverb, fxPan, fxGate, numFx };

const char* const fxIds[numFx]   = { "fx_filter", "fx_flanger", "fx_pitch", "fx_noise", "fx_drive",
                                     "fx_delay", "fx_reverb", "fx_pan", "fx_gate" };
const char* const fxNames[numFx] = { "Filter", "Flanger", "Pitch", "Noise", "Drive",
                                     "Delay", "Reverb", "Pan", "Gate" };

const char* const charNames[6] = { "Pim", "Charlie", "Allan", "Glep", "Mr. Boss", "Mr. Frog" };
const char* const charLong[6]  = { "Pim - Euphoria", "Charlie - Classic", "Allan - Rhythm",
                                   "Glep - Glitch", "Mr. Boss - Madness", "Mr. Frog - Absolute Chaos" };

// How much of each effect every character brings to the build.
const float profile[6][numFx] =
{
    //  filt  flng  pitch noise drive delay rev   pan   gate
    { 0.90f, 0.50f, 0.55f, 0.25f, 0.10f, 0.50f, 1.00f, 0.60f, 0.00f },   // Pim
    { 1.00f, 0.30f, 0.00f, 0.60f, 0.20f, 0.60f, 0.60f, 0.30f, 0.40f },   // Charlie
    { 0.60f, 0.00f, 0.00f, 0.30f, 0.20f, 0.90f, 0.30f, 0.50f, 1.00f },   // Allan
    { 0.80f, 0.70f, 0.40f, 0.40f, 0.50f, 0.80f, 0.30f, 0.90f, 0.80f },   // Glep
    { 1.00f, 0.80f, 0.70f, 0.70f, 0.80f, 0.80f, 0.70f, 0.70f, 0.60f },   // Mr. Boss
    { 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f }    // Mr. Frog
};
const float crushProfile[6] = { 0.0f, 0.0f, 0.0f, 0.45f, 0.30f, 0.85f };
const float riseProfile[6]  = { 0.0f, 0.0f, 0.0f, 0.0f, 7.0f, 12.0f };   // roll pitch rise (semitones at full build)

// Roll divisions in beats: 1/4, 1/8, 1/16, 1/32, 1/64, 1/8T, 1/16T, 1/8D
const double divs[8] = { 1.0, 0.5, 0.25, 0.125, 0.0625, 1.0 / 3.0, 1.0 / 6.0, 0.75 };

Font makeFont (float h, bool bold = false)
{
    return Font (FontOptions (h, bold ? Font::bold : Font::plain));
}

APVTS::ParameterLayout createLayout()
{
    APVTS::ParameterLayout l;

    auto f = [&l] (const char* id, const char* name, float lo, float hi, float def, float skew = 1.0f)
    {
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name,
                                                      NormalisableRange<float> (lo, hi, 0.0f, skew), def));
    };
    auto b = [&l] (const char* id, const char* name, bool def)
    {
        l.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def));
    };
    auto c = [&l] (const char* id, const char* name, StringArray items, int def)
    {
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, items, def));
    };

    f ("build", "Intensity", 0.0f, 1.0f, 0.0f);
    c ("mode", "Character", StringArray { "Pim", "Charlie", "Allan", "Glep", "Mr. Boss", "Mr. Frog" }, 1);

    for (int k = 0; k < numFx; ++k)
        f (fxIds[k], fxNames[k], 0.0f, 1.0f, 1.0f);

    f ("tail", "Tail", 0.1f, 6.0f, 1.5f, 0.5f);

    b ("auto", "Auto build", false);
    c ("bars", "Build length", StringArray { "1 bar", "2 bars", "4 bars", "8 bars", "16 bars", "32 bars" }, 3);
    c ("curve", "Curve", StringArray { "Linear", "Exponential", "Logarithmic", "S curve" }, 0);
    b ("drop", "Drop", false);

    b ("roll", "Roll", false);
    b ("latch", "Roll latch", false);
    c ("rollsrc", "Roll source", StringArray { "Live input", "Sample" }, 0);
    c ("slot", "Sample slot", StringArray { "Slot 1", "Slot 2", "Slot 3", "Slot 4",
                                            "Slot 5", "Slot 6", "Slot 7", "Slot 8" }, 0);
    c ("rate", "Roll rate", StringArray { "1/4", "1/8", "1/16", "1/32", "1/64", "1/8T", "1/16T", "1/8D" }, 2);
    b ("link", "Link to intensity", false);
    f ("roll_pitch", "Roll pitch", -24.0f, 24.0f, 0.0f);
    f ("roll_gate",  "Roll gate",  0.05f, 1.0f, 0.85f);
    f ("roll_fade",  "Roll fade",  0.0f, 1.0f, 0.0f);
    f ("roll_level", "Roll level", 0.0f, 1.0f, 1.0f);

    return l;
}

struct Slot
{
    String name, path;
    MemoryBlock bytes;
    AudioBuffer<float> audio;
    double fileRate = 44100.0;
    bool valid() const { return audio.getNumSamples() > 1; }
};
} // namespace

//==============================================================================
class SmilingBuildup : public AudioProcessor
{
public:
    SmilingBuildup()
        : AudioProcessor (BusesProperties().withInput  ("Input",  AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "STATE", createLayout())
    {
        formats.registerBasicFormats();

        auto g = [this] (const char* id) { return apvts.getRawParameterValue (id); };
        P.build = g ("build");     P.mode = g ("mode");       P.tail = g ("tail");
        P.aut   = g ("auto");      P.bars = g ("bars");       P.curve = g ("curve");
        P.drop  = g ("drop");      P.roll = g ("roll");       P.rollSrc = g ("rollsrc");
        P.slot  = g ("slot");      P.rate = g ("rate");       P.link = g ("link");
        P.rPitch = g ("roll_pitch"); P.rGate = g ("roll_gate");
        P.rFade = g ("roll_fade");   P.rLevel = g ("roll_level");
        for (int k = 0; k < numFx; ++k) P.fx[k] = g (fxIds[k]);
    }

    //==========================================================================
    const String getName() const override              { return "Smiling Buildup"; }
    bool acceptsMidi() const override                  { return false; }
    bool producesMidi() const override                 { return false; }
    bool isMidiEffect() const override                 { return false; }
    double getTailLengthSeconds() const override       { return 8.0; }
    int getNumPrograms() override                      { return 1; }
    int getCurrentProgram() override                   { return 0; }
    void setCurrentProgram (int) override              {}
    const String getProgramName (int) override         { return {}; }
    void changeProgramName (int, const String&) override {}
    bool hasEditor() const override                    { return true; }
    AudioProcessorEditor* createEditor() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet()  == AudioChannelSet::stereo()
            && layouts.getMainOutputChannelSet() == AudioChannelSet::stereo();
    }

    //==========================================================================
    void prepareToPlay (double rate, int maxBlock) override
    {
        sr = rate;
        ring.setSize (2, (int) (sr * 4.0) + 8);     ring.clear();     rw = 0;
        loopBuf.setSize (2, (int) (sr * 3.0) + 8);  loopBuf.clear();  loopLen = 0;

        for (int c = 0; c < 2; ++c)
        {
            delayLine[c].prepare ((int) (sr * 4.0));
            delayLine[c].clear();
            flanger[c].prepare ((int) (sr * 0.012) + 8);
            flanger[c].clear();
            flFb[c] = 0; fbLP[c] = 0;
            hp[c].reset(); noiseF[c].reset();
        }
        pitch.prepare (sr);
        reverb.prepare (sr);

        smoothA = 0; held = 0; gSmooth = 1.0f; rollMix = 0;
        flPh = panPh = 0; dTime = (float) (0.75 * 60.0 / 120.0 * sr);
        voiceOn = false; voicePos = 0; repGain = 1.0f;
        rollWasOn = false; startFlag = false; needCapture = true; lastRi = -1;
        wasAuto = wasPlaying = wasDrop = false;
        freeBeat = 0; buildStart = 0;
        holdCount = 0; holdL = holdR = 0;

        dsp::ProcessSpec spec { rate, (uint32) maxBlock, 2 };
        limiter.prepare (spec);
        limiter.setThreshold (-0.5f);
        limiter.setRelease (60.0f);
    }

    void releaseResources() override {}

    //==========================================================================
    void processBlock (AudioBuffer<float>& buffer, MidiBuffer&) override
    {
        ScopedNoDenormals noDenormals;
        const int n = buffer.getNumSamples();
        if (buffer.getNumChannels() < 2 || n <= 0)
            return;

        // ---- transport -------------------------------------------------------
        double bpm = 120.0, hostBeat = 0.0;
        bool playing = false, haveBeat = false;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                if (auto t = pos->getBpm())         bpm = *t;
                if (auto q = pos->getPpqPosition()) { hostBeat = *q; haveBeat = true; }
                playing = pos->getIsPlaying();
            }
        bpm = jlimit (30.0, 300.0, bpm);
        const double step   = bpm / (60.0 * sr);              // beats per sample
        const double beat0  = (playing && haveBeat) ? hostBeat : freeBeat;
        freeBeat = beat0 + n * step;                           // free clock follows the host

        // ---- parameters ------------------------------------------------------
        const int   mode = jlimit (0, 5, (int) P.mode->load());
        float mul[numFx];
        for (int k = 0; k < numFx; ++k) mul[k] = P.fx[k]->load() * profile[mode][k];

        const float tailSec     = jmax (0.05f, P.tail->load());
        const float releaseCoef = (float) std::pow (0.001, 1.0 / ((double) tailSec * sr));

        const bool aut  = P.aut->load()  > 0.5f;
        const bool drop = P.drop->load() > 0.5f;
        const double rampBeats = 4.0 * (double) (1 << jlimit (0, 5, (int) P.bars->load()));

        if (aut && (! wasAuto || (playing && ! wasPlaying) || (wasDrop && ! drop))) buildStart = beat0;
        if (aut && playing && beat0 < buildStart) buildStart = beat0;

        float target = P.build->load();
        if (aut)
        {
            if (playing)
            {
                float x = jlimit (0.0f, 1.0f, (float) ((beat0 - buildStart) / rampBeats));
                switch ((int) P.curve->load())
                {
                    case 1: x = x * x; break;
                    case 2: x = std::sqrt (x); break;
                    case 3: x = x * x * (3.0f - 2.0f * x); break;
                    default: break;
                }
                target = x;
            }
            else target = 0.0f;
        }
        if (drop) target = 0.0f;
        wasAuto = aut; wasPlaying = playing; wasDrop = drop;

        const bool  rollOn  = P.roll->load() > 0.5f;
        const int   src     = (int) P.rollSrc->load();                 // 0 live, 1 sample
        const int   slotIdx = jlimit (0, kSlots - 1, (int) P.slot->load());
        const bool  link    = P.link->load() > 0.5f;
        const float gateFrac = P.rGate->load();
        const float fadeMul  = 1.0f - 0.4f * P.rFade->load();
        const float rLevel   = P.rLevel->load();

        int ri = jlimit (0, 7, (int) P.rate->load());
        if (link) ri = jlimit (0, 4, (int) (smoothA * 5.0f));
        const double div = divs[ri];
        if (ri != lastRi) { needCapture = true; lastRi = ri; }
        if (src != lastSrc) { needCapture = true; lastSrc = src; }
        if (rollOn && ! rollWasOn) startFlag = true;
        rollWasOn = rollOn;

        const float semis    = P.rPitch->load() + riseProfile[mode] * smoothA;
        const double liveRate = std::pow (2.0, (double) semis / 12.0);

        // ---- per-block constants --------------------------------------------
        const float srF = (float) sr;
        const float lag         = 1.0f - std::exp (-1.0f / (srF * 0.020f));
        const float dropLag     = 1.0f - std::exp (-1.0f / (srF * 0.006f));
        const float gateCoef    = 1.0f - std::exp (-1.0f / (srF * 0.0015f));
        const float delayCoef   = 1.0f - std::exp (-1.0f / (srF * 0.12f));
        const float rollMixCoef = 1.0f - std::exp (-1.0f / (srF * 0.004f));
        const float fadeInSmp   = jmax (1.0f, srF * 0.002f);
        const float fadeOutSmp  = jmax (1.0f, srF * 0.003f);
        const float delayMax    = (float) (sr * 3.9);

        reverb.setDecay (0.8f + 5.2f * jmin (1.0f, held * mul[fxReverb]));

        // sample slot (try-lock: if the UI is loading a file right now, skip it this block)
        const ScopedTryLock tryLock (lock);
        const Slot* slotPtr = nullptr;
        if (tryLock.isLocked() && slots[(size_t) slotIdx].valid())
            slotPtr = &slots[(size_t) slotIdx];
        const double smpRate = slotPtr != nullptr
                             ? slotPtr->fileRate / sr * std::pow (2.0, (double) semis / 12.0) : 1.0;

        float* L = buffer.getWritePointer (0);
        float* R = buffer.getWritePointer (1);
        float* ringL = ring.getWritePointer (0);
        float* ringR = ring.getWritePointer (1);
        float* loopL = loopBuf.getWritePointer (0);
        float* loopR = loopBuf.getWritePointer (1);
        const int ringN = ring.getNumSamples();

        auto capture = [&]
        {
            loopLen = jlimit (32, loopBuf.getNumSamples() - 2, (int) (div * 60.0 / bpm * sr));
            int start = rw - loopLen;
            while (start < 0) start += ringN;
            for (int j = 0; j < loopLen; ++j)
            {
                const int k = (start + j) % ringN;
                loopL[j] = ringL[k];
                loopR[j] = ringR[k];
            }
        };

        float crushSteps = 32768.0f, crushMix = 0.0f;
        int   crushHold = 1;

        // ======================================================================
        for (int i = 0; i < n; ++i)
        {
            const double now = beat0 + i * step;

            // ---- build amount (fast down, slower up) ---------------------------
            smoothA += (target - smoothA) * (target < smoothA ? dropLag : lag);
            const float a = smoothA;

            // "held" follows the build up instantly and falls slowly: this is the tail
            if (a >= held) held = a;
            else { held *= releaseCoef; if (held < 1.0e-4f) held = 0.0f; }

            float in0 = L[i], in1 = R[i];

            // keep the live input in the ring buffer for rolling
            ringL[rw] = in0; ringR[rw] = in1;
            rw = (rw + 1) % ringN;

            // ---- roll ---------------------------------------------------------
            bool tick = false;
            if (rollOn)
            {
                if (startFlag)
                {
                    tick = true; startFlag = false; firstHit = true; repGain = 1.0f; needCapture = true;
                }
                else if (std::floor (now / div) != std::floor ((now - step) / div))
                    tick = true;

                if (tick)
                {
                    if (! firstHit) repGain = jmax (0.02f, repGain * fadeMul);
                    firstHit = false;
                    voicePos = 0.0;
                    voiceOn  = true;
                    if (src == 0 && needCapture) { capture(); needCapture = false; }
                }
            }

            float liveL = 0, liveR = 0, hitL = 0, hitR = 0;
            if (voiceOn)
            {
                if (src == 0)
                {
                    const int gl = jmax (2, (int) (loopLen * gateFrac));
                    const int ix = (int) voicePos;
                    if (loopLen < 4 || ix >= gl - 1) voiceOn = false;
                    else
                    {
                        const float fr  = (float) (voicePos - (double) ix);
                        const float env = jmin ((float) ix / fadeInSmp, (float) (gl - 1 - ix) / fadeOutSmp, 1.0f)
                                        * repGain * rLevel;
                        liveL = (loopL[ix] + (loopL[ix + 1] - loopL[ix]) * fr) * env;
                        liveR = (loopR[ix] + (loopR[ix + 1] - loopR[ix]) * fr) * env;
                        voicePos += liveRate;
                    }
                }
                else if (slotPtr != nullptr)
                {
                    const int len = slotPtr->audio.getNumSamples();
                    const int gl  = jmax (2, (int) (len * gateFrac));
                    const int ix  = (int) voicePos;
                    if (ix >= gl - 1) voiceOn = false;
                    else
                    {
                        const float fr  = (float) (voicePos - (double) ix);
                        const float env = jmin ((float) ix / fadeInSmp, (float) (gl - 1 - ix) / fadeOutSmp, 1.0f)
                                        * repGain * rLevel;
                        const float* s0 = slotPtr->audio.getReadPointer (0);
                        const float* s1 = slotPtr->audio.getReadPointer (1);
                        hitL = (s0[ix] + (s0[ix + 1] - s0[ix]) * fr) * env;
                        hitR = (s1[ix] + (s1[ix + 1] - s1[ix]) * fr) * env;
                        voicePos += smpRate;
                    }
                }
                else voiceOn = false;
            }

            // live roll replaces the input; sample hits are added after the effects
            rollMix += (((rollOn && src == 0) ? 1.0f : 0.0f) - rollMix) * rollMixCoef;
            float x0 = in0, x1 = in1;
            if (rollMix > 0.0001f)
            {
                x0 = in0 * (1.0f - rollMix) + liveL * rollMix;
                x1 = in1 * (1.0f - rollMix) + liveR * rollMix;
            }

            // ---- pitch riser --------------------------------------------------
            const float pAmt = a * mul[fxPitch];
            if ((i & 15) == 0) pitch.setRatio (std::pow (2.0f, pAmt));
            float pl, pr;
            pitch.process (x0, x1, pl, pr);
            const float pBlend = jlimit (0.0f, 1.0f, (pAmt - 0.03f) * 4.0f);
            x0 += (pl - x0) * pBlend;
            x1 += (pr - x1) * pBlend;

            // ---- flanger ------------------------------------------------------
            const float fAmt = a * mul[fxFlanger];
            flPh += (0.12f + 5.0f * fAmt * fAmt) / srF;
            if (flPh >= 1.0f) flPh -= 1.0f;
            const float lfoA = 0.5f + 0.5f * std::sin (sb::kTwoPi * flPh);
            const float lfoB = 0.5f + 0.5f * std::sin (sb::kTwoPi * (flPh + 0.25f));
            const float fbk  = 0.35f + 0.45f * fAmt;
            flanger[0].push (x0 + flFb[0] * fbk);
            flanger[1].push (x1 + flFb[1] * fbk);
            const float fd0 = flanger[0].read ((0.0004f + 0.0036f * lfoA) * srF - 1.0f);
            const float fd1 = flanger[1].read ((0.0004f + 0.0036f * lfoB) * srF - 1.0f);
            flFb[0] = fd0; flFb[1] = fd1;
            x0 = x0 * (1.0f - 0.5f * fAmt) + fd0 * 0.5f * fAmt;
            x1 = x1 * (1.0f - 0.5f * fAmt) + fd1 * 0.5f * fAmt;

            // ---- resonant high-pass sweep -------------------------------------
            const float filtAmt = a * mul[fxFilter];
            if ((i & 15) == 0)
            {
                const float cutoff = 30.0f * std::pow (220.0f, filtAmt);
                const float q = 0.7f + 2.8f * filtAmt * filtAmt;
                hp[0].set (cutoff, q, srF);
                hp[1].set (cutoff, q, srF);
            }
            float lo, bd, h0, h1;
            hp[0].run (x0, lo, bd, h0);
            hp[1].run (x1, lo, bd, h1);
            const float fMix = jmin (1.0f, filtAmt * 6.0f);
            x0 += (h0 - x0) * fMix;
            x1 += (h1 - x1) * fMix;

            // ---- drive + crush ------------------------------------------------
            const float drvAmt = jmin (1.0f, a * mul[fxDrive]);
            if (drvAmt > 0.001f)
            {
                const float g = 1.0f + drvAmt * 7.0f;
                x0 = x0 * (1.0f - drvAmt) + std::tanh (x0 * g) * drvAmt * 0.8f;
                x1 = x1 * (1.0f - drvAmt) + std::tanh (x1 * g) * drvAmt * 0.8f;
            }
            if ((i & 15) == 0)
            {
                const float cr = jmin (1.0f, a * mul[fxDrive] * crushProfile[mode]);
                crushMix   = jmin (1.0f, cr * 4.0f);
                crushSteps = std::pow (2.0f, 15.0f - 10.0f * cr);
                crushHold  = 1 + (int) (cr * 8.0f);
            }
            if (crushMix > 0.01f)
            {
                if (--holdCount <= 0)
                {
                    holdCount = crushHold;
                    holdL = std::round (x0 * crushSteps) / crushSteps;
                    holdR = std::round (x1 * crushSteps) / crushSteps;
                }
                x0 = x0 * (1.0f - crushMix) + holdL * crushMix;
                x1 = x1 * (1.0f - crushMix) + holdR * crushMix;
            }

            // sends for delay and reverb are taken here (before gate and pan)
            const float sL = x0, sR = x1;

            // ---- tempo gate: rate speeds up with the build ----------------------
            const float gAmt = jmin (1.0f, a * mul[fxGate]);
            float gTarget = 1.0f;
            if (gAmt > 0.001f)
            {
                const double gDiv = std::pow (0.5, std::floor (a * 4.99f));
                double gp = now / gDiv;
                gp -= std::floor (gp);
                if (gp >= 0.5) gTarget = 1.0f - 0.95f * gAmt;
            }
            gSmooth += (gTarget - gSmooth) * gateCoef;
            x0 *= gSmooth; x1 *= gSmooth;

            // ---- auto-pan (tempo synced, speeds up with the build) -------------
            const float panAmt = jmin (1.0f, a * mul[fxPan]);
            panPh += (float) (bpm / 60.0) * 0.25f * std::pow (16.0f, a) / srF;
            if (panPh >= 1.0f) panPh -= 1.0f;
            const float pan   = std::sin (sb::kTwoPi * panPh) * panAmt;
            const float angle = (pan + 1.0f) * (sb::kPi * 0.25f);
            x0 *= 1.41421356f * std::cos (angle);
            x1 *= 1.41421356f * std::sin (angle);

            // ---- ping-pong delay (dotted 1/8) -----------------------------------
            const float dTarget = jlimit (1.0f, delayMax, (float) (0.75 * 60.0 / bpm * sr));
            dTime += (dTarget - dTime) * delayCoef;
            const float dAmt  = a * mul[fxDelay];
            const float dHeld = jmin (1.0f, held * mul[fxDelay]);
            const float dl0 = delayLine[0].read (dTime - 1.0f);
            const float dl1 = delayLine[1].read (dTime - 1.0f);
            fbLP[0] += 0.45f * (dl1 - fbLP[0]);
            fbLP[1] += 0.45f * (dl0 - fbLP[1]);
            const float dfb = 0.25f + 0.55f * dHeld;
            delayLine[0].push (sL * dAmt + fbLP[0] * dfb);
            delayLine[1].push (sR * dAmt + fbLP[1] * dfb);

            // ---- reverb ---------------------------------------------------------
            const float rSend = a * mul[fxReverb] * 1.5f;
            const float rRet  = jmin (1.0f, held * mul[fxReverb] * 2.0f);
            float rvL, rvR;
            reverb.process (sL * rSend, sR * rSend, rvL, rvR);

            // ---- noise riser with a real tail ------------------------------------
            float nz0 = 0, nz1 = 0;
            if (held > 0.0f)
            {
                if ((i & 15) == 0)
                {
                    const float cut = 300.0f * std::pow (30.0f, held);
                    noiseF[0].set (cut, 1.5f, srF);
                    noiseF[1].set (cut, 1.5f, srF);
                }
                float b0, b1, hi0, hi1;
                noiseF[0].run (rng.next(), lo, b0, hi0);
                noiseF[1].run (rng.next(), lo, b1, hi1);
                const float lvl = held * std::sqrt (held) * mul[fxNoise] * 0.25f;
                nz0 = (b0 * 0.25f + hi0 * 0.6f) * lvl;
                nz1 = (b1 * 0.25f + hi1 * 0.6f) * lvl;
            }

            L[i] = x0 + dl0 * dHeld * 0.7f + rvL * rRet + nz0 + hitL;
            R[i] = x1 + dl1 * dHeld * 0.7f + rvR * rRet + nz1 + hitR;
        }

        meter.store (smoothA);

        // final safety: transparent below the threshold, no clipping crackle
        dsp::AudioBlock<float> block (buffer);
        dsp::ProcessContextReplacing<float> ctx (block);
        limiter.process (ctx);
    }

    //==========================================================================
    // Sample slots
    bool loadFile (int idx, const File& f)
    {
        MemoryBlock bytes;
        if (! f.loadFileAsData (bytes)) return false;
        return loadBytes (idx, std::move (bytes), f.getFileName(), f.getFullPathName());
    }

    bool loadBytes (int idx, MemoryBlock bytes, const String& name, const String& path)
    {
        if (idx < 0 || idx >= kSlots) return false;

        std::unique_ptr<AudioFormatReader> reader (
            formats.createReaderFor (std::make_unique<MemoryInputStream> (bytes, true)));
        if (reader == nullptr && path.isNotEmpty() && File (path).existsAsFile())
            reader.reset (formats.createReaderFor (File (path)));
        if (reader == nullptr || reader->lengthInSamples <= 1
            || (double) reader->lengthInSamples > reader->sampleRate * kMaxSampleSecs)
            return false;

        const int len = (int) reader->lengthInSamples;
        Slot s;
        s.audio.setSize (2, len);
        s.audio.clear();
        if (! reader->read (&s.audio, 0, len, 0, true, true)) return false;
        if (reader->numChannels == 1) s.audio.copyFrom (1, 0, s.audio, 0, 0, len);
        s.fileRate = reader->sampleRate;
        s.name = name;
        s.path = path;
        if (bytes.getSize() <= kMaxEmbedBytes) s.bytes = std::move (bytes);

        std::vector<float> peaks (346, 0.0f);
        for (int j = 0; j < 346; ++j)
        {
            const int a = (int) ((int64) j * len / 346);
            const int b = jmin (len, jmax (a + 1, (int) ((int64) (j + 1) * len / 346)));
            peaks[(size_t) j] = s.audio.getMagnitude (0, a, b - a);
        }

        {
            const ScopedLock sl (lock);
            slots[(size_t) idx] = std::move (s);
        }
        {
            const ScopedLock sl (uiLock);
            uiNames[(size_t) idx] = name;
            uiPeaks[(size_t) idx] = std::move (peaks);
        }
        return true;
    }

    std::vector<float> getPeaks (int idx)
    {
        const ScopedLock sl (uiLock);
        return uiPeaks[(size_t) jlimit (0, kSlots - 1, idx)];
    }

    String getSlotName (int idx)
    {
        const ScopedLock sl (uiLock);
        return uiNames[(size_t) jlimit (0, kSlots - 1, idx)];
    }

    //==========================================================================
    void getStateInformation (MemoryBlock& dest) override
    {
        auto state = apvts.copyState();
        auto old = state.getChildWithName ("SLOTS");
        if (old.isValid()) state.removeChild (old, nullptr);

        ValueTree tree ("SLOTS");
        {
            const ScopedLock sl (lock);
            for (int i = 0; i < kSlots; ++i)
            {
                const auto& s = slots[(size_t) i];
                if (! s.valid()) continue;
                ValueTree t ("SLOT");
                t.setProperty ("index", i, nullptr);
                t.setProperty ("name", s.name, nullptr);
                t.setProperty ("path", s.path, nullptr);
                if (s.bytes.getSize() > 0) t.setProperty ("data", s.bytes.toBase64Encoding(), nullptr);
                tree.appendChild (t, nullptr);
            }
        }
        state.appendChild (tree, nullptr);
        if (auto xml = state.createXml())
            copyXmlToBinary (*xml, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
        {
            if (! xml->hasTagName (apvts.state.getType())) return;
            auto tree = ValueTree::fromXml (*xml);
            auto slotTree = tree.getChildWithName ("SLOTS");
            if (slotTree.isValid()) tree.removeChild (slotTree, nullptr);
            apvts.replaceState (tree);

            for (int j = 0; j < slotTree.getNumChildren(); ++j)
            {
                auto t = slotTree.getChild (j);
                MemoryBlock mb;
                const String b64 = t.getProperty ("data").toString();
                if (b64.isNotEmpty()) mb.fromBase64Encoding (b64);
                loadBytes ((int) t.getProperty ("index"), std::move (mb),
                           t.getProperty ("name").toString(), t.getProperty ("path").toString());
            }
        }
    }

    //==========================================================================
    APVTS apvts;
    std::atomic<float> meter { 0.0f };

private:
    struct Params
    {
        std::atomic<float> *build, *mode, *tail, *aut, *bars, *curve, *drop, *roll, *rollSrc, *slot, *rate,
                           *link, *rPitch, *rGate, *rFade, *rLevel;
        std::atomic<float>* fx[numFx];
    } P {};

    AudioFormatManager formats;
    CriticalSection lock, uiLock;
    std::array<Slot, kSlots> slots;
    std::array<String, kSlots> uiNames;
    std::array<std::vector<float>, kSlots> uiPeaks;

    double sr = 44100.0;

    // effects
    sb::FDNReverb reverb;
    sb::PitchShifter pitch;
    sb::DelayLine delayLine[2], flanger[2];
    sb::SVF hp[2], noiseF[2];
    sb::Noise rng;
    dsp::Limiter<float> limiter;
    float flFb[2] {}, fbLP[2] {};
    float smoothA = 0, held = 0, gSmooth = 1.0f, flPh = 0, panPh = 0, dTime = 0;
    float holdL = 0, holdR = 0;
    int   holdCount = 0;

    // roll
    AudioBuffer<float> ring, loopBuf;
    int    rw = 0, loopLen = 0, lastRi = -1, lastSrc = -1;
    double voicePos = 0;
    float  repGain = 1.0f, rollMix = 0;
    bool   voiceOn = false, rollWasOn = false, startFlag = false, needCapture = true, firstHit = true;

    // transport / build state
    double freeBeat = 0, buildStart = 0;
    bool   wasAuto = false, wasPlaying = false, wasDrop = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SmilingBuildup)
};

//==============================================================================
// UI
//==============================================================================
namespace
{
class SmileLnF : public LookAndFeel_V4
{
public:
    SmileLnF()
    {
        setColour (ComboBox::backgroundColourId, Colour (0xffeae5da));
        setColour (ComboBox::textColourId, Colours::black);
        setColour (ComboBox::outlineColourId, Colour (0xffcfc8ba));
        setColour (ComboBox::arrowColourId, Colours::black);
        setColour (PopupMenu::backgroundColourId, Colour (0xfffffdf8));
        setColour (PopupMenu::textColourId, Colours::black);
        setColour (PopupMenu::highlightedBackgroundColourId, Colour (0xffffef65));
        setColour (PopupMenu::highlightedTextColourId, Colours::black);
        setColour (TextButton::buttonColourId, Colour (0xffeae5da));
        setColour (TextButton::textColourOffId, Colours::black);
        setColour (ToggleButton::textColourId, Colours::black);
        setColour (ToggleButton::tickColourId, Colours::black);
        setColour (ToggleButton::tickDisabledColourId, Colours::grey);
        setColour (Slider::textBoxTextColourId, Colours::black);
        setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    }

    void drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, Slider& s) override
    {
        auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
        const float radius = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float lw = jmax (4.0f, radius * 0.16f);
        const float arcR = radius - lw * 0.5f;
        const float angle = startAngle + pos * (endAngle - startAngle);

        Path track;
        track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (Colour (0xffd8d2c7));
        g.strokePath (track, PathStrokeType (lw, PathStrokeType::curved, PathStrokeType::rounded));

        Path val;
        val.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, angle, true);
        g.setColour (s.findColour (Slider::rotarySliderFillColourId));
        g.strokePath (val, PathStrokeType (lw, PathStrokeType::curved, PathStrokeType::rounded));

        const float inner = arcR - lw * 0.9f;
        g.setColour (Colour (0xff282828));
        g.fillEllipse (centre.x - inner, centre.y - inner, inner * 2.0f, inner * 2.0f);

        Path p;
        p.addRoundedRectangle (-1.6f, -inner + 2.0f, 3.2f, inner * 0.45f, 1.5f);
        p.applyTransform (AffineTransform::rotation (angle).translated (centre));
        g.setColour (Colours::white);
        g.fillPath (p);
    }
};

// Press-and-hold button bound to a bool parameter (optionally latching).
class HoldPad : public Component
{
public:
    HoldPad (APVTS& s, const String& paramId, const String& label, const String& latchId = {})
        : param (s.getParameter (paramId)), text (label)
    {
        if (latchId.isNotEmpty()) latchParam = s.getParameter (latchId);
    }

    void setAccent (Colour c) { accent = c; }

    void mouseDown (const MouseEvent&) override
    {
        if (param == nullptr) return;
        latchedAtDown = latchParam != nullptr && latchParam->getValue() > 0.5f;
        param->beginChangeGesture();
        if (latchedAtDown) param->setValueNotifyingHost (param->getValue() > 0.5f ? 0.0f : 1.0f);
        else               param->setValueNotifyingHost (1.0f);
        repaint();
    }

    void mouseUp (const MouseEvent&) override
    {
        if (param == nullptr) return;
        if (! latchedAtDown) param->setValueNotifyingHost (0.0f);
        param->endChangeGesture();
        repaint();
    }

    void paint (Graphics& g) override
    {
        const bool on = param != nullptr && param->getValue() > 0.5f;
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (on ? accent : Colour (0xffeae5da));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (Colour (0xff282828));
        g.drawRoundedRectangle (r, 8.0f, 2.0f);
        g.setFont (makeFont (15.0f, true));
        g.drawText (text, getLocalBounds(), Justification::centred);
    }

private:
    RangedAudioParameter* param = nullptr;
    RangedAudioParameter* latchParam = nullptr;
    String text;
    Colour accent { 0xffffef65 };
    bool latchedAtDown = false;
};
} // namespace

//==============================================================================
class SmilingEditor : public AudioProcessorEditor,
                      public FileDragAndDropTarget,
                      private Timer
{
public:
    explicit SmilingEditor (SmilingBuildup& p)
        : AudioProcessorEditor (p), proc (p),
          rollPad (p.apvts, "roll", "ROLL", "latch"),
          dropPad (p.apvts, "drop", "DROP")
    {
        setLookAndFeel (&lnf);
        setSize (850, 560);

        auto knob = [this] (Slider& k, const char* id)
        {
            k.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle (Slider::TextBoxBelow, false, 64, 16);
            k.setColour (Slider::rotarySliderFillColourId, Colour (0xffefb744));
            addAndMakeVisible (k);
            sliderAtt.push_back (std::make_unique<APVTS::SliderAttachment> (proc.apvts, id, k));
        };
        auto combo = [this] (ComboBox& c, const char* id, const StringArray& items)
        {
            c.addItemList (items, 1);
            addAndMakeVisible (c);
            comboAtt.push_back (std::make_unique<APVTS::ComboBoxAttachment> (proc.apvts, id, c));
        };
        auto toggle = [this] (ToggleButton& t, const char* id)
        {
            addAndMakeVisible (t);
            buttonAtt.push_back (std::make_unique<APVTS::ButtonAttachment> (proc.apvts, id, t));
        };

        knob (intensity, "build");
        for (int k = 0; k < numFx; ++k) knob (fxKnob[k], fxIds[k]);
        knob (tailKnob, "tail");
        knob (rPitch, "roll_pitch"); knob (rGate, "roll_gate");
        knob (rFade, "roll_fade");   knob (rLevel, "roll_level");
        intensity.setTextValueSuffix ("");

        combo (mode, "mode", StringArray (charLong, 6));
        combo (rate, "rate", { "1/4", "1/8", "1/16", "1/32", "1/64", "1/8T", "1/16T", "1/8D" });
        combo (bars, "bars", { "1 bar", "2 bars", "4 bars", "8 bars", "16 bars", "32 bars" });
        combo (curve, "curve", { "Linear", "Exponential", "Logarithmic", "S curve" });
        combo (rollSrc, "rollsrc", { "Roll: live input", "Roll: sample" });
        combo (slot, "slot", { "Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7", "Slot 8" });

        toggle (autoBtn, "auto");
        toggle (linkBtn, "link");
        toggle (latchBtn, "latch");

        addAndMakeVisible (browse);
        browse.onClick = [this] { chooseFile(); };
        addAndMakeVisible (rollPad);
        addAndMakeVisible (dropPad);

        const void* data[] = { BinaryData::Pim_png, BinaryData::Charlie_png, BinaryData::Allan_png,
                               BinaryData::Glep_png, BinaryData::MrBoss_png, BinaryData::MrFrog_png };
        const int sizes[] = { BinaryData::Pim_pngSize, BinaryData::Charlie_pngSize, BinaryData::Allan_pngSize,
                              BinaryData::Glep_pngSize, BinaryData::MrBoss_pngSize, BinaryData::MrFrog_pngSize };
        for (int i = 0; i < 6; ++i) art[i] = ImageCache::getFromMemory (data[i], sizes[i]);
        logo = ImageCache::getFromMemory (BinaryData::Logo_png, BinaryData::Logo_pngSize);

        startTimerHz (20);
    }

    ~SmilingEditor() override { setLookAndFeel (nullptr); }

    void resized() override
    {
        intensity.setBounds (196, 152, 180, 190);
        mode.setBounds (40, 346, 334, 28);

        slot.setBounds (450, 224, 92, 26);
        browse.setBounds (548, 224, 92, 26);
        rollSrc.setBounds (646, 224, 162, 26);
        rollPad.setBounds (450, 254, 104, 34);
        latchBtn.setBounds (560, 258, 76, 26);
        linkBtn.setBounds (636, 258, 62, 26);
        rate.setBounds (706, 258, 102, 26);

        Slider* rk[4] = { &rPitch, &rGate, &rFade, &rLevel };
        for (int i = 0; i < 4; ++i) rk[i]->setBounds (450 + i * 89, 296, 74, 78);

        for (int k = 0; k < numFx; ++k) fxKnob[k].setBounds (30 + k * 80, 414, 72, 84);
        tailKnob.setBounds (30 + 9 * 80, 414, 72, 84);

        autoBtn.setBounds (36, 508, 120, 28);
        bars.setBounds (162, 508, 110, 28);
        curve.setBounds (280, 508, 150, 28);
        dropPad.setBounds (454, 504, 150, 34);
    }

    void paint (Graphics& g) override
    {
        const int m = jlimit (0, 5, (int) proc.apvts.getRawParameterValue ("mode")->load());
        const int slotIdx = jlimit (0, kSlots - 1, (int) proc.apvts.getRawParameterValue ("slot")->load());

        g.fillAll (Colour (0xfff6f2e8));

        // header
        g.drawImageWithin (logo, 24, 8, 150, 44, RectanglePlacement::xLeft | RectanglePlacement::yMid);
        g.setColour (Colour (0xff282828));
        g.setFont (makeFont (24.0f, true));
        g.drawText ("BUILDUP MACHINE", 184, 12, 330, 36, Justification::centredLeft);
        auto badge = Rectangle<float> (676, 12, 150, 36);
        g.setColour (Colour (0xffffe45e));
        g.fillRoundedRectangle (badge, 18.0f);
        g.setColour (Colour (0xff282828));
        g.drawRoundedRectangle (badge, 18.0f, 2.0f);
        g.setFont (makeFont (20.0f, true));
        g.drawText ("DJ MJ", badge.toNearestInt().withTrimmedRight (34), Justification::centred);
        g.setFont (makeFont (12.0f));
        g.drawText ("V3", badge.toNearestInt().withTrimmedLeft (110), Justification::centred);

        // character tiles
        for (int i = 0; i < 6; ++i)
        {
            Rectangle<float> r (26.0f + i * 133.0f, 60.0f, 125.0f, 69.0f);
            g.setColour (i == m ? accents[i] : Colour (0xffeae5da));
            g.fillRoundedRectangle (r, 7.0f);
            g.drawImageWithin (art[i], (int) r.getX() + 3, 64, 49, 60, RectanglePlacement::centred);
            g.setColour (Colours::black);
            g.setFont (makeFont (12.0f, true));
            g.drawText (charNames[i], (int) r.getX() + 50, 79, 73, 32, Justification::centredLeft);
        }

        auto panel = [&g] (Rectangle<float> r)
        {
            g.setColour (Colour (0xfffffdf8));
            g.fillRoundedRectangle (r, 9.0f);
            g.setColour (Colour (0xffd8d2c7));
            g.drawRoundedRectangle (r, 9.0f, 1.0f);
        };
        panel ({ 24, 134, 390, 250 });
        panel ({ 432, 134, 394, 250 });
        panel ({ 24, 392, 802, 150 });

        g.drawImageWithin (art[m], 36, 148, 150, 190, RectanglePlacement::centred);

        g.setColour (Colours::black);
        g.setFont (makeFont (12.0f, true));
        g.drawText ("INTENSITY  " + String (roundToInt (proc.meter.load() * 100.0f)) + "%",
                    196, 136, 180, 18, Justification::centred);
        g.drawText ("ROLL MACHINE", 450, 140, 220, 20, Justification::centredLeft);

        // waveform
        g.setColour (Colour (0xff29292d));
        g.fillRoundedRectangle (450.0f, 162.0f, 358.0f, 56.0f, 5.0f);
        g.setColour (accents[m]);
        const auto peaks = proc.getPeaks (slotIdx);
        if (! peaks.empty())
        {
            for (size_t i = 0; i < peaks.size(); ++i)
                g.drawVerticalLine (456 + (int) i, 190.0f - peaks[i] * 24.0f, 190.0f + peaks[i] * 24.0f);
            g.setFont (makeFont (10.0f));
            g.drawText (proc.getSlotName (slotIdx), 456, 163, 346, 14, Justification::centredLeft);
        }
        else
        {
            g.setFont (makeFont (13.0f));
            g.drawText ("Drop a sample here (wav, aiff, mp3, flac)", 450, 162, 358, 56, Justification::centred);
        }

        g.setColour (Colours::black);
        g.setFont (makeFont (12.0f, true));
        const char* rl[4] = { "Pitch", "Gate", "Fade", "Level" };
        for (int i = 0; i < 4; ++i)
            g.drawText (rl[i], 450 + i * 89, 280, 74, 16, Justification::centred);
        for (int k = 0; k < numFx; ++k)
            g.drawText (fxNames[k], 30 + k * 80, 396, 72, 16, Justification::centred);
        g.drawText ("Tail", 30 + 9 * 80, 396, 72, 16, Justification::centred);
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (e.y >= 60 && e.y <= 129)
        {
            const int i = (e.x - 26) / 133;
            if (i >= 0 && i < 6) mode.setSelectedId (i + 1);
        }
    }

    bool isInterestedInFileDrag (const StringArray& f) override { return ! f.isEmpty(); }

    void filesDropped (const StringArray& files, int, int) override
    {
        int idx = jlimit (0, kSlots - 1, (int) proc.apvts.getRawParameterValue ("slot")->load());
        bool any = false;
        for (const auto& path : files)
        {
            if (idx >= kSlots) break;
            if (proc.loadFile (idx, File (path))) { any = true; ++idx; }
            else warn();
        }
        if (any) useSampleSource();
        repaint();
    }

private:
    void chooseFile()
    {
        chooser = std::make_unique<FileChooser> ("Choose a sample (up to 30 seconds)", File{},
                                                 "*.wav;*.aif;*.aiff;*.mp3;*.flac;*.ogg");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [safe = Component::SafePointer<SmilingEditor> (this)] (const FileChooser& fc)
        {
            if (safe == nullptr) return;
            const auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            const int idx = jlimit (0, kSlots - 1, (int) safe->proc.apvts.getRawParameterValue ("slot")->load());
            if (safe->proc.loadFile (idx, f)) safe->useSampleSource();
            else safe->warn();
            safe->repaint();
        });
    }

    void useSampleSource()
    {
        if (auto* p = proc.apvts.getParameter ("rollsrc")) p->setValueNotifyingHost (1.0f);
    }

    void warn()
    {
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Sample error",
                                          "Could not load that file. Use a wav, aiff, mp3 or flac under 30 seconds.");
    }

    void timerCallback() override
    {
        const int m = jlimit (0, 5, (int) proc.apvts.getRawParameterValue ("mode")->load());
        intensity.setColour (Slider::rotarySliderFillColourId, accents[m]);
        rollPad.setAccent (accents[m]);
        dropPad.setAccent (Colour (0xffff8888));
        repaint();
    }

    SmilingBuildup& proc;
    SmileLnF lnf;

    Slider intensity, fxKnob[numFx], tailKnob, rPitch, rGate, rFade, rLevel;
    ComboBox mode, rate, bars, curve, rollSrc, slot;
    ToggleButton autoBtn { "Auto build" }, linkBtn { "Link" }, latchBtn { "Latch" };
    TextButton browse { "Load sample" };
    HoldPad rollPad, dropPad;
    std::unique_ptr<FileChooser> chooser;

    std::vector<std::unique_ptr<APVTS::SliderAttachment>> sliderAtt;
    std::vector<std::unique_ptr<APVTS::ComboBoxAttachment>> comboAtt;
    std::vector<std::unique_ptr<APVTS::ButtonAttachment>> buttonAtt;

    Image art[6], logo;
    const Colour accents[6] = { Colour (0xffffb5eb), Colour (0xffffef65), Colour (0xffff8888),
                                Colour (0xffa9ed9a), Colour (0xffe9be98), Colour (0xff23c75b) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SmilingEditor)
};

AudioProcessorEditor* SmilingBuildup::createEditor() { return new SmilingEditor (*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SmilingBuildup(); }
