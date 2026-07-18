// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Paravirtual EXI device for engine-level Melee netplay (lockstep-from-boot).
// The modified game (doldecomp/melee + src/melee/nw module) exchanges
// frame-indexed PADStatus blocks through this device; the device owns the real
// transport (TCP direct connect for the MVP). See
// aot-dolphin-helper/research/melee-netplay-design.md for the protocol.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/EXI/MeleeNetplayExternalTransport.h"
#include "Core/HW/EXI/MeleeRollbackState.h"

struct MemCheckTraceRing;
class MeleeJukebox;

namespace ExpansionInterface
{
class CEXIMeleeNetplay : public IEXIDevice
{
public:
  explicit CEXIMeleeNetplay(Core::System& system);
  ~CEXIMeleeNetplay() override;

  void ImmWrite(u32 data, u32 size) override;
  u32 ImmRead(u32 size) override;
  void DMAWrite(u32 address, u32 size) override;
  void DMARead(u32 address, u32 size) override;
  bool IsPresent() const override;
  void DoState(PointerWrap& p) override;

private:
  // EXI command words (high byte of the 4-byte imm write). DMA payloads are
  // padded to 32-byte multiples (GC EXI DMA constraint); trailing pad is zero.
  // POLL/RECV carry no tick: lockstep is strictly sequential, so the device
  // serves ticks 0,1,2,... advancing on each RECV.
  enum : u8
  {
    CMD_ID = 0x00,         // EXIGetID probe; ImmRead returns DEVICE_ID
    CMD_HANDSHAKE = 0x01,  // then DMARead 32B handshake blob (blocks for session)
    CMD_SEND = 0x02,       // then DMAWrite 288B {u32 tick, HSD_PadStatus[4], pad}
    CMD_POLL = 0x03,       // ImmRead 4B: 1 if serve-tick frame ready
    CMD_RECV = 0x04,       // then DMARead 288B {HSD_PadStatus[4], pad}; advances serve tick
    CMD_CHECKSUM = 0x05,   // then DMAWrite 32B {u32 tick, u32 crc32, pad}
    // Jukebox: host-side BGM notifications (fire-and-forget, no reply). The
    // volume rides the imm command word itself (cmd<<24 | vol) so play/stop/
    // volume all stay outside the tick-exchange DMA sequence.
    CMD_JUKEBOX_PLAY = 0x06,  // then DMAWrite 64B {char path[0x3C], u8 vol, u8 track, pad}
    CMD_JUKEBOX_STOP = 0x07,  // imm-only
    CMD_JUKEBOX_VOL = 0x08,   // imm-only; volume 0..254 in the low byte
  };

  static constexpr u32 DEVICE_ID = 0x4D4E4554;  // 'MNET'; unknown to CARD -> "no card"
  static constexpr u8 PROTO_VERSION = 5;        // v5: jukebox cmds (v4: N-peer census)
  static constexpr u32 PAD_BYTES = 0x44;  // sizeof(HSD_PadStatus): full post-transform entry
  static constexpr u32 MAX_PLAYERS = 4;

  // v3 POLL return word: status<<24 | arg. v2's bare 0/1 map onto WAIT/READY.
  enum : u8
  {
    POLL_WAIT = 0,    // inputs for the serve tick not here yet — poll again
    POLL_READY = 1,   // proceed: RECV serves the current tick
    POLL_REPLAY = 3,  // arg = K: memory was restored K ticks back; run K
                      // replay ticks (RECV+inject+body each), then poll again
  };

