#!/bin/sh
# Download the local ASR model used by the child config (sherpa-onnx paraformer-zh-small, ~74 MB).
set -eu
cd "$(dirname "$0")"
URL=https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-paraformer-zh-small-2024-03-09.tar.bz2
SHA256=da92b3db5218c5be53aad53e57d1b6e63e7fc98a0e054fbdd6dbe18e9c6b1450
mkdir -p models
curl -fL -o models/asr.tar.bz2 "$URL"
echo "$SHA256  models/asr.tar.bz2" | sha256sum -c -
tar xjf models/asr.tar.bz2 -C models
rm models/asr.tar.bz2
chmod -R a+rX models
echo MODEL_READY
