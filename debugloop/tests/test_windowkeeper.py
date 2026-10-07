from debugloop import windowkeeper


def test_keeper_arranges_until_the_session_ends(tmp_path):
    sessions = [{"pids": {"server": {"pid": 11}, "c1": {"pid": 12}}}] * 2 + [None]
    calls = []
    windowkeeper.keep(tmp_path, arrange=calls.append, alive=lambda _: sessions.pop(0),
                      sleep=lambda _: None)
    assert calls == [{"server": 11, "c1": 12}] * 2


def test_keeper_survives_arrange_errors(tmp_path):
    sessions = [{"pids": {"c1": {"pid": 1}}}, None]

    def boom(_):
        raise RuntimeError("no window")
    windowkeeper.keep(tmp_path, arrange=boom, alive=lambda _: sessions.pop(0), sleep=lambda _: None)
