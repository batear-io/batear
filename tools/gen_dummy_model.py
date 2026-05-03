#!/usr/bin/env python3
"""
gen_dummy_model.py — generate a tiny placeholder TFLite model for the
end-to-end skeleton (feat/tinyml-skeleton).

Schema (must match audio_features.c output and ml_classifier.cpp expectations):
  Input  : int8  [1, 32, 40, 1]   (32 mel frames x 40 mel bands, single channel)
  Output : int8  [1, 2]           (logits: [non_drone, drone])

The model itself is intentionally trivial. It exists only to validate the
Heltec V3 RAM budget, the espressif/esp-tflite-micro integration, the EMBED_FILES
plumbing, and the audio_task → audio_features → ml_classifier wiring.

Stage 2 (separate branch) replaces this with a real model trained on
batear-io/batear-datasets. The C side does not change as long as the input/output
shapes and dtypes stay the same.

Usage:
    python3 tools/gen_dummy_model.py [output_path]

Defaults to: models/drone_v0_dummy.tflite
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def build_and_export(out_path: Path) -> None:
    import numpy as np
    import tensorflow as tf

    print(f"[gen_dummy_model] tensorflow {tf.__version__}")

    inputs = tf.keras.Input(shape=(32, 40, 1), name="mel")
    x = tf.keras.layers.Conv2D(8, 3, padding="same", activation="relu")(inputs)
    x = tf.keras.layers.MaxPool2D(2)(x)
    x = tf.keras.layers.DepthwiseConv2D(3, padding="same", activation="relu")(x)
    x = tf.keras.layers.Conv2D(16, 1, padding="same", activation="relu")(x)
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    logits = tf.keras.layers.Dense(2, name="logits")(x)
    model = tf.keras.Model(inputs, logits, name="drone_v0_dummy")
    model.summary()

    def representative_dataset():
        rng = np.random.default_rng(0)
        for _ in range(64):
            sample = rng.standard_normal((1, 32, 40, 1)).astype("float32") * 4.0
            yield [sample]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8

    tflite_bytes = converter.convert()

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(tflite_bytes)
    print(f"[gen_dummy_model] wrote {out_path} ({len(tflite_bytes)} bytes)")

    interp = tf.lite.Interpreter(model_content=tflite_bytes)
    interp.allocate_tensors()
    in_det = interp.get_input_details()[0]
    out_det = interp.get_output_details()[0]
    print("[gen_dummy_model] input :", in_det["shape"], in_det["dtype"].__name__,
          "scale=", in_det["quantization"][0], "zp=", in_det["quantization"][1])
    print("[gen_dummy_model] output:", out_det["shape"], out_det["dtype"].__name__,
          "scale=", out_det["quantization"][0], "zp=", out_det["quantization"][1])


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("models/drone_v0_dummy.tflite")
    try:
        build_and_export(out_path)
    except ModuleNotFoundError as e:
        sys.stderr.write(
            f"[gen_dummy_model] missing dependency: {e}\n"
            "  pip install 'tensorflow>=2.15'\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
