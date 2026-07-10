// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_DeviceMeleeNetplay.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Random.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace ExpansionInterface
{
namespace
{
u32 ReadBE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

void WriteBE32(u8* p, u32 v)
{
  p[0] = u8(v >> 24);
  p[1] = u8(v >> 16);
  p[2] = u8(v >> 8);
  p[3] = u8(v);
}

bool SendAll(sf::TcpSocket& sock, const u8* data, std::size_t len)
{
  std::size_t total = 0;
  while (total < len)
  {
    std::size_t sent = 0;
    const auto status = sock.send(data + total, len - total, sent);
    if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error)
      return false;
    total += sent;
  }
  return true;
}

bool RecvAll(sf::TcpSocket& sock, u8* data, std::size_t len, const Common::Flag& running)
{
  std::size_t total = 0;
  while (total < len)
  {
    if (!running.IsSet())
      return false;
    std::size_t got = 0;
    const auto status = sock.receive(data + total, len - total, got);
    if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error)
      return false;
    if (got == 0)
      Common::SleepCurrentThread(1);
    total += got;
  }
  return true;
}
}  // namespace

CEXIMeleeNetplay::CEXIMeleeNetplay(Core::System& system) : IEXIDevice(system)
{
  m_is_host = Config::Get(Config::MAIN_MELEE_NETPLAY_IS_HOST);
  m_delay = static_cast<u8>(Config::Get(Config::MAIN_MELEE_NETPLAY_DELAY));

  const int ports_cfg = Config::Get(Config::MAIN_MELEE_NETPLAY_LOCAL_PORTS);
  m_local_mask = ports_cfg != 0 ? static_cast<u8>(ports_cfg) : (m_is_host ? 0x01 : 0x02);

  m_trace_seed_addr = Config::Get(Config::MAIN_MELEE_NETPLAY_TRACE_SEED_WRITES);
  EnsureSeedTraceArmed("device init");

  m_fake_latency_ms = std::max(0, Config::Get(Config::MAIN_MELEE_NETPLAY_FAKE_LATENCY_MS));
  m_fake_jitter_ms = std::max(0, Config::Get(Config::MAIN_MELEE_NETPLAY_FAKE_JITTER_MS));
  m_fake_spike_pct = std::clamp(Config::Get(Config::MAIN_MELEE_NETPLAY_FAKE_SPIKE_PCT), 0, 100);
  m_fake_spike_ms = std::max(0, Config::Get(Config::MAIN_MELEE_NETPLAY_FAKE_SPIKE_MS));
  if (SimulatingNetwork())
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE,
                 "MeleeNetplay: SIMULATING {} ms one-way latency (+0..{} ms jitter, "
                 "{}% of frames +{} ms spike). Delay window is {} frames ~= {} ms.",
                 m_fake_latency_ms, m_fake_jitter_ms, m_fake_spike_pct, m_fake_spike_ms, m_delay,
                 m_delay * 100 / 6);
  }

  if (m_is_host)
  {
    m_seed = Config::Get(Config::MAIN_MELEE_NETPLAY_SEED);
    if (m_seed == 0)
      m_seed = Common::Random::GenerateValue<u32>();
    // Host imposes the rollback window like it imposes delay. Clamp well
    // below RING_SIZE: replays re-read input history and ring snapshots.
    m_window = static_cast<u32>(std::clamp(Config::Get(Config::MAIN_MELEE_NETPLAY_WINDOW), 0,
                                           int(MeleeRollbackState::RING_SIZE) / 2));
  }

  const std::string region_table = Config::Get(Config::MAIN_MELEE_NETPLAY_REGION_TABLE);
  if (!region_table.empty())
  {
    std::string err;
    if (!m_rollback.LoadRegionTable(region_table, &err))
    {
      // A misloaded table must be loud: silently running without snapshots
      // would make a torture soak look like a flawless pass.
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeRollback: {}", err);
    }
    m_torture = Config::Get(Config::MAIN_MELEE_NETPLAY_TORTURE);
    m_torture_interval =
        static_cast<u32>(std::max(1, Config::Get(Config::MAIN_MELEE_NETPLAY_TORTURE_INTERVAL)));
    m_torture_depth =
        static_cast<u32>(std::max(1, Config::Get(Config::MAIN_MELEE_NETPLAY_TORTURE_DEPTH)));
    if (m_torture != 0)
    {
      WARN_LOG_FMT(EXPANSIONINTERFACE,
                   "MeleeRollback: TORTURE mode {} (interval {} depth {})", m_torture,
                   m_torture_interval, m_torture_depth);
    }
  }

  m_running.Set();
  m_net_thread = std::thread(&CEXIMeleeNetplay::NetThread, this);
}

