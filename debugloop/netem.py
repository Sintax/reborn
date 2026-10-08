"""A "bad internet" relay for the game's UDP traffic, all on this PC (no drivers, no admin).

Each client joins the relay's port instead of the server's. The relay forwards every packet to the
real server from its own address (127.0.0.2, 127.0.0.3, ...), so the server sees separate players,
and on the way it adds what a real internet path does: delay, jitter, loss, duplicates, the odd
packet out of order, a bandwidth cap and short dropouts.

    python -m debugloop.run s2-algorithm-2clients-net-home   # a scenario with a [network] table

Scenario TOML:
    [network]
    preset = "home"          # or "bad"; any field below overrides the preset
    delay_ms = 45            # one way, each direction
"""
from __future__ import annotations

import ctypes
import heapq
import json
import random
import select
import socket
import threading
import time
from dataclasses import asdict, dataclass, field, fields, replace
from pathlib import Path

FIRST_RELAY_PORT = 7790


@dataclass(frozen=True)
class Impairment:
    delay_ms: float = 0.0          # one way
    jitter_ms: float = 0.0         # +/- around delay_ms, uniform
    loss_pct: float = 0.0
    duplicate_pct: float = 0.0
    reorder_pct: float = 0.0       # packets allowed to overtake the one before
    up_kbps: float = 0.0           # client -> server cap; 0 = no cap
    down_kbps: float = 0.0         # server -> client cap
    queue_ms: float = 500.0        # a capped link drops packets that would wait longer than this
    blackout_every_s: float = 0.0  # a dropout of blackout_s every this many seconds; 0 = never
    blackout_s: float = 0.0


PRESETS = {
    # A decent home connection to a friend in the same country: ~100 ms round trip.
    "home": Impairment(delay_ms=45, jitter_ms=10, loss_pct=1, duplicate_pct=0.1, reorder_pct=0.5,
                       up_kbps=3000, down_kbps=10000),
    # Busy Wi-Fi far away: ~200 ms round trip, 3% loss, slow upload, a 2 s hiccup every minute.
    "bad": Impairment(delay_ms=90, jitter_ms=40, loss_pct=3, duplicate_pct=0.5, reorder_pct=2,
                      up_kbps=1000, down_kbps=2000, blackout_every_s=60, blackout_s=2),
}


class NetworkError(ValueError):
    pass


def impairment_from(table: dict) -> Impairment:
    """A scenario's [network] table: a preset plus any field overrides."""
    t = dict(table)
    preset = t.pop("preset", None)
    if preset is not None and preset not in PRESETS:
        raise NetworkError(f"unknown network preset {preset!r} (known: {', '.join(PRESETS)})")
    base = PRESETS[preset] if preset else Impairment()
    known = {f.name for f in fields(Impairment)}
    bad = set(t) - known
    if bad:
        raise NetworkError(f"unknown [network] field(s): {', '.join(sorted(bad))}")
    try:
        over = {k: float(v) for k, v in t.items()}
    except (TypeError, ValueError) as e:
        raise NetworkError(f"[network] values must be numbers: {e}") from e
    if any(v < 0 for v in over.values()):
        raise NetworkError("[network] values must not be negative")
    return replace(base, **over)


@dataclass
class DirStats:
    packets: int = 0
    bytes: int = 0
    lost: int = 0
    queue_drops: int = 0
    blackout_drops: int = 0
    duplicated: int = 0
    reordered: int = 0
    delay_ms_sum: float = 0.0
    delivered: int = 0

    def summary(self) -> dict:
        d = asdict(self)
        d["avg_delay_ms"] = round(self.delay_ms_sum / self.delivered, 1) if self.delivered else None
        del d["delay_ms_sum"]
        return d


class _Direction:
    """Decides when (or whether) each packet in one direction goes out."""

    def __init__(self, imp: Impairment, kbps: float, rng: random.Random, t0: float):
        self.imp, self.kbps, self.rng, self.t0 = imp, kbps, rng, t0
        self.link_free = 0.0     # when the capped link finishes sending what is queued
        self.last_out = 0.0      # departure of the previous packet (keeps order)
        self.stats = DirStats()

    def in_blackout(self, now: float) -> bool:
        i = self.imp
        if i.blackout_every_s <= 0 or i.blackout_s <= 0:
            return False
        into = (now - self.t0) % i.blackout_every_s
        return (now - self.t0) >= i.blackout_every_s and into < i.blackout_s

    def schedule(self, size: int, now: float) -> list[float]:
        """Departure times for this packet: [] if it is dropped, two if it is duplicated."""
        i, s = self.imp, self.stats
        s.packets += 1
        s.bytes += size
        if self.in_blackout(now):
            s.blackout_drops += 1
            return []
        if self.rng.random() * 100 < i.loss_pct:
            s.lost += 1
            return []
        start = now
        if self.kbps > 0:
            start = max(now, self.link_free)
            if start - now > i.queue_ms / 1000:
                s.queue_drops += 1
                return []
            self.link_free = start + size * 8 / (self.kbps * 1000)
            start = self.link_free
        delay = max(0.0, i.delay_ms + self.rng.uniform(-i.jitter_ms, i.jitter_ms)) / 1000
        out = start + delay
        if self.rng.random() * 100 < i.reorder_pct and out < self.last_out:
            s.reordered += 1
        else:
            out = max(out, self.last_out)
            self.last_out = out
        times = [out]
        if self.rng.random() * 100 < i.duplicate_pct:
            s.duplicated += 1
            times.append(out + self.rng.uniform(0, 0.005))
        for t in times:
            s.delay_ms_sum += (t - now) * 1000
            s.delivered += 1
        return times


