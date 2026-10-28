#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <limits>

using namespace juce;

StemMasterProcessor::StemMasterProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (int i = 0; i < StemSeparator::numStems; ++i)
    {
        gainP[i] = apvts.getRawParameterValue ("gain" + String (i));
        muteP[i] = apvts.getRawParameterValue ("mute" + String (i));
        soloP[i] = apvts.getRawParameterValue ("solo" + String (i));
    }
    eqLowP      = apvts.getRawParameterValue ("eqLow");
    eqMidP      = apvts.getRawParameterValue ("eqMid");
    eqHighP     = apvts.getRawParameterValue ("eqHigh");
    compThreshP = apvts.getRawParameterValue ("compThresh");
    compRatioP  = apvts.getRawParameterValue ("compRatio");
    limitP      = apvts.getRawParameterValue ("limit");
    outGainP    = apvts.getRawParameterValue ("outGain");
}

StemMasterProcessor::~StemMasterProcessor()
{
    alive.reset();            // invalidates pending callbacks
    separator.cancel();
}

AudioProcessorValueTreeState::ParameterLayout StemMasterProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout l;

    for (int i = 0; i < StemSeparator::numStems; ++i)
    {
        const auto n = StemSeparator::stemNames[i];
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain" + String (i), 1 }, n + " level",
                                                      NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f));
        l.add (std::make_unique<AudioParameterBool> (ParameterID { "mute" + String (i), 1 }, n + " mute", false));
        l.add (std::make_unique<AudioParameterBool> (ParameterID { "solo" + String (i), 1 }, n + " solo", false));
    }

    auto f = [&l] (const char* id, const char* name, float lo, float hi, float def)
    {
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name,
                                                      NormalisableRange<float> (lo, hi, 0.1f), def));
    };
    f ("eqLow",      "EQ Low (120 Hz)",   -12.0f, 12.0f,  0.0f);
    f ("eqMid",      "EQ Mid (1 kHz)",    -12.0f, 12.0f,  0.0f);
    f ("eqHigh",     "EQ High (8 kHz)",   -12.0f, 12.0f,  0.0f);
    f ("compThresh", "Comp Threshold",    -40.0f,  0.0f,  0.0f);
    f ("compRatio",  "Comp Ratio",          1.0f, 10.0f,  2.0f);
    f ("limit",      "Limiter Ceiling",   -12.0f,  0.0f, -0.3f);
    f ("outGain",    "Output Gain",       -12.0f, 12.0f,  0.0f);
    return l;
}

bool StemMasterProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet()  == AudioChannelSet::stereo()
        && l.getMainOutputChannelSet() == AudioChannelSet::stereo();
}

void StemMasterProcessor::prepareToPlay (double sr, int blockSize)
{
    currentSampleRate = sr;
    dsp::ProcessSpec spec { sr, (uint32) blockSize, 2 };
    chain.prepare (spec);
    chain.reset();

    const float nan = std::numeric_limits<float>::quiet_NaN();
    lastLow = lastMid = lastHigh = nan;     // force coefficient refresh
    updateChain();

    for (auto& g : lastGain) g = 0.0f;
}

void StemMasterProcessor::updateChain()
{
    const float lo = eqLowP->load(), mi = eqMidP->load(), hi = eqHighP->load();

    // NOTE: makeXxx() allocates; only done when a knob actually moved.
    if (lo != lastLow)  { *chain.get<0>().state = *Coeffs::makeLowShelf  (currentSampleRate, 120.0f,  0.707f, Decibels::decibelsToGain (lo)); lastLow  = lo; }
    if (mi != lastMid)  { *chain.get<1>().state = *Coeffs::makePeakFilter (currentSampleRate, 1000.0f, 0.7f,   Decibels::decibelsToGain (mi)); lastMid  = mi; }
    if (hi != lastHigh) { *chain.get<2>().state = *Coeffs::makeHighShelf (currentSampleRate, 8000.0f, 0.707f, Decibels::decibelsToGain (hi)); lastHigh = hi; }

    auto& comp = chain.get<3>();
    comp.setThreshold (compThreshP->load());
    comp.setRatio (compRatioP->load());
    comp.setAttack (30.0f);
    comp.setRelease (200.0f);

    auto& lim = chain.get<4>();
    lim.setThreshold (limitP->load());
    lim.setRelease (100.0f);

    chain.get<5>().setGainDecibels (outGainP->load());
}

