# Third-party notices

ff7vr itself is under the MIT licence (see `LICENSE`). It is built with the
third-party components below. Each is fetched at the pinned commit by CMake
(`cmake/ff7vr_deps.cmake`, `src/render/CMakeLists.txt`, `src/xr/CMakeLists.txt`,
`src/engine/CMakeLists.txt`); none of them is stored in this repository. The `-dlss`
build also contains NVIDIA's NGX library and header code, which are not under MIT:
NVIDIA's RTX SDKs licence applies to them (see "NVIDIA DLSS SDK" below).

What ends up where:

| Component | In the mod (`xinput1_3.dll`) | Only in development tools |
|---|---|---|
| MinHook, with the Hacker Disassembler Engine | compiled in (static library) | |
| OpenXR-SDK: the OpenXR loader and headers | compiled in (static loader) | |
| JsonCpp (bundled with the OpenXR loader) | compiled in, as part of the loader | |
| stb: `stb_image_write.h` | compiled in | |
| stb: `stb_truetype.h` | compiled in | |
| stb: `stb_image.h` | | `tools/xr_smoke` |
| NVAPI SDK: headers and `nvapi64.lib` | linked in (static library) | |
| NVIDIA DLSS SDK: headers and the NGX static library `nvsdk_ngx_s.lib` | linked in, only in the `-dlss` build | |

The release package ships the mod DLL, so the notices below apply to it. The
licence texts follow the component list; each was copied from the component's
own source tree at the pinned commit.

## Components

### MinHook

- Upstream: https://github.com/TsudaKageyu/minhook
- Version: v1.3.4, commit `c3fcafdc10146beb5919319d0683e44e3c30d537`
- Used for: function hooks (`ff7vr_core`, linked into the mod DLL). Built from
  `src/buffer.c`, `src/hook.c`, `src/trampoline.c` and `src/hde/hde64.c`.
- Copyright: Copyright (C) 2009-2017 Tsuda Kageyu. All rights reserved.
- Bundled: the Hacker Disassembler Engine 64 (`src/hde`), Copyright (c)
  2008-2009, Vyacheslav Patkov. All rights reserved.
- Licence: BSD 2-Clause, for MinHook and for the Hacker Disassembler Engine
  (text: "MinHook licence" below, from MinHook's `LICENSE.txt`, which carries both
  notices).

### OpenXR-SDK (OpenXR loader and headers)

- Upstream: https://github.com/KhronosGroup/OpenXR-SDK
- Version: release-1.1.63, commit `f2448a8797c85814aa892efc1ab8707900fbcc78`
- Used for: the OpenXR session (`ff7vr_xr`, linked into the mod DLL; also used by
  `tools/xr_smoke`). The loader is built as a static library from `src/loader`,
  a few files of `src/common` and the files generated from the API registry; no
  API layers, no tests.
- Copyright lines in the compiled files:
  - Copyright (c) 2017-2026 The Khronos Group Inc.
  - Copyright (c) 2017-2019 Valve Corporation
  - Copyright (c) 2017-2019 LunarG, Inc.
  - Copyright (c) 2019 Collabora, Ltd.
  - Copyright (c) 2020-2021, Collabora, Ltd.
