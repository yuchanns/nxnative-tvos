# Building the tvOS application

Use macOS, Xcode with tvOS SDK, Python, .NET 10, spirv-cross, and xcodegen. Upstream development documentation describes the remaining host dependencies. This repository deliberately contains no ready-to-run game or credential.

Keep all private inputs and generated outputs outside the checkout. Obtain the matching game files and keys independently. The P5 profile records the accepted module build IDs; stop if they differ. Do not copy fixed patch addresses to a different game build.

## Translator and profile

Build the translator into the private work area:

```sh
export PRIVATE_WORK=/path/to/private/game-work
export DM_GAME=01004B10157F2000
export GAME="$DM_GAME"
export DM_ROOT="$PRIVATE_WORK"
export XLATE="$PRIVATE_WORK/tools/dmsh_xlate_bin/dmsh_xlate"
dotnet publish dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj \
  -c Release -o "$PRIVATE_WORK/tools/dmsh_xlate_bin"
python3 dm/gfx/tools/dmsh_chain.py --check-tools
python3 dm/gfx/tools/dmsh_chain.py
```

The retained lock records the exact pinned tool fingerprints. If a fresh toolchain differs, do not relabel old libraries as matching. Copy the profile into the external workspace, select it with `DM_GAMES_DIR` and `DM_GAMES_FIRST=1`, update its tool lock/header, then rebuild every shader target, variant, constant table, and host. Keep repository profiles unchanged while experimenting.

## External build layout

The original workspace held these private generated inputs:

- `extract/v65536/ExeFS` and `extract/v65536/RomFS`.
- `patched_v65536`, generated from the accepted modules with `dm/tools/patch_nso.py`.
- `shader/pkg`, the split game shader inventory.
- `shader/cache/b-<chain>`, `shader/ios/b-<chain>`, and `shader/tvos/b-<chain>`.
- `distribution/assets`, `distribution/shaders`, and `distribution/astc`, containing manifests and chunks.
- A public, secret-free app environment file and separately provisioned runtime credentials.

The upstream entry point `scripts/build_ipa_from_nsp.py` surveys unknown profiles and may launch a Mac guest for particular game families or heap checks. Review the selected path and disable guest execution when the task requires real-device-only testing. P5's CPK/Needle extraction does not need a Mac GPU run.

Follow `docs/development.md` for extraction/rewrite/split, then build the source-matched translator and libraries. `dm/tvos/tools/compile_shaders.py` performs tvOS lowering. Regenerate `c1_full.tsv` and `cb_extent.tsv`; do not omit framebuffer-fetch variants. Before packaging, run:

```sh
python3 dm/tvos/tools/verify_shader_chain.py \
  --work "$PRIVATE_WORK" \
  --profile dm/games/01004B10157F2000 \
  --output "$PRIVATE_WORK/shader-completeness.json"
```

The checker expects P5's 30,646 original keys and checks all targets, reflection, generated variants, unresolved driver/resource bindings, and library headers. It does not reconstruct missing game inputs.

Use `pack_assets.py` for bounded chunks and `build_app.py --help` for the explicit application inputs. Supply icon/cover/theme files from outside the checkout; game artwork has been removed. Use the manifest flags for RomFS, shaders and ASTC. The thin app contains resource manifests, not a full embedded RomFS; the manifests require a configured resource service. Cloud-save deployment configuration uses `YOUR_SAVE_BUCKET` and has no deployed secrets.

## CPU checks

These tests compile CPU-only doubles around production paths; they do not create a Metal device:

```sh
python3 dm/gfx/tests/test_buffer_cache.py
python3 dm/gfx/tests/test_texture_cache.py
python3 dm/gfx/tests/test_staging_retirement.py
python3 dm/gfx/tests/test_pool_ranges.py
python3 dm/gfx/tests/test_bc_copy_batch.py
```

Real-device validation must independently cover picture correctness, cold/warm load, stable scene timing, repeated transitions, same-process sleep/resume, memory pressure, saves and audio. Never use compilation, mock tests, or a desktop GPU result as proof of Apple TV performance.