  // Wire message types
  enum : u8
  {
    // v4 HELLO carries the whole session census, because with >2 peers a client
    // can no longer infer the port map from its own mask plus the host's.
    // 12 bytes, host->client:
    //   [0] type  [1] ver  [2] your_mask  [3] delay  [4..7] seed (BE32)
    //   [8] window  [9] combined_mask  [10] barrier_mask  [11] players
    // your_mask     - ports THIS client owns (host assigns; no guess-and-collide)
    // combined_mask - every participating port, session-wide
    // barrier_mask  - one bit per PEER: that peer's first-owned port, i.e. the
    //                 block it stamps its scene-barrier flag into. Equals
    //                 combined_mask when every peer owns exactly one pad.
    MSG_HELLO = 0x10,
    MSG_HELLO_ACK = 0x11,  // client->host {u8 ver, u8 client_mask}
    MSG_INPUTS = 0x01,     // {tick} payload: 68B per set mask bit, ascending port order
    MSG_CHECKSUM = 0x02,   // {tick} payload: 4B crc
  };
  static constexpr u32 HELLO_BYTES = 12;

  struct Frame
  {
    u8 have_mask = 0;
    std::array<std::array<u8, PAD_BYTES>, 4> pads{};
    // Testing only: when simulating latency, the remote half of this frame is
    // withheld from the game until this instant. Delaying *visibility* rather
    // than sleeping the receive thread is what actually models a network: a
    // sleeping receiver also throttles throughput, so messages queue up and
    // the measured stall grows without bound instead of settling at the true
    // (latency - budget) figure.
    std::chrono::steady_clock::time_point visible_at{};
  };

  // One link to one remote console. The host holds Players-1 of these (one per
  // client) and RELAYS inputs between them; a client holds exactly one (the
  // host). Star topology: clients never talk to each other directly. See
  // research/4player-design.md for why not mesh.
  //
  // Each link owns its send queue and threads, so a blocking send NEVER runs on
  // a receive thread or under m_frames_lock -- with N peers a single shared send
  // path would let one slow console stall every other link (and the original
  // 2-peer deadlock, documented below, gets N times more ways to bite).
  struct PeerLink
  {
    // TcpLinkStream (device-owned SFML socket) or ExternalLinkStream
    // (app-pumped mailbox, iOS) -- see MeleeNetplayExternalTransport.h.
    // shared_ptr because external streams are co-owned by the registry.
    std::shared_ptr<ILinkStream> stream;
    u8 mask = 0;  // ports this peer owns
    std::mutex sendq_lock;
    std::condition_variable sendq_cv;
    std::deque<std::vector<u8>> sendq;
    std::thread send_thread;
    std::thread rx_thread;
  };

  void NetThread();
  bool HostHandshake();    // accept Players-1 clients, deal the census
  bool ClientHandshake();  // connect to the host, learn the census
  // External transport: adopt `count` app-attached links (attach order =
  // census deal order) as fresh PeerLinks. Blocks until they exist.
  bool AdoptExternalLinks(u32 count);
  void StartPeerThreads();
  void PeerSendThread(PeerLink* link);
  void PeerRecvThread(PeerLink* link);
  // Queue a framed message to every peer except `exclude` (nullptr = all).
  void BroadcastRaw(const u8* bytes, u32 len, const PeerLink* exclude);
  void HandleMessage(PeerLink* from, u8 type, u8 mask, u32 tick, const u8* payload, u32 len);
  void SendInputs(u32 tick, const u8* pads);
  void SendMessageRaw(u8 type, u8 mask, u32 tick, const u8* payload, u16 len);
  bool FrameReady(u32 tick);
  bool RemoteArrivedLocked(u32 tick) const;  // real remote data present & visible
  void ValidateSpeculationLocked();          // advance frontier; detect mispredicts
  void PredictIntoLocked(u32 tick);          // fill remote ports from newest confirmed

  // --- session (written by net thread before m_session_ready, read-only after)
  std::atomic<bool> m_session_ready{false};
  std::atomic<bool> m_session_failed{false};
  bool m_is_host = false;
  bool m_transport_external = false;  // MeleeNetplay.Transport=1: app owns the links
  u32 m_players = 2;
  u8 m_local_mask = 0x01;
  u8 m_remote_mask = 0x02;   // union of every OTHER peer's ports
  u8 m_combined_mask = 0x03;  // every participating port
  u8 m_barrier_mask = 0x03;   // one bit per peer: its first-owned port
  u8 m_delay = 2;
  u32 m_seed = 0;

