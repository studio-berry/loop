#!/usr/bin/env python3
"""Inspect final Linux AppImage or Windows MSI payload dependencies.

The package boundary is intentionally checked from the produced artifact, not
from CMake intent or the staged build tree.  The inspector uses only the
platform tools already present on the packaging runners and fails closed when
an executable or library cannot be classified or inspected.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable, Sequence


FULL_SHA = re.compile(r"^[0-9a-fA-F]{40}$")
FORBIDDEN_WIDGETS = re.compile(r"^(?:lib)?qt6widgets(?:[.\\-]|$)", re.IGNORECASE)
FORBIDDEN_WIDGETS_SURFACE = re.compile(
    r"^(?:lib)?qt6(?:widgets|quickwidgets|printsupport)(?:[.\\-]|$)",
    re.IGNORECASE,
)
ELF_SUFFIX = re.compile(r"(?:\.so(?:\..*)?|\.elf)$", re.IGNORECASE)
PE_SUFFIX = re.compile(r"\.(?:dll|drv|exe|ocx|sys)$", re.IGNORECASE)


class InspectionError(RuntimeError):
    """Raised when the final artifact cannot be proven safe."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def relative_path(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def read_prefix(path: Path, size: int = 4096) -> bytes:
    with path.open("rb") as stream:
        return stream.read(size)


def binary_format(path: Path) -> str | None:
    """Return ELF/PE for a recognized binary, otherwise None."""
    prefix = read_prefix(path)
    if prefix.startswith(b"\x7fELF"):
        return "ELF"
    if prefix[:2] == b"MZ" and len(prefix) >= 0x40:
        pe_offset = int.from_bytes(prefix[0x3C:0x40], "little")
        if pe_offset + 4 <= len(prefix) and prefix[pe_offset : pe_offset + 4] == b"PE\0\0":
            return "PE"
        # The PE header can be beyond the initial probe for a valid image.
        with path.open("rb") as stream:
            stream.seek(0x3C)
            raw_offset = stream.read(4)
            if len(raw_offset) == 4:
                stream.seek(int.from_bytes(raw_offset, "little"))
                if stream.read(4) == b"PE\0\0":
                    return "PE"
    return None


def looks_like_binary(path: Path) -> bool:
    name = path.name.casefold()
    return bool(ELF_SUFFIX.search(name) or PE_SUFFIX.search(name))


def is_widgets_name(name: str) -> bool:
    normalized = name.replace("\\", "/")
    basename = normalized.rsplit("/", 1)[-1]
    return bool(FORBIDDEN_WIDGETS.search(basename))


def is_forbidden_widgets_surface_name(name: str) -> bool:
    normalized = name.replace("\\", "/")
    basename = normalized.rsplit("/", 1)[-1]
    return bool(FORBIDDEN_WIDGETS_SURFACE.search(basename))


def parse_readelf_needed(output: str) -> list[str]:
    pattern = re.compile(r"Shared library:\s*\[([^\]]+)\]")
    return unique_preserving_order(pattern.findall(output))


def parse_ldd(output: str) -> tuple[list[dict[str, str]], list[str]]:
    resolved: list[dict[str, str]] = []
    unresolved: list[str] = []
    for line in output.splitlines():
        text = line.strip()
        if not text:
            continue
        if " => not found" in text:
            unresolved.append(text.split(" =>", 1)[0].strip())
            continue
        if " => " in text:
            name, target = text.split(" => ", 1)
            target = target.split("(", 1)[0].strip()
            if target:
                resolved.append({"name": name.strip(), "path": target})
            continue
        # The loader and linux-vdso are printed as absolute entries without
        # an arrow. Keep them as resolved system dependencies.
        match = re.match(r"^(\S+)\s+\((?:0x)?[0-9a-fA-F]+\)$", text)
        if match:
            resolved.append({"name": match.group(1), "path": match.group(1)})
    return resolved, unique_preserving_order(unresolved)


