import zipfile

import pytest

from debugloop import launch, package


def _files(tmp_path):
    mod, dxgi = tmp_path / "reborn.dll", tmp_path / "dxgi.dll"
    mod.write_bytes(b"m")
    dxgi.write_bytes(b"d")
    win64 = tmp_path / "Win64"
    win64.mkdir()
    return mod, dxgi, win64


def test_zip_has_mod_instructions_and_log_collector(tmp_path):
    mod, dxgi, win64 = _files(tmp_path)
    out = package.make_zip(tmp_path / "o.zip", "abc", False, win64, mod, dxgi)
    names = set(zipfile.ZipFile(out).namelist())
    assert names == {"dxgi.dll", "reborn.dll", "README-PLAYTEST.txt", "CollectLogs.bat"}
    assert "abc" in zipfile.ZipFile(out).read("README-PLAYTEST.txt").decode()


def test_with_loader_adds_loader_files_or_names_the_missing_one(tmp_path):
    mod, dxgi, win64 = _files(tmp_path)
    with pytest.raises(RuntimeError, match="not found"):
        package.make_zip(tmp_path / "o.zip", "abc", True, win64, mod, dxgi)
    for f in (*launch.LOADER_FILES, package.LOADER_INI):
        (win64 / f).write_bytes(b"x")
    out = package.make_zip(tmp_path / "o.zip", "abc", True, win64, mod, dxgi)
    assert package.LOADER_INI in zipfile.ZipFile(out).namelist()


def test_zip_takes_the_proxy_from_where_its_project_builds_it():
    assert package.DXGI_DLL.parents[2] == package.DXGI_PROJECT.parent


def test_log_collector_does_not_claim_a_zip_it_could_not_make():
    bat = package.COLLECT_LOGS_BAT
    assert bat.index('if not exist "%OUT%" goto nozip') < bat.index("echo Saved")
    assert "Zip this folder yourself" in bat
