# ADR-008: Semantic Versioning for Loupe releases

**Status:** accepted
**Implemented-at:** not implemented
**Last-verified:** 2026-08-12 @ b47d63e47bebfc1fdd74a75374fc9cecccddc325
**Superseded-by:** none
**Date:** 2026-08-12
**Deciders:** repository versioning policy

## Context

The inherited LOUPE `LOUPE_VERSION` was a four-part `MAJOR.MINOR.PATCH.TWEAK`
value (`1.6.0.0`). That is not Semantic Versioning. Loupe planning used a
parallel 0.0.x milestone line (`0.0.1` through `0.0.6`) that also needed a
single SemVer scheme.

## Decision

Loupe adopts **Semantic Versioning 2.0.0**. The current product version is
**0.1.0-alpha**.

- Canonical core is `MAJOR.MINOR.PATCH` in `LOUPE_VERSION` (`0.1.0`).
- Pre-release label is `LOUPE_VERSION_PRERELEASE` (`alpha`).
- Git tags are `vMAJOR.MINOR.PATCH` with optional pre-release / build metadata
  (`v0.1.0-alpha`).
- Appx derives `MAJOR.MINOR.PATCH.0` as `LOUPE_WINDOWS_VERSION`.
- Former planning numbers 0.0.3–0.0.6 remap to 0.1.1–0.1.4.
- Policy lives in [`docs/version-policy.json`](../version-policy.json) and
  [`docs/VERSIONING.md`](../VERSIONING.md), checked by
  `scripts/ci/check_version_policy.py`.

## Consequences

- Four-part tags such as `v1.6.0.0` are rejected.
- CMake, MSI, About strings, and PdfTool's envelope `version` share one SemVer
  identity (`0.1.0-alpha` while the pre-release label is set).
- Living docs and gates say 0.1.1, not 0.0.3. Completed 0.0.1 / 0.0.2 history
  is left in place.
