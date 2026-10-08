"""The "bad internet" relay (debugloop/netem.py)."""
import random
import socket
import time

import pytest

from debugloop import netem, run, scenario
from debugloop.netem import Impairment, _Direction


def _dir(imp, kbps=0.0, seed=1):
    return _Direction(imp, kbps, random.Random(seed), t0=0.0)


# Deciding when packets leave

def test_clean_link_sends_each_packet_once_after_the_delay():
    d = _dir(Impairment(delay_ms=50))
    assert d.schedule(100, 10.0) == [pytest.approx(10.05)]
    assert d.stats.lost == 0 and d.stats.delivered == 1


def test_loss_rate_is_roughly_what_was_asked():
    d = _dir(Impairment(loss_pct=5))
    sent = sum(bool(d.schedule(100, i * 0.01)) for i in range(20000))
    assert 0.94 < sent / 20000 < 0.96
    assert d.stats.lost == 20000 - sent


def test_jitter_keeps_packets_in_order_unless_reordering_is_allowed():
    d = _dir(Impairment(delay_ms=50, jitter_ms=40))
    outs = [d.schedule(100, i * 0.001)[0] for i in range(2000)]
    assert outs == sorted(outs)
    d = _dir(Impairment(delay_ms=50, jitter_ms=40, reorder_pct=20))
    outs = [d.schedule(100, i * 0.001)[0] for i in range(2000)]
    assert outs != sorted(outs) and d.stats.reordered > 0


def test_bandwidth_cap_spaces_packets_and_drops_when_the_queue_is_full():
    d = _dir(Impairment(queue_ms=100), kbps=80)   # 80 kbit/s: a 1000-byte packet takes 100 ms
    first, second = d.schedule(1000, 0.0)[0], d.schedule(1000, 0.0)[0]
    assert second - first == pytest.approx(0.1)
    assert d.schedule(1000, 0.0) == []            # would wait 200 ms > queue_ms
    assert d.stats.queue_drops == 1


def test_blackouts_drop_everything_for_their_length_and_not_before_the_first_one():
    d = _dir(Impairment(blackout_every_s=60, blackout_s=2))
    assert d.schedule(100, 1.0)          # first minute is clean
    assert d.schedule(100, 60.5) == []
    assert d.schedule(100, 62.5)
    assert d.schedule(100, 121.0) == []
    assert d.stats.blackout_drops == 2


def test_duplicates_send_twice():
    d = _dir(Impairment(duplicate_pct=100))
    assert len(d.schedule(100, 0.0)) == 2 and d.stats.duplicated == 1


# Settings from a scenario

def test_presets_and_overrides():
    assert netem.impairment_from({"preset": "home"}) == netem.PRESETS["home"]
    imp = netem.impairment_from({"preset": "bad", "delay_ms": 10})
    assert imp.delay_ms == 10 and imp.loss_pct == netem.PRESETS["bad"].loss_pct
    with pytest.raises(netem.NetworkError, match="unknown network preset"):
        netem.impairment_from({"preset": "dialup"})
    with pytest.raises(netem.NetworkError, match="unknown"):
        netem.impairment_from({"lag": 5})
    with pytest.raises(netem.NetworkError, match="negative"):
        netem.impairment_from({"delay_ms": -1})


BASE = """name = "x"
step = 2
time_limit_s = 60
[[process]]
name = "server"
role = "server"
[[process]]
name = "c1"
role = "client"
"""


def test_scenario_network_table(tmp_path):
    assert scenario.parse(BASE, tmp_path / "x.toml").network is None
    s = scenario.parse(BASE + '[network]\npreset = "home"\n', tmp_path / "x.toml")
    assert s.network == netem.PRESETS["home"]
    with pytest.raises(scenario.ScenarioError, match="preset"):
        scenario.parse(BASE + '[network]\npreset = "nope"\n', tmp_path / "x.toml")


def test_internet_scenarios_are_found_by_name_but_stay_off_the_ladder():
    for p in ("home", "bad"):
        s = scenario.find_scenario(f"s2-algorithm-2clients-net-{p}")
        assert s.network == netem.PRESETS[p]
    assert not any(s.network for step in range(4) for smoke in (False, True)
                   for s in scenario.ladder(step, smoke))


def test_clients_join_the_port_they_are_given(tmp_path):
    spec = scenario.ProcessSpec("c1", "client", [])
    assert "-rbjoin=127.0.0.1:7790" in run._args(spec, 18081, tmp_path, 2, 7790)
    assert "-rbjoin=127.0.0.1:7777" in run._args(spec, 18081, tmp_path, 2)


# Real packets through a real relay

def _free_udp_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def test_relay_delays_both_ways_and_the_server_sees_its_own_address():
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(("127.0.0.1", 0))
    server.settimeout(3)
    rs = netem.RelaySet.start(["c1", "c2"], server.getsockname()[1], Impairment(delay_ms=40),
                              first_port=_free_udp_port())
    try:
        seen = {}
        for name in ("c1", "c2"):
            client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            client.settimeout(3)
            t = time.perf_counter()
            client.sendto(name.encode(), ("127.0.0.1", rs.port(name)))
            data, addr = server.recvfrom(100)
            seen[data.decode()] = addr[0]
            server.sendto(b"pong-" + data, addr)
            reply, raddr = client.recvfrom(100)
            rtt_ms = (time.perf_counter() - t) * 1000
            client.close()
            assert reply == b"pong-" + name.encode() and raddr == ("127.0.0.1", rs.port(name))
            assert 75 < rtt_ms < 200, rtt_ms
        assert seen == {"c1": "127.0.0.2", "c2": "127.0.0.3"}
        st = rs.stats()["c1"]
        assert st["client_to_server"]["delivered"] == 1 and st["server_to_client"]["delivered"] == 1
    finally:
        rs.stop()
        server.close()


def test_relay_survives_the_server_not_listening_yet():
    port = _free_udp_port()   # nothing listens here: Windows answers with "port unreachable"
    rs = netem.RelaySet.start(["c1"], port, Impairment(), first_port=_free_udp_port())
    try:
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for _ in range(3):
            client.sendto(b"hello", ("127.0.0.1", rs.port("c1")))
            time.sleep(0.05)
        client.close()
        assert rs.relays["c1"]._thread.is_alive()
    finally:
        rs.stop()