def parse_dumpbin_dependents(output: str) -> list[str]:
    dependencies: list[str] = []
    for line in output.splitlines():
        text = line.strip()
        if re.fullmatch(r"[A-Za-z0-9_.-]+\.(?:dll|exe|drv|ocx|sys)", text, re.IGNORECASE):
            dependencies.append(text)
    return unique_preserving_order(dependencies)


def unique_preserving_order(values: Iterable[str]) -> list[str]:
    seen: set[str] = set()
    result: list[str] = []
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


def pe_architecture(path: Path) -> str:
    with path.open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise InspectionError(f"not a PE image: {path}")
        stream.seek(0x3C)
        raw_offset = stream.read(4)
        if len(raw_offset) != 4:
            raise InspectionError(f"PE header offset missing: {path}")
        stream.seek(int.from_bytes(raw_offset, "little") + 4)
        raw_machine = stream.read(2)
    if len(raw_machine) != 2:
        raise InspectionError(f"PE machine field missing: {path}")
    machine = int.from_bytes(raw_machine, "little")
    names = {0x8664: "x64", 0x014C: "x86", 0xAA64: "arm64"}
    return names.get(machine, f"machine-0x{machine:04x}")


def parse_objdump_architecture(output: str) -> str:
    lowered = output.casefold()
    if "x86-64" in lowered or "i386:x86-64" in lowered or "x86_64" in lowered:
        return "x86-64"
    if "aarch64" in lowered:
        return "aarch64"
    if "i386" in lowered or "i686" in lowered:
        return "x86"
    return "unknown"


def run_command(
    command: Sequence[str],
    *,
    env: dict[str, str] | None = None,
    cwd: Path | None = None,
) -> str:
    completed = subprocess.run(command, capture_output=True, text=True, check=False, env=env, cwd=cwd)
    output = (completed.stdout + completed.stderr).strip()
    if completed.returncode != 0:
        raise InspectionError(
            f"command failed with exit code {completed.returncode}: {' '.join(command)}\n{output[:1200]}"
        )
    return output


def tool_record(name: str, path: str, version_args: Sequence[str]) -> dict[str, str]:
    version = "unknown"
    try:
        version_output = run_command([path, *version_args])
        version = version_output.splitlines()[0][:240] if version_output else "unknown"
    except (OSError, InspectionError):
        # Version reporting is useful evidence, but a tool's normal inspection
        # invocation remains the authoritative failure boundary.
        pass
    return {"name": name, "path": path, "version": version}


def resolve_required_tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise InspectionError(f"required inspection tool is unavailable: {name}")
    return path


def resolve_dumpbin() -> str:
    direct = shutil.which("dumpbin.exe") or shutil.which("dumpbin")
    if direct:
        return direct

    vswhere_candidates = (
        Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio/Installer/vswhere.exe",
        Path(os.environ.get("ProgramFiles", "")) / "Microsoft Visual Studio/Installer/vswhere.exe",
    )
    for vswhere in vswhere_candidates:
        if not vswhere.is_file():
            continue
        try:
            output = run_command(
                [
                    str(vswhere),
                    "-latest",
                    "-products",
                    "*",
                    "-requires",
                    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-find",
                    r"VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe",
                ]
            )
        except InspectionError:
            continue
        for candidate in output.splitlines():
            path = Path(candidate.strip())
            if path.is_file():
                return str(path)

    roots = (
        Path(os.environ.get("ProgramFiles", "")) / "Microsoft Visual Studio",
        Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio",
    )
    for root in roots:
        if not root.is_dir():
            continue
        matches = sorted(root.glob("2022/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"))
        if matches:
            return str(matches[-1])
    raise InspectionError("required inspection tool is unavailable: dumpbin.exe")


