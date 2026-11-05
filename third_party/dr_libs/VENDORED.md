# Vendored audio decoders

Unmodified single-file libraries used by `src/core/file_data.cpp` to decode
files that JSFX scripts open with `file_open()`.

| File | Upstream | Version | License |
|---|---|---|---|
| `dr_wav.h` | https://github.com/mackron/dr_libs | v0.14.6 | public domain (Unlicense) or MIT-0 |
| `dr_flac.h` | https://github.com/mackron/dr_libs | v0.13.4 | public domain (Unlicense) or MIT-0 |
| `dr_mp3.h` | https://github.com/mackron/dr_libs | v0.7.4 | public domain (Unlicense) or MIT-0 |
| `../stb/stb_vorbis.c` | https://github.com/nothings/stb | v1.22 | public domain (Unlicense) or MIT |

dr_libs commit `35b5c392cc15456736c440a68dbe073178bbffe0`, stb commit
`d14d46f6b44456eb88beac58cba833a4135d4c89`, both vendored on 2026-11-05.
Each file carries its license text at the end.
