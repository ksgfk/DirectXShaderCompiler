"""Compiler-side regression matrix for the RadRay shader layout contract.

Builds utils/radray_wire_probe.cpp against a freshly built dxcompiler and drives
it over utils/radray_probe_tests, asserting the schema 8 wire each fixture must
produce and the diagnostic each rejected fixture must raise. This is the
compiler-side vehicle: it needs no RadRay checkout, so the fork stays verifiable
on its own.

    python utils/radray_probe_matrix.py [--build-dir build_radray] [--no-build]
                                        [--case NAME] [--verbose]

Expected output is matched line by line after normalization, so payload sizes -
which move with any codegen change - never fail the matrix, while declaration
owners, placements, kinds, counts, stage masks, sampler states and diagnostics do.
"""

import argparse
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "utils", "radray_probe_tests")
PROBE_SOURCE = os.path.join(ROOT, "utils", "radray_wire_probe.cpp")

VCVARS = (
    r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build"
    r"\vcvars64.bat"
)

DXIL = 1
SPIRV = 2
BOTH = 3

# name -> (target mask, expected exit code, expected normalized output lines).
# A `!` prefix on a line means "must not appear".
CASES = {
    # Every placement the policy can produce, on both targets at once.
    "ok_policy.hlsl": (BOTH, 0, [
        "DXIL lane",
        "schema=8 headerSize=152 target=0 stageMask=0x3",
        "rootSignature=260 bytes",
        "Scene group=0 binding=1 kind=CBuffer count=1 stages=0x1 "
        "placement=RootDescriptor sampler=-1 flags=0x0 payload=SceneData",
        "Albedo group=0 binding=0 kind=Texture count=2 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=none",
        "Points group=0 binding=2 kind=StructuredBuffer count=1 stages=0x2 "
        "placement=RootDescriptor sampler=-1 flags=0x0 payload=none",
        "Output group=0 binding=0 kind=RWStructuredBuffer count=1 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=none",
        "LinearClamp group=0 binding=0 kind=Sampler count=1 stages=0x2 "
        "placement=StaticSampler sampler=-1 flags=0x0 payload=none",
        "Push space=0 register=0 offset=0 size=16 stages=0x3 flags=0x0 "
        "payload=PushData",
        "samplers (0):",
        "SPIRV lane",
        "schema=8 headerSize=152 target=1 stageMask=0x3",
        # Vulkan reads its immutable samplers from the records, so no carrier.
        "rootSignature=0 bytes",
        "Scene group=0 binding=0 kind=CBuffer count=1 stages=0x1 "
        "placement=RootDescriptor sampler=-1 flags=0x0 payload=SceneData",
        "Albedo group=0 binding=1 kind=Texture count=2 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=none",
        "Output group=0 binding=3 kind=RWStructuredBuffer count=1 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=none",
        "Points group=0 binding=4 kind=StructuredBuffer count=1 stages=0x2 "
        "placement=RootDescriptor sampler=-1 flags=0x0 payload=none",
        # A D3D static sampler becomes a table slot plus an immutable record.
        "LinearClamp group=0 binding=5 kind=Sampler count=1 stages=0x2 "
        "placement=Table sampler=0 flags=0x0 payload=none",
        "Push space=0 register=0 offset=0 size=16 stages=0x3 flags=0x0 "
        "payload=PushData",
        "samplers (1):",
        "mag=1 min=1 mip=1 addr=2/3/0 bias=0.0 aniso=0/1.0 cmp=0/3 "
        "lod=0.0..8.0 border=4 reduction=0 flags=0x0",
    ]),

    # No policy: implicit topology, every resource stays in a table.
    "ok_no_policy.hlsl": (BOTH, 0, [
        "rootSignature=0 bytes",
        "A group=0 binding=0 kind=CBuffer count=1 stages=0x2 placement=Table "
        "sampler=-1 flags=0x0 payload=SceneData",
        "Raw group=0 binding=1 kind=RawBuffer count=1 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=none",
        "Typed group=0 binding=0 kind=RWTypedBuffer count=1 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=none",
        "root constants (0):",
        "samplers (0):",
        "!placement=RootDescriptor",
        "!placement=StaticSampler",
    ]),

    # The D3D -> Vulkan sampler translation, including the reduction modes.
    "ok_sampler_states.hlsl": (BOTH, 0, [
        "Shadow group=0 binding=0 kind=Sampler count=1 stages=0x2 "
        "placement=StaticSampler sampler=-1 flags=0x0 payload=none",
        "Shadow group=0 binding=1 kind=Sampler count=1 stages=0x2 "
        "placement=Table sampler=0 flags=0x0 payload=none",
        "MinPoint group=0 binding=1 kind=Sampler count=1 stages=0x2 "
        "placement=StaticSampler sampler=-1 flags=0x0 payload=none",
        "MinPoint group=0 binding=2 kind=Sampler count=1 stages=0x2 "
        "placement=Table sampler=1 flags=0x0 payload=none",
        "samplers (2):",
        # COMPARISON_ANISOTROPIC: linear everywhere, anisotropy on, compare on,
        # GREATER -> 4, MIRROR/MIRROR_ONCE/BORDER -> 1/4/3, unset maxLOD -> none.
        "mag=1 min=1 mip=1 addr=1/4/3 bias=1.5 aniso=1/8.0 cmp=1/4 "
        "lod=0.0..1000.0 border=2 reduction=0 flags=0x0",
        # MINIMUM_MIN_MAG_MIP_POINT: nearest everywhere, reduction = min.
        "mag=0 min=0 mip=0 addr=0/0/0 bias=0.0 aniso=0/1.0 cmp=0/3 "
        "lod=0.0..1000.0 border=4 reduction=1 flags=0x0",
    ]),

    # Compute collapses to one stage bit on both lanes.
    "ok_compute.hlsl": (BOTH, 0, [
        "schema=8 headerSize=152 target=0 stageMask=0x4",
        "schema=8 headerSize=152 target=1 stageMask=0x4",
        "Scene group=0 binding=1 kind=CBuffer count=1 stages=0x4 "
        "placement=RootDescriptor sampler=-1 flags=0x0 payload=SceneData",
        "Push space=0 register=0 offset=0 size=4 stages=0x4 flags=0x0 "
        "payload=PushData",
    ]),

    # Schema 8 publishes scalar kind, matrix shape, and non-struct array elements.
    "ok_type_payload.hlsl": (BOTH, 0, [
        "schema=8 headerSize=152 target=0 stageMask=0x4",
        "schema=8 headerSize=152 target=1 stageMask=0x4",
        "Payload group=0 binding=0 kind=CBuffer count=1 stages=0x4 "
        "placement=Table sampler=-1 flags=0x0 payload=PayloadData",
        "Transform         parent=0 kind=Matrix count=1 offset=0 size=64 "
        "stride=64 flags=0x0 scalar=float rows=4 cols=4 nested=none",
        "ShadowSphere      parent=0 kind=Array  count=4 offset=64 size=64 "
        "stride=16 flags=0x0 scalar=float rows=1 cols=4 nested=none",
        "Count             parent=0 kind=Scalar count=1 offset=128 size=4 "
        "stride=4 flags=0x0 scalar=uint rows=1 cols=1 nested=none",
        "SignedCount       parent=0 kind=Scalar count=1 offset=132 size=4 "
        "stride=4 flags=0x0 scalar=sint rows=1 cols=1 nested=none",
        "Scale             parent=0 kind=Scalar count=1 offset=136 size=4 "
        "stride=4 flags=0x0 scalar=float rows=1 cols=1 nested=none",
    ]),

    # Two declarations share one canonical root block in each target lane.
    "ok_shared_root.hlsl": (BOTH, 0, [
        "schema=8 headerSize=152 target=0 stageMask=0x3",
        "First group=0 binding=0 kind=CBuffer count=1 stages=0x1 "
        "placement=Table sampler=-1 flags=0x0 payload=SharedRoot",
        "Second group=0 binding=1 kind=CBuffer count=1 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=SharedRoot",
        "schema=8 headerSize=152 target=1 stageMask=0x3",
        "First group=0 binding=4 kind=CBuffer count=1 stages=0x1 "
        "placement=Table sampler=-1 flags=0x0 payload=SharedRoot",
        "Second group=0 binding=5 kind=CBuffer count=1 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=SharedRoot",
        "metadata deterministic=yes",
    ]),

    # A directly bound root remains the nested member target of another root.
    "ok_nested_roots.hlsl": (BOTH, 0, [
        "schema=8 headerSize=152 target=0 stageMask=0x3",
        "Inner group=0 binding=0 kind=CBuffer count=1 stages=0x1 "
        "placement=Table sampler=-1 flags=0x0 payload=InnerRoot",
        "Outer group=0 binding=1 kind=CBuffer count=1 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=OuterRoot",
        "schema=8 headerSize=152 target=1 stageMask=0x3",
        "Inner group=0 binding=6 kind=CBuffer count=1 stages=0x1 "
        "placement=Table sampler=-1 flags=0x0 payload=InnerRoot",
        "Outer group=0 binding=7 kind=CBuffer count=1 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=OuterRoot",
        "metadata deterministic=yes",
    ]),

    # A policy that cannot be parsed at all.
    "neg_bad_policy.hlsl": (BOTH, 1, [
        "[2117] [RootSignature] could not be compiled:",
    ]),

    # A declaration type the contract does not model, kept live.
    "neg_unsupported_type.hlsl": (BOTH, 1, [
        "[2118] resource 'Tb' has a declaration type outside the RadRay shader "
        "contract",
    ]),

    # A push block with no register cannot be matched against the policy. The
    # DXIL lane rejects it earlier with 2111, so this needs a Vulkan-only build.
    "neg_push_no_register.hlsl": (SPIRV, 1, [
        "[2120] push constant block 'Push' needs a register() annotation to be "
        "placed on both targets",
    ]),
    "neg_push_no_register.hlsl@dxil": (DXIL, 1, [
        "[2111] DXIL resource 'Push' is missing an explicit register() binding",
    ]),

    # D3D needs every live resource placed; Vulkan treats a gap as target-only.
    "neg_uncovered.hlsl": (DXIL, 1, [
        "[2121] resource 'Extra' is not covered by the [RootSignature] policy",
        "[2121] resource 'S' is not covered by the [RootSignature] policy",
    ]),
    "neg_uncovered.hlsl@spirv": (SPIRV, 0, [
        "A group=0 binding=0 kind=CBuffer count=1 stages=0x2 "
        "placement=RootDescriptor sampler=-1 flags=0x0 payload=SceneData",
        "Extra group=0 binding=1 kind=Texture count=1 stages=0x2 "
        "placement=Table sampler=-1 flags=0x0 payload=none",
        "S group=0 binding=2 kind=Sampler count=1 stages=0x2 placement=Table "
        "sampler=-1 flags=0x0 payload=none",
    ]),

    # Only a single buffer can be reached through a root descriptor.
    "neg_root_texture.hlsl": (BOTH, 1, [
        "[2122] resource 'Albedo' cannot be a root descriptor: only a single "
        "buffer or constant buffer can be bound by address",
    ]),
    "neg_root_array.hlsl": (BOTH, 1, [
        "[2122] resource 'Points' cannot be a root descriptor: only a single "
        "buffer or constant buffer can be bound by address",
    ]),

    # The policy hides the resource from a stage that uses it.
    "neg_stage_gap.hlsl": (BOTH, 1, [
        "[2123] resource 'A' is used by a stage the [RootSignature] policy does "
        "not make it visible to",
    ]),

    # Root constants must be authored so both targets lower them the same way.
    "neg_push_missing_attr.hlsl": (BOTH, 1, [
        "[2124] 'Push' is root constants in the policy, so it needs "
        "[[vk::push_constant]] to lower the same way on both targets",
    ]),

    # The policy is a translation unit fact, so stages cannot disagree on it.
    "neg_two_policies.hlsl": (BOTH, 1, [
        "[2105] translation unit declares more than one distinct "
        "[RootSignature] policy",
    ]),
}