  // --- input frames
  std::mutex m_frames_lock;
  std::map<u32, Frame> m_frames;
  std::map<u32, u32> m_local_crcs;
  // tick -> (peer mask -> crc). Per-PEER, not per-tick-scalar: with 3 remotes a
  // single slot would be clobbered by whichever peer's checksum landed last, and
  // two of the three comparisons would silently never happen. Every peer
  // compares against the HOST's value (single authority ⇒ transitive: if A and B
  // both agree with the host they agree with each other), so a fork is caught on
  // both the forker and the host, and the log names which peer diverged.
  std::map<u32, std::map<u8, u32>> m_remote_crcs;
  // tick -> OR of the port bits of peers whose checksum we have already
  // compared. A tick retires only once this covers m_remote_mask.
  std::map<u32, u8> m_crc_seen;
  // Device-side sync oracle: every HASH_INTERVAL ticks, hash the ring
  // snapshot of the newest CONFIRMED hash tick (SnapshotChecksum) and
  // exchange it. Snapshot hashes only ever cover confirmed, immutable
  // pre-tick state, so no defer/park/discard protocol is needed — the
  // game-side CMD_CHECKSUM hash-at-submission was speculative under
  // prediction and produced false desyncs at byte-identical sims (target
  // run 3). m_hash_tick = last hash tick emitted (or skipped).
  // INTERVAL 1 = every tick: the class-(B) hunt needs the FIRST divergent
  // tick, not a 60-coarse bracket — the entry spans are only visible in the
  // pre-state of that exact tick (~20KB FNV per tick, trivial next to the
  // 0.5ms captures; wire cost 12 bytes/tick).
  static constexpr u32 HASH_INTERVAL = 1;
  u32 m_hash_tick = 0;
  // Hash ticks are hashed AT CAPTURE (ring[tick] is fresh then) and parked
  // here until the frontier confirms them. Hashing at emission from the ring
  // starved the oracle under rollback churn: with a rollback every ~2 ticks
  // the 16-slot ring recycles a hash tick before the frontier passes it, and
  // the two peers skip DIFFERENT ticks so pairs almost never complete
  // (target11c: 2 comparisons out of ~120 in-match intervals). Replayed
  // RECVs re-capture and re-hash their ticks, so a parked value is corrected
  // by the same mechanism that corrects the ring. CPU thread only.
  std::map<u32, u32> m_tick_hashes;
  void HashCapturedTick(u32 tick);
  void EmitSnapshotChecksumsLocked();
  void CompareChecksumLocked(u32 tick, u8 peer_mask, u32 local_crc, u32 remote_crc);

  // --- transport
  std::vector<std::unique_ptr<PeerLink>> m_peers;  // host: N-1 clients; client: {host}
  std::thread m_net_thread;                        // session setup, then idle
  Common::Flag m_running;
  // Sends are ALWAYS deferred onto a peer's queue and performed by that peer's
  // send thread. A blocking send while holding m_frames_lock deadlocks
  // distributed-ly: the peer's receive thread needs ITS m_frames_lock to process
  // our message, so it stops draining its socket, our send never completes, and
  // both peers park symmetrically (target9 wedge: CPU thread sampled inside the
  // EXI CR MMIO write for 54 minutes). No thread ever blocks on the network while
  // holding m_frames_lock -- and with N peers, no console can stall another's
  // link either.
  void EnqueueMessage(u8 type, u8 mask, u32 tick, const u8* payload, u16 len);

  // --- wedge diagnostics: logs the emulated PC/LR while the exchange is
  // stalled. The disc5/disc9 wedge signature is the game running frames at
  // full speed but never reaching the exchange again; native samples only
  // show anonymous JIT blocks, so name the loop from the PPC side instead.
  // PC/LR reads are cross-thread and racy — diagnostic only.
  std::thread m_diag_thread;
  std::atomic<s64> m_last_exchange_us{0};
  void DiagThread();

