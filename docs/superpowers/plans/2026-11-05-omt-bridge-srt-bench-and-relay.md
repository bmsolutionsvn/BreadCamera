# OMT Bridge: SRT Benchmark and Relay Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A portable `srt-bench` tool and a `bmb-relay` server (both on libsrt, Windows and Linux/Docker) so the owner can measure SRT at 10 / 20 / 40 / 50 Mbps, direct and through the relay, before any Bridge remote code is written.

**Architecture:** A new self-contained folder `remote/` in OMT-Bridge holds a small portable library (`bmb_net`: stream IDs, SRT options, stats, the bench's paced payload) and two programs on top of it. It builds inside the Windows tree (vcpkg `libsrt`) and on its own on Linux (Debian's `libsrt-openssl-dev`) in a Docker image. The relay pairs publishers with requesters by stream ID and forwards their messages without parsing them.

**Tech Stack:** C++20, libsrt 1.5, doctest, CMake, Docker (Debian bookworm).

**Spec:** `F:\Coding\BreadMedia\OMT-Bridge\docs\superpowers\specs\2026-11-05-omt-bridge-remote-design.md` §4 and §6 (and §5.2's stream IDs). Plan 1 of the remote link. Plans for Host / Join and relay mode in the Bridge come after the owner's benchmark numbers.

Repo: `F:\Coding\BreadMedia\OMT-Bridge`, branch `ob-remote-bench` from `main`, merged back with `--no-ff`. Never push. Change no other repo. Nothing here runs on, or connects to, any VPS: the owner runs the benchmark.

## Global Constraints

- libsrt only (vcpkg `libsrt` on Windows; `libsrt-openssl-dev` on Debian bookworm). No Go SRT, no FFmpeg SRT protocol layer.
- Code under `remote/src` is portable C++20: no Windows headers there (use `<chrono>`, `<thread>`, libsrt's own socket API). Windows-only code, if any, stays out of `remote/`.
- `remote/` depends only on libsrt and doctest (tests). It must build with `cmake -S remote -B <dir>` on its own.
- Stream ID (SRT access-control syntax): `#!::r=bmb/<session>/<bridge>/<stream>,m=<publish|request|bidirectional>[,u=<token>]`. `<session>`, `<bridge>`, `<stream>`: 1–64 characters of `[A-Za-z0-9._-]`; the whole ID ≤ 512 bytes. `<stream>` = a source id, or `control` (with `m=bidirectional`).
- Bench pacing: 60 bursts per second; a keyframe burst every 2 s (120 frames) at 4× the size of the others, so the average equals the set bitrate.
- Benchmark pass bar (for the owner's run, recorded in `remote/BENCHMARK.md`): zero unrecovered drops at 40 Mbps for 10 minutes, tuned, direct and via the relay.
- Tests bind to `127.0.0.1` only (no firewall prompt).
- Windows tests: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1` (also runs `remote_tests.exe`, see Task 1). Linux: `docker build -f remote/docker/Dockerfile -t bmb-remote .` (runs `remote_tests` in the build stage).

## Review Focus

1. A publisher drops and reconnects while a requester stays connected: the requester keeps receiving after the reconnect, without reconnecting itself. → Task 3 test.
2. A requester connects before its publisher exists: it waits and receives once the publisher arrives. → Task 3 test.
3. A malformed or over-long stream ID, or a wrong token: refused at the handshake with a reject reason, never accepted and dropped later. → Tasks 1 and 3 tests.
4. The bench's "×RTT" latency on a link whose RTT probe fails (no reply within 3 s): it stops with a clear message, never runs with latency 0. → Task 2 test.
5. A burst bigger than the send buffer (a 50 Mbps keyframe with a small `--sndbuf`): the sender reports it in the summary (`snd_drop`) instead of blocking the pacing loop. → Task 2 test.

---

### Task 1: `remote/` skeleton, stream IDs, SRT options and stats

**Files:**
- Create: `remote/CMakeLists.txt`, `remote/src/stream_id.h/.cpp`, `remote/src/srt_util.h/.cpp`, `remote/tests/main.cpp`, `remote/tests/stream_id_test.cpp`, `remote/tests/srt_util_test.cpp`
- Modify: `vcpkg.json` (add `"libsrt"`), `CMakeLists.txt` (`add_subdirectory(remote)`), `scripts/test.ps1` (also run `remote_tests.exe` from the same build folder; exit non-zero if either fails)

**Interfaces:**
- Produces:
  - Targets: `bmb_net` (static library, `remote/src`), `remote_tests` (doctest exe). On Windows link libsrt as the vcpkg port's `usage` file says (expected: `find_package(libsrt CONFIG REQUIRED)` and `libsrt::srt_static` or `libsrt::srt_shared`); elsewhere `pkg_check_modules(SRT REQUIRED IMPORTED_TARGET srt)`.
  - `enum class StreamMode { Publish, Request, Bidirectional };`
  - `struct StreamId { std::string session, bridge, stream; StreamMode mode; std::string token; };`
  - `std::string formatStreamId(const StreamId&);` — throws `std::invalid_argument` on a part that breaks the Global Constraints rule.
  - `std::optional<StreamId> parseStreamId(std::string_view);` — nullopt for anything malformed (missing `#!::`, unknown mode, bad characters, empty part, extra `/`, > 512 bytes). Keys may come in any order; unknown keys are ignored.
  - `std::string resourceOf(const StreamId&);` → `bmb/<session>/<bridge>/<stream>`.
  - `struct SrtOptions { int latencyMs = 120; int sndBufBytes = 0; int rcvBufBytes = 0; int fcPackets = 0; int payloadSize = 1316; int overheadPct = 25; std::string passphrase; };` — 0 means "libsrt default". Passphrase: empty = no encryption; otherwise 10–79 characters and AES-256 (`SRTO_PBKEYLEN = 32`).
  - `void applyOptions(SRTSOCKET, const SrtOptions&);` — sets live mode (`SRTO_TRANSTYPE = SRTT_LIVE`), `SRTO_LATENCY`, `SRTO_SNDBUF`, `SRTO_RCVBUF`, `SRTO_FC`, `SRTO_PAYLOADSIZE`, `SRTO_OHEADBW`, passphrase; throws `std::runtime_error` with `srt_getlasterror_str()` on failure.
  - `struct SrtStats { double mbpsSent, mbpsRecv; int64_t pktSent, pktRetrans, pktLossRecv, pktDropRecv, pktDropSend; double rttMs, sndBufMs, rcvBufMs, estBwMbps; };`
  - `SrtStats statsOf(SRTSOCKET, bool clearIntervals);` — from `srt_bstats`.
  - `class SrtLibrary` — RAII `srt_startup()` / `srt_cleanup()`, one per process.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("StreamId: round trip and resource") {
  const StreamId id{"show-1", "Venue", "cam1", StreamMode::Publish, "tok"};
  const auto s = formatStreamId(id);
  CHECK(s == "#!::r=bmb/show-1/Venue/cam1,m=publish,u=tok");
  const auto back = parseStreamId(s);
  REQUIRE(back);
  CHECK(back->bridge == "Venue");
  CHECK(back->mode == StreamMode::Publish);
  CHECK(resourceOf(*back) == "bmb/show-1/Venue/cam1");
  CHECK(parseStreamId("#!::m=request,r=bmb/s/b/control")->mode == StreamMode::Request);
}

TEST_CASE("StreamId: malformed ids are refused") {
  for (const char* bad : {"", "bmb/s/b/c", "#!::r=bmb/s/b,m=publish", "#!::r=bmb/s/b/c,m=send",
                          "#!::r=bmb/s/b c/x,m=publish", "#!::r=bmb/s//c,m=publish", "#!::r=other/s/b/c,m=publish"})
    CHECK_FALSE(parseStreamId(bad));
  CHECK_FALSE(parseStreamId("#!::r=bmb/s/b/" + std::string(65, 'x') + ",m=publish"));
  CHECK_THROWS_AS(formatStreamId({"s", "b/c", "x", StreamMode::Publish, ""}), std::invalid_argument);
}

TEST_CASE("SrtOptions: applied values read back") {
  SrtLibrary lib;
  const SRTSOCKET s = srt_create_socket();
  applyOptions(s, {.latencyMs = 250, .rcvBufBytes = 64 * 1024 * 1024, .fcPackets = 60000, .passphrase = "0123456789abcdef"});
  int v = 0; int len = sizeof v;
  srt_getsockflag(s, SRTO_LATENCY, &v, &len);  CHECK(v == 250);
  srt_getsockflag(s, SRTO_FC, &v, &len);       CHECK(v == 60000);
  srt_getsockflag(s, SRTO_PBKEYLEN, &v, &len); CHECK(v == 32);
  CHECK_THROWS(applyOptions(s, {.passphrase = "short"}));
  srt_close(s);
}
```

- [ ] **Step 2: Run and see them fail** — `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1`: the build fails (no `remote/`).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `scripts/test.ps1`: `ob_tests` and `remote_tests` pass.
- [ ] **Step 5: Commit**

```bash
git add vcpkg.json CMakeLists.txt scripts/test.ps1 remote
git commit -m "feat(remote): stream ids, SRT options and stats on libsrt"
```

---

### Task 2: `srt-bench`

**Files:**
- Create: `remote/src/bench_payload.h/.cpp`, `remote/src/bench.h/.cpp`, `remote/apps/srt_bench.cpp`, `remote/tests/bench_test.cpp`
- Modify: `remote/CMakeLists.txt` (target `srt-bench`)

**Interfaces:**
- Consumes: Task 1.
- Produces:
  - `std::vector<int> frameSizes(int64_t bitrateBps, int fps, int keyEvery, double keyScale);` — one GOP of `keyEvery` sizes in bytes, index 0 the keyframe = `keyScale` × the others, summing to `bitrateBps / 8 × keyEvery / fps` (± one byte per frame from rounding).
  - Payload header, 32 bytes little-endian at the start of every SRT message: `uint64 seq` (per stream, from 0), `int64 sendUs` (sender's steady clock), `uint32 frame`, `uint16 part`, `uint16 parts`, `uint64 reserved`. `void writeHeader(std::span<uint8_t>, const PayloadHeader&)`, `PayloadHeader readHeader(std::span<const uint8_t>)`.
  - `class SeqTracker { void add(uint64_t seq); int64_t gaps() const; int64_t received() const; };` — `received()` counts distinct seqs, `gaps() = highest + 1 − received()`, so a late seq fills its gap and a duplicate counts once. Track only a recent window (e.g. the last 65 536 seqs as a bitmap); a 10-minute 50 Mbps run sends ~3 M messages, so don't keep a set of every seq.
  - `struct BenchConfig { std::string role /* send|recv */; std::string host; int port; std::string streamId; int64_t bitrateBps; int seconds; SrtOptions srt; std::optional<double> latencyRttFactor; bool listen; std::string csvPath; };`
  - `int runBench(const BenchConfig&, std::ostream& log);` — exit code 0 on success. Sender: if `latencyRttFactor`, first a 3 s probe connection (small messages) to read `msRTT`, then `latencyMs = max(120, round(factor × rtt))`; no RTT in 3 s → message `RTT probe failed: no reply from <host>:<port>` and exit code 2. Then the paced send: each frame split into `payloadSize`-byte messages, each with a header, sent with `srt_sendmsg2`; a send that fails because the buffer is full is counted as `snd_drop` and the loop keeps its schedule. Receiver: `--listen` or caller mode, `SeqTracker` on every message.
  - Once per second, both roles write a CSV row: `t_s,mbps_sent,mbps_recv,pkt_sent,pkt_retrans,pkt_loss_recv,pkt_drop_recv,pkt_drop_send,rtt_ms,snd_buf_ms,rcv_buf_ms,est_bw_mbps,seq_gaps,snd_drop`; at the end one Markdown summary row to the log: `| bitrate | latency | options | mbps avg | retrans % | drop_recv | seq_gaps | snd_drop | rtt avg | result |` with `result` = `PASS` when `drop_recv == 0 && seq_gaps == 0 && snd_drop == 0`.
  - CLI: `srt-bench send|recv --host H --port P [--listen] [--streamid S] [--mbps N] [--seconds N] [--latency MS | --latency-rtt X] [--sndbuf B] [--rcvbuf B] [--fc N] [--payload N] [--overhead PCT] [--passphrase P] [--csv FILE]`; `--help` prints this and exits 0; a bad argument prints the reason and exits 64.

- [ ] **Step 1: Write the failing tests** (`remote/tests/bench_test.cpp`):
  - `"Bench: frame sizes keep the average"` — `frameSizes(40'000'000, 60, 120, 4.0)`: 120 sizes, `[0] == 4 × [1]` (± 1), sum within 120 bytes of `40e6 / 8 × 2`.
  - `"Bench: header round trip"` and `"Bench: seq tracker counts only missing numbers"` — add 0,1,2,4,3,4,7 → `received() == 6` (4 twice counts once), `gaps() == 2` (5 and 6).
  - `"Bench: 10 Mbps for 3 s over loopback passes"` — a receiver thread (`listen`, 127.0.0.1, a free port) and a sender (`mbps 10`, `seconds 3`) → both return 0; the receiver's summary has `seq_gaps 0` and average Mbps within 10 % of 10.
  - `"Bench: a failed RTT probe stops with a message"` — sender with `latencyRttFactor 4` to a port nobody listens on → returns 2 and the log contains `RTT probe failed`.
  - `"Bench: a too-small send buffer shows as snd_drop"` — 50 Mbps, `sndBufBytes` = 64 KiB, 2 s, with a receiver → the sender's summary `snd_drop > 0` and `result` is not `PASS`; the run still ends on time (≤ 3 s).
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `scripts/test.ps1`, all pass; `build/release/srt-bench.exe --help` exits 0.
- [ ] **Step 5: Commit**

```bash
git add remote
git commit -m "feat(remote): srt-bench, a paced SRT load test with per-second stats"
```

---

### Task 3: `bmb-relay` pairing core

**Files:**
- Create: `remote/src/relay.h/.cpp`, `remote/apps/bmb_relay.cpp`, `remote/tests/relay_test.cpp`
- Modify: `remote/CMakeLists.txt` (target `bmb-relay`)

**Interfaces:**
- Consumes: Task 1 (`parseStreamId`, `resourceOf`, `applyOptions`, `statsOf`).
- Produces:
  - `struct RelayConfig { std::string bindHost = "0.0.0.0"; int port = 7000; std::string passphrase; std::string token; double maxTotalMbps = 0; /* 0 = unlimited */ int latencyMs = 120; };`
  - `class Relay { public: explicit Relay(RelayConfig); ~Relay(); void start(); void stop(); struct SessionStats { std::string session; double mbpsIn, mbpsOut; int publishers, requesters; }; std::vector<SessionStats> stats() const; };`
  - Handshake check in `srt_listen_callback`: a stream ID that does not parse → reject `SRT_REJX_BAD_REQUEST` (1400); a wrong `u=` token (when `token` is set) → `SRT_REJX_UNAUTHORIZED` (1401); a second publisher for the same resource → `SRT_REJX_CONFLICT` (1409); a new publisher while the measured total input (last 1 s) ≥ `maxTotalMbps` → `SRT_REJX_OVERLOAD` (1402). Set with `srt_setrejectreason`; check the values against the installed `srt.h`.
  - Forwarding (one epoll thread): every message from a `publish` socket goes to every `request` socket whose resource is the same, in order, with `srt_sendmsg2`; a `request` socket with no publisher yet stays open and receives once one arrives; a publisher that disconnects leaves its requesters open. `bidirectional` sockets with the same resource form a hub: each message goes to every other member. The relay never reads past the stream ID.
  - Every 10 s the program prints one line per session: `session=<s> in=<Mbps> out=<Mbps> pub=<n> req=<n>`.
  - `bmb-relay` reads `BMB_RELAY_PORT`, `BMB_RELAY_PASSPHRASE`, `BMB_RELAY_TOKEN`, `BMB_RELAY_MAX_MBPS`, `BMB_RELAY_LATENCY_MS` from the environment (command-line flags of the same names override), runs until SIGINT / SIGTERM, exits 0.

- [ ] **Step 1: Write the failing tests** (`remote/tests/relay_test.cpp`; a relay on `127.0.0.1` and a free port; helpers `publish(resource)`, `request(resource)` dial with Task 1's stream IDs; each message carries a bench header with `seq`):
  - `"Relay: a requester gets the publisher's messages in order"` — 1000 messages → requester sees seq 0…999, no gaps.
  - `"Relay: two requesters both receive"`.
  - `"Relay: a requester that comes first waits for the publisher"`.
  - `"Relay: a publisher can reconnect under a waiting requester"` — publisher sends 100, closes, reconnects with the same resource, sends 100 more → the same requester socket receives 200 (its seq restarts are fine).
  - `"Relay: control hub forwards both ways"` — two `bidirectional` sockets on `bmb/s/hub/control`: A's message reaches B and B's reaches A; neither gets its own.
  - `"Relay: bad stream id, wrong token and a second publisher are refused"` — `srt_connect` fails and `srt_getrejectreason` returns 1400, 1401, 1409 respectively.
  - `"Relay: the total cap refuses a new publisher"` — `maxTotalMbps = 5`, one publisher at 10 Mbps for 1.5 s, then a second publisher → 1402.
  - `"Relay: srt-bench passes through the relay"` — `runBench` recv as requester and send as publisher (both `--host 127.0.0.1 --port <relay>` with their stream IDs), 10 Mbps for 3 s → receiver summary `PASS`. This is the spec's `--via <relay>`: the bench goes through a relay by pointing `--host/--port` at it with a `--streamid`.
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `scripts/test.ps1`, all pass (run `remote_tests.exe --test-case="Relay*"` 10 times: 0 failures).
- [ ] **Step 5: Commit**

```bash
git add remote
git commit -m "feat(remote): bmb-relay pairs publishers and requesters by stream id"
```

---

### Task 4: Linux build, Docker image, and the owner's benchmark kit

**Files:**
- Create: `remote/docker/Dockerfile`, `remote/docker/compose.yml`, `remote/docker/selftest.sh`, `remote/run-matrix.ps1`, `remote/BENCHMARK.md`
- Modify: `.gitattributes` if needed so `*.sh` keep LF

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces:
  - `Dockerfile`: build stage `debian:bookworm` with `build-essential cmake pkg-config libsrt-openssl-dev doctest-dev`; `cmake -S remote -B /b -DCMAKE_BUILD_TYPE=Release && cmake --build /b && /b/remote_tests`; runtime stage `debian:bookworm-slim` with `libsrt1.5-openssl`, `bmb-relay` and `srt-bench` in `/usr/local/bin`, `ENTRYPOINT ["bmb-relay"]`, `EXPOSE 7000/udp`.
  - `compose.yml`: service `bmb-relay`, image built from the Dockerfile, `ports: ["7000:7000/udp"]`, `env_file: .env` (keys from Task 3), `restart: unless-stopped`. No secrets committed; a `.env.example` beside it lists the keys with placeholder values.
  - `selftest.sh` (runs inside the image): starts `bmb-relay` on 7000 with a test token, runs `srt-bench recv` (requester) and `srt-bench send` (publisher) through it for 5 s at 10 Mbps, exits 0 when the receiver's summary says `PASS`.
  - `run-matrix.ps1 -Host <vps> -Port 7000 -Token <t> [-Passphrase <p>] [-Minutes 10]`: runs, for 10 / 20 / 40 / 50 Mbps, default options and the tuned set (`--latency-rtt 4 --sndbuf 64MiB --rcvbuf 64MiB --fc 60000`), both **direct** (a `srt-bench recv --listen` the owner starts on the VPS with `docker run --rm --network host --entrypoint srt-bench bmb-remote recv --listen --port 7001`) and **via the relay** (publisher and requester both from this PC through `<vps>:7000`); writes `remote/bench-results/<yyyy-MM-dd-HHmm>.md` with every summary row, and prints where.
  - `BENCHMARK.md`: what the matrix measures, the exact commands (build and start the relay on the VPS, the direct receiver, `run-matrix.ps1`), the pass bar from Global Constraints, and "commit the results file next to the design and tell the reviewer".
- [ ] **Step 1: Write the failing check** — `docker build -f remote/docker/Dockerfile -t bmb-remote .` (fails: no Dockerfile).
- [ ] **Step 2: Implement** the files above.
- [ ] **Step 3: Verify Linux**
  - Run: `docker build -f remote/docker/Dockerfile -t bmb-remote .` → succeeds (its build stage ran `remote_tests`).
  - Run: `docker run --rm --entrypoint sh bmb-remote /usr/local/bin/selftest.sh` (copy `selftest.sh` into the image) → exit 0, output ends with a `PASS` row.
- [ ] **Step 4: Verify Windows ↔ Linux** — `docker run -d --name bmb-relay-test -p 7000:7000/udp -e BMB_RELAY_TOKEN=t bmb-remote`; then from Windows `build/release/srt-bench.exe recv` (requester `#!::r=bmb/test/pc/cam1,m=request,u=t` to 127.0.0.1:7000) and `srt-bench.exe send` (publisher, 10 Mbps, 10 s) → `PASS`; `docker rm -f bmb-relay-test`. If Docker Desktop cannot forward UDP on this PC, log a `Ruling:` with the error and keep Step 3's result as the Linux proof.
- [ ] **Step 5: Commit**

```bash
git add remote .gitattributes
git commit -m "feat(remote): Docker image, self-test and the benchmark kit"
```

---

### Task 5: Merge

- [ ] **Step 1:** `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1` and the Docker build: both pass.
- [ ] **Step 2:** `git checkout main; git merge --no-ff ob-remote-bench -m "merge: SRT benchmark and relay core"`; run both again on `main`. Do not push.
