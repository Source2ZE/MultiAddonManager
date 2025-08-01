#!/bin/bash
mkdir dockerbuild
cd dockerbuild
pwd
python ../configure.py --enable-optimize --sdks cs2 --hl2sdk-manifests ./hl2sdk-manifests
ambuild