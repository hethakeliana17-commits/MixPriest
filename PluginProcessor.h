#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "StemSeparator.h"

class StemMasterProcessor : public juce::AudioProcessor
{
public:
    StemMasterProcessor();
    ~StemMasterProcessor() override;

    // --- AudioProcessor ---
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                   { return true; }
    const juce::String getName() const override       { return "StemMaster"; }
    bool acceptsMidi() const override                 { return false; }
    bool producesMidi() const override                { return false; }
    double getTailLengthSeconds() const override      { return 0.0; }
    int getNumPrograms() override                     { return 1; }
    int getCurrentProgram() override                  { return 0; }
    void setCurrentProgram (int) override             {}
    const juce::String getProgramName (int) override  { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // --- Stem workflow (call from message thread) ---
    juce::Result loadModelFile (const juce::File& onnx);
    void splitFile (const juce::File& audio);
    juce::Result exportStems (const juce::File& folder, const juce::String& baseName);

    bool hasStems();
    bool isSeparating() const            { return separator.isBusy(); }
    float getSeparationProgress() const  { return separator.getProgress(); }
    juce::String getStatusText() const   { return separator.getStatus(); }

    bool isPlaying() const               { return playing.load(); }
    void setPlaying (bool p)             { playing = p; }
    void rewind()                        { rewindRequested = true; }

    static juce::File getDefaultModelFile();

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void updateChain();
    void renderStems (juce::AudioBuffer<float>&, const StemSeparator::Stems&);

    StemSeparator separator;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    // Stems are swapped under this lock; the audio thread only ever try-locks.
    juce::SpinLock stemLock;
    std::shared_ptr<StemSeparator::Stems> stems;

    std::atomic<bool> playing { false }, rewindRequested { false };
    double playPos = 0.0;                 // audio thread only (in model-rate samples)
    double currentSampleRate = 44100.0;
    float lastGain[StemSeparator::numStems] {};

    std::atomic<float>* gainP[StemSeparator::numStems] {};
    std::atomic<float>* muteP[StemSeparator::numStems] {};
    std::atomic<float>* soloP[StemSeparator::numStems] {};
    std::atomic<float> *eqLowP, *eqMidP, *eqHighP, *compThreshP, *compRatioP, *limitP, *outGainP;

    using Filter = juce::dsp::IIR::Filter<float>;
    using Coeffs = juce::dsp::IIR::Coefficients<float>;
    using StereoFilter = juce::dsp::ProcessorDuplicator<Filter, Coeffs>;
    juce::dsp::ProcessorChain<StereoFilter, StereoFilter, StereoFilter,
                              juce::dsp::Compressor<float>,
                              juce::dsp::Limiter<float>,
                              juce::dsp::Gain<float>> chain;
    float lastLow = 0, lastMid = 0, lastHigh = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemMasterProcessor)
};
