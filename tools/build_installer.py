"""Stage the minimal runtime and compile the CMCL installer with Inno Setup."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_RELEASE = (
    Path(os.environ["LOCALAPPDATA"]) / "Temp" / "cmcl-release-build" / "Release"
)
RUNTIME_DLLS = [
    "msvcp140.dll",
    "msvcp140_atomic_wait.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
]


def program_files() -> list[Path]:
    result = []
    for name in ("ProgramFiles", "ProgramFiles(x86)"):
        value = os.environ.get(name)
        if value:
            result.append(Path(value))
    return result


def find_crt_dir() -> Path | None:
    """定位任意 Visual Studio 版本自带的 VC++ 运行库目录。"""
    candidates = []
    for base in program_files():
        candidates.extend(
            base.glob(
                "Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/"
                "Microsoft.VC*.CRT"
            )
        )
    if not candidates:
        return None

    def version_key(path: Path) -> tuple[int, ...]:
        try:
            return tuple(int(part) for part in path.parts[-3].split("."))
        except ValueError:
            return (0,)

    return max(candidates, key=version_key)


def find_iscc() -> Path | None:
    for base in program_files():
        candidate = base / "Inno Setup 6" / "ISCC.exe"
        if candidate.is_file():
            return candidate
    return None


def project_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(\s*CMCL\s+VERSION\s+([0-9.]+)", text)
    return match.group(1) if match else "0.0.0"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--release-dir", type=Path, default=DEFAULT_RELEASE)
    args = parser.parse_args()

    release: Path = args.release_dir
    exe = release / "cmcl-ui.exe"
    if not exe.is_file():
        print(f"missing Release executable: {exe}")
        return 1

    iscc = find_iscc()
    if iscc is None:
        print("missing Inno Setup 6 compiler (ISCC.exe)")
        return 1

    crt_dir = find_crt_dir()
    if crt_dir is None:
        print("missing Visual C++ redistributable directory")
        return 1

    stage = Path(tempfile.mkdtemp(prefix="cmcl-installer-stage-"))
    shutil.copy2(exe, stage / "cmcl-ui.exe")
    for name in RUNTIME_DLLS:
        source = crt_dir / name
        if not source.is_file():
            print(f"missing VC runtime file: {source}")
            return 1
        shutil.copy2(source, stage / name)

    shutil.copytree(
        ROOT / "web",
        stage / "web",
        ignore=shutil.ignore_patterns("README.md"),
    )

    dist = ROOT / "dist"
    dist.mkdir(parents=True, exist_ok=True)
    setup = dist / f"CMCL-Setup-{project_version()}.exe"
    if setup.exists():
        # 保留上一次安装包，避免被新构建静默覆盖。
        index = 1
        while True:
            old = setup.with_name(f"{setup.stem}_older{index}{setup.suffix}")
            if not old.exists():
                break
            index += 1
        setup.rename(old)

    command = [
        str(iscc),
        f"/DStageDir={stage}",
        str(ROOT / "installer" / "CMCL.iss"),
    ]
    print("running:", subprocess.list2cmdline(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        return result.returncode

    payload = sum(
        p.stat().st_size for p in stage.rglob("*") if p.is_file()
    )
    print(f"stage:   {stage}")
    print(f"payload: {payload / 1024 / 1024:.2f} MB")
    print(f"setup:   {setup} ({setup.stat().st_size / 1024 / 1024:.2f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
