# Vendored RNNoise

- Upstream: https://github.com/xiph/rnnoise (release v0.2, 2024-04-15)
- Archive: `rnnoise-0.2.tar.gz`, SHA-256 `90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37`
- License: BSD 3-clause, see `COPYING`
- Contents: `include/rnnoise.h`, the generic C sources from `RNNOISE_SOURCES` in upstream `Makefile.am`
  (model weights in `src/rnnoise_data.c`) and their headers, including the header-only `src/x86/*.h` the generic
  build includes. The x86 SSE4.1/AVX2 runtime-dispatch sources,
  examples, training tools and autotools files are not included. Files are unmodified.

Not packaged for Ubuntu, hence vendored. Built as a static library by the top-level `CMakeLists.txt`.
