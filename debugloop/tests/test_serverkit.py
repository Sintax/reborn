import zipfile

import pytest

from debugloop import launch, serverkit


def _files(tmp_path, loader=True):
    mod, dxgi = tmp_path / "reborn.dll", tmp_path / "dxgi.dll"
    mod.write_bytes(b"m")
    dxgi.write_bytes(b"d")
    win64 = tmp_path / "Win64"
    win64.mkdir()
    if loader:
        for f in launch.LOADER_FILES:
            (win64 / f).write_bytes(b"x")
    return mod, dxgi, win64


def test_zip_has_mod_loader_scripts_and_instructions(tmp_path):
    mod, dxgi, win64 = _files(tmp_path)
    out = serverkit.make_zip(tmp_path / "o.zip", "abc", win64, mod, dxgi)
    names = set(zipfile.ZipFile(out).namelist())
    assert names == {"dxgi.dll", "reborn.dll", *launch.LOADER_FILES, *serverkit.SCRIPTS,
                     "CollectLogs.bat", "README-SERVER.txt"}


def test_pdb_goes_in_when_built(tmp_path):
    mod, dxgi, win64 = _files(tmp_path)
    mod.with_suffix(".pdb").write_bytes(b"p")
    out = serverkit.make_zip(tmp_path / "o.zip", "abc", win64, mod, dxgi)
    assert "reborn.pdb" in zipfile.ZipFile(out).namelist()


def test_missing_loader_file_is_named(tmp_path):
    mod, dxgi, win64 = _files(tmp_path, loader=False)
    with pytest.raises(RuntimeError, match="steamclient_loader_x64.exe not found"):
        serverkit.make_zip(tmp_path / "o.zip", "abc", win64, mod, dxgi)


def test_scripts_are_stamped_and_use_windows_line_endings(tmp_path):
    mod, dxgi, win64 = _files(tmp_path)
    z = zipfile.ZipFile(serverkit.make_zip(tmp_path / "o.zip", "abc", win64, mod, dxgi))
    ps1 = z.read("StartServer.ps1").decode()
    assert '$KitVersion = "abc"' in ps1 and f'$SteamId = "{serverkit.server_steamid()}"' in ps1
    for name in (*serverkit.SCRIPTS, "CollectLogs.bat", "README-SERVER.txt"):
        text = z.read(name).decode()
        assert "{{" not in text, name
        assert "\n" not in text.replace("\r\n", ""), name


def test_server_starts_with_the_same_flags_as_the_host_launcher():
    ps1 = (serverkit.FILES / "StartServer.ps1").read_text(encoding="utf-8")
    for flag in launch.base_args("server"):
        assert f"{flag} " in ps1, flag
    assert f"-rbinstance=$Name" in ps1 and f'$Name = "{serverkit.SERVER_NAME}"' in ps1