CEXIMeleeNetplay::~CEXIMeleeNetplay()
{
  m_running.Clear();
  m_socket.disconnect();
  if (m_net_thread.joinable())
    m_net_thread.join();
}

bool CEXIMeleeNetplay::IsPresent() const
{
  return true;
}

void CEXIMeleeNetplay::DoState(PointerWrap& p)
{
  if (!m_warned_savestate)
  {
    m_warned_savestate = true;
    ERROR_LOG_FMT(EXPANSIONINTERFACE,
                  "MeleeNetplay: savestates are not supported during a netplay session");
  }
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

bool CEXIMeleeNetplay::EstablishSession(sf::TcpSocket& sock)
{
  sock.setBlocking(true);
  if (m_is_host)
  {
    u8 hello[9];
    hello[0] = MSG_HELLO;
    hello[1] = PROTO_VERSION;
    hello[2] = m_local_mask;
    hello[3] = m_delay;
    WriteBE32(&hello[4], m_seed);
    hello[8] = static_cast<u8>(m_window);
    if (!SendAll(sock, hello, sizeof(hello)))
      return false;

    u8 ack[3];
    sock.setBlocking(false);
    if (!RecvAll(sock, ack, sizeof(ack), m_running))
      return false;
    if (ack[0] != MSG_HELLO_ACK || ack[1] != PROTO_VERSION)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: bad HELLO_ACK (type {:#x} ver {})", ack[0],
                    ack[1]);
      return false;
    }
    m_remote_mask = ack[2];
  }
  else
  {
    u8 hello[9];
    sock.setBlocking(false);
    if (!RecvAll(sock, hello, sizeof(hello), m_running))
      return false;
    if (hello[0] != MSG_HELLO || hello[1] != PROTO_VERSION)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: bad HELLO (type {:#x} ver {})", hello[0],
                    hello[1]);
      return false;
    }
    m_remote_mask = hello[2];
    m_delay = hello[3];
    m_seed = ReadBE32(&hello[4]);
    m_window = hello[8];
    // Host assigns; if it claims our default port, take the other one.
    if ((m_local_mask & m_remote_mask) != 0)
      m_local_mask = m_remote_mask == 0x01 ? 0x02 : 0x01;

    u8 ack[3] = {MSG_HELLO_ACK, PROTO_VERSION, m_local_mask};
    sock.setBlocking(true);
    if (!SendAll(sock, ack, sizeof(ack)))
      return false;
    sock.setBlocking(false);
  }

  if ((m_local_mask & m_remote_mask) != 0)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: overlapping port masks {:#x}/{:#x}",
                  m_local_mask, m_remote_mask);
    return false;
  }
  return true;
}

void CEXIMeleeNetplay::NetThread()
{
  Common::SetCurrentThreadName("MeleeNetplay");

  const u16 port = static_cast<u16>(Config::Get(Config::MAIN_MELEE_NETPLAY_PORT));

  if (m_is_host)
  {
    sf::TcpListener listener;
    if (listener.listen(port) != sf::Socket::Status::Done)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: failed to listen on port {}", port);
      m_session_failed = true;
      return;
    }
    Core::DisplayMessage(fmt::format("MeleeNetplay: waiting for peer on port {}", port), 8000);
    listener.setBlocking(false);
    while (m_running.IsSet())
    {
      if (listener.accept(m_socket) == sf::Socket::Status::Done)
        break;
      Common::SleepCurrentThread(5);
    }
  }
  else
  {
    const std::string remote = Config::Get(Config::MAIN_MELEE_NETPLAY_REMOTE_HOST);
    Core::DisplayMessage(fmt::format("MeleeNetplay: connecting to {}:{}", remote, port), 8000);
    const auto remote_addr = sf::IpAddress::resolve(remote);
    if (!remote_addr)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: cannot resolve host '{}'", remote);
      m_session_failed = true;
      return;
    }
    m_socket.setBlocking(true);
    while (m_running.IsSet())
    {
      if (m_socket.connect(*remote_addr, port, sf::seconds(2)) == sf::Socket::Status::Done)
        break;
      Common::SleepCurrentThread(500);
    }
  }

  if (!m_running.IsSet())
    return;

  if (!EstablishSession(m_socket))
  {
    m_session_failed = true;
    return;
  }

  // Pre-seed the delay window with neutral pads so the game's first POLLs
  // (ticks 0..delay-1, before either peer's first SEND lands) never deadlock.
  {
    std::lock_guard lk(m_frames_lock);
    for (u32 t = 0; t < m_delay; t++)
      m_frames[t].have_mask = 0xFF;
  }

  m_session_ready = true;
  Core::DisplayMessage(fmt::format("MeleeNetplay: session up (local ports {:#x}, delay {}, "
                                   "window {})",
                                   m_local_mask, m_delay, m_window),
                       8000);
  if (m_window != 0)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: prediction+rollback ON (window {})", m_window);
  }

  // Receive loop
  while (m_running.IsSet())
  {
    u8 header[8];
    if (!RecvAll(m_socket, header, sizeof(header), m_running))
      break;
    const u8 type = header[0];
    const u8 mask = header[1];
    const u16 len = static_cast<u16>((header[2] << 8) | header[3]);
    const u32 tick = ReadBE32(&header[4]);
    if (len > 4 * PAD_BYTES)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: oversized message ({} bytes)", len);
      break;
    }
    u8 payload[4 * PAD_BYTES];
    if (len != 0 && !RecvAll(m_socket, payload, len, m_running))
      break;

    HandleMessage(type, mask, tick, payload, len);
  }

  if (m_running.IsSet())
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: peer disconnected");
    Core::DisplayMessage("MeleeNetplay: peer disconnected", 10000);
  }
}

