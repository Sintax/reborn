import pytest

from debugloop import serverkit, vm


def test_scripts_are_stamped_with_the_version(tmp_path):
    vm.install_scripts("abc1234", tmp_path)
    for name in (*serverkit.SCRIPTS, "CollectLogs.bat", "README-SERVER.txt"):
        assert "{{" not in (tmp_path / name).read_text(encoding="utf-8"), name
    assert '$KitVersion = "abc1234"' in (tmp_path / "StartServer.ps1").read_text(encoding="utf-8")


def test_matchmaking_update_keeps_the_owners_roster(tmp_path):
    published, win64 = tmp_path / "pub", tmp_path / "Win64"
    published.mkdir()
    (published / "gamecontroller.exe").write_bytes(b"new")
    (published / "appsettings.json").write_text('{"Roster": "repo"}')
    (win64 / "matchmaking").mkdir(parents=True)
    (win64 / "matchmaking" / "appsettings.json").write_text('{"Roster": "owner"}')
    (win64 / "matchmaking" / "stale.dll").write_bytes(b"old")
    vm.install_matchmaking(published, win64)
    mm = win64 / "matchmaking"
    assert (mm / "gamecontroller.exe").read_bytes() == b"new"
    assert (mm / "appsettings.json").read_text() == '{"Roster": "owner"}'
    assert not (mm / "stale.dll").exists()


def test_first_matchmaking_install_gets_the_repos_settings(tmp_path):
    published, win64 = tmp_path / "pub", tmp_path / "Win64"
    published.mkdir()
    win64.mkdir()
    (published / "appsettings.json").write_text('{"Roster": "repo"}')
    vm.install_matchmaking(published, win64)
    assert (win64 / "matchmaking" / "appsettings.json").read_text() == '{"Roster": "repo"}'


def test_download_names_a_missing_file(tmp_path, monkeypatch):
    def fake_run(cmd, **kw):
        out = tmp_path / "mod"
        out.mkdir(exist_ok=True)
        (out / "reborn.dll").write_bytes(b"m")
        return ""
    monkeypatch.setattr(vm, "run", fake_run)
    with pytest.raises(vm.VmError, match="has no reborn.pdb"):
        vm.download_mod("abc", tmp_path / "mod")


@pytest.mark.parametrize("status,remote,msg", [(" M x.cpp", "origin/main", "uncommitted"),
                                                ("", "", "not on GitHub")])
def test_release_refuses_code_the_vm_cannot_get(monkeypatch, status, remote, msg):
    answers = {"status": status, "branch": remote}
    monkeypatch.setattr(vm, "git", lambda *a, **kw: answers.get(a[0], "abc"))
    with pytest.raises(vm.VmError, match=msg):
        vm.check_pushed()


def test_install_mod_copies_all_three_files(tmp_path):
    src, win64 = tmp_path / "src", tmp_path / "Win64"
    src.mkdir()
    win64.mkdir()
    for f in vm.MOD_FILES:
        (src / f).write_bytes(f.encode())
    vm.install_mod(src, win64)
    assert all((win64 / f).read_bytes() == f.encode() for f in vm.MOD_FILES)
