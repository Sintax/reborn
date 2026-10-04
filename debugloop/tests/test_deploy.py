import pytest

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


def test_two_deploys_keep_both_backups(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    (win64 / "reborn.dll").write_bytes(b"original")
    (win64 / "Battleborn.exe").write_bytes(b"exe")
    dll = tmp_path / "reborn.dll"
    dll.write_bytes(b"first")
    r1 = deploy.deploy(dll, win64)
    dll.write_bytes(b"second")
    r2 = deploy.deploy(dll, win64)
    assert r1.backups[0] != r2.backups[0]
    assert r1.backups[0].read_bytes() == b"original"
    assert r2.backups[0].read_bytes() == b"first"
    assert (win64 / "reborn.dll").read_bytes() == b"second"


def test_backup_never_overwrites_existing(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    f = win64 / "reborn.dll"
    f.write_bytes(b"a")
    b1 = deploy._backup(f, win64)
    f.write_bytes(b"b")
    b2 = deploy._backup(f, win64)
    assert b1 != b2 and b1.read_bytes() == b"a" and b2.read_bytes() == b"b"


def test_serverborn_backed_up_before_refresh(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    (win64 / "Battleborn.exe").write_bytes(b"v2")
    (win64 / "Serverborn.exe").write_bytes(b"v1")
    deploy.ensure_serverborn(win64)
    backups = list((win64 / "rb_backups").glob("Serverborn.exe.*"))
    assert len(backups) == 1 and backups[0].read_bytes() == b"v1"


def test_pdb_backed_up_before_replace(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    (win64 / "Battleborn.exe").write_bytes(b"exe")
    (win64 / "reborn.pdb").write_bytes(b"oldpdb")
    dll = tmp_path / "reborn.dll"; dll.write_bytes(b"new")
    (tmp_path / "reborn.pdb").write_bytes(b"newpdb")
    deploy.deploy(dll, win64)
    assert (win64 / "reborn.pdb").read_bytes() == b"newpdb"
    backups = list((win64 / "rb_backups").glob("reborn.pdb.*"))
    assert len(backups) == 1 and backups[0].read_bytes() == b"oldpdb"


def test_missing_battleborn_exe_is_clear_error(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    with pytest.raises(deploy.DeployError) as e:
        deploy.ensure_serverborn(win64)
    assert str(win64) in str(e.value) and "BB_GAME_DIR" in str(e.value)


def test_deploy_into_wrong_folder_changes_nothing(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    dll = tmp_path / "reborn.dll"; dll.write_bytes(b"new")
    with pytest.raises(deploy.DeployError):
        deploy.deploy(dll, win64)
    assert not (win64 / "reborn.dll").exists()


def test_locked_dll_is_deploy_error(tmp_path, monkeypatch):
    win64 = tmp_path / "Win64"; win64.mkdir()
    (win64 / "Battleborn.exe").write_bytes(b"exe")
    dll = tmp_path / "reborn.dll"; dll.write_bytes(b"new")

    def locked(*a, **k):
        raise PermissionError(13, "The process cannot access the file", str(win64 / "reborn.dll"))
    monkeypatch.setattr(deploy.shutil, "copy2", locked)
    with pytest.raises(deploy.DeployError, match="cannot access"):
        deploy.deploy(dll, win64)
