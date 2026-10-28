#include "StemSeparator.h"
#include <cmath>
#include <algorithm>

StemSeparator::StemSeparator()
    : juce::Thread ("StemSeparator"),
      env (ORT_LOGGING_LEVEL_WARNING, "StemMaster")
{
}

StemSeparator::~StemSeparator()
{
    stopThread (30000);
}

juce::String StemSeparator::getStatus() const
{
    const juce::ScopedLock sl (statusLock);
    return status;
}

void StemSeparator::setStatus (const juce::String& s)
{
    const juce::ScopedLock sl (statusLock);
    status = s;
}

juce::Result StemSeparator::loadModel (const juce::File& f)
{
    if (isThreadRunning())  return juce::Result::fail ("Busy");
    if (! f.existsAsFile()) return juce::Result::fail ("Model file not found: " + f.getFullPathName());

    try
    {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads (juce::jmax (1, juce::SystemStats::getNumPhysicalCpus()));
        opts.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);

       #if JUCE_WINDOWS
        auto s = std::make_unique<Ort::Session> (env, f.getFullPathName().toWideCharPointer(), opts);
       #else
        auto s = std::make_unique<Ort::Session> (env, f.getFullPathName().toRawUTF8(), opts);
       #endif

        auto inShape = s->GetInputTypeInfo (0).GetTensorTypeAndShapeInfo().GetShape();
        if (inShape.size() != 3)
            return juce::Result::fail ("Model input must be [1, 2, samples]");

        segmentLength = inShape[2] > 0 ? inShape[2] : 343980;

        Ort::AllocatorWithDefaultOptions alloc;
        inputName  = s->GetInputNameAllocated (0, alloc).get();
        outputName = s->GetOutputNameAllocated (0, alloc).get();

        session = std::move (s);
        setStatus ("Model loaded: " + f.getFileName());
        return juce::Result::ok();
    }
    catch (const Ort::Exception& e)
    {
        return juce::Result::fail (juce::String ("Could not load model: ") + e.what());
    }
}

void StemSeparator::split (const juce::File& file, DoneFn done)
{
    if (isThreadRunning() || session == nullptr)
        return;

    audioFile = file;
    onDone    = std::move (done);
    progress  = 0.0f;
    setStatus ("Reading " + file.getFileName() + "...");
    startThread();
}

void StemSeparator::cancel()
{
    signalThreadShouldExit();
}

void StemSeparator::run()
{
    auto stems  = std::make_shared<Stems>();
    auto result = juce::Result::ok();

    try                              { result = process (*stems); }
    catch (const std::exception& e)  { result = juce::Result::fail (e.what()); }

    if (threadShouldExit())
        result = juce::Result::fail ("Cancelled");

    setStatus (result.wasOk() ? "Done - stems ready" : result.getErrorMessage());

    auto cb = onDone;
    juce::MessageManager::callAsync ([cb, result, stems]
    {
        if (cb) cb (result, stems);
    });
}