def linux_system_dependency(name: str) -> bool:
    folded = name.casefold()
    prefixes = (
        "linux-vdso",
        "ld-linux",
        "libc.so",
        "libdl.so",
        "libgcc_s.so",
        "libm.so",
        "libpthread.so",
        "libresolv.so",
        "librt.so",
        "libstdc++.so",
        "libX11.so".casefold(),
        "libXau.so".casefold(),
        "libXdmcp.so".casefold(),
        "libXext.so".casefold(),
        "libXfixes.so".casefold(),
        "libXi.so".casefold(),
        "libXrender.so".casefold(),
        "libxcb".casefold(),
        "libGL.so".casefold(),
        "libEGL.so".casefold(),
        "libdrm.so".casefold(),
        "libasound.so".casefold(),
        "libfontconfig.so".casefold(),
        "libfreetype.so".casefold(),
        "libpulse.so".casefold(),
        "libspeechd.so".casefold(),
        "libcups.so".casefold(),
        "libz.so".casefold(),
    )
    return folded.startswith(prefixes)


def linux_system_path(path: str) -> bool:
    normalized = path.replace("\\", "/")
    return normalized.startswith(("/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/"))


def windows_system_dependency(name: str) -> bool:
    folded = name.casefold()
    if folded.startswith(("api-ms-win-", "ext-ms-win-")):
        return True
    system_names = {
        "advapi32.dll",
        "bcrypt.dll",
        "cfgmgr32.dll",
        "comdlg32.dll",
        "comctl32.dll",
        "crypt32.dll",
        "d2d1.dll",
        "d3d11.dll",
        "d3d12.dll",
        "dcomp.dll",
        "dnsapi.dll",
        "dwmapi.dll",
        "gdi32.dll",
        "gdi32full.dll",
        "imm32.dll",
        "iphlpapi.dll",
        "kernel32.dll",
        "kernelbase.dll",
        "msimg32.dll",
        "msvcrt.dll",
        "ntdll.dll",
        "ole32.dll",
        "oleaut32.dll",
        "opengl32.dll",
        "pathcch.dll",
        "powrprof.dll",
        "propsys.dll",
        "psapi.dll",
        "rpcrt4.dll",
        "secur32.dll",
        "setupapi.dll",
        "shell32.dll",
        "shlwapi.dll",
        "user32.dll",
        "uxtheme.dll",
        "version.dll",
        "winmm.dll",
        "winspool.drv",
        "ws2_32.dll",
        "windowscodecs.dll",
        "wtsapi32.dll",
    }
    if folded in system_names or folded == "ucrtbase.dll":
        return True
    # These are Microsoft runtime components supplied by the OS or the
    # supported VC++ redistributable, not unresolved application dependencies.
    return bool(re.fullmatch(r"(?:vcruntime|msvcp|concrt)\d+(?:_\d+)?(?:\.dll)", folded))


def package_files(root: Path) -> list[Path]:
    return sorted((path for path in root.rglob("*") if path.is_file()), key=lambda path: path.as_posix())


def plugin_candidate(path: Path) -> bool:
    parts = {part.casefold() for part in path.parts}
    return bool(
        parts
        & {
            "plugins",
            "pdfplugins",
            "loop",
            "platforms",
            "imageformats",
            "iconengines",
            "texttospeech",
            "qml",
        }
    )


def package_index(binaries: list[dict[str, Any]]) -> dict[str, str]:
    index: dict[str, str] = {}
    for binary in binaries:
        name = Path(str(binary["path"])).name.casefold()
        index.setdefault(name, str(binary["path"]))
    return index


def dependency_resolution(
    name: str,
    index: dict[str, str],
    platform: str,
) -> tuple[str, str | None]:
    folded = Path(name.replace("\\", "/")).name.casefold()
    if folded in index:
        return "package", index[folded]
    if platform == "linux" and linux_system_dependency(folded):
        return "system", None
    if platform == "windows" and windows_system_dependency(folded):
        return "system", None
    return "external", None


def package_closure(path: str, records: dict[str, dict[str, Any]]) -> list[str]:
    visited: set[str] = set()
    result: list[str] = []
    pending = list(records.get(path, {}).get("package_dependencies", []))
    while pending:
        candidate = pending.pop(0)
        if candidate in visited:
            continue
        visited.add(candidate)
        result.append(candidate)
        pending.extend(records.get(candidate, {}).get("package_dependencies", []))
    return result