class Relay:
    """One client's relay: listens on 127.0.0.1:listen_port, talks to the server from source_ip."""

    def __init__(self, name: str, listen_port: int, server: tuple[str, int], source_ip: str,
                 imp: Impairment, seed: int = 0, clock=time.perf_counter):
        self.name, self.server, self.imp, self.clock = name, server, imp, clock
        self.down_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)   # faces the client
        self.down_sock.bind(("127.0.0.1", listen_port))
        self.up_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)     # faces the server
        self.up_sock.bind((source_ip, 0))
        for s in (self.down_sock, self.up_sock):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
            _no_connreset(s)
        self.listen_port = listen_port
        self.source = self.up_sock.getsockname()
        self.client: tuple[str, int] | None = None
        t0 = clock()
        rng = random.Random(seed)
        self.up = _Direction(imp, imp.up_kbps, rng, t0)
        self.down = _Direction(imp, imp.down_kbps, rng, t0)
        self._heap: list = []
        self._seq = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, name=f"relay-{name}", daemon=True)

    def start(self) -> "Relay":
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5)
        for s in (self.down_sock, self.up_sock):
            s.close()

    def stats(self) -> dict:
        return {"listen": f"127.0.0.1:{self.listen_port}", "server_sees": f"{self.source[0]}:{self.source[1]}",
                "impairment": asdict(self.imp),
                "client_to_server": self.up.stats.summary(), "server_to_client": self.down.stats.summary()}

    def _queue(self, when: float, sock, data: bytes, addr) -> None:
        self._seq += 1
        heapq.heappush(self._heap, (when, self._seq, sock, data, addr))

    def _loop(self) -> None:
        socks = [self.down_sock, self.up_sock]
        while not self._stop.is_set():
            now = self.clock()
            while self._heap and self._heap[0][0] <= now:
                _, _, sock, data, addr = heapq.heappop(self._heap)
                try:
                    sock.sendto(data, addr)
                except OSError:
                    pass
            wait = 0.05 if not self._heap else max(0.0, min(0.05, self._heap[0][0] - now))
            try:
                ready, _, _ = select.select(socks, [], [], wait)
            except OSError:
                if self._stop.is_set():
                    return
                raise
            now = self.clock()
            for s in ready:
                try:
                    data, addr = s.recvfrom(65535)
                except OSError:
                    continue
                if s is self.down_sock:
                    self.client = addr
                    for t in self.up.schedule(len(data), now):
                        self._queue(t, self.up_sock, data, self.server)
                elif addr == self.server and self.client:
                    for t in self.down.schedule(len(data), now):
                        self._queue(t, self.down_sock, data, self.client)


def _no_connreset(sock: socket.socket) -> None:
    """Windows reports an ICMP 'port unreachable' as an error on the next recvfrom of a UDP socket
    (WSAECONNRESET), which would kill the relay while the server is still starting. Turn it off."""
    if hasattr(socket, "SIO_UDP_CONNRESET"):
        sock.ioctl(socket.SIO_UDP_CONNRESET, False)


class _TimerResolution:
    """Ask Windows for 1 ms timers for this process only, so 45 ms means 45 ms, not 45-60."""

    def __enter__(self):
        try:
            self._winmm = ctypes.WinDLL("winmm")
            self._winmm.timeBeginPeriod(1)
        except (OSError, AttributeError):
            self._winmm = None
        return self

    def __exit__(self, *exc):
        if self._winmm:
            self._winmm.timeEndPeriod(1)


@dataclass
class RelaySet:
    """The relays for one run: one per client, started and stopped together."""
    relays: dict[str, Relay] = field(default_factory=dict)
    _timer: _TimerResolution | None = None

    @classmethod
    def start(cls, clients: list[str], server_port: int, imp: Impairment, seed: int = 0,
              first_port: int = FIRST_RELAY_PORT) -> "RelaySet":
        rs = cls()
        rs._timer = _TimerResolution().__enter__()
        try:
            for i, name in enumerate(clients):
                rs.relays[name] = Relay(name, first_port + i, ("127.0.0.1", server_port),
                                        f"127.0.0.{2 + i}", imp, seed * 100 + i).start()
        except OSError:
            rs.stop()
            raise
        return rs

    def port(self, client: str) -> int:
        return self.relays[client].listen_port

    def stats(self) -> dict:
        return {n: r.stats() for n, r in self.relays.items()}

    def write_stats(self, path: Path) -> None:
        path.write_text(json.dumps(self.stats(), indent=2), encoding="utf-8")

    def stop(self) -> None:
        for r in self.relays.values():
            r.stop()
        if self._timer:
            self._timer.__exit__(None, None, None)
            self._timer = None
