# Third-party notices

gmod-mcp binaries include code from the following projects. Sources are downloaded at build time by CMake
(`FetchContent`, pinned versions verified by SHA-256); they are not stored in this repository.

| Project | Version | License | Use |
|---|---|---|---|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.65.0 | MIT | HTTP server and client |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | JSON |
| [json-schema-validator](https://github.com/pboettch/json-schema-validator) | 2.4.1 | MIT | Validation of tool arguments |
| [garrysmod_common](https://github.com/danielga/garrysmod_common) | commit `f77a18d` | BSD-3-Clause | `ILuaShared` interface header |

The full license texts are shipped with each project's source. The BSD-3-Clause license of garrysmod_common requires the
following notice:

> Copyright (c) 2015-2023, Daniel Almeida. All rights reserved.
> Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
> following conditions are met: (1) redistributions of source code must retain the above copyright notice, this list of
> conditions and the following disclaimer; (2) redistributions in binary form must reproduce the above copyright notice,
> this list of conditions and the following disclaimer in the documentation and/or other materials provided with the
> distribution; (3) neither the name of the copyright holder nor the names of its contributors may be used to endorse or
> promote products derived from this software without specific prior written permission.
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
> INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
> DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
> SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
> SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
> WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
> OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Garry's Mod is a trademark of Facepunch Studios. This project is not affiliated with or endorsed by Facepunch Studios
or Valve.