def inspect_linux(root: Path, binaries: list[dict[str, Any]]) -> tuple[list[dict[str, str]], list[dict[str, Any]]]:
    readelf = resolve_required_tool("readelf")
    objdump = resolve_required_tool("objdump")
    ldd = resolve_required_tool("ldd")
    tools = [
        tool_record("readelf", readelf, ["--version"]),
        tool_record("objdump", objdump, ["--version"]),
        tool_record("ldd", ldd, ["--version"]),
    ]

    library_dirs = sorted({str(path.parent) for path in root.rglob("*") if path.is_file() and ".so" in path.name})
    library_dirs.extend([str(root), str(root / "usr/lib"), str(root / "lib")])
    runtime_env = os.environ.copy()
    runtime_env["LD_LIBRARY_PATH"] = ":".join(unique_preserving_order(library_dirs))
    for variable in (
        "QT_PLUGIN_PATH",
        "QML2_IMPORT_PATH",
        "QML_IMPORT_PATH",
        "QT_QPA_PLATFORM_PLUGIN_PATH",
        "QTDIR",
        "QT_ROOT_DIR",
        "Qt6_DIR",
        "LOOP_QT_ROOT",
    ):
        runtime_env.pop(variable, None)

    index = package_index(binaries)
    records: dict[str, dict[str, Any]] = {}
    for binary in binaries:
        path = root / str(binary["path"])
        dynamic = run_command([readelf, "--dynamic", str(path)])
        needed = parse_readelf_needed(dynamic)
        objdump_output = run_command([objdump, "-f", str(path)])
        ldd_output = run_command([ldd, str(path)], env=runtime_env)
        resolved, unresolved = parse_ldd(ldd_output)
        resolved_by_name = {
            str(item["name"]).casefold(): str(item["path"])
            for item in resolved
        }
        package_dependencies: list[str] = []
        dependency_rows: list[dict[str, Any]] = []
        external_system_dependencies: list[str] = []
        for name in needed:
            kind, target = dependency_resolution(name, index, "linux")
            resolved_target = resolved_by_name.get(name.casefold())
            if kind == "external" and resolved_target and linux_system_path(resolved_target):
                kind = "system"
                external_system_dependencies.append(name)
            if target:
                package_dependencies.append(target)
            dependency_rows.append({"name": name, "kind": kind, "target": target})
        record = {
            **binary,
            "architecture": parse_objdump_architecture(objdump_output),
            "direct_dependencies": needed,
            "package_dependencies": unique_preserving_order(package_dependencies),
            "resolved_dependencies": resolved,
            "unresolved_dependencies": unresolved,
            "dependency_rows": dependency_rows,
            "external_system_dependencies": unique_preserving_order(external_system_dependencies),
            "tool_output": {"readelf_dynamic": dynamic, "ldd": ldd_output},
        }
        records[str(binary["path"])] = record

    for record in records.values():
        record["package_dependency_closure"] = package_closure(str(record["path"]), records)
    return tools, list(records.values())


def inspect_windows(root: Path, binaries: list[dict[str, Any]]) -> tuple[list[dict[str, str]], list[dict[str, Any]]]:
    dumpbin = resolve_dumpbin()
    tools = [tool_record("dumpbin", dumpbin, ["/VERSION"])]
    index = package_index(binaries)
    records: dict[str, dict[str, Any]] = {}
    for binary in binaries:
        path = root / str(binary["path"])
        output = run_command([dumpbin, "/DEPENDENTS", str(path)])
        dependencies = parse_dumpbin_dependents(output)
        package_dependencies: list[str] = []
        dependency_rows: list[dict[str, Any]] = []
        external_system_dependencies: list[str] = []
        for name in dependencies:
            kind, target = dependency_resolution(name, index, "windows")
            if kind == "system":
                external_system_dependencies.append(name)
            if target:
                package_dependencies.append(target)
            dependency_rows.append({"name": name, "kind": kind, "target": target})
        records[str(binary["path"])] = {
            **binary,
            "architecture": pe_architecture(path),
            "direct_dependencies": dependencies,
            "package_dependencies": unique_preserving_order(package_dependencies),
            "dependency_rows": dependency_rows,
            "external_system_dependencies": unique_preserving_order(external_system_dependencies),
            "tool_output": {"dumpbin_dependents": output},
        }
    for record in records.values():
        record["package_dependency_closure"] = package_closure(str(record["path"]), records)
    return tools, list(records.values())


