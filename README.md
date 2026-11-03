# nxnative tvOS

A tvOS port of [nxnative](https://github.com/seedds/nxnative), bringing native ARM64 game execution and an NVN-to-Metal graphics backend to Apple TV.

The port adds a tvOS application shell, resource streaming, a phone controller, cloud saves, and device diagnostics. Its graphics backend targets Apple5 capabilities and manages GPU residency for memory-constrained devices.

## Graphics and runtime

- Visibility-query batching and attachment analysis reduce unnecessary render-pass boundaries.
- Bounded guest-memory views and completion-scoped upload buffers control residency.
- Fence-safe texture and BC-buffer snapshots preserve content while retiring idle resources.
- Deferred texture initialization, content-addressed ASTC caches, pipeline warmup and batched BC copies reduce loading stalls.
- Lifecycle-aware services support backgrounding, resume and controller reconnection.

Persona 5 Royal 1.0.2 is the current validated profile. The current implementation targets native 1920x1080 rendering on the A12 Apple TV. See the [optimization record](docs/optimization-notes.md) for measurements, mechanisms and remaining limitations.

## Build and development

Start with the [build guide](docs/building.md) and [tvOS implementation notes](dm/tvos/README.md). Game inputs and generated outputs belong in an external workspace. The [development documentation](docs/development.md) describes the host and shader pipeline.

## Attribution

Based on [seedds/nxnative](https://github.com/seedds/nxnative) at `3de606f`, with shared tvOS work and the P5 changes. The upstream MIT license and third-party notices are preserved.
