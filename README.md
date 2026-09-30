# eventree

Header-only C++20 library for event-camera data. See `docs/adr/` for design decisions.

## Build and test

Requires gcc with C++20 support and CMake 3.24+. GoogleTest is fetched by CMake on the first configure, so network access is needed once.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```
