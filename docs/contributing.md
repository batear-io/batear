# Contributing

## Current Status

**Batear is functioning as a flashable baseline and has been successfully tested against prerecorded drone audio.** We need real-world testing! Acoustic drone detection in real environments depends heavily on distance, wind, background noise, and drone type.

## Areas We Need Help

If you have a micro drone, a soldering iron, and some free time, we would love your help:

- Real-world threshold calibration per drone type
- Noise filtering improvements
- ML-based detection (ESP-NN / TFLite Micro)
- Multi-gateway mesh networking
- Battery/solar power optimizations

## Getting Started

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

See the [Build & Flash](build-flash.md) guide to set up your development environment.

## 📊 Datasets & Benchmarking

To ensure the reliability of our DSP pipeline, we maintain a dedicated repository for high-fidelity acoustic samples:

👉 **[batear-io/batear-datasets](https://github.com/batear-io/batear-datasets)**

This repository contains:
* **Real-world Flight Data:** Recordings from various drone models (e.g., DJI Mavic series).
* **Interference Profiles:** Bio-acoustic signals (bats, birds) and environmental noise for stress-testing SNR thresholds.
* **DSP Tools:** Python scripts for resampling and spectrogram visualization.

**Contribute your data:** If you have successfully captured drone or bat activity using Batear, please consider contributing your `.wav` files to our datasets repo via Git LFS.
