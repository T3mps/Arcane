# Fuzzing the ArcaneCore parsers

libFuzzer harnesses for the ArcaneCore code that reads untrusted bytes. The
top priority is the client wire protocol: the Aphelyon game servers link
ArcaneCore and parse it straight off the open internet.

Everything here builds **standalone** -- one `clang++` invocation per target over
the ArcaneCore headers plus only the `.cpp` files that target needs. No premake,
no engine build, no SDL, so it works from `main` on any Linux box.

## Targets

| Target | Harness | Code under test | Input |
|---|---|---|---|
| `protocol` | `fuzz/protocol_fuzz.cpp` | `ExtractLengthFramed` (`Net/TcpSocket.hpp`), `Message::ParseBody`/`ToString`/`HasToken`, `RequiresAuthentication` (`Net/Protocol.hpp`) | a raw TCP byte stream (first byte = segmentation/cap mode) |
| `artifact` | `fuzz/artifact_fuzz.cpp` | `ReadClientArtifact`, `ReadClientMeshArtifact`, `FindArtifactForGuid`, `ReadClientExternalBuffers` (`Assets/ArtifactReader.cpp`) | one `.arcart` file (also read as a `.gltf`/`.glb` source) |
| `scene` | `fuzz/scene_fuzz.cpp` | `Scene::ReadSceneFile` -> `ApplySceneDocument` -> `LoadJson` + the reflection reader (`Serialization/`) | one `.arcscene` |
| `sprite` | `fuzz/sprite_fuzz.cpp` | `LoadSpriteAsset`, `ComputeSpriteGeom` (`Sprite/`), `Guid::FromString` (the `.meta` sidecar / asset `id` parser) | one `.arcsprite` |
| `cli` | `fuzz/cli_fuzz.cpp` | `Arcane::Cli::Parse` (`Cli/`) and `ArcaneServer`'s `ServerConfig::Parse` | an argv, arguments separated by NUL |

Besides ASan/UBSan, each harness checks the properties a correct parser must
have and aborts with `<target>_fuzz: property violated (line N): <expr>` when
one breaks. The interesting ones:

- **protocol** replays the server reassembly loop (append chunk -> extract
  frames until `needMoreData` -> erase `consumed`; `error` drops the
  connection; buffer capped at `MAX_RECEIVE_BUFFER_SIZE`) over the stream cut
  into 1..16-byte chunks AND over the whole stream at once, and requires the
  same frames and the same drop verdict: **framing must not depend on TCP
  segmentation.** Accepted frames must have a plain-decimal LENGTH equal to
  the body size; an accepted TYPE must be plain decimal naming exactly that id
  (no uint16 wrap); every accepted message must survive `Serialize` -> frame ->
  `ParseBody`.
- **artifact** checks the contract consumers rely on without rechecking:
  `thumbRgba` is exactly `thumbWidth*thumbHeight*4` bytes (`Assets::PixelsFor`
  and the texture upload trust those dims), one `MipView` per declared mip,
  every mip inside the payload and exactly the bytes its dims imply (the
  upload reads `slicePitch` from there), mesh sections inside the index buffer,
  and never both kinds at once.
- **cli** checks that an accepted argv only ever yields values that parse
  fully as their declared type, honour `Choices`, and supply every `Required`
  option.

The reader-side targets write their input to a private temp file
(`$TMPDIR/arcane-<target>-fuzz-<pid>*`) because the APIs take paths.

## Prerequisites

- clang with libFuzzer and the sanitizer runtimes. On Ubuntu 24.04:
  `apt-get install clang-19 libclang-rt-19-dev` (without `libclang-rt-19-dev`
  the link fails on `libclang_rt.fuzzer.a`).
- `llvm-symbolizer` on `PATH`, or `ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-18`
  (any recent version works) for symbolized stacks.

## Running

```sh
scripts/run-fuzz.sh protocol 3600     # build + fuzz one target for an hour
scripts/run-fuzz.sh all 600           # every target, 10 min each, in turn
scripts/run-fuzz.sh build all         # build only -> fuzz/build/<target>_fuzz
scripts/run-fuzz.sh regress all       # replay fuzz/regressions + fuzz/corpus once (CI smoke)
FUZZ_JOBS=4 scripts/run-fuzz.sh scene 1800   # 4 parallel workers (logs: fuzz/build/fuzz-<n>.log)
```