void StemMasterProcessor::renderStems (AudioBuffer<float>& buffer, const StemSeparator::Stems& st)
{
    const int n = buffer.getNumSamples();
    const int len = st.front().getNumSamples();
    const double ratio = StemSeparator::modelSampleRate / currentSampleRate;

    if (rewindRequested.exchange (false))
        playPos = 0.0;

    buffer.clear();

    const int valid = (int) std::min<double> (n, std::floor ((len - 2 - playPos) / ratio));
    if (valid <= 0)
    {
        playing = false;
        playPos = 0.0;
        return;
    }

    bool anySolo = false;
    for (int s = 0; s < StemSeparator::numStems; ++s)
        anySolo = anySolo || soloP[s]->load() > 0.5f;

    const int count = std::min ((int) st.size(), (int) StemSeparator::numStems);
    for (int s = 0; s < count; ++s)
    {
        const bool audible = muteP[s]->load() < 0.5f && (! anySolo || soloP[s]->load() > 0.5f);
        const float target = audible ? Decibels::decibelsToGain (gainP[s]->load(), -60.0f) : 0.0f;
        const float g0 = lastGain[s];
        lastGain[s] = target;
        if (g0 == 0.0f && target == 0.0f)
            continue;

        for (int c = 0; c < 2; ++c)
        {
            float* dst = buffer.getWritePointer (c);
            const float* src = st[(size_t) s].getReadPointer (c);
            for (int i = 0; i < valid; ++i)
            {
                const double pos = playPos + i * ratio;
                const int i0 = (int) pos;
                const float fr = (float) (pos - i0);
                const float smp = src[i0] + fr * (src[i0 + 1] - src[i0]);
                const float g = g0 + (target - g0) * ((float) i / (float) valid);
                dst[i] += smp * g;
            }
        }
    }

    playPos += valid * ratio;
    if (valid < n)
    {
        playing = false;
        playPos = 0.0;
    }
}

void StemMasterProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    // If stems are loaded and playing, they replace the incoming audio.
    if (playing.load())
    {
        const SpinLock::ScopedTryLockType lock (stemLock);
        if (lock.isLocked() && stems != nullptr && ! stems->empty())
            renderStems (buffer, *stems);
    }

    updateChain();
    dsp::AudioBlock<float> block (buffer);
    dsp::ProcessContextReplacing<float> ctx (block);
    chain.process (ctx);
}

// ---------------------------------------------------------------------------
File StemMasterProcessor::getDefaultModelFile()
{
    auto dir = File::getSpecialLocation (File::userApplicationDataDirectory);
   #if JUCE_MAC
    dir = dir.getChildFile ("Application Support");
   #endif
    return dir.getChildFile ("StemMaster").getChildFile ("model.onnx");
}

Result StemMasterProcessor::loadModelFile (const File& f)
{
    auto r = separator.loadModel (f);
    if (r.wasOk())
        apvts.state.setProperty ("modelPath", f.getFullPathName(), nullptr);
    else
        separator.setStatus (r.getErrorMessage());
    return r;
}

void StemMasterProcessor::splitFile (const File& audio)
{
    if (! separator.isModelLoaded())
    {
        File candidate (apvts.state.getProperty ("modelPath").toString());
        if (! candidate.existsAsFile())
            candidate = getDefaultModelFile();

        if (! candidate.existsAsFile() || loadModelFile (candidate).failed())
        {
            separator.setStatus ("No model found. Click 'Load model...' (see models/README.md)");
            return;
        }
    }

    setPlaying (false);
    std::weak_ptr<bool> weak = alive;
    separator.split (audio, [this, weak] (Result r, std::shared_ptr<StemSeparator::Stems> s)
    {
        if (weak.expired() || r.failed())
            return;

        {
            const SpinLock::ScopedLockType lock (stemLock);
            std::swap (stems, s);      // old stems released after the lock is dropped
        }
        rewindRequested = true;
    });
}

bool StemMasterProcessor::hasStems()
{
    const SpinLock::ScopedLockType lock (stemLock);
    return stems != nullptr && ! stems->empty();
}

Result StemMasterProcessor::exportStems (const File& folder, const String& baseName)
{
    std::shared_ptr<StemSeparator::Stems> snap;
    {
        const SpinLock::ScopedLockType lock (stemLock);
        snap = stems;
    }
    if (snap == nullptr || snap->empty())
        return Result::fail ("No stems to export");

    folder.createDirectory();
    WavAudioFormat wav;

    for (size_t s = 0; s < snap->size(); ++s)
    {
        const String name = s < (size_t) StemSeparator::numStems ? StemSeparator::stemNames[(int) s]
                                                                 : "Stem" + String ((int) s + 1);
        auto file = folder.getChildFile (baseName + "_" + name + ".wav");
        file.deleteFile();

        std::unique_ptr<FileOutputStream> os (file.createOutputStream());
        if (os == nullptr)
            return Result::fail ("Cannot write " + file.getFullPathName());

        std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), StemSeparator::modelSampleRate,
                                                                   2, 24, StringPairArray(), 0));
        if (w == nullptr)
            return Result::fail ("Cannot create WAV writer");
        os.release();                           // writer owns the stream now

        const auto& b = (*snap)[s];
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }
    return Result::ok();
}

// ---------------------------------------------------------------------------
void StemMasterProcessor::getStateInformation (MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void StemMasterProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessorEditor* StemMasterProcessor::createEditor()
{
    return new StemMasterEditor (*this);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StemMasterProcessor();
}
