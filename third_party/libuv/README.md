# libuv minimal subset (darwin + linux + windows)

This directory vendors only the minimal libuv closure required by Feng.

Version: 1.49.2
Platform closure: darwin + linux + windows

Included:
- public headers (uv.h + include/uv/*.h)
- common core sources (loop/threadpool/timer/inet/idna/random)
- unix base sources (async/poll/stream/tcp/udp/fs/process/thread/dns)
- darwin-specific sources from upstream CMake target graph
- linux-specific sources from upstream CMake target graph
- windows-specific sources from upstream CMake target graph
- recursively discovered internal src/* headers required by the selected sources

Excluded:
- tests/benchmarks/examples/docs/tools
- upstream CMake/Autotools build system

Build:
- `make` builds the native static archive and stages it into `../../std/std/extlib/<host-platform>` by default.
- `make OUTPUT_DIR=<path>` overrides the staging directory.
- `make install` is an alias of the staging step.
- default staged library name: `libfeng_std_uv.a`