`run-fuzz.sh` fuzzes into a scratch corpus (`fuzz/build/corpus/<target>`,
gitignored) seeded from the checked-in `fuzz/corpus/<target>`, uses
`fuzz/dict/<target>.dict` when present, and writes crashes to
`fuzz/build/artifacts/<target>/`. `-max_len` is set per format (protocol
16 KiB -- deliberately below the 64 KiB receive cap, see the harness comment;
artifact 8 KiB; scene 16 KiB; sprite 4 KiB; cli 1 KiB). Pass extra libFuzzer
flags in `FUZZ_FLAGS`; set `FUZZ_OUT` to keep runs apart.

The artifact seeds are generated (byte-exact to the format in
`ArtifactReader.hpp`, carrying the harness's guid and the hash of empty source
bytes so they get past the header gates): `python3 fuzz/tools/make_artifact_seeds.py`.

## Reproducing and minimizing a crash

```sh
scripts/run-fuzz.sh build protocol
fuzz/build/protocol_fuzz path/to/crash-<sha>                  # replay once
UBSAN_OPTIONS=print_stacktrace=1 fuzz/build/scene_fuzz crash  # UBSan stacks need this
fuzz/build/protocol_fuzz -minimize_crash=1 -runs=50000 \
    -artifact_prefix=/tmp/min- path/to/crash-<sha>            # smallest input that still crashes
```

`-minimize_crash` keeps ANY crash, so check that the minimized input still
names the same property or stack. Then:

1. Copy it to `fuzz/regressions/<target>/<what-it-is>` (a descriptive name).
2. Classify it: **real** (the engine misbehaves on this input) or **harness
   bug** (the harness asserted something the code never promised).
3. Fix a real one in ArcaneCore so the bad input is refused cleanly -- no
   exception escaping, no UB -- and add a Catch2 case under `ArcaneTests/src/`
   tagged `[fuzz]` that reproduces it.
4. `scripts/run-fuzz.sh regress <target>` must pass.

## Adding a target

1. `fuzz/<name>_fuzz.cpp` defining `LLVMFuzzerTestOneInput`. Call the real
   public API; check properties with a `FUZZ_CHECK` macro that prints and
   `abort()`s (see any existing harness). Keep it deterministic.
2. In `scripts/run-fuzz.sh`: add the name to `ALL_TARGETS`, its engine
   sources to `sources_for` (only what the linker asks for; small stand-ins
   live in `fuzz/support/` -- `LogStub.cpp` silences `Arcane::Log`,
   `DiagnosticsStub.cpp` stubs `Diagnostics::Publish`), and a `-max_len` to
   `maxlen_for`.
3. Seeds in `fuzz/corpus/<name>/`: a few small, VALID inputs that reach deep
   (hand-made, or real assets from `ReferenceProject/` minified).
4. A dictionary in `fuzz/dict/<name>.dict` if the format has keywords.

## Deferred targets

- **Cvars / Config, ProjectManifest / ProjectPaths** -- deferred until the
  settings arc merges (a large unmerged branch is rewriting them).
- **RateLimiter** (`Net/RateLimiter.hpp`) parses nothing: keys are opaque
  strings into an `LruCache`, and its `Config` comes from server code, not the
  wire. Not a parser target.
- **TcpSocket receive reassembly**: the recv loop itself lives in the
  (private) Aphelyon server; ArcaneCore ships the frame extractor it calls.
  `protocol_fuzz` replays that loop over `ExtractLengthFramed` socket-free.
- **AssetRegistry `.meta` sidecars** parse with nlohmann (exceptions off) and
  then `Guid::FromString`; the guid parser is covered by `sprite_fuzz`.
  AssetRegistry itself was not linked (it drags in the diagnostics envelope).

## Findings

First campaign: 2026-10-07, Linux, clang 19.1.1, ASan + UBSan, 4 cores.
Run times are wall-clock per target after the last fix landed; "lines" is
`run-fuzz.sh coverage` over the final corpus.

| Target | Run | Executions | Coverage of the code under test |
|---|---|---|---|
| protocol | 65 min (+25 min `-use_value_profile=1`) | 29.2 M (+19.4 M) | `ExtractLengthFramed` 100% lines, 24/26 branches; `ParseBody`, `Serialize`, `ToString`, `HasToken` 100% lines |
| artifact | 35 min (+25 min value profile) | 4.1 M (+2.6 M) | `ArtifactReader.cpp` 98.3% lines, 92.3% branches |
| scene | 35 + 10 min | 2.3 M + 0.9 M | reader paths of `SceneSerializer.hpp`/`ReflectionJson.hpp`/`SceneAsset.hpp` (the unhit lines are the writers: `SaveJson`, `Write*`) |
| sprite | 35 min | 5.3 M | `LoadSpriteAsset` + `ComputeSpriteGeom` every line but the can't-open branch; `Guid::FromString`/`ToString` 100% |
| cli | 35 min | 23.6 M | `Cli.cpp` 100% lines / 94.7% branches; `ServerConfig.cpp` 100% lines |