juce::Result StemSeparator::process (Stems& out)
{
    // ---- 1. Read audio -------------------------------------------------
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (audioFile));
    if (reader == nullptr)
        return juce::Result::fail ("Could not read that audio file");

    const int srcLen = (int) reader->lengthInSamples;
    if (srcLen < 4096)
        return juce::Result::fail ("Audio file is too short");

    juce::AudioBuffer<float> src (2, srcLen + 16);
    src.clear();
    reader->read (&src, 0, srcLen, 0, true, true);
    if (reader->numChannels < 2)
        src.copyFrom (1, 0, src, 0, 0, srcLen);

    // ---- 2. Resample to model rate --------------------------------------
    juce::AudioBuffer<float> mix;
    if (std::abs (reader->sampleRate - modelSampleRate) > 1.0)
    {
        const double ratio  = reader->sampleRate / modelSampleRate;
        const int    outLen = (int) std::floor ((double) srcLen / ratio) - 1;
        mix.setSize (2, outLen);
        for (int c = 0; c < 2; ++c)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, src.getReadPointer (c), mix.getWritePointer (c), outLen);
        }
    }
    else
    {
        mix.setSize (2, srcLen);
        for (int c = 0; c < 2; ++c)
            mix.copyFrom (c, 0, src, c, 0, srcLen);
    }
    src.setSize (0, 0);

    const int total = mix.getNumSamples();

    // ---- 3. Normalise (Demucs-style) -------------------------------------
    double sum = 0.0, sq = 0.0;
    {
        const float* l = mix.getReadPointer (0);
        const float* r = mix.getReadPointer (1);
        for (int i = 0; i < total; ++i)
        {
            const double m = 0.5 * (l[i] + r[i]);
            sum += m;
            sq  += m * m;
        }
    }
    const double mean = sum / total;
    double sd = std::sqrt (std::max (sq / total - mean * mean, 1.0e-12));
    if (sd < 1.0e-4) sd = 1.0;

    // ---- 4. Chunked inference with overlap-add ---------------------------
    const int seg = (int) segmentLength;
    const int hop = seg * 3 / 4;
    const int numChunks = std::max (1, (int) std::ceil ((double) (total - seg) / hop) + 1);

    std::vector<float> window ((size_t) seg);
    for (int i = 0; i < seg; ++i)
        window[(size_t) i] = std::max (1.0e-3f, (float) (1.0 - std::abs ((i + 0.5) / (seg * 0.5) - 1.0)));

    std::vector<float> weightSum ((size_t) total, 0.0f);
    std::vector<float> inData ((size_t) (2 * seg));
    Stems accum;
    int numOutStems = 0;

    auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
    const int64_t inShape[3] = { 1, 2, (int64_t) seg };
    const char* inNames[]  = { inputName.c_str() };
    const char* outNames[] = { outputName.c_str() };

    for (int k = 0; k < numChunks; ++k)
    {
        if (threadShouldExit())
            return juce::Result::fail ("Cancelled");

        const int start = k * hop;
        const int n     = std::min (seg, total - start);
        if (n <= 0) break;

        for (int c = 0; c < 2; ++c)
        {
            const float* s = mix.getReadPointer (c);
            float* d = inData.data() + (size_t) c * seg;
            for (int i = 0; i < seg; ++i)
                d[i] = i < n ? (float) ((s[start + i] - mean) / sd) : 0.0f;
        }

        auto inTensor = Ort::Value::CreateTensor<float> (mem, inData.data(), inData.size(), inShape, 3);
        auto outputs  = session->Run (Ort::RunOptions { nullptr }, inNames, &inTensor, 1, outNames, 1);

        auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 4 || shape[2] != 2 || shape[3] != seg)
            return juce::Result::fail ("Unexpected model output shape (expected [1, stems, 2, samples])");

        const float* o = outputs[0].GetTensorData<float>();

        if (accum.empty())
        {
            numOutStems = (int) shape[1];
            for (int s = 0; s < numOutStems; ++s)
            {
                accum.emplace_back (2, total);
                accum.back().clear();
            }
        }

        for (int s = 0; s < numOutStems; ++s)
            for (int c = 0; c < 2; ++c)
            {
                float* d = accum[(size_t) s].getWritePointer (c);
                const float* p = o + ((size_t) s * 2 + c) * seg;
                for (int i = 0; i < n; ++i)
                    d[start + i] += p[i] * window[(size_t) i];
            }

        for (int i = 0; i < n; ++i)
            weightSum[(size_t) (start + i)] += window[(size_t) i];

        progress = (float) (k + 1) / (float) numChunks;
        setStatus ("Separating... " + juce::String (juce::roundToInt (progress.load() * 100.0f)) + "%");
    }

    // ---- 5. Normalise overlap and undo input scaling ---------------------
    for (auto& b : accum)
        for (int c = 0; c < 2; ++c)
        {
            float* d = b.getWritePointer (c);
            for (int i = 0; i < total; ++i)
                d[i] = (float) ((d[i] / std::max (weightSum[(size_t) i], 1.0e-3f)) * sd + mean);
        }

    out = std::move (accum);
    return juce::Result::ok();
}