def extract_appimage(package: Path, work_dir: Path) -> Path:
    extraction = work_dir / "appimage"
    extraction.mkdir(parents=True, exist_ok=False)
    run_command(
        [str(package), "--appimage-extract"],
        env={**os.environ, "PATH": "/usr/bin:/bin"},
        cwd=extraction,
    )
    # Run extraction from the isolated directory so the AppImage helper cannot
    # leave its squashfs-root beside the repository or package artifact.
    extracted = extraction / "squashfs-root"
    if not extracted.is_dir():
        raise InspectionError("AppImage extraction did not create squashfs-root")
    return extracted


def extract_msi(package: Path, work_dir: Path) -> Path:
    extraction = work_dir / "msi-admin"
    extraction.mkdir(parents=True, exist_ok=False)
    log_path = work_dir / "msiexec-admin.log"
    run_command(
        [
            "msiexec.exe",
            "/a",
            str(package),
            "/qn",
            "/norestart",
            f"TARGETDIR={extraction}",
            "/l*v",
            str(log_path),
        ]
    )
    return extraction


def extract_package(platform: str, package: Path, work_dir: Path) -> Path:
    if platform == "linux":
        return extract_appimage(package, work_dir)
    return extract_msi(package, work_dir)


def build_evidence(
    platform: str,
    package: Path,
    root: Path,
    source_sha: str,
    tools: list[dict[str, str]],
    binaries: list[dict[str, Any]],
    expected_architecture: str | None = None,
) -> dict[str, Any]:
    package = package.resolve()
    root = root.resolve()
    files: list[dict[str, Any]] = []
    forbidden: list[dict[str, str]] = []
    for path in package_files(root):
        relative = relative_path(root, path)
        fmt = binary_format(path)
        file_row = {
            "path": relative,
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
            "format": fmt or "file",
        }
        files.append(file_row)
        if is_forbidden_widgets_surface_name(relative):
            forbidden.append(
                {"kind": "payload", "path": relative, "reason": "Widgets-bound Qt module filename"}
            )
        if fmt is None and looks_like_binary(path):
            forbidden.append({"kind": "uninspected-binary", "path": relative, "reason": "unknown binary format"})

    for row in binaries:
        architecture = str(row.get("architecture", "unknown"))
        if architecture in {"unknown", "machine-0x0000"}:
            forbidden.append({"kind": "uninspected-binary", "path": str(row["path"]), "reason": "unknown architecture"})
        elif expected_architecture and architecture != expected_architecture:
            forbidden.append(
                {
                    "kind": "architecture",
                    "path": str(row["path"]),
                    "architecture": architecture,
                    "expected": expected_architecture,
                    "reason": "binary architecture does not match package target",
                }
            )
        if is_forbidden_widgets_surface_name(str(row["path"])):
            forbidden.append(
                {
                    "kind": "payload",
                    "path": str(row["path"]),
                    "reason": "Widgets-bound Qt module binary filename",
                }
            )
        for dependency in row.get("direct_dependencies", []):
            if is_forbidden_widgets_surface_name(str(dependency)):
                forbidden.append(
                    {
                        "kind": "dependency",
                        "path": str(row["path"]),
                        "dependency": str(dependency),
                        "reason": "direct Widgets-bound Qt module dependency",
                    }
                )
        for dependency in row.get("resolved_dependencies", []):
            name = str(dependency.get("name", ""))
            resolved_path = str(dependency.get("path", ""))
            if is_forbidden_widgets_surface_name(name) or is_forbidden_widgets_surface_name(resolved_path):
                forbidden.append(
                    {
                        "kind": "runtime-dependency",
                        "path": str(row["path"]),
                        "dependency": name,
                        "reason": "resolved Widgets-bound Qt module dependency",
                    }
                )
        for unresolved in row.get("unresolved_dependencies", []):
            if is_forbidden_widgets_surface_name(str(unresolved)):
                forbidden.append(
                    {
                        "kind": "unresolved-dependency",
                        "path": str(row["path"]),
                        "dependency": str(unresolved),
                        "reason": "unresolved Widgets-bound Qt module dependency",
                    }
                )
            else:
                forbidden.append({"kind": "unresolved-dependency", "path": str(row["path"]), "dependency": str(unresolved), "reason": "non-system dependency was not resolved"})
        for dependency in row.get("dependency_rows", []):
            if dependency.get("kind") == "external":
                name = str(dependency.get("name", ""))
                if is_forbidden_widgets_surface_name(name):
                    forbidden.append(
                        {
                            "kind": "dependency",
                            "path": str(row["path"]),
                            "dependency": name,
                            "reason": "external Widgets-bound Qt module dependency",
                        }
                    )
                else:
                    forbidden.append(
                        {
                            "kind": "unresolved-dependency",
                            "path": str(row["path"]),
                            "dependency": name,
                            "reason": "non-system dependency is outside the package",
                        }
                    )
        for dependency in row.get("package_dependency_closure", []):
            if is_forbidden_widgets_surface_name(str(dependency)):
                forbidden.append(
                    {
                        "kind": "transitive-dependency",
                        "path": str(row["path"]),
                        "dependency": str(dependency),
                        "reason": "transitive Widgets-bound Qt module dependency",
                    }
                )

    # Keep evidence deterministic and avoid repeated findings from filename,
    # direct-import, and loader-resolution checks.
    unique_findings: list[dict[str, str]] = []
    seen_findings: set[str] = set()
    for finding in forbidden:
        identity = json.dumps(finding, sort_keys=True)
        if identity not in seen_findings:
            seen_findings.add(identity)
            unique_findings.append(finding)

    plugin_paths = [
        {
            "path": str(file_row["path"]),
            "format": str(file_row["format"]),
            "sha256": str(file_row["sha256"]),
            "inspected": any(str(row["path"]) == str(file_row["path"]) for row in binaries),
        }
        for file_row in files
        if plugin_candidate(Path(str(file_row["path"])))
    ]
    def finding_mentions_widgets(finding: dict[str, str]) -> bool:
        return any(is_widgets_name(str(finding.get(key, ""))) for key in ("path", "dependency"))

    def finding_mentions_widgets_surface(finding: dict[str, str]) -> bool:
        return any(
            is_forbidden_widgets_surface_name(str(finding.get(key, "")))
            for key in ("path", "dependency")
        )

    status = "passed" if not unique_findings else "failed"
    return {
        "schema_version": 1,
        "kind": "loop-package-boundary-evidence",
        "source_sha": source_sha.lower(),
        "platform": platform,
        "expected_architecture": expected_architecture or "unspecified",
        "package": {
            "name": package.name,
            "format": "AppImage" if platform == "linux" else "MSI",
            "sha256": sha256_file(package),
            "size": package.stat().st_size,
        },
        "payload": {
            "root": "extracted-payload",
            "file_count": len(files),
            "files": files,
        },
        "binaries": binaries,
        "runtime_plugin_candidates": plugin_paths,
        "inspection_tools": tools,
        "forbidden_findings": unique_findings,
        "checks": {
            "all_payload_files_hashed": True,
            "all_binary_files_inspected": not any(item["kind"] == "uninspected-binary" for item in unique_findings),
            "target_architecture_matches": not any(item["kind"] == "architecture" for item in unique_findings),
            "qt6widgets_absent": not any(finding_mentions_widgets(item) for item in unique_findings),
            "qt6widgets_surface_absent": not any(
                finding_mentions_widgets_surface(item) for item in unique_findings
            ),
            "unresolved_non_system_dependencies_absent": not any(item["kind"] == "unresolved-dependency" for item in unique_findings),
        },
        "status": status,
    }


