// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Paravirtual EXI device for engine-level Melee netplay (lockstep-from-boot).
// The modified game (doldecomp/melee + src/melee/nw module) exchanges
// frame-indexed PADStatus blocks through this device; the device owns the real
// transport (TCP direct connect for the MVP). See
// aot-dolphin-helper/research/melee-netplay-design.md for the protocol.

#pragma once

#include <SFML/Network.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/EXI/MeleeRollbackState.h"

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
  };

  static constexpr u32 DEVICE_ID = 0x4D4E4554;  // 'MNET'; unknown to CARD -> "no card"
  static constexpr u8 PROTO_VERSION = 3;        // v3: POLL status word + replay serving
  static constexpr u32 PAD_BYTES = 0x44;  // sizeof(HSD_PadStatus): full post-transform entry

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
    MSG_HELLO = 0x10,   // host->client {u8 ver, u8 host_mask, u8 delay, u8 pad, u32be seed}
    MSG_HELLO_ACK = 0x11,  // client->host {u8 ver, u8 client_mask}
    MSG_INPUTS = 0x01,     // {tick} payload: 68B per set mask bit, ascending port order
    MSG_CHECKSUM = 0x02,   // {tick} payload: 4B crc
  };

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

  void NetThread();
  bool EstablishSession(sf::TcpSocket& sock);
  void HandleMessage(u8 type, u8 mask, u32 tick, const u8* payload, u32 len);
  void SendInputs(u32 tick, const u8* pads);
  void SendMessageRaw(u8 type, u8 mask, u32 tick, const u8* payload, u16 len);
  bool FrameReady(u32 tick);
  bool RemoteArrivedLocked(u32 tick) const;  // real remote data present & visible
  void ValidateSpeculationLocked();          // advance frontier; detect mispredicts
  void PredictIntoLocked(u32 tick);          // fill remote half from newest confirmed

  // --- session (written by net thread before m_session_ready, read-only after)
  std::atomic<bool> m_session_ready{false};
  std::atomic<bool> m_session_failed{false};
  bool m_is_host = false;
  u8 m_local_mask = 0x01;
  u8 m_remote_mask = 0x02;
  u8 m_delay = 2;
  u32 m_seed = 0;

  // --- input frames
  std::mutex m_frames_lock;
  std::map<u32, Frame> m_frames;
  std::map<u32, u32> m_local_crcs;
  std::map<u32, u32> m_remote_crcs;

  // --- transport
  sf::TcpSocket m_socket;
  std::mutex m_send_lock;
  std::thread m_net_thread;
  Common::Flag m_running;

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
  bool MatchStateEvolving() const
  {
    return m_game_crc_seen && m_game_crc_last != m_game_crc_prev;
  }
  void MaybeTorture();

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
  // stats (CPU thread only)
  u64 m_predicted_ticks = 0;
  u64 m_validated_ok = 0;
  u32 m_rollback_count = 0;
  u32 m_rollback_depth_max = 0;
  u32 m_rollback_refused_scene = 0;
  u32 m_checksums_skipped = 0;

  // --- CPU-thread transaction state
  u8 m_command = CMD_ID;
  u32 m_serve_tick = 0;
  bool m_warned_savestate = false;
};
}  // namespace ExpansionInterface
