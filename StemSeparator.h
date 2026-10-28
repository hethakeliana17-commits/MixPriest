#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <onnxruntime_cxx_api.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

/**
 * Offline AI stem separation using an ONNX model.
 *
 * MODEL CONTRACT (see models/README.md):
 *   input  : float32 [1, 2, N]       stereo waveform @ 44.1 kHz
 *   output : float32 [1, S, 2, N]    S stereo stems (default order: drums, bass, other, vocals)
 *
 * Runs on a background thread. Never call from the audio thread.
 */
class StemSeparator : private juce::Thread
{
public:
    static constexpr double modelSampleRate = 44100.0;
    static constexpr int    numStems        = 4;
    static inline const juce::StringArray stemNames { "Drums", "Bass", "Other", "Vocals" };

    using Stems  = std::vector<juce::AudioBuffer<float>>;
    using DoneFn = std::function<void (juce::Result, std::shared_ptr<Stems>)>;

    StemSeparator();
    ~StemSeparator() override;

    juce::Result loadModel (const juce::File& onnxFile);
    bool isModelLoaded() const            { return session != nullptr; }

    /** Starts separation. onDone is called on the message thread. */
    void split (const juce::File& audioFile, DoneFn onDone);
    void cancel();

    bool  isBusy() const                  { return isThreadRunning(); }
    float getProgress() const             { return progress.load(); }
    juce::String getStatus() const;
    void setStatus (const juce::String& s);

private:
    void run() override;
    juce::Result process (Stems& out);

    Ort::Env env;
    std::unique_ptr<Ort::Session> session;
    std::string inputName, outputName;
    int64_t segmentLength = 343980;

    juce::File audioFile;
    DoneFn onDone;
    std::atomic<float> progress { 0.0f };
    mutable juce::CriticalSection statusLock;
    juce::String status { "Load a model to begin" };
};
