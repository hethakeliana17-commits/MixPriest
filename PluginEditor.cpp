#include "PluginEditor.h"

using namespace juce;

StemMasterEditor::StemMasterEditor (StemMasterProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (760, 520);

    for (auto* b : { &loadAudio, &loadModel, &rewindBtn, &playBtn, &exportBtn })
        addAndMakeVisible (*b);
    addAndMakeVisible (status);
    addChildComponent (progressBar);
    status.setJustificationType (Justification::centredLeft);

    // ---- stem strips ----
    for (int i = 0; i < StemSeparator::numStems; ++i)
    {
        auto& s = strips[(size_t) i];
        s.name.setText (StemSeparator::stemNames[i], dontSendNotification);
        s.name.setJustificationType (Justification::centred);
        s.gain.setSliderStyle (Slider::LinearVertical);
        s.gain.setTextBoxStyle (Slider::TextBoxBelow, false, 60, 20);
        s.gain.setTextValueSuffix (" dB");
        s.mute.setColour (ToggleButton::tickColourId, Colours::orangered);
        s.solo.setColour (ToggleButton::tickColourId, Colours::gold);

        s.gainAtt = std::make_unique<SliderAtt> (proc.apvts, "gain" + String (i), s.gain);
        s.muteAtt = std::make_unique<ButtonAtt> (proc.apvts, "mute" + String (i), s.mute);
        s.soloAtt = std::make_unique<ButtonAtt> (proc.apvts, "solo" + String (i), s.solo);

        for (Component* c : std::initializer_list<Component*> { &s.name, &s.gain, &s.mute, &s.solo })
            addAndMakeVisible (*c);
    }

    // ---- master knobs ----
    const char* ids[]    = { "eqLow", "eqMid", "eqHigh", "compThresh", "compRatio", "limit", "outGain" };
    const char* labels[] = { "Low", "Mid", "High", "Comp thr", "Comp ratio", "Limiter", "Output" };
    const char* suffix[] = { " dB", " dB", " dB", " dB", ":1", " dB", " dB" };
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& k = knobs[i];
        k.slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        k.slider.setTextBoxStyle (Slider::TextBoxBelow, false, 70, 18);
        k.slider.setTextValueSuffix (suffix[i]);
        k.label.setText (labels[i], dontSendNotification);
        k.label.setJustificationType (Justification::centred);
        k.att = std::make_unique<SliderAtt> (proc.apvts, ids[i], k.slider);
        addAndMakeVisible (k.slider);
        addAndMakeVisible (k.label);
    }

    // ---- buttons ----
    loadModel.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Select ONNX stem model", File(), "*.onnx");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (f.existsAsFile())
                                      proc.loadModelFile (f);
                              });
    };

    loadAudio.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Select audio to split", File(),
                                                 "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (f.existsAsFile())
                                      proc.splitFile (f);
                              });
    };

    playBtn.onClick   = [this] { proc.setPlaying (! proc.isPlaying()); };
    rewindBtn.onClick = [this] { proc.rewind(); };

    exportBtn.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Choose a folder for the stems", File());
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                              [this] (const FileChooser& fc)
                              {
                                  auto dir = fc.getResult();
                                  if (! dir.isDirectory()) return;
                                  auto r = proc.exportStems (dir, "stem");
                                  status.setText (r.wasOk() ? "Exported to " + dir.getFullPathName()
                                                            : r.getErrorMessage(), dontSendNotification);
                              });
    };

    startTimerHz (15);
}

StemMasterEditor::~StemMasterEditor()
{
    stopTimer();
}

void StemMasterEditor::timerCallback()
{
    const bool busy = proc.isSeparating();
    const bool ready = proc.hasStems();

    progressValue = proc.getSeparationProgress();
    progressBar.setVisible (busy);
    if (! busy && ! status.getText().startsWith ("Exported"))
        status.setText (proc.getStatusText(), dontSendNotification);
    else if (busy)
        status.setText (proc.getStatusText(), dontSendNotification);

    loadAudio.setEnabled (! busy);
    loadModel.setEnabled (! busy);
    playBtn.setEnabled (ready);
    rewindBtn.setEnabled (ready);
    exportBtn.setEnabled (ready && ! busy);
    playBtn.setButtonText (proc.isPlaying() ? "Pause" : "Play");
}

void StemMasterEditor::paint (Graphics& g)
{
    g.fillAll (Colour (0xff1b1d22));
    g.setColour (Colours::white.withAlpha (0.85f));
    g.setFont (FontOptions (13.0f, Font::bold));
    g.drawText ("STEMS", 16, 62, 120, 18, Justification::left);
    g.drawText ("MIX / MASTER", 470, 62, 200, 18, Justification::left);
    g.setColour (Colours::white.withAlpha (0.12f));
    g.drawLine (450.0f, 70.0f, 450.0f, 440.0f);
}

void StemMasterEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto top = area.removeFromTop (34);
    loadModel.setBounds (top.removeFromLeft (110));
    top.removeFromLeft (6);
    loadAudio.setBounds (top.removeFromLeft (170));
    top.removeFromLeft (16);
    rewindBtn.setBounds (top.removeFromLeft (40));
    top.removeFromLeft (4);
    playBtn.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (16);
    exportBtn.setBounds (top.removeFromLeft (130));

    auto bottom = area.removeFromBottom (44);
    status.setBounds (bottom.removeFromTop (22));
    progressBar.setBounds (bottom.reduced (0, 2));

    area.removeFromTop (28);

    auto stemArea = area.removeFromLeft (430);
    const int w = stemArea.getWidth() / StemSeparator::numStems;
    for (auto& s : strips)
    {
        auto col = stemArea.removeFromLeft (w).reduced (6, 0);
        s.name.setBounds (col.removeFromTop (22));
        auto btns = col.removeFromBottom (26);
        s.mute.setBounds (btns.removeFromLeft (btns.getWidth() / 2));
        s.solo.setBounds (btns);
        s.gain.setBounds (col);
    }

    area.removeFromLeft (30);
    const int kw = area.getWidth() / 3;
    const int kh = 140;
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        const int row = (int) i / 3, col = (int) i % 3;
        Rectangle<int> cell (area.getX() + col * kw, area.getY() + row * (kh + 10), kw, kh);
        knobs[i].label.setBounds (cell.removeFromTop (18));
        knobs[i].slider.setBounds (cell);
    }
}
