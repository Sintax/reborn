from debugloop import host


class FakeHandle:
    def __init__(self, pid):
        self.pid = pid


class FakeLauncher:
    def __init__(self):
        self.started = []

    def start(self, name, role, args):
        self.started.append((name, role, args))
        return FakeHandle(len(self.started))


def test_starts_server_then_host_game():
    f = FakeLauncher()
    assert host.main(["--map", "Portal_P", "--players", "3"], launcher=f) == 0
    (sname, srole, sargs), (cname, crole, cargs) = f.started
    assert (srole, crole) == ("server", "client")
    assert "-rbservermap=Portal_P" in sargs and "-rbplayers=3" in sargs and "-nullrhi" in sargs
    assert f"-rbinstance={cname}" in cargs and sname != cname
    assert not any(a.startswith(("-rbautopilot", "-rbcombat", "-rbdebugport")) for a in sargs + cargs)


def test_no_client_starts_server_only():
    f = FakeLauncher()
    host.main(["--no-client"], launcher=f)
    assert [r for _, r, _ in f.started] == ["server"]