- Licence: the loader sources and the main headers are marked
  `Apache-2.0 OR MIT`; the generated dispatch-table and loader sources also carry
  an `Apache-2.0` notice. ff7vr uses the OpenXR-SDK under the Apache License,
  Version 2.0 (full text below, from the SDK's `LICENSE`). The SDK has no NOTICE
  file.

### JsonCpp (bundled with the OpenXR loader)

- Upstream: https://github.com/open-source-parsers/jsoncpp
- Version: 1.9.6, the unmodified copy in OpenXR-SDK's `src/external/jsoncpp`
  (at the OpenXR-SDK commit above)
- Used for: reading OpenXR runtime manifests inside the OpenXR loader
  (`json_reader.cpp`, `json_value.cpp`, `json_writer.cpp`; compiled into the mod
  DLL as part of the loader).
- Copyright: Copyright (c) 2007-2010 Baptiste Lepilleur and The JsonCpp Authors
- Licence: public domain where recognised, otherwise MIT (text: "JsonCpp
  licence" below, from `src/external/jsoncpp/LICENSE`).

### stb

- Upstream: https://github.com/nothings/stb
- Version: commit `2c980bb59875b0d32144a71867fbdebb2f77cd20`
- Used for:
  - `stb_image_write.h` (v1.16): writing PNG captures of the eye images
    (`ff7vr_xr`, compiled into the mod DLL).
  - `stb_truetype.h` (v1.26): drawing the text of the settings panel with a font
    installed with Windows (`ff7vr_render`, compiled into the mod DLL; no font is
    shipped).
  - `stb_image.h` (v2.30): reading PNGs back for verification, in the
    development tool `tools/xr_smoke` only; not part of the mod DLL.
- Copyright: Copyright (c) 2017 Sean Barrett
- Licence: MIT or public domain (Unlicense), at the user's choice (text: "stb
  licence" below, from stb's `LICENSE`).

### NVAPI SDK

- Upstream: https://github.com/NVIDIA/nvapi
- Version: R615 developer SDK, commit `70d337db9186e968eab622f7e786de7e437faf3d`
- Used for: variable rate shading for foveated rendering (`ff7vr_render`,
  linked into the mod DLL). The headers and the static library `amd64/nvapi64.lib`
  are used; the library loads the driver's `nvapi64.dll` at run time, which is part
  of the NVIDIA driver and is not shipped.
- Copyright:
  - Libraries: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights
    reserved.
  - Headers (`nvapi.h`): Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES.
    All rights reserved.
- Licence: MIT (text: "NVAPI licence" below, from the SDK's `License.txt`; the
  headers carry the same MIT text).

### NVIDIA DLSS SDK (only in the `-dlss` build)

- Upstream: https://github.com/NVIDIA/DLSS
- Version: DLSS SDK 310.9.1, commit `374959484e79a640feaba44c93ac8cfb0a03f5b5`
- Used for: DLSS super resolution per eye (`src/engine/src/dlss.cpp`, built only with
  `-DFF7VR_DLSS=ON`: the packages whose name ends in `-dlss`). The SDK's headers and
  the static library `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib` are compiled and linked
  into the mod DLL. The DLSS model itself, `nvngx_dlss.dll`, is not shipped: the player
  obtains it from NVIDIA (the same SDK repository, `lib/Windows_x86_64/rel`, or the
  NVIDIA App's DLSS override) under NVIDIA's terms; its own third-party notices are in
  the SDK's programming guide (section 9.6).
- Copyright: Copyright (c) 2018-2026 NVIDIA CORPORATION & AFFILIATES. All rights
  reserved (the headers carry years from 2018 to 2026).
- Licence: NVIDIA RTX SDKs LICENSE (v. March 14, 2024) with the NVIDIA RTX Supplement
  (text: "NVIDIA DLSS SDK licence" below, from the SDK's `LICENSE.txt`). These parts
  are not under ff7vr's MIT licence: in the `-dlss` build they remain NVIDIA's and are
  distributed as object code under that licence (its section 1.c); the MIT licence
  covers ff7vr's own code only. Notice from that licence (its section 2.b): "This
  software contains source code provided by NVIDIA Corporation."
- Attribution (the licence's supplement, 7.1(b)): the `-dlss` build uses the NVIDIA DLSS
  SDK. The mod has no splash screen, about box or credits, so this file and the README
  carry the attribution. NVIDIA's names are used here only for that attribution and to
  tell players which NVIDIA file to obtain; no NVIDIA logos or badges are used. NVIDIA RTX
  and NVIDIA GeForce RTX are trademarks of NVIDIA Corporation; DLSS is NVIDIA's product
  name. ff7vr is not sponsored, endorsed or reviewed by NVIDIA. The DLSS SDK is licensed
  for use on systems with NVIDIA GPUs only.

## Licence texts

### MinHook licence

```
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

================================================================================
Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.
================================================================================
Hacker Disassembler Engine 32 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
Hacker Disassembler Engine 64 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### JsonCpp licence

```
The JsonCpp library's source code, including accompanying documentation,
tests and demonstration applications, are licensed under the following
conditions...

Baptiste Lepilleur and The JsonCpp Authors explicitly disclaim copyright in all
jurisdictions which recognize such a disclaimer. In such jurisdictions,
this software is released into the Public Domain.

In jurisdictions which do not recognize Public Domain property (e.g. Germany as of
2010), this software is Copyright (c) 2007-2010 by Baptiste Lepilleur and
The JsonCpp Authors, and is released under the terms of the MIT License (see below).

In jurisdictions which recognize Public Domain property, the user of this
software may choose to accept it either as 1) Public Domain, 2) under the
conditions of the MIT License (see below), or 3) under the terms of dual
Public Domain/MIT License conditions described here, as they choose.

The MIT License is about as close to Public Domain as a license can get, and is
described in clear, concise terms at:

   http://en.wikipedia.org/wiki/MIT_License

The full text of the MIT License follows:

========================================================================
Copyright (c) 2007-2010 Baptiste Lepilleur and The JsonCpp Authors

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
========================================================================
(END LICENSE TEXT)

The MIT license is compatible with both the GPL and commercial
software, affording one all of the rights of Public Domain with the
minor nuisance of being required to keep the above copyright notice
and license text in the source code. Note also that by accepting the
Public Domain "license" you can re-license your copy using whatever
license you like.
```

### stb licence

```
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### NVAPI licence

```
nvapi.lib, nvapi64.lib, nvapia64.lib and nvapia64ec.lib are licensed under the following terms:

SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: MIT

Permission is hereby granted, free of charge, to any person obtaining a 
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

### Apache License, Version 2.0 (OpenXR-SDK)

```

                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright [yyyy] [name of copyright owner]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
```

### NVIDIA DLSS SDK licence

```
NVIDIA RTX SDKs LICENSE

This license is a legal agreement between you and NVIDIA Corporation ("NVIDIA") and governs the use of the NVIDIA RTX software development kits, including the DLSS SDK, NGX SDK, RTXGI SDK, RTXDI SDK, RTX Video SDK, RTX Dynamic Vibrance SDK and/or NRD SDK, if and when made available to you under this license (in each case, the “SDK”).
This license can be accepted only by an adult of legal age of majority in the country in which the SDK is used. If you are under the legal age of majority, you must ask your parent or legal guardian to consent to this license. If you are entering this license on behalf of a company or other legal entity, you represent that you have legal authority and “you” will mean the entity you represent.
By using the SDK, you affirm that you have reached the legal age of majority, you accept the terms of this license, and you take legal and financial responsibility for the actions of your permitted users.

You agree to use the SDK only for purposes that are permitted by (a) this license, and (b) any applicable law, regulation or generally accepted practices or guidelines in the relevant jurisdictions.

1. LICENSE. Subject to the terms of this license and the terms in the supplement attached, NVIDIA hereby grants you a non-exclusive, non-transferable license, without the right to sublicense (except as expressly provided in this license) to:
a. Install and use the SDK,
b. Modify and create derivative works of sample source code delivered in the SDK, and
c. Distribute any software and materials within the SDK, other than developer tools provided for your internal use, as incorporated in object code format into a software application subject to the distribution requirements indicated in this license.

2. DISTRIBUTION REQUIREMENTS. These are the distribution requirements for you to exercise the grants above:
a.	An application must have material additional functionality, beyond the included portions of the SDK.
b.	The following notice shall be included in modifications and derivative works of source code distributed: “This software contains source code provided by NVIDIA Corporation.”
c.	You agree to distribute the SDK subject to the terms at least as protective as the terms of this license, including (without limitation) terms relating to the license grant, license restrictions and protection of NVIDIA’s intellectual property rights. Additionally, you agree that you will protect the privacy, security and legal rights of your application users.
d.	You agree to notify NVIDIA in writing of any known or suspected distribution or use of the SDK not in compliance with the requirements of this license, and to enforce the terms of your agreements with respect to the distributed portions of the SDK.

3. AUTHORIZED USERS. You may allow employees and contractors of your entity or of your subsidiary(ies) to access and use the SDK from your secure network to perform work on your behalf. If you are an academic institution you may allow users enrolled or employed by the academic institution to access and use the SDK from your secure network. You are responsible for the compliance with the terms of this license by your authorized users.

4. LIMITATIONS. Your license to use the SDK is restricted as follows:
a.	You may not reverse engineer, decompile or disassemble, or remove copyright or other proprietary notices from any portion of the SDK or copies of the SDK.
b.	Except as expressly provided in this license, you may not copy, sell, rent, sublicense, transfer, distribute, modify, or create derivative works of any portion of the SDK. For clarity, you may not distribute or sublicense the SDK as a stand-alone product.
c.	 Unless you have an agreement with NVIDIA for this purpose, you may not indicate that an application created with the SDK is sponsored or endorsed by NVIDIA.
d.	 You may not bypass, disable, or circumvent any technical limitation, encryption, security, digital rights management or authentication mechanism in the SDK.
e.	You may not use the SDK in any manner that would cause it to become subject to an open source software license. As examples, licenses that require as a condition of use, modification, and/or distribution that the SDK be: (i) disclosed or distributed in source code form; (ii) licensed for the purpose of making derivative works; or (iii) redistributable at no charge.
f.	 Unless you have an agreement with NVIDIA for this purpose, you may not use the SDK with any system or application where the use or failure of the system or application can reasonably be expected to threaten or result in personal injury, death, or catastrophic loss. Examples include use in avionics, navigation, military, medical, life support or other life critical applications. NVIDIA does not design, test or manufacture the SDK for these critical uses and NVIDIA shall not be liable to you or any third party, in whole or in part, for any claims or damages arising from such uses.
g.	You agree to defend, indemnify and hold harmless NVIDIA and its affiliates, and their respective employees, contractors, agents, officers and directors, from and against any and all claims, damages, obligations, losses, liabilities, costs or debt, fines, restitutions and expenses (including but not limited to attorney’s fees and costs incident to establishing the right of indemnification) arising out of or related to your use of the SDK outside of the scope of this license, or not in compliance with its terms.

5. UPDATES. NVIDIA may, at its option, make available patches, workarounds or other updates to this SDK. Unless the updates are provided with their separate governing terms, they are deemed part of the SDK licensed to you as provided in this license. Further, NVIDIA may, at its option, automatically update the SDK or other software in the system, except for those updates that you may opt-out via the SDK API. You agree that the form and content of the SDK that NVIDIA provides may change without prior notice to you. While NVIDIA generally maintains compatibility between versions, NVIDIA may in some cases make changes that introduce incompatibilities in future versions of the SDK.

6. PRE-RELEASE VERSIONS. SDK versions identified as alpha, beta, preview, early access or otherwise as pre-release may not be fully functional, may contain errors or design flaws, and may have reduced or different security, privacy, availability, and reliability standards relative to commercial versions of NVIDIA software and materials. You may use a pre-release SDK version at your own risk, understanding that these versions are not intended for use in production or business-critical systems. NVIDIA may choose not to make available a commercial version of any pre-release SDK. NVIDIA may also choose to abandon development and terminate the availability of a pre-release SDK at any time without liability.

7. THIRD-PARTY COMPONENTS. The SDK may include third-party components with separate legal notices or terms as may be described in proprietary notices accompanying the SDK. If and to the extent there is a conflict between the terms in this license and the third-party license terms, the third-party terms control only to the extent necessary to resolve the conflict.

8. OWNERSHIP.

8.1 NVIDIA reserves all rights, title and interest in and to the SDK not expressly granted to you under this license. NVIDIA and its suppliers hold all rights, title and interest in and to the SDK, including their respective intellectual property rights. The SDK is copyrighted and protected by the laws of the United States and other countries, and international treaty provisions.

8.2 Subject to the rights of NVIDIA and its suppliers in the SDK, you hold all rights, title and interest in and to your applications and your derivative works of the sample source code delivered in the SDK including their respective intellectual property rights.

9. FEEDBACK. You may, but are not obligated to, provide Feedback to NVIDIA. “Feedback” means all suggestions, fixes, modifications, feature requests or other feedback regarding the SDK. Feedback, even if designated as confidential by you, shall not create any confidentiality obligation for NVIDIA. If you provide Feedback, you hereby grant NVIDIA, its affiliates and its designees a non-exclusive, perpetual, irrevocable, sublicensable, worldwide, royalty-free, fully paid-up and transferable license, under your intellectual property rights, to publicly perform, publicly display, reproduce, use, make, have made, sell, offer for sale, distribute (through multiple tiers of distribution), import, create derivative works of and otherwise commercialize and exploit the Feedback at NVIDIA’s discretion. You will not give Feedback (i) that you have reason to believe is subject to any restriction that impairs the exercise of the grant stated in this section, such as third-party intellectual property rights or (ii) subject to license terms which seek to require any product incorporating or developed using such Feedback, or other intellectual property of NVIDIA or its affiliates, to be licensed to or otherwise shared with any third party.

10. NO WARRANTIES. THE SDK IS PROVIDED AS-IS. TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW NVIDIA AND ITS AFFILIATES EXPRESSLY DISCLAIM ALL WARRANTIES OF ANY KIND OR NATURE, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING, BUT NOT LIMITED TO, WARRANTIES OF MERCHANTABILITY, NON-INFRINGEMENT, FITNESS FOR A PARTICULAR PURPOSE, USAGE OF TRADE AND COURSE OF DEALING. NVIDIA DOES NOT WARRANT THAT THE SDK WILL MEET YOUR REQUIREMENTS OR THAT THE OPERATION THEREOF WILL BE UNINTERRUPTED OR ERROR-FREE, OR THAT ALL ERRORS WILL BE CORRECTED.

11. LIMITATIONS OF LIABILITY. TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW NVIDIA AND ITS AFFILIATES SHALL NOT BE LIABLE FOR ANY (I) SPECIAL, INCIDENTAL, PUNITIVE OR CONSEQUENTIAL DAMAGES, OR FOR DAMAGES FOR  (A) ANY LOST PROFITS, PROJECT DELAYS, LOSS OF USE, LOSS OF DATA OR LOSS OF GOODWILL, OR (B) THE COSTS OF PROCURING SUBSTITUTE PRODUCTS, ARISING OUT OF OR IN CONNECTION WITH THIS LICENSE OR THE USE OR PERFORMANCE OF THE SDK, WHETHER SUCH LIABILITY ARISES FROM ANY CLAIM BASED UPON BREACH OF CONTRACT, BREACH OF WARRANTY, TORT (INCLUDING NEGLIGENCE), PRODUCT LIABILITY OR ANY OTHER CAUSE OF ACTION OR THEORY OF LIABILITY, EVEN IF NVIDIA HAS PREVIOUSLY BEEN ADVISED OF, OR COULD REASONABLY HAVE FORESEEN, THE POSSIBILITY OF SUCH DAMAGES. IN NO EVENT WILL NVIDIA’S AND ITS AFFILIATES TOTAL CUMULATIVE LIABILITY UNDER OR ARISING OUT OF THIS LICENSE EXCEED US$10.00. THE NATURE OF THE LIABILITY OR THE NUMBER OF CLAIMS OR SUITS SHALL NOT ENLARGE OR EXTEND THIS LIMIT.

12. TERMINATION. Your rights under this license will terminate automatically without notice from NVIDIA if you fail to comply with any term and condition of this license or if you commence or participate in any legal proceeding against NVIDIA with respect to the SDK. NVIDIA may terminate this license with advance written notice to you, if NVIDIA decides to no longer provide the SDK in a country or, in NVIDIA’s sole discretion, the continued use of it is no longer commercially viable. Upon any termination of this license, you agree to promptly discontinue use of the SDK and destroy all copies in your possession or control. Your prior distributions in accordance with this license are not affected by the termination of this license. All provisions of this license will survive termination, except for the license granted to you.

13. APPLICABLE LAW. This license will be governed in all respects by the laws of the United States and of the State of Delaware, without regard to the conflicts of laws principles. The United Nations Convention on Contracts for the International Sale of Goods is specifically disclaimed. You agree to all terms of this license in the English language. The state or federal courts residing in Santa Clara County, California shall have exclusive jurisdiction over any dispute or claim arising out of this license. Notwithstanding this, you agree that NVIDIA shall still be allowed to apply for injunctive remedies or urgent legal relief in any jurisdiction.

14. NO ASSIGNMENT. This license and your rights and obligations thereunder may not be assigned by you by any means or operation of law without NVIDIA’s permission. Any attempted assignment not approved by NVIDIA in writing shall be void and of no effect. NVIDIA may assign, delegate or transfer this license and its rights and obligations, and if to a non-affiliate you will be notified.

15. EXPORT. The SDK is subject to United States export laws and regulations. You agree to comply with all applicable U.S. and international export laws, including the Export Administration Regulations (EAR) administered by the U.S. Department of Commerce and economic sanctions administered by the U.S. Department of Treasury’s Office of Foreign Assets Control (OFAC). These laws include restrictions on destinations, end-users and end-use. By accepting this license, you confirm that you are not currently residing in a country or region currently embargoed by the U.S. and that you are not otherwise prohibited from receiving the SDK.

16. GOVERNMENT USE. The SDK, documentation and technology (“Protected Items”) are “Commercial products” as this term is defined at 48 C.F.R. 2.101, consisting of “commercial computer software” and “commercial computer software documentation” as such terms are used in, respectively, 48 C.F.R. 12.212 and 48 C.F.R. 227.7202 & 252.227-7014(a)(1). Before any Protected Items are supplied to the U.S. Government, you will (i) inform the U.S. Government in writing that the Protected Items are and must be treated as commercial computer software and commercial computer software documentation developed at private expense; (ii) inform the U.S. Government that the Protected Items are provided subject to the terms of the Agreement; and (iii) mark the Protected Items as commercial computer software and commercial computer software documentation developed at private expense. In no event will you permit the U.S. Government to acquire rights in Protected Items beyond those specified in 48 C.F.R. 52.227-19(b)(1)-(2) or 252.227-7013(c) except as expressly approved by NVIDIA in writing.

17. NOTICES. You agree that any notices that NVIDIA sends you electronically, such as via email, will satisfy any legal communication requirements. Please direct your legal notices or other correspondence to NVIDIA Corporation, 2788 San Tomas Expressway, Santa Clara, California 95051, United States of America, Attention: Legal Department.

18. ENTIRE AGREEMENT. This license is the final, complete and exclusive agreement between the parties relating to the subject matter of this license and supersedes all prior or contemporaneous understandings and agreements relating to this subject matter, whether oral or written. If any court of competent jurisdiction determines that any provision of this license is illegal, invalid or unenforceable, the remaining provisions will remain in full force and effect. Any amendment or waiver under this license shall be in writing and signed by representatives of both parties.

19. LICENSING. If the distribution terms in this license are not suitable for your organization, or for any questions regarding this license, please contact NVIDIA at nvidia-rtx-license-questions@nvidia.com.
(v. March 14, 2024)


NVIDIA RTX SUPPLEMENT TO SOFTWARE LICENSE AGREEMENT FOR NVIDIA SOFTWARE DEVELOPMENT KITS
The terms in this supplement govern your use of the NVIDIA RTX SDKs, including the DLSS SDK, NGX SDK, RTXGI SDK, RTXDI SDK, RTX Video SDK, RTX Dynamic Vibrance SDK and/or NRD SDK, if and when made available to you (in each case, the “SDK”) under the terms of your license agreement (“Agreement”) as modified by this supplement. Capitalized terms used but not defined below have the meaning assigned to them in the Agreement.
This supplement is an exhibit to the Agreement and is incorporated as an integral part of the Agreement. In the event of conflict between the terms in this supplement and the terms in the Agreement, the terms in this supplement govern.

1. Interoperability. Your applications that incorporate, or are based on, the SDK must be fully interoperable with compatible GPU hardware products designed by NVIDIA or its affiliates. Further, the DLSS SDK, NGX SDK and RTX Dynamic Vibrance SDK are licensed for you to develop applications only for their use in systems with NVIDIA GPUs.

2. Game License. You may, but are not obligated to, provide your game or related content (“Game Content”) to NVIDIA. If you provide Game Content, you hereby grant NVIDIA, its affiliates and its designees a non-exclusive, perpetual, irrevocable, worldwide, royalty-free, fully paid-up license, to use the Game Content to improve NVIDIA DLSS SDK and DLSS Model Training.

3. Limitations for the DLSS SDK, NGX SDK,RTX Video SDK and RTX Dynamic Vibrance SDK. Your applications that incorporate, or are based on, the DLSS SDK, NGX SDK, RTX Video SDK or RTX Dynamic Vibrance SDK may be deployed in a cloud service that runs on systems that consume NVIDIA vGPU software, and any other cloud service use of such SDKs or their functionality is outside of the scope of the Agreement. For the purpose of this section, cloud services include application service providers or service bureaus, operators of hosted/virtual system environments, or hosting, time sharing or providing any other type of service to others.

4. Notification for the DLSS SDK, NGX SDK and RTX Dynamic Vibrance SDK. You are required to notify NVIDIA prior to commercial release of an application (including a plug-in to a commercial application) that incorporates, or is based on, the DLSS SDK, NGX SDK or RTX Dynamic Vibrance SDK. Please send notifications to: https://developer.nvidia.com/sw-notification and provide the following information in the email: company name, publisher and developer name, NVIDIA SDK used, application name, platform (i.e. PC, Linux), scheduled ship date, and weblink to product/video.

5. Audio and Video Encoders and Decoders. You acknowledge and agree that it is your sole responsibility to obtain any additional third-party licenses required to make, have made, use, have used, sell, import, and offer for sale your products or services that include or incorporate any third-party software and content relating to audio and/or video encoders and decoders from, including but not limited to, Microsoft, Thomson, Fraunhofer IIS, Sisvel S.p.A., MPEG-LA, and Coding Technologies. NVIDIA does not grant to you under this Agreement any necessary patent or other rights with respect to any audio and/or video encoders and decoders.

6. SDK Terms.
6.1 Over the Air Updates. By installing or using the SDK you agree that NVIDIA can make over-the-air updates of the SDK in systems that have the SDK installed, including (without limitation) for quality, stability or performance improvements or to support new hardware.

6.2 SDK Integration. If you publicly release a DLSS integration in an end user game or application that presents material stability, performance, image quality, or other technical issues impacting the user experience, you will work to quickly address the integration issues. In the case issues are not addressed, NVIDIA reserves the right, as a last resort, to temporarily disable the DLSS integration until the issues can be fixed.

7. Marketing.
7.1 Marketing Activities. Your license to the SDK(s) under the Agreement is subject to your compliance with the following marketing terms:
(a)  Identification by You in the DLSS SDK, NGX SDK, RTX Video SDK or Dynamic Vibrance SDK.  During the term of the Agreement, NVIDIA agrees that you may identify NVIDIA on your websites, printed collateral, trade-show displays and other retail packaging materials, as the supplier of the DLSS SDK,  NGX SDK, RTX Video SDK or Dynamic Vibrance SDK for the applications that were developed with use of such SDKs, provided that all such references to NVIDIA will be subject to NVIDIA's prior review and written approval, which will not be unreasonably withheld or delayed.
(b)  NVIDIA Trademark Placement in Applications with the DLSS SDK, NGX SDK, or RTX Video SDK.  For applications that incorporate the DLSS SDK or NGX SDK or portions thereof, you must attribute the use of the applicable SDK and include the NVIDIA Marks on splash screens, in the about box of the application (if present), and in credits for game applications.
(c) NVIDIA Trademark Placement in Applications with a licensed SDK, other than the DLSS SDK, RTX Video SDK or NGX SDK. For applications that incorporates and/or makes use of a licensed SDK, other than the DLSS SDK, RTX Video SDK or NGX SDK, you must attribute the use of the applicable SDK and include the NVIDIA Marks on the credit screen for applications that have such credit screen, or where a credit screen is not present prominently in end user documentation for the application.
(d) Identification by NVIDIA in the DLSS SDK, NGX SDK, RTX Video SDK or Dynamic Vibrance SDK.  You agree that NVIDIA may identify you on NVIDIA's websites, printed collateral, trade-show displays, and other retail packaging materials as an individual or entity that produces products and services which incorporate the DLSS SDK, NGX SDK, RTX Video SDK or Dynamic Vibrance SDK as applicable. To the extent that you provide NVIDIA with input or usage requests with regard to the use of your logo or materials, NVIDIA will use commercially reasonable efforts to comply with such requests.  For the avoidance of doubt, NVIDIA’s rights pursuant to this section shall survive any expiration or termination of the Agreement with respect to existing applications which incorporate the DLSS SDK, RTX Video SDK or NGX SDK.
(e) Applications Marketing Material in the DLSS SDK, NGX SDK, RTX Video SDK or Dynamic Vibrance SDK. You may provide NVIDIA with screenshots, imagery, and video footage of applications representative of your use of the NVIDIA DLSS SDK or NGX SDKs in your application (collectively, “Assets”). You hereby grant to NVIDIA the right to create and display self-promotional demo materials using the Assets, and after release of the application to the public to distribute, sub-license, and use the Assets to promote and market the NVIDIA RTX SDKs.  To the extent you provide NVIDIA with input or usage requests with regard to the use of your logo or materials, NVIDIA will use commercially reasonable efforts to comply with such requests.  For the avoidance of doubt, NVIDIA’s rights pursuant to this section shall survive any termination of the Agreement with respect to applications which incorporate the NVIDIA RTX SDK.

7.2 Trademark Ownership and Licenses. Trademarks are owned and licenses as follows:
(a)	Ownership of Trademarks.  Each party owns the trademarks, logos, and trade names (collectively "Marks") for their respective products or services, including without limitation in applications, and the NVIDIA RTX SDKs. Each party agrees to use the Marks of the other only as permitted in this exhibit.

(b)	Trademark License to NVIDIA.  You grant to NVIDIA a non-exclusive, non-sub licensable, non-transferable (except as set forth in the assignment provision of the Agreement), worldwide license to refer to you and your applications, and to use your Marks on NVIDIA's marketing materials and on NVIDIA's website (subject to any reasonable conditions of you) solely for NVIDIA’s marketing activities set forth in this exhibit Sections 7 (d)-(e) above.  NVIDIA will follow your specifications for your Marks as to style, color, and typeface as reasonably provided to NVIDIA.

(c)	Trademark License to You.  NVIDIA grants to you a non-exclusive, non-sub licensable, non-transferable (except as set forth in the assignment provision of the Agreement), worldwide license, subject to the terms of this exhibit and the Agreement, to use NVIDIA RTX™, NVIDIA GeForce RTX™ in combination with GeForce products, and/or NVIDIA Quadro RTX™  in combination with Quadro products (collectively, the “NVIDIA Marks”) on your marketing materials and on your website (subject to any reasonable conditions of NVIDIA) solely for your marketing activities set forth in this exhibit Sections 7.1 (a)-(c) above.  For the avoidance of doubt, you will not and will not permit others to use any NVIDIA Mark for any other goods or services, or in a way that tarnishes, degrades, disparages or reflects adversely any of the NVIDIA Marks or NVIDIA’s business or reputation, or that dilutes or otherwise harms the value, reputation or distinctiveness of or NVIDIA’s goodwill in any NVIDIA Mark. In addition to the termination rights set forth in the Agreement, NVIDIA may terminate this trademark license at any time upon written notice to you. You will follow NVIDIA's use guidelines and specifications for NVIDIA's Marks as to style, color and typeface as provided in NVIDIA Marks and submit a sample of each proposed use of NVIDIA's Marks at least two (2) weeks prior to the desired implementation of such use to obtain NVIDIA's prior written approval (which approval will not be unreasonably withheld or delayed).  If NVIDIA does not respond within ten (10) business days of your submission of such sample, the sample will be deemed unapproved. All goodwill associated with use of NVIDIA Marks will inure to the sole benefit of NVIDIA. For the RTX Video SDK, contact NVIDIA at nvidia-rtx-video-sdk-license-questions@nvidia.com.

7.3 Use Guidelines. Use of the NVIDIA Marks is subject to the following guidelines:
(a)	Business Practices.  You covenant that you will: (a)  conduct business with respect to NVIDIA’s products in a manner that reflects favorably at all times on the good name, goodwill and reputation of such products; (b) avoid deceptive, misleading or unethical practices that are detrimental to NVIDIA, its customers, or end users; (c) make no false or misleading representations with regard to NVIDIA or its products; and (d) not publish or employ or cooperate in the publication or employment of any misleading or deceptive advertising or promotional materials.

(b)	No Combination Marks or Similar Marks.  You agree not to (a) combine NVIDIA Marks with any other content without NVIDIA’s prior written approval, or (b) use any other trademark, trade name, or other designation of source which creates a likelihood of confusion with NVIDIA Marks.

(v. March 14, 2024)
```