Every finding below is **real** (no harness false positives); each has a
minimized input under `fuzz/regressions/<target>/` and a `[fuzz]`-tagged
Catch2 case. None is a memory-safety bug reachable from the network.

| # | Target | Finding | Reach | Fix | Test |
|---|---|---|---|---|---|
| 1 | protocol | `ParseBody` read TYPE with `std::stoi` + uint16 cast: trailing junk (`"2\xC1\xBF"` -> 2), sign/space, and wrap (`65537`, `-65535` -> 1) accepted | network; parse strictness only -- every reachable id could be sent directly | `fix(net): parse the wire TYPE as strict decimal in [1, 65535]` | `FramingTest.cpp` |
| 2 | protocol | `ExtractLengthFramed`'s verdict depended on TCP segmentation (`"000000000000065:"` accepted whole, refused split); `+5`/` 5` LENGTH accepted | network; framing determinism only | `fix(net): decide the LENGTH prefix from its own bytes, not TCP segmentation` | `FramingTest.cpp` |
| 3 | artifact | texture header dims never checked against section sizes: thumbnail/mip byte counts, `mipCount` vs table, 2^31x2^31 mip wrapping to 0 -- the texture upload (`NriTextureCache`) and `PixelsFor` trust the dims -> heap over-read | a malformed/modded `.arcart` on disk | `fix(assets): refuse a texture artifact whose dims disagree with its bytes` | `ArtifactSizeContractTest.cpp` |
| 4 | artifact | `.gltf` buffer `"uri": ""` resolved to a directory; libstdc++ opens it, `tellg()` = `LLONG_MAX`, `bad_alloc` escapes the exception-free path (terminate) | a malformed asset, Linux/macOS | `fix(assets): never read a directory as a glTF external buffer` | `ExternalBufferPathTest.cpp` |
| 5 | scene | reflection reader's integer fields used `get<T>`: a double out of range (`"hi": 35089691696507535956`) is float->int UB; out-of-range integers wrapped | any `.arcscene`/reflected JSON | `fix(serialization): range-check JSON numbers read into integer fields` | `SceneNumberRangeTest.cpp` |

Observations not fixed here (no UB, or outside ArcaneCore):

- `MeshImporter.cpp`'s `ReadWholeFileBytes` (arccook, the pipeline peer of #4)
  has the same directory/`tellg` pattern, and the pipeline's own
  `ReadTextureArtifact` does not apply #3's size contract.
- `ReadClientExternalBuffers` follows absolute and `..` uris: a `.gltf` can make
  the engine read any file the process can read (only to hash it).
- `.arcsprite` `"ppu": 1e39` reads as `+inf` (passes `ppu > 0`) and yields a
  zero-size sprite; float fields in scenes likewise accept overflow-to-inf.
  IEEE-defined, not UB.
- `Cli`: a `Required()` option with an empty default given an explicit empty
  value (`--name ""`) is reported missing.
- Mesh artifacts do not check `indices[i] < vertexCount`; no CPU consumer
  indexes vertices by index today (the GPU path relies on robust access).
- On `main`, `Net/TcpSocket.hpp` does not include `<fcntl.h>` on POSIX
  (fixed on `linux/port`); it only compiles here because `Protocol.hpp` pulls
  it in transitively.
- `RequiresAuthentication` is only exercised with no `protocol.json` loaded
  (the unknown-id branch); loading one in the harness is a cheap follow-up.

## Proposed CI smoke job (not wired in)

`.github/workflows` is being reworked elsewhere, so this is a proposal only. It
replays every regression and seed (seconds), then fuzzes each target briefly;
a crash fails the job and uploads the input.

```yaml
  fuzz-smoke:
    runs-on: ubuntu-24.04
    timeout-minutes: 30
    steps:
      - uses: actions/checkout@<pinned-sha>
      - run: sudo apt-get update && sudo apt-get install -y clang-19 libclang-rt-19-dev llvm-19
      - run: scripts/run-fuzz.sh regress all
      - run: |
          for t in protocol artifact scene sprite cli; do
            scripts/run-fuzz.sh "$t" 120 || exit 1
          done
        env:
          ASAN_SYMBOLIZER_PATH: /usr/bin/llvm-symbolizer-19
      - if: failure()
        uses: actions/upload-artifact@<pinned-sha>
        with:
          name: fuzz-crashes
          path: fuzz/build/artifacts/
```

A nightly variant with a longer budget (and the scratch corpus cached between
runs) is the natural next step.