def inspect(
    platform: str,
    package: Path,
    source_sha: str,
    root: Path | None,
    work_dir: Path,
    expected_architecture: str | None = None,
) -> dict[str, Any]:
    if not FULL_SHA.fullmatch(source_sha):
        raise InspectionError("--source-sha must be a full 40-character Git SHA")
    if expected_architecture is None:
        expected_architecture = "x86-64" if platform == "linux" else "x64"
    package = package.resolve()
    if not package.is_file():
        raise InspectionError(f"package does not exist: {package}")
    work_dir = work_dir.resolve()
    work_dir.mkdir(parents=True, exist_ok=True)
    if root is None:
        root = extract_package(platform, package, work_dir)
    else:
        root = root.resolve()
        if not root.is_dir():
            raise InspectionError(f"payload root does not exist: {root}")

    files = package_files(root)
    binaries: list[dict[str, Any]] = []
    for path in files:
        fmt = binary_format(path)
        if fmt:
            binaries.append(
                {
                    "path": relative_path(root, path),
                    "format": fmt,
                    "sha256": sha256_file(path),
                    "size": path.stat().st_size,
                }
            )
        elif looks_like_binary(path):
            # Keep the file in the evidence list; build_evidence turns it into
            # a hard failure instead of silently omitting it.
            binaries.append(
                {
                    "path": relative_path(root, path),
                    "format": "unknown",
                    "sha256": sha256_file(path),
                    "size": path.stat().st_size,
                    "architecture": "unknown",
                    "direct_dependencies": [],
                    "package_dependencies": [],
                    "dependency_rows": [],
                }
            )

    recognized = [row for row in binaries if row.get("format") in {"ELF", "PE"}]
    if platform == "linux":
        tools, inspected = inspect_linux(root, recognized)
    else:
        tools, inspected = inspect_windows(root, recognized)
    all_binary_rows = inspected + [row for row in binaries if row.get("format") == "unknown"]
    evidence = build_evidence(
        platform,
        package,
        root,
        source_sha,
        tools,
        all_binary_rows,
        expected_architecture,
    )
    evidence["payload"]["source_root"] = str(root)
    return evidence


