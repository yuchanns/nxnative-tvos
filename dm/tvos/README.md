# tvOS build and validation

Build on macOS with the selected game profile and the matching translator lock.
The normal shader chain procedure still applies: regenerate `dmsh_chain.h`, build
all Mac and iOS shaders, regenerate `c1_full.tsv` and `cb_extent.tsv`, then rebuild
the host. For a workspace profile that also exists in the repository, set
`DM_GAMES_FIRST=1` as well as `DM_GAMES_DIR`.

`tools/compile_shaders.py` compiles that chain's MSL for tvOS. Its Apple5 lowering
is implemented in `../gfx/tools/dmsh_legacy.py` and is part of the chain hash:

- Compute collectives retain 32 lanes using threadgroup storage. Only converged
  control flow and complete 32-lane groups of at most 512 threads are accepted.
  Scratch indexing uses the hardware SIMD group and lane IDs. The runtime checks
  `threadExecutionWidth` before using the pipeline.
- An unused-result elected fragment counter can become one atomic increment per
  covered fragment, after proving its address is draw-uniform. Metal suppresses
  helper-thread stores and atomics. Other helper queries are rejected.
- Fragment exchanges become quad operations only when all call sites prove that
  their XOR target stays within the same four lanes.
- More than sixteen logical scalar sampler bindings use a table of sixteen
  physical states and an immutable index map for each draw. The host interns
  effective sampler descriptors. A draw with more than sixteen distinct states
  fails explicitly; it never aliases different settings.

Framebuffer-fetch variants must produce the same compatibility metadata as their
base shader. Unknown patterns stop the build. The Mac and iOS libraries retain
the original MSL.

Run the lowering rejection tests with:

```sh
python3 -m unittest discover -s dm/gfx/tests -p 'test_legacy_*.py'
```

The `legacy_subgroup.metal`, `legacy_fragment.metal`, and `legacy_quad.metal`
fixtures have standalone Metal GPU harnesses beside them. Compare the native and
lowered fixtures, including 8-by-8 and 16-by-16 compute groups. The `t_metal3`
suite also exercises the production compact sampler encoder with distinct wrap
and filter modes, separate draw mappings, and an overflow rejection. Run it both
normally and with `DM_NVN_TIER1=1`.

`tools/build_app.py` builds an unsigned application and can pack its IPA. A thin
package embeds the asset manifests; existing verified RomFS and ASTC chunks stay
in the application cache when the app is updated. A new shader chain gets its own
shader chunks. Device scene, picture, and input validation remain necessary:
compilation and desktop GPU tests alone do not establish Apple TV playability.

Pass `--icon` and `--cover` for game artwork, and optionally `--theme` for a JSON
presentation file. Its `colors` and `controller` objects override the keys in
`tools/brand_resources.py`. Colors are six-digit RGB strings. The `resources`
object can replace `controller-icon.png`, `controller-skin.jpg`, and
`controller-theme.css` using paths inside the theme directory. Keep game artwork
and theme files in the external game workspace. Generated resources provide the
app icon, Top Shelf, loading cover and colors, and browser controller presentation;
controller input handling stays shared. HTTP-served PNGs are copied after Xcode's
resource processing so browsers receive standard PNG rather than CgBI images.

For a private WebDAV resource source, build without `--gofile-resources` and set
`DM_TV_ASSET_URL` to the HTTPS directory containing `assets`, `shaders`, and
`astc`, with `DM_TV_ASSET_AUTH=basic`. Scope the server account to that resource
directory and allow reads only. The server must proxy bytes without redirects;
the client refuses redirects for authenticated requests and verifies each
download against the embedded size and SHA-256 before unpacking it.

The downloader uses up to four workers. Each worker retains its slot through
verification and unpacking, bounding in-flight and queued archives to four
(256 MiB for 64 MiB chunks). Archive writes and receipt commits remain serial
because fragments can share a sparse destination file. Failed requests retry
three times; a terminal failure cancels sibling downloads and preserves committed
receipts. The diagnostic `/assets` response exposes active downloads and aggregate
byte progress. No game or GPU execution is needed for the downloader tests:

```sh
clang -fobjc-arc -fblocks -Wno-deprecated-declarations \
  -framework Foundation -framework Security -framework JavaScriptCore \
  dm/tvos/tests/download_harness.m dm/tvos/Sources/asset_source.m \
  dm/tvos/Sources/gofile.m dm/tvos/Sources/ui_text.m -o /tmp/nx-download-harness
python3 dm/tvos/tests/test_downloads.py /tmp/nx-download-harness
```

Provision an `asset-credential.json` file in `Library/Caches/nxnative` through
the paired device connection. Its `endpoint` must exactly match the configured
URL without a trailing slash; its other fields are `username` and `password`.
The app stores this credential in its device-only Keychain and removes the
provisioning file, including when all resources are already cached. Keep this
file out of the IPA and logs. Updating the app preserves the credential and
verified resource cache. Validate both initial download and a process relaunch
with no provisioning file on a real Apple TV.