void CEXIMeleeNetplay::HandleMessage(u8 type, u8 mask, u32 tick, const u8* payload, u32 len)
{
  switch (type)
  {
  case MSG_INPUTS:
  {
    std::lock_guard lk(m_frames_lock);
    Frame& frame = m_frames[tick];
    u32 off = 0;
    for (u32 port = 0; port < 4; port++)
    {
      if ((mask & (1 << port)) == 0)
        continue;
      if (off + PAD_BYTES > len)
        return;
      std::memcpy(frame.pads[port].data(), payload + off, PAD_BYTES);
      off += PAD_BYTES;
    }
    frame.have_mask |= mask;
    if (SimulatingNetwork())
    {
      int hold = m_fake_latency_ms;
      if (m_fake_jitter_ms > 0)
        hold += std::uniform_int_distribution<int>(0, m_fake_jitter_ms)(m_jitter_rng);
      if (m_fake_spike_pct > 0 &&
          std::uniform_int_distribution<int>(1, 100)(m_jitter_rng) <= m_fake_spike_pct)
      {
        hold += m_fake_spike_ms;
      }
      frame.visible_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(hold);
    }
    break;
  }
  case MSG_CHECKSUM:
  {
    if (len < 4)
      return;
    const u32 remote_crc = ReadBE32(payload);
    std::lock_guard lk(m_frames_lock);
    m_remote_crcs[tick] = remote_crc;
    const auto local = m_local_crcs.find(tick);
    if (local != m_local_crcs.end())
    {
      if (local->second != remote_crc)
      {
        ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: DESYNC at tick {} ({:08x} != {:08x})",
                      tick, local->second, remote_crc);
        Core::DisplayMessage(fmt::format("MeleeNetplay: DESYNC at tick {}", tick), 20000);
        if (!m_desync_dumped && m_dump_armed_tick < 0)
          m_dump_armed_tick = tick;
      }
      else
      {
        // A matching checksum only demonstrates determinism if the state it
        // hashes is actually changing: a constant crc means both peers are
        // parked on a static screen, not that the cores agree under load.
        INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: checksum ok tick={} crc={:08x}", tick,
                     remote_crc);
      }
      m_local_crcs.erase(m_local_crcs.begin(), m_local_crcs.upper_bound(tick));
      m_remote_crcs.erase(m_remote_crcs.begin(), m_remote_crcs.upper_bound(tick));
    }
    break;
  }
  default:
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: unknown message type {:#x}", type);
    break;
  }
}

void CEXIMeleeNetplay::SendMessageRaw(u8 type, u8 mask, u32 tick, const u8* payload, u16 len)
{
  u8 buf[8 + 4 * PAD_BYTES];
  buf[0] = type;
  buf[1] = mask;
  buf[2] = u8(len >> 8);
  buf[3] = u8(len);
  WriteBE32(&buf[4], tick);
  if (len != 0)
    std::memcpy(&buf[8], payload, len);

  std::lock_guard lk(m_send_lock);
  m_socket.setBlocking(true);
  if (!SendAll(m_socket, buf, 8u + len))
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: send failed (tick {})", tick);
  m_socket.setBlocking(false);
}

