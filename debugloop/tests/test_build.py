from pathlib import Path
from debugloop import build


def test_msbuild_command_targets_release_x64():
    cmd = build.msbuild_command("Release")
    assert cmd[0].lower().endswith("msbuild.exe")
    assert "/p:Configuration=Release" in cmd
    assert "/p:Platform=x64" in cmd
    assert cmd[1].endswith("reborn.vcxproj")


def test_find_vcvars_exists():
    assert build.find_vcvars().name == "vcvars64.bat"


def test_build_native_compiles_hello(tmp_path: Path):
    src = tmp_path / "hello.cpp"
    src.write_text('#include <cstdio>\nint main(){std::puts("hi");return 0;}\n')
    res = build.build_native("hello", [src], tmp_path)
    assert res.ok, res.output
    assert res.artifact == tmp_path / "hello.exe" and res.artifact.exists()


def test_build_native_reports_compile_error(tmp_path: Path):
    src = tmp_path / "bad.cpp"
    src.write_text("int main(){ return nope; }\n")
    res = build.build_native("bad", [src], tmp_path)
    assert not res.ok
    assert "nope" in res.output
