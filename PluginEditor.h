#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class StemMasterEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit StemMasterEditor (StemMasterProcessor&);
    ~StemMasterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void timerCallback() override;

    StemMasterProcessor& proc;

    juce::TextButton loadAudio { "Load audio & split..." }, loadModel { "Load model..." },
                     rewindBtn { "|<" }, playBtn { "Play" }, exportBtn { "Export stems..." };
    juce::Label status;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };

    struct Strip
    {
        juce::Label name;
        juce::Slider gain;
        juce::ToggleButton mute { "M" }, solo { "S" };
        std::unique_ptr<SliderAtt> gainAtt;
        std::unique_ptr<ButtonAtt> muteAtt, soloAtt;
    };
    std::array<Strip, StemSeparator::numStems> strips;

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAtt> att;
    };
    std::array<Knob, 7> knobs;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemMasterEditor)
};
