#!/usr/bin/env python3
"""Build and package the forked RadRay DXC as a relocatable CMake SDK archive.

Stages the distribution components (dxc, dxcompiler, dxc-headers) plus the
dxil validator binary into a prefix tree, zips the tree, and reports the
SHA-256 of the archive. Optionally writes the hash back into a RadRay
project_manifest.json Artifact entry so the RadRay fetch tooling can pin the
local package.

The archive layout mirrors the relocatable RadRayDXC CMake package:

  bin/dxc.exe
  bin/dxcompiler.dll
  bin/dxil.dll
  lib/dxcompiler.lib
  lib/dxil.lib
  include/dxc/dxcapi.h
  include/dxc/dxcapi_radrayext.h
  lib/cmake/RadRayDXC/RadRayDXCConfig.cmake
  lib/cmake/RadRayDXC/RadRayDXCConfigVersion.cmake

Usage:
  python utils/package_radray_sdk.py
      [--build <build_dir>] [--config <config>]
      [--version <version>] [--platform <platform>] [--arch <arch>]
      [--out <output_dir>] [--manifest <project_manifest.json>]
      [--name <artifact_name>] [--no-build]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DISTRIBUTION_COMPONENTS = ("dxc", "dxcompiler", "dxc-headers")
BUILD_TARGETS = ("dxc", "dxcompiler", "dxildll")
DEFAULT_VERSION = "1.9.2607.radray.4"
VERSION_RE = re.compile(r'set\(RADRAY_DXC_PACKAGE_VERSION\s+"([^"]+)"\)')


def fork_root() -> Path:
    return Path(__file__).resolve().parents[1]


def read_package_version(repo_root: Path) -> str:
    cmake_file = repo_root / "CMakeLists.txt"
    match = VERSION_RE.search(cmake_file.read_text(encoding="utf-8"))
    if not match:
        raise SystemExit("error: RADRAY_DXC_PACKAGE_VERSION not found in CMakeLists.txt")
    return match.group(1)


def archive_name(platform: str, arch: str) -> str:
    return f"dxc-{platform}-{arch}.zip"


def run(args: list[str], cwd: Path | None = None) -> None:
    print(f"$ {' '.join(args)}")
    result = subprocess.run(args, cwd=str(cwd) if cwd else None)
    if result.returncode != 0:
        raise SystemExit(f"error: command failed ({result.returncode}): {' '.join(args)}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def update_manifest_hash(
    manifest_path: Path,
    name: str,
    version: str,
    platform: str,
    arch: str,
    hash_value: str,
) -> None:
    manifest_path = manifest_path.resolve()
    if not manifest_path.exists():
        raise SystemExit(f"error: manifest not found: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    artifacts = manifest.get("Artifacts")
    if not isinstance(artifacts, list):
        raise SystemExit("error: manifest has no Artifacts array")
    for artifact in artifacts:
        if artifact.get("Name") != name:
            continue
        artifact["Version"] = version
        triplets = artifact.get("Triplets")
        if not isinstance(triplets, list):
            raise SystemExit(f"error: artifact {name} has no Triplets")
        for triplet in triplets:
            if triplet.get("Platform") == platform and triplet.get("Arch") == arch:
                triplet["Hash"] = hash_value
                content = json.dumps(manifest, indent=4) + "\n"
                manifest_path.write_text(content, encoding="utf-8", newline="\n")
                print(f"[package] manifest updated: {name} {platform}-{arch} hash={hash_value}")
                return
        raise SystemExit(f"error: no triplet {platform}-{arch} in artifact {name}")
    print(f"[package] warning: artifact {name} not found in manifest, hash not written")


def main() -> int:
    repo_root = fork_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=repo_root / "build_radray",
                        help="Fork CMake build directory (default build_radray).")
    parser.add_argument("--config", default="Release",
                        help="Build/install configuration (default Release).")
    parser.add_argument("--version", default=None,
                        help="Package version, defaults to RADRAY_DXC_PACKAGE_VERSION.")
    parser.add_argument("--platform", default="windows", help="Host platform tag.")
    parser.add_argument("--arch", default="x64", help="Host architecture tag.")
    parser.add_argument("--out", type=Path, default=repo_root / "package",
                        help="Output directory for the archive (default <fork>/package).")
    parser.add_argument("--manifest", type=Path, default=None,
                        help="RadRay project_manifest.json to update the artifact hash in.")
    parser.add_argument("--name", default="radray_dxc",
                        help="Artifact name in --manifest (default radray_dxc).")
    parser.add_argument("--no-build", action="store_true",
                        help="Skip configure/build steps and only stage existing outputs.")
    args = parser.parse_args()

    build_dir = args.build.resolve()
    if not (build_dir / "CMakeCache.txt").exists():
        raise SystemExit(f"error: build directory is not configured: {build_dir}")

    version = args.version or read_package_version(repo_root)
    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    archive_path = out_dir / archive_name(args.platform, args.arch)

    if not args.no_build:
        run(["cmake", "-B", str(build_dir)])
        run(["cmake", "--build", str(build_dir), "--config", args.config,
             "--target", *BUILD_TARGETS, "--parallel"])

    with tempfile.TemporaryDirectory(prefix="radray-dxc-package-") as tmp:
        stage = Path(tmp) / "stage"
        stage.mkdir(parents=True, exist_ok=True)
        for component in DISTRIBUTION_COMPONENTS:
            run(["cmake", "--install", str(build_dir), "--config", args.config,
                 "--prefix", str(stage), "--component", component])

        config = args.config
        dxil_dll = next(
            (candidate for candidate in (
                build_dir / "bin" / config / "dxil.dll",
                build_dir / config / "bin" / "dxil.dll",
            ) if candidate.is_file()),
            None,
        )
        dxil_lib = next(
            (candidate for candidate in (
                build_dir / "lib" / config / "dxil.lib",
                build_dir / config / "lib" / "dxil.lib",
            ) if candidate.is_file()),
            None,
        )
        if dxil_dll is None:
            raise SystemExit(
                f"error: dxil.dll not found under {build_dir}/bin/<config> or {build_dir}/<config>/bin"
            )
        if dxil_lib is None:
            raise SystemExit(
                f"error: dxil.lib not found under {build_dir}/lib/<config> or {build_dir}/<config>/lib"
            )
        shutil.copy2(dxil_dll, stage / "bin" / "dxil.dll")
        shutil.copy2(dxil_lib, stage / "lib" / "dxil.lib")

        for expected in (
            stage / "bin" / "dxcompiler.dll",
            stage / "bin" / "dxc.exe",
            stage / "include" / "dxc" / "dxcapi.h",
            stage / "include" / "dxc" / "dxcapi_radrayext.h",
            stage / "lib" / "cmake" / "RadRayDXC" / "RadRayDXCConfig.cmake",
        ):
            if not expected.exists():
                raise SystemExit(f"error: staged package is missing {expected}")

        print(f"[package] staging {args.platform}-{args.arch} v{version}")
        temp_zip = Path(tmp) / archive_path.name
        shutil.make_archive(str(temp_zip.with_suffix("")), "zip", root_dir=stage)
        if archive_path.exists():
            archive_path.unlink()
        shutil.move(str(temp_zip), archive_path)

    hash_value = sha256(archive_path)
    print(f"[package] wrote {archive_path}")
    print(f"[package] sha256 {hash_value}")
    if args.manifest:
        update_manifest_hash(
            args.manifest, args.name, version, args.platform, args.arch, hash_value
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
