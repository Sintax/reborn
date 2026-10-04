import pytest

from debugloop import launch


def _game(tmp_path):
    w = tmp_path / "Win64"
    w.mkdir()
    for f in launch.LOADER_FILES:
        (w / f).write_bytes(b"x")
    return w


def test_identity_copies_loader(tmp_path):
    w = _game(tmp_path)
    d = launch.ensure_identity("solo", "Battleborn.exe", ["-a"], w)
    assert (d / "steamclient_loader_x64.exe").read_bytes() == b"x"


def test_missing_loader_names_the_file_and_antivirus(tmp_path):
    w = _game(tmp_path)
    (w / "steamclient_loader_x64.exe").unlink()
    with pytest.raises(RuntimeError, match=r"steamclient_loader_x64\.exe.*(antivirus|Defender)"):
        launch.ensure_identity("solo", "Battleborn.exe", ["-a"], w)


def test_unreadable_loader_names_the_file_and_antivirus(tmp_path, monkeypatch):
    w = _game(tmp_path)
    real = launch._sha

    def blocked(p):
        if p.name == "steamclient_loader_x64.exe" and p.parent == w:
            raise OSError(22, "Invalid argument", str(p))
        return real(p)

    monkeypatch.setattr(launch, "_sha", blocked)
    with pytest.raises(RuntimeError, match=r"steamclient_loader_x64\.exe.*(antivirus|Defender)"):
        launch.ensure_identity("solo", "Battleborn.exe", ["-a"], w)
