# eld-encoder licensing

**Source in this directory** (`main.c`, `build.sh`, `test_native.py`, and the docs) is under
the MIT license (header of `main.c`), so it can be combined with the FDK AAC library.

**The built binary** (`eld-encoder`, `eld-encoder.exe`) statically links the **Fraunhofer FDK
AAC Codec Library** (fdk-aac 2.0.3, <https://github.com/mstorsjo/fdk-aac>). That library is
under its own license, the "Software License for The Fraunhofer FDK AAC Codec Library". A
verbatim copy from the pinned source release is in [FDK-AAC-NOTICE.txt](FDK-AAC-NOTICE.txt);
the upstream copy is <https://github.com/mstorsjo/fdk-aac/blob/v2.0.3/NOTICE>. Anyone who
distributes the binary must include that notice with it. The Windows build places it next to
the exe as `eld-encoder-FDK-AAC-NOTICE.txt`.

The FDK license grants no patent rights. AAC patent licenses, where needed, come from Via
Licensing or the individual patent holders (see the notice's introduction and section 3).

**Relationship to overflow-helper.** `overflow-helper` (GPLv3; its vendored doubletake code is
LGPL-3.0) is a separate program. It contains no fdk-aac code and does not link `eld-encoder`.
It launches `eld-encoder` as a child process and exchanges raw PCM and encoded audio frames
over stdin/stdout pipes (see [README.md](README.md)). The FDK license is widely regarded as
incompatible with the GPL for distribution, so the two are deliberately kept as separate
programs that communicate at arm's length. The helper works without `eld-encoder`. Receivers
that require AAC-ELD then get video without audio.