void CEXIMeleeNetplay::SendInputs(u32 tick, const u8* pads)
{
  // Each console's player uses its own physical controller port 1 (index 0),
  // but owns whichever netplay port the handshake assigned (client owns port
  // 1). So local physical pads map in ascending order onto the owned netplay
  // ports: physical 0 -> lowest owned port, and so on. For the host (owns port
  // 0) this is the identity mapping.
  u8 payload[4 * PAD_BYTES];
  u16 len = 0;
  {
    std::lock_guard lk(m_frames_lock);
    Frame& frame = m_frames[tick];
    u32 physical = 0;
    for (u32 port = 0; port < 4; port++)
    {
      if ((m_local_mask & (1 << port)) == 0)
        continue;
      const u8* src = pads + physical * PAD_BYTES;
      std::memcpy(frame.pads[port].data(), src, PAD_BYTES);
      std::memcpy(payload + len, src, PAD_BYTES);
      len += PAD_BYTES;
      physical++;
    }
    frame.have_mask |= m_local_mask;
  }
  // Log non-neutral input. A peer whose controller never reaches the exchange
  // (wrong port mapping, dead input pipe) otherwise looks identical to a peer
  // whose player is simply holding still.
  if (len >= 4 && (pads[0] | pads[1] | pads[2] | pads[3]) != 0)
  {
    const u32 btn = (u32(pads[0]) << 24) | (u32(pads[1]) << 16) | (u32(pads[2]) << 8) | u32(pads[3]);
    INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: input tick={} localmask={:#x} button={:#06x}",
                 tick, m_local_mask, btn);
  }
  SendMessageRaw(MSG_INPUTS, m_local_mask, tick, payload, len);
}

void CEXIMeleeNetplay::RecordStall(u32 micros)
{
  m_stall_samples_us.push_back(micros);
  // Report once per ~10s of game time (600 ticks at 60Hz).
  if (m_stall_samples_us.size() >= 600)
    ReportStalls();
}

void CEXIMeleeNetplay::ReportStalls()
{
  if (m_stall_samples_us.empty())
    return;

  const auto now = std::chrono::steady_clock::now();
  if (m_rate_window_start.time_since_epoch().count() == 0)
  {
    // First window: no elapsed baseline yet, so only arm the counters.
    m_rate_window_start = now;
    m_rate_window_tick = m_serve_tick;
    m_stall_samples_us.clear();
    return;
  }

  std::vector<u32> sorted = m_stall_samples_us;
  std::sort(sorted.begin(), sorted.end());
  const size_t n = sorted.size();
  const auto pct = [&](double p) { return sorted[std::min(n - 1, size_t(p * n))]; };

  // THE playability number: ticks actually advanced per wall-clock second.
  // Lockstep never desyncs under latency, it just runs the whole session
  // slower -- so achieved rate against 60Hz is exactly what a player feels.
  //
  // Note the POLL stall is NOT that number. Even at zero latency a peer waits
  // most of each frame for the other peer's next throttled frame, so stalls
  // near a frame budget are normal at full speed.
  const double elapsed_s =
      std::chrono::duration_cast<std::chrono::duration<double>>(now - m_rate_window_start).count();
  const double rate = elapsed_s > 0.0 ? double(m_serve_tick - m_rate_window_tick) / elapsed_s : 0.0;

  INFO_LOG_FMT(EXPANSIONINTERFACE,
               "MeleeNetplay: rate={:.1f} ticks/s ({:.0f}% of 60Hz) | stall us over {} ticks: "
               "p50={} p90={} p99={} max={}",
               rate, 100.0 * rate / 60.0, n, pct(0.50), pct(0.90), pct(0.99), sorted[n - 1]);
  if (m_window != 0)
  {
    INFO_LOG_FMT(EXPANSIONINTERFACE,
                 "MeleeNetplay: rollback stats: predicted={} validated_ok={} rollbacks={} "
                 "max_depth={} refused_scene={} refused_io={} checksums_skipped={} "
                 "frontier_lag={}",
                 m_predicted_ticks, m_validated_ok, m_rollback_count, m_rollback_depth_max,
                 m_rollback_refused_scene, m_restore_refused_io, m_checksums_skipped,
                 m_serve_tick - m_confirmed_frontier);
  }

  m_stall_samples_us.clear();
  m_rate_window_start = now;
  m_rate_window_tick = m_serve_tick;
}

bool CEXIMeleeNetplay::FrameReady(u32 tick)
{
  const u8 need = m_local_mask | m_remote_mask;
  std::lock_guard lk(m_frames_lock);
  const auto it = m_frames.find(tick);
  if (it == m_frames.end() || (it->second.have_mask & need) != need)
    return false;
  // Simulated latency: the bytes are here, but pretend they are still in flight.
  if (it->second.visible_at.time_since_epoch().count() != 0 &&
      std::chrono::steady_clock::now() < it->second.visible_at)
  {
    return false;
  }
  return true;
}

bool CEXIMeleeNetplay::RemoteArrivedLocked(u32 tick) const
{
  const auto it = m_frames.find(tick);
  if (it == m_frames.end() || (it->second.have_mask & m_remote_mask) != m_remote_mask)
    return false;
  // Fake latency delays *visibility*: for mispredict detection too, or the
  // simulation would validate predictions against data still "in flight".
  if (it->second.visible_at.time_since_epoch().count() != 0 &&
      std::chrono::steady_clock::now() < it->second.visible_at)
  {
    return false;
  }
  return true;
}

