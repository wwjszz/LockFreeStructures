# Third-party notices

## moodycamel::ConcurrentQueue

The benchmark vendors `benchmark/third_party/moodycamel/concurrentqueue.h`.
Upstream project: https://github.com/cameron314/concurrentqueue

Simplified BSD License:

Copyright (c) 2013-2020, Cameron Desrochers.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this list of
  conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice, this list of
  conditions and the following disclaimer in the documentation and/or other materials
  provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The header is also available under the Boost Software License 1.0; its text is stored beside
that header.

## Optional benchmark dependencies

These dependencies are used only when `LOCKFREESTRUCTURES_BUILD_BENCHMARKS=ON`. Their own
source distributions retain the complete license and copyright files.

- Google Benchmark v1.9.0 — Apache License 2.0.
- oneTBB v2021.13.0 — Apache License 2.0.
- Boost.Lockfree (Boost 1.74 or newer; locally verified with 1.90.0) — Boost Software License 1.0.