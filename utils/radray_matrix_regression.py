"""Exercise source array identity and matrix layout through the public DXC extension."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve()
    checked = 0
    with tempfile.TemporaryDirectory(prefix="radray-matrices-") as directory:
        source = Path(directory) / "matrix.hlsl"
        for rows in range(2, 5):
            for columns in range(2, 5):
                for row_major in (False, True):
                    major, minor = (rows, columns) if row_major else (columns, rows)
                    element_size = (major - 1) * 16 + minor * 4
                    for count in (0, 1, 2):
                        suffix = f"[{count}]" if count else ""
                        access = "[i]" if count else ""
                        orientation = "row_major" if row_major else "column_major"
                        source.write_text(f'''
struct Payload {{ {orientation} float{rows}x{columns} M{suffix}; }};
[[vk::binding(0, 0)]] ConstantBuffer<Payload> Constants : register(b0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> Output : register(u0);
[shader("compute")][numthreads(1,1,1)]
void CSMain() {{
  [unroll] for (uint i = 0; i < {max(count, 1)}; ++i)
    [unroll] for (uint r = 0; r < {rows}; ++r)
      [unroll] for (uint c = 0; c < {columns}; ++c)
        Output[i * 16 + r * 4 + c] = Constants.M{access}[r][c];
}}
''', encoding="utf-8")
                        stride = major * 16 if count else 0
                        span = (max(count, 1) - 1) * stride + element_size
                        kind = "Array" if count else "Matrix"
                        pattern = (rf"\bM\s+parent=\d+ kind={kind}\s+count={max(count, 1)} "
                                   rf"offset=0 size={span} stride={stride} flags=0x{int(row_major):x} "
                                   rf"scalar=float rows={rows} cols={columns} nested=none "
                                   rf"element=Matrix elementSize={element_size} matrixStride=16")
                        for target in (1, 2, 3):
                            result = subprocess.run([str(probe), str(source), "60", str(target)],
                                                    text=True, capture_output=True)
                            expected_lanes = 2 if target == 3 else 1
                            if result.returncode or len(re.findall(pattern, result.stdout)) != expected_lanes:
                                print(f"FAIL {orientation} float{rows}x{columns}{suffix}, target={target}")
                                print(result.stdout, result.stderr)
                                return 1
                            checked += 1
        print(f"PASS: {checked} matrix source/target combinations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