// Advance the confirmed frontier over ticks whose real remote inputs have
// arrived, validating any prediction served for them. Runs on the CPU thread
// only. A mismatch stops the frontier: that tick is the rollback target.
void CEXIMeleeNetplay::ValidateSpeculationLocked()
{
  while (m_confirmed_frontier < m_serve_tick && !m_rollback_needed)
  {
    const u32 t = m_confirmed_frontier;
    if (!RemoteArrivedLocked(t))
      break;
    const auto spec = m_speculative.find(t);
    if (spec != m_speculative.end())
    {
      const Frame& frame = m_frames[t];
      bool match = true;
      for (u32 port = 0; port < 4 && match; port++)
      {
        if ((m_remote_mask & (1 << port)) == 0)
          continue;
        match = std::memcmp(frame.pads[port].data(), spec->second[port].data(), PAD_BYTES) == 0;
      }
      m_speculative.erase(spec);
      if (!match)
      {
        m_rollback_needed = true;
        break;  // frontier stays at t: the rollback target
      }
      m_validated_ok++;
    }
    m_confirmed_frontier = t + 1;
  }
  // Nothing speculative below the frontier: keep the map from growing if the
  // peer vanished mid-speculation.
  m_speculative.erase(m_speculative.begin(), m_speculative.lower_bound(m_confirmed_frontier));
}

// Fill tick's remote half with the newest confirmed remote inputs
// (repeat-last-received, the standard GGPO/Slippi predictor) and remember
// exactly what was served for later validation. have_mask is NOT touched:
// its remote bits keep meaning "real data arrived".
void CEXIMeleeNetplay::PredictIntoLocked(u32 tick)
{
  const u32 src_tick = m_confirmed_frontier == 0 ? 0 : m_confirmed_frontier - 1;
  const auto src = m_frames.find(src_tick);
  Frame& frame = m_frames[tick];
  auto& served = m_speculative[tick];
  for (u32 port = 0; port < 4; port++)
  {
    if ((m_remote_mask & (1 << port)) == 0)
      continue;
    if (src != m_frames.end())
      frame.pads[port] = src->second.pads[port];
    else
      frame.pads[port].fill(0);
    served[port] = frame.pads[port];
  }
  m_predicted_ticks++;
}

// ---------------------------------------------------------------------------
// EXI interface (CPU thread)
// ---------------------------------------------------------------------------

void CEXIMeleeNetplay::ImmWrite(u32 data, u32 size)
{
  m_command = static_cast<u8>(data >> 24);
}