  // --- simulated network conditions (testing only; 0 = disabled)
  int m_fake_latency_ms = 0;
  int m_fake_jitter_ms = 0;
  int m_fake_spike_pct = 0;
  int m_fake_spike_ms = 0;
  std::mt19937 m_jitter_rng{0xC0FFEE};
  bool SimulatingNetwork() const
  {
    return m_fake_latency_ms != 0 || m_fake_jitter_ms != 0 || m_fake_spike_pct != 0;
  }

  // --- lockstep pacing statistics (CPU thread only; no lock needed)
  //
  // The playability metric is the ACHIEVED TICK RATE, not the POLL stall. Even
  // on loopback a peer spends most of each frame waiting for the other peer's
  // next (throttled, 60Hz) frame, so a large stall is normal and says nothing
  // about speed. Stall percentiles remain useful for seeing jitter.
  std::chrono::steady_clock::time_point m_stall_start{};
  bool m_stall_active = false;
  std::vector<u32> m_stall_samples_us;
  std::chrono::steady_clock::time_point m_rate_window_start{};
  u32 m_rate_window_tick = 0;
  u64 m_rate_window_cycles = 0;  // CoreTiming cycles at window start
  void RecordStall(u32 micros);
  void ReportStalls();

  // --- rollback snapshot machinery (P4; CPU thread only)
  // Captures happen at CMD_SEND: the game is parked in the EXI transaction at
  // the top of tick m_serve_tick, so ring[T] = pre-state of tick T.
  MeleeRollbackState m_rollback;
  int m_torture = 0;
  u32 m_torture_interval = 120;
  u32 m_torture_depth = 4;
  u32 m_pending_replay = 0;  // ticks the game must replay; announced via POLL
  s64 m_verify_tick = -1;    // after replay, byte-verify live state vs ring[tick]
  // Match-activity gate: the game's periodic state checksums (CMD_CHECKSUM)
  // change only while a fight is simulating. Replay is only valid there —
  // menus/movies stream from disc (DVD/THP state is real-time, outside the
  // restorable region set) and wedge when re-run.
  u32 m_game_crc_last = 0;
  u32 m_game_crc_prev = 0;
  bool m_game_crc_seen = false;
  // Cross-peer forensics: first DESYNC arms a dump; the NEXT checksum
  // submission (same game tick on both peers, both parked at the exchange)
  // writes the live region set to <userdir>/desync-<tick>.bin for offline
  // host-vs-client byte diffing (scripts/diff-desync-dumps.py). Guarded by
  // m_frames_lock (armed from the net thread, consumed on the CPU thread).
  s64 m_dump_armed_tick = -1;
  bool m_desync_dumped = false;
  // Entry-tick forensics (per-tick oracle): the FIRST mismatching hash tick T
  // also queues a RING dump — ring[T] is the pre-state of the first divergent
  // tick, and ring[T-1] the last-agreed reference, identical capture points on
  // both peers. Queued under m_frames_lock (compare can run on the net
  // thread); written on the CPU thread at the next CMD_SEND, while the ring
  // still holds the ticks (detection lags T by ~latency << RING_SIZE).
  s64 m_ring_dump_tick = -1;
  bool m_ring_dumped = false;
  void MaybeDumpDesyncRing();
  bool MatchStateEvolving() const
  {
    return m_game_crc_seen && m_game_crc_last != m_game_crc_prev;
  }
  // Strict prediction gate: the major-scene watch says we are inside a real
  // VS match (major 0x02, minor MATCH/SUDDEN DEATH). MatchStateEvolving also
  // passes attract demos, whose surrounding scenes are memcard/DVD heavy — a
  // rollback there can erase the game's record of an already-delivered
  // completion and park both peers in the card/DVD wait loop forever
  // (target10b). Policy: rollback only inside real matches, lockstep
  // everywhere else.
  bool InRealMatch();
  bool m_in_match_gate = false;  // last InRealMatch() result, for edge logs
  // Rollback pacing (MeleeNetplay.MatchPacing): suspend Dolphin's speed
  // throttle while a replay burst executes so the replayed ticks' emulated
  // cycles are not charged against the wall-clock budget (they capped
  // confirmed ticks at ~35/s in target12c). While suspended the throttle
  // continuously re-anchors its reference, so re-enabling never demands
  // catch-up. Mode 2 suspends for the whole match (diagnostic ceiling).
  int m_match_pacing = 1;
  bool m_throttle_suspended = false;
  void SuspendThrottle(bool on);
  // Mode 4: pace serving against the 60Hz tick SCHEDULE instead of the wall
  // clock. Wall-anchored throttling makes the peers' rates interlock at
  // whatever rate the match started with (each peer's window-edge park lasts
  // until the OTHER sends more ticks -- any common rate is an equilibrium;
  // pace10 sat at 28.5/s with parks fully debt-forgiven). Suspending while
  // serving lags the schedule adds the missing restoring force toward 60.
  std::chrono::steady_clock::time_point m_sched_anchor_wall{};
  u32 m_sched_anchor_tick = 0;
  bool m_sched_valid = false;
  void UpdateSchedulePacing();
  // Quiet seed-write tracing (TraceSeedQuiet): memcheck hits append to this
  // ring instead of NOTICE-logging; dumped as seedtrace.txt at first desync.
  std::unique_ptr<MemCheckTraceRing> m_seed_trace_ring;
  bool m_seed_trace_dumped = false;
  void DumpSeedTraceRing(u32 desync_tick);
  // Burst cost instrumentation: wall time from a REPLAY directive to the
  // first post-replay POLL (= restore + all replayed tick bodies + their
  // re-captures). pace1 arithmetic said ~120ms/burst ~= depth x 16.7ms,
  // i.e. replayed ticks were still paying full throttle-paced frames --
  // these counters replace that inference with a measurement.
  std::chrono::steady_clock::time_point m_burst_start{};
  bool m_burst_active = false;
  u64 m_burst_count = 0;
  u64 m_burst_us_total = 0;
  u64 m_burst_depth_total = 0;
  u64 m_burst_start_cycles = 0;
  u64 m_burst_cycles_total = 0;  // emulated cycles consumed inside bursts
  u64 m_prev_sleep_us = 0;       // per-rate-window deltas
  u64 m_prev_burst_cycles = 0;
  u64 m_prev_burst_us = 0;
  // Per-serve emulated-cost histogram (POLL READY to POLL READY, in VI
  // fields, rounded) with rollback proximity: in-match window-mode serves
  // average ~1.8 fields while lockstep holds 1.00 -- the extra cost tracks
  // the rollback rate (~6 fields per rollback) yet lands OUTSIDE the
  // measured burst window. This splits post-rollback ticks from steady
  // -state ticks.
  u64 m_last_serve_cycles = 0;
  u32 m_ticks_since_rollback = 1000;
  u32 m_hist_fields[3] = {};      // serves costing 1 / 2 / >=3 fields
  u32 m_hist_2plus_near_rb = 0;   // 2+-field serves within 6 ticks of a rollback
  void NoteServeCycles();
  u64 m_restore_us_total = 0;  // R1 restore memcpy only
  u64 m_suspend_engaged = 0;   // times mode 1 actually flipped the throttle off
  void MaybeTorture();
  // No emulator-level async I/O toward the game is pending (ARAM DMA, DVD
  // reads, DI command completions). Restore/rollback must not run otherwise:
  // the rolled-back game re-waits on a completion that will never re-fire.
  bool AsyncIOQuiescent() const;
  // (Re-)install the TraceSeedWrites watchpoint if configured and missing.
  void EnsureSeedTraceArmed(const char* when);
  u32 m_trace_seed_addr = 0;

