# vita-tracy

Tracy profiler port for PS Vita: userland client, kernel sampling backend,
and offline symbolication tooling.

## Layout

- `client/` — Vita platform port of the Tracy client (`libtracy_vita`).
- `kernel/` — `tracy_kernel.skprx`, the privileged sampling/metadata backend.
- `include/vita_tracy/` — ABI shared between userland and kernel.
- `samples/` — example apps exercising the client and kernel backend.
- `agent/` — optional userland agent for profiling uninstrumented apps.
- `tools/` — host-side offline symbolication and trace validation.
- `tests/` — host-native unit tests (doctest) for platform-independent logic.
- `docs/` — design notes, firmware compatibility, RE findings.

## Building

Requires [VitaSDK](https://vitasdk.org) (`$VITASDK` set) for the Vita
targets, and a host C++ compiler for the native test suite.

```sh
# Vita targets (client, kernel plugin, samples)
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
cmake --build build

# Host-native unit tests
cmake -B build-host -DVITA_TRACY_BUILD_HOST_TESTS=ON
cmake --build build-host
ctest --test-dir build-host
```

## Status

Early bring-up. See `docs/implementation.md` for phase status and
`docs/reverse_engineering.md` for open reverse-engineering questions.

## License

MIT, see `LICENSE`.