u32 CEXIMeleeNetplay::ImmRead(u32 size)
{
  switch (m_command)
  {
  case CMD_ID:
    return DEVICE_ID;
  case CMD_POLL:
  {
    // Replay directive outranks readiness: memory has already been restored,
    // so the game must consume the replayed ticks before anything else.
    if (m_pending_replay != 0)
    {
      const u32 k = m_pending_replay;
      m_pending_replay = 0;
      m_replay_serving = k;
      return (u32(POLL_REPLAY) << 24) | k;
    }
    if (m_window != 0)
    {
      std::lock_guard lk(m_frames_lock);
      ValidateSpeculationLocked();
      if (m_rollback_needed)
      {
        const u32 target = m_confirmed_frontier;
        const u32 depth = m_serve_tick - target;
        if (!AsyncIOQuiescent())
        {
          // In-flight ARAM/DVD transfer (see AsyncIOQuiescent). Unlike the
          // scene case this is recoverable: the transfer completes in bounded
          // emulated time, so leave m_rollback_needed set and retry at the
          // next POLL — the game plays on predictions a tick longer.
          m_restore_refused_io++;
        }
        else if (!m_rollback.IsLoaded() || !m_rollback.SameScene(m_system, target))
        {
          // Unrecoverable: the mispredicted ticks straddle a scene change (or
          // there is no snapshot machinery). Accept the divergence loudly —
          // the checksum oracle downstream will scream if it mattered.
          ERROR_LOG_FMT(EXPANSIONINTERFACE,
                        "MeleeNetplay: ROLLBACK REFUSED at tick {} depth {} (scene changed)",
                        m_serve_tick, depth);
          m_rollback_refused_scene++;
          m_rollback_needed = false;
          m_confirmed_frontier++;  // treat the mismatched tick as confirmed-wrong
        }
        else if (m_rollback.Restore(m_system, target))
        {
          // Ticks in the window whose real inputs are still missing get a
          // fresh prediction (newer information than the one they were first
          // served with); the rest replay with the real data now in m_frames.
          for (u32 t = target; t < m_serve_tick; t++)
          {
            if (!RemoteArrivedLocked(t))
              PredictIntoLocked(t);
            else
              m_speculative.erase(t);
          }
          INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: ROLLBACK tick {} -> {} (replay {})",
                       m_serve_tick, target, depth);
          m_serve_tick = target;
          m_replay_serving = depth;
          m_rollback_count++;
          m_rollback_depth_max = std::max(m_rollback_depth_max, depth);
          m_rollback_needed = false;
          return (u32(POLL_REPLAY) << 24) | depth;
        }
        else
        {
          ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: ROLLBACK restore failed at tick {}",
                        m_serve_tick);
          m_rollback_needed = false;
          m_confirmed_frontier++;
        }
      }
    }
    if (FrameReady(m_serve_tick))
    {
      if (m_stall_active)
      {
        const auto waited = std::chrono::steady_clock::now() - m_stall_start;
        RecordStall(static_cast<u32>(
            std::chrono::duration_cast<std::chrono::microseconds>(waited).count()));
        m_stall_active = false;
      }
      else
      {
        RecordStall(0);  // peer's inputs were already here: zero stall
      }
      return u32(POLL_READY) << 24;
    }
    // Remote inputs late: predict instead of stalling, if allowed. Gates:
    // window budget vs the confirmed frontier; fights only (predicting
    // through menus/movies is pointless and, worse, a rollback there is
    // unrecoverable — same disc-streaming reasoning as the torture gate);
    // local inputs must already be looped back (they always are: the game
    // SENDs tick+delay before polling tick).
    if (m_window != 0 && m_rollback.IsLoaded() && MatchStateEvolving())
    {
      std::lock_guard lk(m_frames_lock);
      const u32 ahead = m_serve_tick - m_confirmed_frontier;
      const auto it = m_frames.find(m_serve_tick);
      const bool local_here =
          it != m_frames.end() && (it->second.have_mask & m_local_mask) == m_local_mask;
      if (ahead < m_window && local_here)
      {
        PredictIntoLocked(m_serve_tick);
        RecordStall(0);
        m_stall_active = false;
        return u32(POLL_READY) << 24;
      }
    }
    if (!m_stall_active)
    {
      m_stall_start = std::chrono::steady_clock::now();
      m_stall_active = true;
    }
    // Be polite to the host CPU while the game spins on us.
    Common::SleepCurrentThread(1);
    return u32(POLL_WAIT) << 24;
  }
  default:
    return 0;
  }
}

void CEXIMeleeNetplay::DMAWrite(u32 address, u32 size)
{
  auto& memory = m_system.GetMemory();
  u8 buf[320];
  if (size > sizeof(buf))
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: oversized DMAWrite ({} bytes)", size);
    return;
  }
  memory.CopyFromEmu(buf, address, size);

  switch (m_command)
  {
  case CMD_SEND:
    if (size >= 4 + 4 * PAD_BYTES)
    {
      SendInputs(ReadBE32(buf), buf + 4);
      EnsureSeedTraceArmed("CMD_SEND");
      // Snapshot point: the game is parked in this transaction at the top of
      // tick m_serve_tick, before that tick's inputs are injected or its
      // logic runs — ring[T] is the pre-state of tick T.
      if (m_rollback.IsLoaded())
      {
        m_rollback.Capture(m_system, m_serve_tick);
        if (m_rollback.TotalCaptures() % 600 == 0)
        {
          INFO_LOG_FMT(EXPANSIONINTERFACE,
                       "MeleeRollback: {} captures, {:.2f} MB each, last {} us",
                       m_rollback.TotalCaptures(),
                       m_rollback.SnapshotBytes() / (1024.0 * 1024.0),
                       m_rollback.LastCaptureMicros());
        }
        MaybeTorture();
      }
    }
    break;
  case CMD_CHECKSUM:
    if (size >= 8)
    {
      const u32 tick = ReadBE32(buf);
      const u32 crc = ReadBE32(buf + 4);
      m_game_crc_prev = m_game_crc_seen ? m_game_crc_last : crc;
      m_game_crc_last = crc;
      m_game_crc_seen = true;
      // A desync armed a forensic dump: the first checksum AFTER the desynced
      // tick is the same parked game tick on both peers, so their region
      // dumps are directly byte-comparable offline.
      {
        bool do_dump = false;
        {
          std::lock_guard lk(m_frames_lock);
          if (m_dump_armed_tick >= 0 && !m_desync_dumped && tick > static_cast<u32>(m_dump_armed_tick))
          {
            m_desync_dumped = true;
            do_dump = true;
          }
        }
        if (do_dump && m_rollback.IsLoaded())
        {
          const std::string path =
              File::GetUserPath(D_USER_IDX) + fmt::format("desync-{}.bin", tick);
          const bool ok = m_rollback.DumpLive(m_system, path);
          ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: desync region dump {} -> {}",
                        ok ? "written" : "FAILED", path);
        }
      }
      // Confirmed-frontier policy: a checksum computed while any earlier tick
      // was still speculative may hash mispredicted state. Neither transmit
      // nor compare it — each peer skips independently, so a tick is
      // cross-checked iff BOTH peers considered it clean.
      if (m_window != 0)
      {
        std::lock_guard lk(m_frames_lock);
        ValidateSpeculationLocked();
        if (m_confirmed_frontier < tick || m_rollback_needed)
        {
          m_checksums_skipped++;
          break;
        }
      }
      for (const auto& watch : m_rollback.Watches())
      {
        INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: watch {}={:08x} tick={}", watch.label,
                     m_rollback.ReadWatch(m_system, watch), tick);
      }
      SendMessageRaw(MSG_CHECKSUM, 0, tick, buf + 4, 4);
      std::lock_guard lk(m_frames_lock);
      m_local_crcs[tick] = crc;
      const auto remote = m_remote_crcs.find(tick);
      if (remote != m_remote_crcs.end() && remote->second != crc)
      {
        ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: DESYNC at tick {} ({:08x} != {:08x})",
                      tick, crc, remote->second);
        Core::DisplayMessage(fmt::format("MeleeNetplay: DESYNC at tick {}", tick), 20000);
        if (!m_desync_dumped && m_dump_armed_tick < 0)
          m_dump_armed_tick = tick;
      }
    }
    break;
  default:
    break;
  }
}

