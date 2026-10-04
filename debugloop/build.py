import subprocess
from dataclasses import dataclass
from pathlib import Path

from . import config

VSWHERE = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")


@dataclass
class BuildResult:
    ok: bool
    output: str
    artifact: Path | None


def _vs_root() -> Path:
    out = subprocess.run(
        [str(VSWHERE), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        capture_output=True, text=True, check=True).stdout.strip()
    if not out:
        raise RuntimeError("Visual Studio with C++ tools not found")
    return Path(out.splitlines()[0])


def find_msbuild() -> Path:
    p = _vs_root() / "MSBuild" / "Current" / "Bin" / "MSBuild.exe"
    if not p.exists():
        raise RuntimeError(f"MSBuild not found at {p}")
    return p


def find_vcvars() -> Path:
    p = _vs_root() / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not p.exists():
        raise RuntimeError(f"vcvars64.bat not found at {p}")
    return p


def msbuild_command(configuration: str = "Release") -> list[str]:
    return [str(find_msbuild()), str(config.MOD_PROJECT),
            f"/p:Configuration={configuration}", "/p:Platform=x64",
            "/m", "/nologo", "/v:minimal"]


def build_mod() -> BuildResult:
    p = subprocess.run(msbuild_command(), capture_output=True, text=True)
    out = p.stdout + p.stderr
    ok = p.returncode == 0 and config.MOD_DLL.exists()
    return BuildResult(ok, out, config.MOD_DLL if ok else None)


def build_native(name: str, sources: list[Path], out_dir: Path,
                 extra_flags: list[str] = ()) -> BuildResult:
    out_dir.mkdir(parents=True, exist_ok=True)
    obj = out_dir / f"{name}.obj.d"
    obj.mkdir(exist_ok=True)
    exe = out_dir / f"{name}.exe"
    srcs = " ".join(f'"{s.resolve()}"' for s in sources)
    flags = " ".join(extra_flags)
    bat = out_dir / f"build_{name}.bat"
    bat.write_text(
        "@echo off\r\n"
        f'call "{find_vcvars()}" >nul\r\n'
        f'cd /d "{obj}"\r\n'
        f'cl /nologo /std:c++20 /EHsc /O2 /Zi /MT {flags} {srcs} '
        f'/Fe:"{exe}" /link /DEBUG\r\n')
    p = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True)
    out = p.stdout + p.stderr
    ok = p.returncode == 0 and exe.exists()
    return BuildResult(ok, out, exe if ok else None)