  // --- prediction + rollback (R1; window > 0 enables, 0 = pure lockstep)
  //
  // Invariants (TCP delivers MSG_INPUTS in tick order, so real remote data
  // arrives in order):
  //  - every tick < m_confirmed_frontier was simulated with real remote
  //    inputs or a prediction later proven byte-identical;
  //  - the first known mispredict is always exactly at the frontier, and
  //    ring[frontier] was captured before any wrong input was injected, so
  //    it is always a clean rollback target.
  // All fields below are guarded by m_frames_lock; validation runs lazily on
  // the CPU thread at POLL (never on the net thread) so the fake-latency
  // visibility model applies to mispredict detection too.
  u32 m_window = 0;
  u32 m_confirmed_frontier = 0;
  std::map<u32, std::array<std::array<u8, PAD_BYTES>, 4>> m_speculative;  // tick -> served pads
  bool m_rollback_needed = false;  // mispredict at the frontier awaits restore
  u32 m_replay_serving = 0;        // RECVs left in an in-flight replay window
  // Scene-barrier fence: true while the game's outgoing pad block carries the
  // barrier ready bit (nw_SceneBarrier stamp, byte 0x42 bit 0 of the first
  // owned port's block). While armed, prediction is suppressed so no
  // speculative window can span a barrier release (see the predict gate).
  bool m_barrier_armed = false;
  // Match-end quiesce stamp (flag byte 0x42 bit 7): GAME! through scene
  // exit. Same treatment as an armed barrier fence.
  bool m_quiesce_stamped = false;
  // Polls parked draining outstanding speculation behind an armed barrier
  // stamp (see the barrier drain block in CMD_POLL): once a barrier flag is
  // up, no unconfirmed tick may remain behind it, or a mispredict rollback
  // can erase the game's scene-exit request after the stamp is on the wire.
  u64 m_barrier_drain_polls = 0;
  // Stall-diagnostic watch addresses (Dolphin.MeleeNetplay.DiagWatch).
  std::vector<u32> m_diag_watches;
  // Payload fence (see CMD_POLL): prediction quiesced while async payloads
  // are landing in restored memory (scene preloads); renewed per journaled
  // delivery, expires this many ticks after the last one.
  static constexpr u32 PAYLOAD_FENCE_TICKS = 90;
  u64 m_last_delivery_seq = 0;
  u32 m_payload_fence_until = 0;
  // stats (CPU thread only)
  u64 m_predicted_ticks = 0;
  u64 m_validated_ok = 0;
  u32 m_rollback_count = 0;
  u32 m_rollback_depth_max = 0;
  u32 m_rollback_refused_scene = 0;
  u32 m_checksums_skipped = 0;
  u32 m_restore_refused_io = 0;  // deferred/skipped restores: async I/O in flight
  u32 m_restore_refused_epoch = 0;  // R1: refused, DVD completion in window; torture: skipped
  u32 m_aram_redelivered = 0;       // ARAM completions re-raised into restored state
  u32 m_rollback_io_defer_streak = 0;  // consecutive in-flight defers (escape valve)
  // Park targets: completion-counter values that mean "everything in flight
  // when this rollback began parking has completed" (see the rollback path).
  u64 m_park_aram_target = 0;
  u64 m_park_dvd_target = 0;
  u64 m_park_di_target = 0;

  // --- jukebox (host-side BGM; see MeleeJukebox.h). Lazily created on the
  // first jukebox command when MeleeNetplay.Jukebox is enabled (or eagerly by
  // the JukeboxDebugTrack dev hook); owned by the device so it can never
  // outlive the session. nullptr when disabled or creation failed.
  std::unique_ptr<MeleeJukebox> m_jukebox;
  bool m_jukebox_disabled = false;  // creation refused/failed; don't retry
  MeleeJukebox* GetJukebox();

  // --- CPU-thread transaction state
  u8 m_command = CMD_ID;
  u32 m_serve_tick = 0;
  bool m_warned_savestate = false;
  // Index of the "rng-seed" watch in the region table, resolved lazily at the
  // first RECV; -1 = not looked up yet, -2 = table has none (stop looking).
  int m_seed_watch = -1;
};
}  // namespace ExpansionInterface