void CEXIMeleeNetplay::EnsureSeedTraceArmed(const char* when)
{
  if (m_trace_seed_addr == 0)
    return;
  auto& memchecks = m_system.GetPowerPC().GetMemChecks();
  // Something between device init and gameplay emptied the list on earlier
  // attempts (0 MBP hits at full speed with the watchpoint "armed"), so
  // re-arm opportunistically and say WHEN it was found missing.
  if (memchecks.GetMemCheck(m_trace_seed_addr, 4) != nullptr)
    return;
  TMemCheck mc;
  mc.start_address = m_trace_seed_addr;
  mc.end_address = m_trace_seed_addr + 3;
  mc.is_ranged = true;
  mc.is_break_on_write = true;  // gates logging; break_on_hit=false so it never pauses
  mc.log_on_hit = true;
  mc.break_on_hit = false;
  memchecks.Add(std::move(mc));
  WARN_LOG_FMT(EXPANSIONINTERFACE,
               "MeleeNetplay: TRACING writes to {:08x} (armed at: {}; HasAny={} count={})",
               m_trace_seed_addr, when, memchecks.HasAny(), memchecks.GetMemChecks().size());
}

bool CEXIMeleeNetplay::AsyncIOQuiescent() const
{
  // Restoring memory while an ARAM DMA or DVD read is in flight rolls the
  // game's request bookkeeping back to before the request, but the
  // emulator-level completion interrupt belongs to the OLD request and will
  // not re-fire for the restored one — the game then waits on a callback
  // that never comes, outside the exchange, forever (observed ~1/200
  // tortures). All three signals are CoreTiming-event-backed, so a pending
  // transfer always completes in bounded emulated time; callers defer or
  // skip, never block.
  return !m_system.GetDSP().IsARAMDMAInProgress() &&
         !m_system.GetDVDThread().HasPendingReads() &&
         !m_system.GetDVDInterface().IsCommandPending();
}

