# Playwright image-comparison fixtures

Vendored from microsoft/playwright, `tests/image_tools/fixtures/`, Apache License 2.0,
Copyright (c) Microsoft Corporation. `LICENSE` is upstream's, unmodified.

These are the CONFORMANCE ORACLE for `Arcane/Assets/ImageCompare`. Our comparator is a
reimplementation of theirs with identical constants and identical `double` arithmetic, so
it must classify every pair here the same way they do: everything under `should-match/`
passes at the default zero budget, everything under `should-fail/` does not.

Do not "fix" a failing case by loosening a constant. A divergence here means the port
diverged, which is the only thing this corpus is for.