def normalize(text):
    """Drop the parts of the probe dump that move with unrelated codegen."""
    lines = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        line = re.sub(r"metadata \d+ bytes, bytecode \d+ bytes", "", line)
        line = re.sub(r"\btotal=\d+\s*", "", line)
        line = re.sub(r"\s+", " ", line).strip(" =")
        lines.append(line)
    return lines


def build_probe(build_dir, verbose):
    library = os.path.join(build_dir, "Release", "lib", "dxcompiler.lib")
    if not os.path.exists(library):
        sys.stderr.write("missing %s, build the dxcompiler target first\n" % library)
        return None
    output = os.path.join(build_dir, "Release", "bin", "radray_wire_probe.exe")
    script = os.path.join(build_dir, "radray_probe_build.bat")
    with open(script, "w") as handle:
        handle.write(
            '@echo off\r\n'
            'call "%s" >nul\r\n'
            'cd /d "%s"\r\n'
            'cl /std:c++17 /EHsc /nologo /MD /Iinclude "%s" /Fo:"%s" /Fe:"%s" '
            '/link "%s"\r\n'
            % (VCVARS, ROOT, PROBE_SOURCE,
               os.path.join(build_dir, "probe."), output, library))
    result = subprocess.run(["cmd", "/c", script], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    # The toolchain localizes its diagnostics, so decode defensively.
    log = result.stdout.decode("utf-8", errors="replace")
    if result.returncode != 0 or not os.path.exists(output):
        sys.stderr.write(log)
        return None
    if verbose:
        sys.stdout.write(log)
    return output


def check(name, probe, verbose):
    mask, expected_code, expectations = CASES[name]
    source = os.path.join(TESTS, name.split("@", 1)[0])
    if not os.path.exists(source):
        return ["missing fixture %s" % source]
    result = subprocess.run([probe, source, "60", str(mask)],
                            cwd=os.path.dirname(probe),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    # A stale dxcompiler emits a different envelope layout, so the probe decodes
    # garbage rather than text; never let that surface as a decode crash.
    output = result.stdout.decode("utf-8", errors="replace")
    if verbose:
        sys.stdout.write(output)
    schema = re.search(r"schema=(\d+)", output)
    if schema is not None and int(schema.group(1)) != 8:
        return ["probe read schema %s, expected 8: the dxcompiler in this build "
                "tree predates the contract, rebuild the dxcompiler target"
                % schema.group(1)]
    result.stdout = output
    failures = []
    if result.returncode != expected_code:
        failures.append("exit code %d, expected %d" % (result.returncode, expected_code))
    lines = normalize(result.stdout)
    for expectation in expectations:
        if expectation.startswith("!"):
            needle = normalize(expectation[1:])[0]
            if any(needle in line for line in lines):
                failures.append("unexpected: %s" % needle)
            continue
        needle = normalize(expectation)[0]
        if not any(needle in line for line in lines):
            failures.append("missing: %s" % needle)
    if failures and not verbose:
        failures.append("--- probe output ---\n%s" % output.strip())
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build_radray")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--case", action="append")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    build_dir = args.build_dir
    if not os.path.isabs(build_dir):
        build_dir = os.path.join(ROOT, build_dir)
    probe = os.path.join(build_dir, "Release", "bin", "radray_wire_probe.exe")
    if not args.no_build:
        probe = build_probe(build_dir, args.verbose)
        if probe is None:
            return 2
    elif not os.path.exists(probe):
        sys.stderr.write("missing %s, drop --no-build\n" % probe)
        return 2

    untested = sorted(
        os.path.basename(path) for path in glob.glob(os.path.join(TESTS, "*.hlsl"))
        if os.path.basename(path) not in
        {name.split("@", 1)[0] for name in CASES})
    if untested:
        sys.stderr.write("fixtures with no expectations: %s\n" % ", ".join(untested))
        return 2

    names = args.case if args.case else sorted(CASES)
    failed = 0
    for name in names:
        if name not in CASES:
            sys.stderr.write("unknown case %s\n" % name)
            return 2
        failures = check(name, probe, args.verbose)
        if failures:
            failed += 1
            print("FAIL %s" % name)
            for failure in failures:
                print("     %s" % failure)
        else:
            print("ok   %s" % name)
    print("%d/%d cases passed" % (len(names) - failed, len(names)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