void CEXIMeleeNetplay::MaybeTorture()
{
  // R0a (mode 1): restore the snapshot captured microseconds ago for THIS
  // tick. The write-back is byte-identical by construction, so any behavior
  // change (crash, checksum split, slowdown) is a bug in the copy plan or
  // the copy machinery itself — this validates addressing and measures cost
  // before the game-side replay path exists.
  if (m_torture == 1)
  {
    if (!m_rollback.Restore(m_system, m_serve_tick))
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeRollback: R0a restore failed at tick {}",
                    m_serve_tick);
    return;
  }
  // R0 (mode 2): every TortureInterval ticks, restore the pre-state of
  // (T - depth) and direct the game to replay those ticks from recorded
  // inputs. A perfect replay reconverges exactly; any missed region shows up
  // as a cross-peer checksum DESYNC within one checksum interval.
  if (m_torture == 2 && m_serve_tick >= m_torture_depth &&
      m_serve_tick % m_torture_interval == 0)
  {
    // Fights only: replaying menu/movie scenes wedges their disc-streaming
    // state machines (observed: opening movie frozen at chapter 0). Matches
    // are where rollback matters and where replay is sound.
    if (!MatchStateEvolving())
      return;
    // Don't compose with live speculation (window mode): a torture replay
    // through predicted ticks would verify against a pass the real inputs
    // may yet contradict.
    if (m_window != 0)
    {
      std::lock_guard lk(m_frames_lock);
      if (m_confirmed_frontier < m_serve_tick || !m_speculative.empty())
        return;
    }
    const u32 target = m_serve_tick - m_torture_depth;
    // Never roll back across a scene transition — scene loads happen outside
    // the replayable tick body (see MeleeRollbackState::SameScene). Real
    // rollback carries the same gate; a prediction window that straddles a
    // transition must fall back to a lockstep stall instead.
    if (!m_rollback.SameScene(m_system, target))
    {
      INFO_LOG_FMT(EXPANSIONINTERFACE,
                   "MeleeRollback: torture skipped at tick {} (scene changed in window)",
                   m_serve_tick);
    }
    else if (!AsyncIOQuiescent())
    {
      // See AsyncIOQuiescent: restoring under an in-flight ARAM/DVD transfer
      // wedges the game on a completion that never re-fires. Skip this
      // interval; the next one is a tick-count away.
      m_restore_refused_io++;
      INFO_LOG_FMT(EXPANSIONINTERFACE,
                   "MeleeRollback: torture skipped at tick {} (async I/O in flight: aram={} "
                   "dvdread={} dicmd={})",
                   m_serve_tick, m_system.GetDSP().IsARAMDMAInProgress(),
                   m_system.GetDVDThread().HasPendingReads(),
                   m_system.GetDVDInterface().IsCommandPending());
    }
    else if (m_rollback.Restore(m_system, target))
    {
      INFO_LOG_FMT(EXPANSIONINTERFACE,
                   "MeleeRollback: torture restore tick {} -> {} (replay {})", m_serve_tick,
                   target, m_torture_depth);
      m_serve_tick = target;
      m_pending_replay = m_torture_depth;
      m_verify_tick = static_cast<s64>(target) + m_torture_depth;
    }
    else
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeRollback: torture restore failed at tick {}",
                    m_serve_tick);
    }
  }
}

void CEXIMeleeNetplay::DMARead(u32 address, u32 size)
{
  auto& memory = m_system.GetMemory();

  switch (m_command)
  {
  case CMD_HANDSHAKE:
  {
    // Block until the session resolves; the game shows the boot screens meanwhile.
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + seconds(60);
    while (!m_session_ready && !m_session_failed && steady_clock::now() < deadline &&
           m_running.IsSet())
    {
      Common::SleepCurrentThread(5);
    }

    u8 blob[32] = {};
    WriteBE32(&blob[0], DEVICE_ID);
    blob[4] = m_session_ready ? 1 : 0;
    blob[5] = m_local_mask;
    blob[6] = m_delay;
    blob[7] = PROTO_VERSION;
    WriteBE32(&blob[8], m_seed);
    WriteBE32(&blob[12], 0);  // flags
    memory.CopyToEmu(address, blob, std::min<u32>(size, sizeof(blob)));
    break;
  }
  case CMD_RECV:
  {
    // Post-replay fidelity check: the game is back at the exchange point of
    // the tortured tick, where memory must byte-match the snapshot taken on
    // the first pass. Diffs name exactly the state replay failed to restore.
    if (m_verify_tick >= 0 && m_serve_tick == static_cast<u32>(m_verify_tick))
    {
      m_rollback.VerifyAgainstRing(m_system, static_cast<u32>(m_verify_tick));
      m_verify_tick = -1;
    }
    // Replayed ticks issue no SENDs, so re-capture their pre-states here (the
    // RECV entry is the same semantic point). Without this, ring slots inside
    // a corrected window would keep the mispredicted pass's state and a later
    // rollback into that window would restore garbage.
    if (m_replay_serving != 0)
    {
      if (m_rollback.IsLoaded())
        m_rollback.Capture(m_system, m_serve_tick);
      m_replay_serving--;
    }
    u8 pads[320] = {};
    {
      std::lock_guard lk(m_frames_lock);
      const auto it = m_frames.find(m_serve_tick);
      if (it != m_frames.end())
      {
        for (u32 port = 0; port < 4; port++)
          std::memcpy(pads + port * PAD_BYTES, it->second.pads[port].data(), PAD_BYTES);
        // Keep a rollback window of history — replays re-read these ticks.
        // (Pure lockstep pruned everything up to the serve tick here.)
        if (m_serve_tick > MeleeRollbackState::RING_SIZE)
        {
          m_frames.erase(m_frames.begin(),
                         m_frames.upper_bound(m_serve_tick - MeleeRollbackState::RING_SIZE));
        }
      }
      else
      {
        ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: RECV for unready tick {}", m_serve_tick);
      }
    }
    m_serve_tick++;
    memory.CopyToEmu(address, pads, std::min<u32>(size, sizeof(pads)));
    break;
  }
  default:
    break;
  }
}
}  // namespace ExpansionInterface
