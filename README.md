# Stem separation model

The plugin runs an **ONNX** model locally via ONNX Runtime. Model weights are
large (tens to hundreds of MB), so they are NOT stored in this repo
(`*.onnx` is git-ignored).

## Where to put it
Either click **Load model...** in the plugin, or copy the file to:

- Windows: `%APPDATA%\StemMaster\model.onnx`
- macOS:   `~/Library/Application Support/StemMaster/model.onnx`
- Linux:   `~/.config/StemMaster/model.onnx`

## What model the code expects (the "contract")
- Input : `float32 [1, 2, N]`    stereo waveform at 44.1 kHz (N fixed by the model, e.g. 343980)
- Output: `float32 [1, S, 2, N]` S stereo stems, in the order Drums, Bass, Other, Vocals

This is a *waveform in / waveform out* model. IMPORTANT: the newest Demucs
models (htdemucs) do an STFT inside the network, which cannot be exported to
ONNX directly, and community ONNX exports of them take spectrogram inputs
as well. Those need an STFT/iSTFT step added in `StemSeparator.cpp::process()`.
If your model differs, that one function is the only place to change.

## Licensing
Check the licence of whichever model you choose (e.g. Demucs code is MIT, but
trained weights can carry their own terms) before distributing the plugin.
