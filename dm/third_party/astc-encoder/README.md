# Arm ASTC Encoder

Library sources from [Arm ASTC Encoder 5.7.0](https://github.com/ARM-software/astc-encoder/tree/baff485b0ff36d2f95d28961605106502c653966), commit `baff485b0ff36d2f95d28961605106502c653966`, under the Apache 2.0 license in `LICENSE.txt`.

Only the unmodified library sources and headers from `Source/` are included. The command-line application and image loaders are excluded. The host Makefile builds the static library with C++14, invariant floating-point arithmetic, and a maximum block size of 36 texels; the renderer uses ASTC 4x4 and 6x6 LDR only. `dm/ios/tools/gen_build.py` imports those same compilation inputs for Apple mobile targets.

`dm/gfx/tex/nvn_astc.mm` provides the C interface. The optional BC transcoding path keeps compressed results in a content-addressed disk cache; GPU-written BC4/BC5 textures retain their separate lossless decode path.