def write_report(evidence: dict[str, Any], path: Path) -> None:
    findings = evidence["forbidden_findings"]
    lines = [
        f"status: {evidence['status']}",
        f"platform: {evidence['platform']}",
        f"source_sha: {evidence['source_sha']}",
        f"package: {evidence['package']['name']}",
        f"package_sha256: {evidence['package']['sha256']}",
        f"payload_files: {evidence['payload']['file_count']}",
        f"binaries: {len(evidence['binaries'])}",
        f"runtime_plugin_candidates: {len(evidence['runtime_plugin_candidates'])}",
        "findings:",
    ]
    lines.extend(f"- {json.dumps(finding, sort_keys=True)}" for finding in findings)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument(
        "--expected-architecture",
        choices=("x86-64", "x64", "arm64", "aarch64"),
        default=None,
        help="Expected architecture; defaults to x86-64 on Linux and x64 on Windows.",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, default=None)
    parser.add_argument("--work-dir", type=Path, default=None)
    parser.add_argument(
        "--payload-root",
        type=Path,
        default=None,
        help="Use an already extracted payload; intended for local fixtures and package re-checks.",
    )
    args = parser.parse_args(argv)
    work_dir = args.work_dir or Path(tempfile.mkdtemp(prefix="loop-package-boundary-"))
    try:
        evidence = inspect(
            args.platform,
            args.package,
            args.source_sha,
            args.payload_root,
            work_dir,
            args.expected_architecture,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
        report = args.report or args.output.with_suffix(".txt")
        write_report(evidence, report)
    except (OSError, InspectionError, ValueError) as exc:
        print(f"Package boundary inspection FAILED: {exc}", file=sys.stderr)
        return 1
    print(
        "Package boundary inspection "
        f"{evidence['status']}: platform={evidence['platform']} "
        f"source_sha={evidence['source_sha']} "
        f"package_sha256={evidence['package']['sha256']} "
        f"binaries={len(evidence['binaries'])}"
    )
    return 0 if evidence["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
