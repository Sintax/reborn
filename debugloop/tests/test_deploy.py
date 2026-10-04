from debugloop import deploy


def test_deploy_backs_up_then_copies(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    (win64 / "reborn.dll").write_bytes(b"old")
    (win64 / "Battleborn.exe").write_bytes(b"exe")
    dll = tmp_path / "reborn.dll"; dll.write_bytes(b"new")
    r = deploy.deploy(dll, win64)
    assert (win64 / "reborn.dll").read_bytes() == b"new"
    assert r.changed == ["reborn.dll"]
    assert r.backups and r.backups[0].read_bytes() == b"old"
    assert (win64 / "Serverborn.exe").read_bytes() == b"exe"


def test_deploy_same_file_is_noop(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    (win64 / "reborn.dll").write_bytes(b"same")
    (win64 / "Battleborn.exe").write_bytes(b"exe")
    dll = tmp_path / "reborn.dll"; dll.write_bytes(b"same")
    r = deploy.deploy(dll, win64)
    assert r.changed == [] and r.backups == []


def test_serverborn_refreshed_when_exe_changes(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    (win64 / "Battleborn.exe").write_bytes(b"v2")
    (win64 / "Serverborn.exe").write_bytes(b"v1")
    deploy.ensure_serverborn(win64)
    assert (win64 / "Serverborn.exe").read_bytes() == b"v2"
