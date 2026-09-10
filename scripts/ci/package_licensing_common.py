"""Shared helpers for final-artifact SBOM and third-party notices."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[2]
LICENSE_DIR = ROOT / "3rdparty_licenses"
FULL_SHA = re.compile(r"^[0-9a-fA-F]{40}$")

QT_PATTERN = re.compile(r"^(?:lib)?qt6", re.IGNORECASE)
LOOP_PATTERN = re.compile(r"^(?:lib)?loop", re.IGNORECASE)
PREFLIGHT_PATTERN = re.compile(r"loop-?preflight", re.IGNORECASE)


@dataclass(frozen=True)
class ComponentLicense:
    name: str
    spdx_id: str
    notice_file: str | None = None
    summary: str | None = None


# Basename patterns for shipped shared libraries and executables. Order matters:
# first match wins.
KNOWN_COMPONENTS: tuple[tuple[re.Pattern[str], ComponentLicense], ...] = (
    (QT_PATTERN, ComponentLicense("Qt 6", "LGPL-3.0-only", "Qt-LGPL-3.0.txt")),
    (re.compile(r"^(?:lib)?ssl\d*|libcrypto", re.IGNORECASE), ComponentLicense("OpenSSL", "Apache-2.0", "OpenSSL_license.txt")),
    (re.compile(r"^liblcms2", re.IGNORECASE), ComponentLicense("Little CMS", "MIT", "LittleCMS_COPYING.txt")),
    (re.compile(r"^libopenjp2", re.IGNORECASE), ComponentLicense("OpenJPEG", "BSD-2-Clause", "OpenJPEG_LICENSE.txt")),
    (re.compile(r"^libfreetype", re.IGNORECASE), ComponentLicense("FreeType", "FTL", "freetype_FTL.TXT")),
    (re.compile(r"^libjpeg", re.IGNORECASE), ComponentLicense("libjpeg-turbo", "IJG", "libjpeg_README.txt")),
    (re.compile(r"^libpng\d*", re.IGNORECASE), ComponentLicense("libpng", "Libpng", None)),
    (re.compile(r"^libz\.so|^zlib1\.dll$", re.IGNORECASE), ComponentLicense("zlib", "Zlib", "zlib_README.txt")),
    (re.compile(r"^libharfbuzz", re.IGNORECASE), ComponentLicense("HarfBuzz", "MIT-Olden", None)),
    (re.compile(r"^libbrotli", re.IGNORECASE), ComponentLicense("Brotli", "MIT", None)),
    (re.compile(r"^libdouble-conversion", re.IGNORECASE), ComponentLicense("double-conversion", "BSD-3-Clause", None)),
    (re.compile(r"^libpcre2", re.IGNORECASE), ComponentLicense("PCRE2", "BSD-3-Clause", None)),
    (re.compile(r"^libicu", re.IGNORECASE), ComponentLicense("ICU", "ICU", None)),
    (re.compile(r"^libsentry", re.IGNORECASE), ComponentLicense("sentry-native", "MIT", None)),
    (LOOP_PATTERN, ComponentLicense("Loop", "MIT", "LOOP-MIT.txt")),
    (PREFLIGHT_PATTERN, ComponentLicense("loop-preflight", "MIT", None)),
)


class LicensingError(ValueError):
    """Raised when package licensing evidence cannot be generated."""


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def load_boundary_evidence(path: Path) -> dict[str, Any]:
    try:
        evidence = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise LicensingError(f"unable to read evidence {path}: {exc}") from exc
    if evidence.get("schema_version") != 1 or evidence.get("kind") != "loop-package-boundary-evidence":
        raise LicensingError(f"unsupported evidence schema: {path}")
    if not FULL_SHA.fullmatch(str(evidence.get("source_sha", ""))):
        raise LicensingError(f"evidence source SHA is not full length: {path}")
    return evidence


def basename(path: str) -> str:
    return path.replace("\\", "/").rsplit("/", 1)[-1]


def classify_binary(path: str) -> ComponentLicense:
    name = basename(path)
    for pattern, component in KNOWN_COMPONENTS:
        if pattern.search(name):
            return component
    if name.lower() in {"loopeditor", "loopeditor.exe", "pdftool", "pdftool.exe"}:
        return ComponentLicense("Loop", "MIT", "LOOP-MIT.txt")
    return ComponentLicense(name, "NOASSERTION", None)


def iter_shipped_binaries(evidence: dict[str, Any]) -> list[dict[str, Any]]:
    binaries = evidence.get("binaries")
    if not isinstance(binaries, list):
        raise LicensingError("evidence.binaries must be a list")
    shipped: list[dict[str, Any]] = []
    for row in binaries:
        if not isinstance(row, dict):
            continue
        if row.get("format") not in {"ELF", "PE"}:
            continue
        shipped.append(row)
    return shipped


def group_components(binaries: Iterable[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    groups: dict[str, dict[str, Any]] = {}
    for row in binaries:
        path = str(row.get("path", ""))
        component = classify_binary(path)
        key = component.name
        entry = groups.setdefault(
            key,
            {
                "component": component,
                "artifacts": [],
            },
        )
        entry["artifacts"].append(
            {
                "path": path,
                "sha256": row.get("sha256"),
                "size": row.get("size"),
            }
        )
    return groups


def read_notice_text(component: ComponentLicense) -> str | None:
    if component.notice_file:
        path = LICENSE_DIR / component.notice_file
        if path.is_file():
            return path.read_text(encoding="utf-8")
    if component.name == "Qt 6":
        return (
            "Qt 6 runtime libraries are redistributed with this package under the "
            "GNU Lesser General Public License, version 3. Recipients may replace "
            "and relink these libraries per docs/PACKAGING_LICENSING.md."
        )
    return component.summary


def spdx_ref(name: str) -> str:
    normalized = re.sub(r"[^A-Za-z0-9.-]+", "-", name).strip("-")
    return f"SPDXRef-{normalized or 'UNKNOWN'}"
