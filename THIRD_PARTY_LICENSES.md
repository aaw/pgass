# Third-party licenses

pgass itself is [Unlicense](LICENSE) (public domain). A compiled `pgass`
binary or Python wheel statically links code from the projects below, each
under its own license. This file lists them and what, if anything, that
means for someone distributing or receiving a compiled artifact.

Two of these (GMP and LibPoly) are LGPLv3, which requires that anyone who
receives a `pgass` binary be able to relink it against a different build of
that library. That requirement is satisfied by the project being fully open
source: `cmake/cvc5.cmake` accepts `-Dcvc5_ROOT=<your own cvc5 build>`, so
anyone can build their own cvc5 against a different GMP or LibPoly and point
pgass at it. No further action is needed to distribute binaries, beyond
carrying this file along with them.

## cvc5

Modified BSD license. Copyright (C) 2009-2026 by its authors and
contributors (see cvc5's own `AUTHORS` file).

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT OWNERS AND CONTRIBUTORS
''AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

pgass builds cvc5 with `--no-gpl` (the default, and the only option the
prebuilt release archives offer), so none of CLN, glpk-cut-log, or
CoCoALib are linked in, and the GPLv3 terms that would otherwise apply to
those never come into play.

## GMP (GNU Multiple Precision Arithmetic Library)

LGPL v3, statically linked into cvc5 (and therefore into pgass). Full
license text: <https://www.gnu.org/licenses/lgpl-3.0.txt>.

See the relinking note at the top of this file.

## LibPoly

LGPL v3, statically linked into cvc5 (and therefore into pgass). Full
license text: <https://www.gnu.org/licenses/lgpl-3.0.txt>. Project:
<https://github.com/SRI-CSL/libpoly>.

See the relinking note at the top of this file.

## CaDiCaL

MIT license.

```
Copyright (c) 2016-2021 Armin Biere, Johannes Kepler University Linz, Austria
Copyright (c) 2020-2021 Mathias Fleury, Johannes Kepler University Linz, Austria
Copyright (c) 2020-2021 Nils Froleyks, Johannes Kepler University Linz, Austria
Copyright (c) 2022-2026 Katalin Fazekas, Vienna University of Technology, Austria
Copyright (c) 2021-2026 Armin Biere, University of Freiburg, Germany
Copyright (c) 2021-2026 Mathias Fleury, University of Freiburg, Germany
Copyright (c) 2023-2026 Florian Pollitt, University of Freiburg, Germany
Copyright (c) 2024-2026 Tobias Faller, University of Freiburg, Germany

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## symfpu

BSD 3-clause license. Compiled directly into cvc5's floating-point solver.

```
SymFPU is copyright (C) 2018 by Martin Brain, University of Oxford.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Abseil

Apache License 2.0, statically linked into every pgass target (not just
through cvc5). Full license text:
<https://www.apache.org/licenses/LICENSE-2.0>. Project:
<https://github.com/abseil/abseil-cpp>.

## nanobind

BSD 3-clause license. Compiled into the `pgass._native` extension module
shipped in the Python wheels (not into the `pgass` CLI binary). Project:
<https://github.com/wjakob/nanobind>.

## robin-map

MIT license. Vendored by nanobind as its hash map implementation, compiled
into the `pgass._native` extension module. Project:
<https://github.com/Tessil/robin-map>.
