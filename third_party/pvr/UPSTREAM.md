The unmodified `PVRTDecompress.cpp` and `PVRTDecompress.h` are from
[PowerVR Native SDK](https://github.com/powervr-graphics/Native_SDK/tree/63f0098acdc11ec9a072c30000d2fa670a955bee/framework/PVRCore/texture)
at commit `63f0098acdc11ec9a072c30000d2fa670a955bee`.

Copyright (c) Imagination Technologies Ltd. Distributed under the MIT license;
the upstream license is retained in `LICENSE.md`.

Badanaut uses the existing PVRTC decoder for Nova's compressed textures,
following touchHLE's decode-to-RGBA upload path. The upstream ETC decoder is
retained unchanged in the same source file.
