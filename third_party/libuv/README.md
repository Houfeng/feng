# libuv minimal subset (darwin)

This directory vendors only the host-target minimal libuv closure required by Feng.

Version: 1.49.2
Platform closure: darwin

Included:
- Unix public headers used by uv.h
- common core sources (loop/threadpool/timer/inet/idna/random)
- unix base sources (async/poll/stream/tcp/udp/fs/process/thread)
- darwin-specific sources from upstream CMake target graph
- recursively discovered internal src/* headers required by the selected sources

Excluded:
- Windows sources/headers
- tests/benchmarks/examples/docs/tools
- upstream CMake/Autotools build system

Build:
- `make` builds static archive and stages it into `../../std/extlib/<host-target>` by default.
- `make OUTPUT_DIR=<path>` overrides the staging directory.
- `make install` is an alias of the staging step.
- default staged library name: `libfeng_std_uv.a`
