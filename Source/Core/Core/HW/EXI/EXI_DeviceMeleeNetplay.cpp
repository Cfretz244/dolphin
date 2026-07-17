// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_DeviceMeleeNetplay.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <vector>

#include <SFML/Network.hpp>
#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/Random.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/HW/EXI/MeleeNetplayExternalTestPump.h"
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

// Device-owned SFML socket behind the ILinkStream seam -- the qualified Mac
// path. Send toggles the socket to blocking exactly like the old send sites
// did; Recv deliberately does NOT toggle, so the socket's blocking mode
// follows the same sequence it always has (non-blocking through the
// handshake, blocking from the first steady-state send onward). Close() from
// another thread is what unblocks a blocking receive, as before.
class TcpLinkStream final : public ILinkStream
{
public:
  sf::TcpSocket& Socket() { return m_socket; }

  bool Send(const u8* data, std::size_t len) override
  {
    m_socket.setBlocking(true);
    return SendAll(m_socket, data, len);
  }

  bool Recv(u8* data, std::size_t len, const Common::Flag& running) override
  {
    return RecvAll(m_socket, data, len, running);
  }

  void Close() override { m_socket.disconnect(); }

private:
  sf::TcpSocket m_socket;
};
}  // namespace

CEXIMeleeNetplay::CEXIMeleeNetplay(Core::System& system) : IEXIDevice(system)
{
  m_is_host = Config::Get(Config::MAIN_MELEE_NETPLAY_IS_HOST);
  m_transport_external = Config::Get(Config::MAIN_MELEE_NETPLAY_TRANSPORT) == 1;
  m_delay = static_cast<u8>(Config::Get(Config::MAIN_MELEE_NETPLAY_DELAY));

  m_players = static_cast<u32>(
      std::clamp(Config::Get(Config::MAIN_MELEE_NETPLAY_PLAYERS), 2, int(MAX_PLAYERS)));

  // The host deals every client's port in the HELLO census, so a client's
  // pre-handshake mask is only a placeholder. Only an explicit LocalPorts
  // override is honored as-is (multi-pad-per-console sessions).
  const int ports_cfg = Config::Get(Config::MAIN_MELEE_NETPLAY_LOCAL_PORTS);
  m_local_mask = ports_cfg != 0 ? static_cast<u8>(ports_cfg) : (m_is_host ? 0x01 : 0x02);

  m_trace_seed_addr = Config::Get(Config::MAIN_MELEE_NETPLAY_TRACE_SEED_WRITES);
  {
    // Stall-diagnostic watch addresses (comma-separated hex, from the DOL's
    // linker map): their live u32 values ride every stall NOTICE line.
    const std::string watch_cfg = Config::Get(Config::MAIN_MELEE_NETPLAY_DIAG_WATCH);
    std::stringstream ws(watch_cfg);
    std::string tok;
    while (std::getline(ws, tok, ','))
    {
      const u32 a = static_cast<u32>(std::strtoul(tok.c_str(), nullptr, 16));
      if (a >= 0x80000000 && a < 0x81800000)
        m_diag_watches.push_back(a);
    }
  }
  if (m_trace_seed_addr != 0 && Config::Get(Config::MAIN_MELEE_NETPLAY_TRACE_SEED_QUIET))
  {
    // Quiet tracing: per-hit NOTICE lines crawl the game below 1 tick/s once
    // a fight starts (the particle VM rolls the seed hundreds of times per
    // frame). Hits append to this ring instead; DumpSeedTraceRing() writes it
    // out at the first desync, giving full within-tick caller attribution
    // exactly where it matters.
    m_seed_trace_ring = std::make_unique<MemCheckTraceRing>();
    SetMemCheckTraceRing(m_seed_trace_ring.get());
  }
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
  m_match_pacing = Config::Get(Config::MAIN_MELEE_NETPLAY_MATCH_PACING);

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

  if (m_transport_external)
  {
    // Test-only app stand-in (Mac harness); a real app leaves this empty and
    // attaches links itself. See MeleeNetplayExternalTestPump.h.
    const std::string pump = Config::Get(Config::MAIN_MELEE_NETPLAY_EXTERNAL_TEST_TCP);
    if (!pump.empty())
      MeleeNetplayExternalTestPump::Start(m_is_host, pump, m_is_host ? m_players - 1 : 1);
  }

  // NOTICE-level boot breadcrumbs (S4 triage): visible at any verbosity so an
  // on-device boot hang localizes from the log ladder alone.
  NOTICE_LOG_FMT(EXPANSIONINTERFACE,
                 "MeleeNetplay: device constructed (host={} transport={} players={} delay={})",
                 m_is_host, m_transport_external ? "external" : "tcp", m_players, m_delay);
  m_running.Set();
  m_net_thread = std::thread(&CEXIMeleeNetplay::NetThread, this);
  m_diag_thread = std::thread(&CEXIMeleeNetplay::DiagThread, this);
}

CEXIMeleeNetplay::~CEXIMeleeNetplay()
{
  if (m_seed_trace_ring)
    SetMemCheckTraceRing(nullptr);
  SuspendThrottle(false);
  m_running.Clear();
  for (auto& peer : m_peers)
  {
    peer->sendq_cv.notify_all();
    peer->stream->Close();
  }
  if (m_net_thread.joinable())
    m_net_thread.join();
  for (auto& peer : m_peers)
  {
    if (peer->rx_thread.joinable())
      peer->rx_thread.join();
    if (peer->send_thread.joinable())
      peer->send_thread.join();
  }
  if (m_diag_thread.joinable())
    m_diag_thread.join();
  MeleeNetplayExternalTestPump::Stop();  // no-op unless the test pump ran
}

// One send thread per link: see the header. Every outbound message goes through
// here, so no producer ever blocks on the network.
void CEXIMeleeNetplay::PeerSendThread(PeerLink* link)
{
  Common::SetCurrentThreadName("MeleeNetplaySend");
  while (m_running.IsSet())
  {
    std::vector<u8> msg;
    {
      std::unique_lock lk(link->sendq_lock);
      link->sendq_cv.wait(lk, [&] { return !link->sendq.empty() || !m_running.IsSet(); });
      if (!m_running.IsSet())
        return;
      msg = std::move(link->sendq.front());
      link->sendq.pop_front();
    }
    if (!link->stream->Send(msg.data(), msg.size()))
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: send failed to peer (ports {:#x})",
                    link->mask);
      return;
    }
  }
}

void CEXIMeleeNetplay::BroadcastRaw(const u8* bytes, u32 len, const PeerLink* exclude)
{
  for (auto& peer : m_peers)
  {
    if (peer.get() == exclude)
      continue;
    {
      std::lock_guard lk(peer->sendq_lock);
      peer->sendq.emplace_back(bytes, bytes + len);
    }
    peer->sendq_cv.notify_one();
  }
}

void CEXIMeleeNetplay::EnqueueMessage(u8 type, u8 mask, u32 tick, const u8* payload, u16 len)
{
  std::vector<u8> buf(8u + len);
  buf[0] = type;
  buf[1] = mask;
  buf[2] = u8(len >> 8);
  buf[3] = u8(len);
  WriteBE32(&buf[4], tick);
  if (len != 0)
    std::memcpy(&buf[8], payload, len);
  BroadcastRaw(buf.data(), static_cast<u32>(buf.size()), nullptr);
}

void CEXIMeleeNetplay::DiagThread()
{
  Common::SetCurrentThreadName("MeleeNetplayDiag");
  while (m_running.IsSet())
  {
    Common::SleepCurrentThread(500);
    if (!m_session_ready.load())
      continue;
    const s64 last = m_last_exchange_us.load();
    if (last == 0)
      continue;
    const s64 now = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (now - last < 2'000'000)
      continue;
    const auto& ppc = m_system.GetPPCState();
    std::string watches;
    for (const u32 a : m_diag_watches)
    {
      // Racy cross-thread read of emulated RAM -- fine for a diagnostic.
      watches += fmt::format(" w{:08x}={:08x}", a, m_system.GetMemory().Read_U32(a));
    }
    NOTICE_LOG_FMT(EXPANSIONINTERFACE,
                   "MeleeNetplay: exchange stalled {} ms: serve_tick={} pc={:08x} lr={:08x} "
                   "pending_replay={} replay_serving={} rollback_needed={}{}",
                   (now - last) / 1000, m_serve_tick, ppc.pc, LR(ppc), m_pending_replay,
                   m_replay_serving, m_rollback_needed, watches);
  }
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

// Accept Players-1 clients, then deal each one its port and the session census.
// The census is computed only once every client is attached, so the assignment
// is coherent in one shot (a client cannot infer it, unlike the old 1v1
// "if the host took my port, take the other one" flip).
bool CEXIMeleeNetplay::AdoptExternalLinks(u32 count)
{
  Core::DisplayMessage(fmt::format("MeleeNetplay: waiting for {} app-provided link(s)", count),
                       8000);
  NOTICE_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: waiting for {} app-provided link(s)", count);
  std::vector<std::shared_ptr<ExternalLinkStream>> links;
  while (m_running.IsSet())
  {
    links = MeleeNetplayExternal::AttachedLinks();
    if (links.size() >= count)
      break;
    Common::SleepCurrentThread(5);
  }
  if (!m_running.IsSet())
    return false;
  if (links.size() > count)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE,
                 "MeleeNetplay: {} external links attached, session uses the first {}",
                 links.size(), count);
  }
  for (u32 i = 0; i < count; i++)
  {
    auto link = std::make_unique<PeerLink>();
    link->stream = links[i];
    m_peers.push_back(std::move(link));
  }
  NOTICE_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: adopted {} external link(s)", count);
  return true;
}

bool CEXIMeleeNetplay::HostHandshake()
{
  if (m_transport_external)
  {
    if (!AdoptExternalLinks(m_players - 1))
      return false;
  }
  else
  {
    const u16 port = static_cast<u16>(Config::Get(Config::MAIN_MELEE_NETPLAY_PORT));

    sf::TcpListener listener;
    if (listener.listen(port) != sf::Socket::Status::Done)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: failed to listen on port {}", port);
      return false;
    }
    Core::DisplayMessage(
        fmt::format("MeleeNetplay: waiting for {} peer(s) on port {}", m_players - 1, port), 8000);
    listener.setBlocking(false);

    while (m_peers.size() < m_players - 1 && m_running.IsSet())
    {
      auto tcp = std::make_shared<TcpLinkStream>();
      if (listener.accept(tcp->Socket()) != sf::Socket::Status::Done)
      {
        Common::SleepCurrentThread(5);
        continue;
      }
      auto link = std::make_unique<PeerLink>();
      link->stream = std::move(tcp);
      m_peers.push_back(std::move(link));
      Core::DisplayMessage(
          fmt::format("MeleeNetplay: peer {}/{} connected", m_peers.size(), m_players - 1), 5000);
    }
    if (!m_running.IsSet())
      return false;
  }

  // Deal ports: the host keeps m_local_mask, each client takes the next free
  // port in ascending order.
  u8 assigned = m_local_mask;
  for (auto& peer : m_peers)
  {
    u8 mask = 0;
    for (u32 p = 0; p < MAX_PLAYERS; p++)
    {
      if ((assigned & (1 << p)) == 0)
      {
        mask = static_cast<u8>(1 << p);
        break;
      }
    }
    if (mask == 0)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: out of ports for {} players", m_players);
      return false;
    }
    peer->mask = mask;
    assigned |= mask;
  }
  m_combined_mask = assigned;
  m_remote_mask = static_cast<u8>(m_combined_mask & ~m_local_mask);

  // One barrier bit per PEER: the lowest port it owns, which is the block it
  // stamps its scene-barrier flag into (nw_SceneBarrier).
  const auto lowest_bit = [](u8 m) -> u8 { return static_cast<u8>(m & (~m + 1)); };
  m_barrier_mask = lowest_bit(m_local_mask);
  for (const auto& peer : m_peers)
    m_barrier_mask |= lowest_bit(peer->mask);

  for (auto& peer : m_peers)
  {
    u8 hello[HELLO_BYTES] = {};
    hello[0] = MSG_HELLO;
    hello[1] = PROTO_VERSION;
    hello[2] = peer->mask;
    hello[3] = m_delay;
    WriteBE32(&hello[4], m_seed);
    hello[8] = static_cast<u8>(m_window);
    hello[9] = m_combined_mask;
    hello[10] = m_barrier_mask;
    hello[11] = static_cast<u8>(m_players);
    if (!peer->stream->Send(hello, sizeof(hello)))
      return false;

    u8 ack[3];
    if (!peer->stream->Recv(ack, sizeof(ack), m_running))
      return false;
    if (ack[0] != MSG_HELLO_ACK || ack[1] != PROTO_VERSION || ack[2] != peer->mask)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE,
                    "MeleeNetplay: bad HELLO_ACK (type {:#x} ver {} mask {:#x}, wanted {:#x})",
                    ack[0], ack[1], ack[2], peer->mask);
      return false;
    }
  }
  return true;
}

bool CEXIMeleeNetplay::ClientHandshake()
{
  if (m_transport_external)
  {
    if (!AdoptExternalLinks(1))
      return false;
  }
  else
  {
    const u16 port = static_cast<u16>(Config::Get(Config::MAIN_MELEE_NETPLAY_PORT));
    const std::string remote = Config::Get(Config::MAIN_MELEE_NETPLAY_REMOTE_HOST);
    Core::DisplayMessage(fmt::format("MeleeNetplay: connecting to {}:{}", remote, port), 8000);

    const auto remote_addr = sf::IpAddress::resolve(remote);
    if (!remote_addr)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: cannot resolve host '{}'", remote);
      return false;
    }

    auto tcp = std::make_shared<TcpLinkStream>();
    tcp->Socket().setBlocking(true);
    while (m_running.IsSet())
    {
      if (tcp->Socket().connect(*remote_addr, port, sf::seconds(2)) == sf::Socket::Status::Done)
        break;
      Common::SleepCurrentThread(500);
    }
    if (!m_running.IsSet())
      return false;
    tcp->Socket().setBlocking(false);

    auto link = std::make_unique<PeerLink>();
    link->stream = std::move(tcp);
    m_peers.push_back(std::move(link));
  }

  PeerLink* link = m_peers.front().get();
  u8 hello[HELLO_BYTES];
  if (!link->stream->Recv(hello, sizeof(hello), m_running))
    return false;
  if (hello[0] != MSG_HELLO || hello[1] != PROTO_VERSION)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: bad HELLO (type {:#x} ver {})", hello[0],
                  hello[1]);
    return false;
  }
  m_local_mask = hello[2];
  m_delay = hello[3];
  m_seed = ReadBE32(&hello[4]);
  m_window = hello[8];
  m_combined_mask = hello[9];
  m_barrier_mask = hello[10];
  m_players = hello[11];
  m_remote_mask = static_cast<u8>(m_combined_mask & ~m_local_mask);
  // The client's single link is to the host, which owns every other port: the
  // host relays the other clients' inputs, so from here they are indistinguishable
  // from the host's own.
  link->mask = m_remote_mask;

  u8 ack[3] = {MSG_HELLO_ACK, PROTO_VERSION, m_local_mask};
  if (!link->stream->Send(ack, sizeof(ack)))
    return false;

  return true;
}

void CEXIMeleeNetplay::StartPeerThreads()
{
  for (auto& peer : m_peers)
  {
    peer->send_thread = std::thread(&CEXIMeleeNetplay::PeerSendThread, this, peer.get());
    peer->rx_thread = std::thread(&CEXIMeleeNetplay::PeerRecvThread, this, peer.get());
  }
}

void CEXIMeleeNetplay::NetThread()
{
  Common::SetCurrentThreadName("MeleeNetplay");

  NOTICE_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: net thread up (host={})", m_is_host);
  if (!(m_is_host ? HostHandshake() : ClientHandshake()))
  {
    NOTICE_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: handshake FAILED — session dead");
    m_session_failed = true;
    return;
  }
  if (m_local_mask == 0 || (m_local_mask & m_remote_mask) != 0)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: bad port partition local={:#x} remote={:#x}",
                  m_local_mask, m_remote_mask);
    m_session_failed = true;
    return;
  }

  // Pre-seed the delay window with neutral pads so the game's first POLLs
  // (ticks 0..delay-1, before any peer's first SEND lands) never deadlock.
  {
    std::lock_guard lk(m_frames_lock);
    for (u32 t = 0; t < m_delay; t++)
      m_frames[t].have_mask = 0xFF;
  }

  StartPeerThreads();
  m_session_ready = true;
  // WARN (not just DisplayMessage): DisplayMessage goes to the OSD and the CORE
  // log, which the harnesses do not enable -- so a 4-way pairing failure would
  // have been invisible in the logs the verdict actually reads.
  WARN_LOG_FMT(EXPANSIONINTERFACE,
               "MeleeNetplay: session up ({} players, local ports {:#x}, combined {:#x}, "
               "barrier {:#x}, delay {}, window {})",
               m_players, m_local_mask, m_combined_mask, m_barrier_mask, m_delay, m_window);
  Core::DisplayMessage(
      fmt::format("MeleeNetplay: session up ({} players, local ports {:#x}, combined {:#x}, "
                  "delay {}, window {})",
                  m_players, m_local_mask, m_combined_mask, m_delay, m_window),
      8000);
  if (m_window != 0)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: prediction+rollback ON (window {})", m_window);
  }
}

// One receive loop per link. On the host this is also the RELAY point: a
// client's inputs are forwarded to every OTHER client before local ingest, so
// the extra star hop costs as little as possible. Relaying happens outside
// m_frames_lock (BroadcastRaw only takes send-queue locks), preserving the
// no-blocking-send-under-m_frames_lock invariant.
void CEXIMeleeNetplay::PeerRecvThread(PeerLink* link)
{
  Common::SetCurrentThreadName("MeleeNetplayRecv");

  while (m_running.IsSet())
  {
    u8 header[8];
    if (!link->stream->Recv(header, sizeof(header), m_running))
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
    if (len != 0 && !link->stream->Recv(payload, len, m_running))
      break;

    // Star relay: only INPUTS need to reach the other clients. Checksums do
    // not — every peer compares against the host, and agreement with the host
    // is transitive (see m_remote_crcs).
    if (m_is_host && type == MSG_INPUTS && m_peers.size() > 1)
    {
      u8 relay[8 + 4 * PAD_BYTES];
      std::memcpy(relay, header, sizeof(header));
      if (len != 0)
        std::memcpy(relay + 8, payload, len);
      BroadcastRaw(relay, 8u + len, link);
    }

    HandleMessage(link, type, mask, tick, payload, len);
  }

  if (m_running.IsSet())
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: peer disconnected (ports {:#x})", link->mask);
    Core::DisplayMessage("MeleeNetplay: peer disconnected", 10000);
  }
}

void CEXIMeleeNetplay::HandleMessage(PeerLink* from, u8 type, u8 mask, u32 tick, const u8* payload,
                                     u32 len)
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
    m_remote_crcs[tick][from->mask] = remote_crc;
    const auto local = m_local_crcs.find(tick);
    if (local != m_local_crcs.end())
      CompareChecksumLocked(tick, from->mask, local->second, remote_crc);
    break;
  }
  default:
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: unknown message type {:#x}", type);
    break;
  }
}

// Fans out to every peer. Formerly a direct blocking send on the one socket;
// with N peers that would let the slowest console stall the CPU thread (and
// every other link behind it), so all sends now go through the per-peer queues.
void CEXIMeleeNetplay::SendMessageRaw(u8 type, u8 mask, u32 tick, const u8* payload, u16 len)
{
  EnqueueMessage(type, mask, tick, payload, len);
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
  // Scene-barrier fence. The game stamps its barrier ready bit into byte 0x42
  // of its first OWNED netplay port's block (nw_SceneBarrier); peek exactly
  // that slot. While the bit is up the game is waiting at (or approaching) a
  // barrier release, and the release must be evaluated on REAL flag data at
  // the same tick on both peers: nw_SceneBarrier lives in the OUTER scene
  // loop, not the replayed tick body, so a rollback replay spanning the true
  // release tick can never re-evaluate it -- the rolled-back peer releases
  // LATE and the seed re-sync (agreed ^ release_tick) forks by the tick skew
  // (smoke #19: XOR 0x06 at the fight-exit barrier, desync at the next demo
  // roll). Suppressing prediction from the stamp on guarantees every barrier
  // evaluation from stamp to release sees confirmed data: the stamp rides
  // blocks `delay` ticks ahead of consumption, pre-stamp speculation resolves
  // before the release tick can be served, and pre-stamp barrier evaluations
  // are release-insensitive (both-up needs the local flag, down there).
  {
    // The game stamps its flag into PHYSICAL block 0 -- the block that actually
    // goes on the wire, because SendInputs above maps physical pads onto owned
    // ports in ascending order (physical 0 -> lowest owned port).
    //
    // It used to stamp the block at its NETPLAY port index instead, which for
    // any peer not owning port 0 is a different block than the one transmitted:
    // the client stamped block 1 while block 0 went out as its port-1 payload.
    // Bytes 0x42/0x43 are HSD_PadStatus alignment padding that the pad transform
    // never writes, so block 0 still held the previous RECV's memcpy -- the
    // HOST's served flag. The client was echoing the host's own readiness back
    // at it, and its own never reached the wire. Effect in 1v1: the barrier
    // degraded to "the host decides" (still deterministic on both peers, hence
    // no desync -- which is why it survived qualification). Fatal at 4 players,
    // where 3 of 4 peers own a non-zero port.
    const u8 flag_byte = pads[0x42];
    const bool stamped = (flag_byte & 1) != 0;
    if (stamped != m_barrier_armed)
    {
      m_barrier_armed = stamped;
      INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: barrier fence {} at tick {}",
                   stamped ? "armed" : "disarmed", tick);
    }
    // Bit 7: match-end quiesce stamp (GAME! through scene exit). The game
    // issues results-screen loads in this window whose completion flags live
    // in restored heap; treat it exactly like an armed barrier fence.
    const bool quiesce = (flag_byte & 0x80) != 0;
    if (quiesce != m_quiesce_stamped)
    {
      m_quiesce_stamped = quiesce;
      INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: match-end quiesce {} at tick {}",
                   quiesce ? "armed" : "disarmed", tick);
    }
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

void CEXIMeleeNetplay::SuspendThrottle(bool on)
{
  if (m_throttle_suspended == on)
    return;
  m_throttle_suspended = on;
  if (on)
    m_suspend_engaged++;
  // NOT Core::SetIsThrottlerTempDisabled: DolphinQt's HotkeyScheduler
  // rewrites that flag from the hotkey state every pass, so a programmatic
  // set is stomped back to false within milliseconds (pace1/pace2: suspends
  // engaged yet 66% of CPU-thread samples still slept in Throttle).
  m_system.GetCoreTiming().SetSpeedUnlimitedOverride(on);
}

// Write the quiet seed-trace ring out as text lines in the exact format
// diff-seed-writes.py already parses (MBP + trace-tick markers). Once per
// run, at the first desync -- the ring holds the last ~256k writes, which at
// particle-flood rates still spans hundreds of ticks around the fork.
void CEXIMeleeNetplay::DumpSeedTraceRing(u32 desync_tick)
{
  if (!m_seed_trace_ring || m_seed_trace_dumped)
    return;
  m_seed_trace_dumped = true;
  const std::string path = File::GetUserPath(D_LOGS_IDX) + "seedtrace.txt";
  std::ofstream f(path);
  if (!f)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: cannot open {}", path);
    return;
  }
  auto& ring = *m_seed_trace_ring;
  const u64 head = ring.head.load(std::memory_order_relaxed);
  const u64 count = std::min<u64>(head, MemCheckTraceRing::CAPACITY);
  f << fmt::format("seedtrace dump at desync tick={} entries={}\n", desync_tick, count);
  for (u64 i = head - count; i < head; i++)
  {
    const auto& e = ring.entries[i % MemCheckTraceRing::CAPACITY];
    if (e.pc == 0)
      f << fmt::format("trace tick={} replaying={}\n", e.value, e.lr);
    else
      f << fmt::format("MBP {:08x} ( ) Write32 {:x} at {:08x} lr={:08x}\n", e.pc, e.value, e.addr,
                       e.lr);
  }
  ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: seed trace ring dumped to {} ({} entries)",
                path, count);
}

void CEXIMeleeNetplay::UpdateSchedulePacing()
{
  if (!InRealMatch())
  {
    if (m_sched_valid)
    {
      m_sched_valid = false;
      SuspendThrottle(false);
    }
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!m_sched_valid)
  {
    m_sched_valid = true;
    m_sched_anchor_wall = now;
    m_sched_anchor_tick = m_serve_tick;
    SuspendThrottle(false);
    return;
  }
  const double elapsed = std::chrono::duration<double>(now - m_sched_anchor_wall).count();
  const double behind = m_sched_anchor_tick + elapsed * 60.0 - double(m_serve_tick);
  // Cap the backlog at 2s so a long stall (transport hiccup, scene barrier)
  // does not turn into a minutes-long unthrottled burst afterwards.
  if (behind > 120.0)
  {
    m_sched_anchor_tick = m_serve_tick;
    m_sched_anchor_wall = now - std::chrono::seconds(2);
  }
  // Hysteresis: suspend while >1 tick behind schedule, resume once caught up.
  // On-schedule serving runs under the normal wall throttle (A/V pacing
  // intact); the suspension only ever shortens the lag back to zero.
  if (behind > 1.0)
    SuspendThrottle(true);
  else if (behind <= 0.0)
    SuspendThrottle(false);
}

void CEXIMeleeNetplay::NoteServeCycles()
{
  const u64 c = static_cast<u64>(m_system.GetCoreTiming().GetTicks());
  if (m_last_serve_cycles != 0 && c > m_last_serve_cycles)
  {
    const double field_cycles = double(m_system.GetSystemTimers().GetTicksPerSecond()) / 60.0;
    const u32 fields = static_cast<u32>((double(c - m_last_serve_cycles) / field_cycles) + 0.5);
    const u32 idx = fields <= 1 ? 0 : (fields == 2 ? 1 : 2);
    m_hist_fields[idx]++;
    if (idx >= 1 && m_ticks_since_rollback <= 6)
      m_hist_2plus_near_rb++;
  }
  m_last_serve_cycles = c;
  m_ticks_since_rollback++;
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
  const u64 cycles_now = static_cast<u64>(m_system.GetCoreTiming().GetTicks());
  if (m_rate_window_start.time_since_epoch().count() == 0)
  {
    // First window: no elapsed baseline yet, so only arm the counters.
    m_rate_window_start = now;
    m_rate_window_tick = m_serve_tick;
    m_rate_window_cycles = cycles_now;
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
  // Emulated-time cost of a served tick: how many VI fields' worth of
  // CoreTiming cycles pass per serve. 1.0 = the game makes every field
  // deadline; ~2.0 = every tick slips to the next field (30Hz cadence) and
  // the throttle bills the slack as real time.
  {
    const u32 tick_delta = m_serve_tick - m_rate_window_tick;
    const double field_cycles = double(m_system.GetSystemTimers().GetTicksPerSecond()) / 60.0;
    const double fields_per_tick =
        tick_delta != 0 ? double(cycles_now - m_rate_window_cycles) / field_cycles / tick_delta :
                          0.0;
    // Split the window's wall time: throttle sleeps vs burst execution vs
    // everything else, and the window's emulated advance vs how much of it
    // happened inside (suspension-covered) bursts. Deltas since last window.
    const u64 sleep_us_now = m_system.GetCoreTiming().GetThrottleSleepUsTotal();
    const double burst_fields = double(m_burst_cycles_total - m_prev_burst_cycles) / field_cycles;
    INFO_LOG_FMT(EXPANSIONINTERFACE,
                 "MeleeNetplay: emu-time: {:.2f} fields/tick over {} ticks | throttle_sleep_ms={} "
                 "burst_fields={:.0f} burst_wall_ms={} | serve fields hist 1/2/3+: {}/{}/{} "
                 "(2+ near rollback: {})",
                 fields_per_tick, tick_delta, (sleep_us_now - m_prev_sleep_us) / 1000,
                 burst_fields, (m_burst_us_total - m_prev_burst_us) / 1000, m_hist_fields[0],
                 m_hist_fields[1], m_hist_fields[2], m_hist_2plus_near_rb);
    m_prev_sleep_us = sleep_us_now;
    m_prev_burst_cycles = m_burst_cycles_total;
    m_prev_burst_us = m_burst_us_total;
    m_hist_fields[0] = m_hist_fields[1] = m_hist_fields[2] = 0;
    m_hist_2plus_near_rb = 0;
  }
  // Async-I/O epoch source diagnosis: which completion class churns, and how
  // often each in-flight predicate is up at the sample instant. R1 smoke #1
  // showed ~100 completions/s starving both restore gates; the split names
  // the autonomous (never-awaited) source to exclude, like DTK in gate v2.
  INFO_LOG_FMT(EXPANSIONINTERFACE,
               "MeleeNetplay: io epoch: aram={} dvdread={} dicmd={} | inflight now: aram={} "
               "dvdread={} dicmd={}",
               m_system.GetDSP().GetARAMDMACompletionCount(),
               m_system.GetDVDThread().GetNonDTKReadsCompleted(),
               m_system.GetDVDInterface().GetNonDTKCommandsCompleted(),
               m_system.GetDSP().IsARAMDMAInProgress(), m_system.GetDVDThread().HasPendingReads(),
               m_system.GetDVDInterface().IsCommandPending());
  if (m_window != 0)
  {
    INFO_LOG_FMT(EXPANSIONINTERFACE,
                 "MeleeNetplay: rollback stats: predicted={} validated_ok={} rollbacks={} "
                 "max_depth={} refused_scene={} refused_io={} refused_epoch={} "
                 "aram_redelivered={} checksums_skipped={} frontier_lag={} barrier_drain={} "
                 "payload_redelivered={} payload_gaps={}",
                 m_predicted_ticks, m_validated_ok, m_rollback_count, m_rollback_depth_max,
                 m_rollback_refused_scene, m_restore_refused_io, m_restore_refused_epoch,
                 m_aram_redelivered, m_checksums_skipped, m_serve_tick - m_confirmed_frontier,
                 m_barrier_drain_polls, m_rollback.RedeliveredSpans(), m_rollback.RedeliveryGaps());
    // Where a replay burst's wall time goes (cumulative since boot). A burst
    // spans REPLAY directive -> first post-replay POLL; depth_avg gives the
    // replayed tick count that wall time bought.
    if (m_burst_count != 0)
    {
      INFO_LOG_FMT(EXPANSIONINTERFACE,
                   "MeleeNetplay: burst stats: bursts={} depth_avg={:.1f} burst_avg_ms={:.1f} "
                   "restore_avg_us={} suspends={} pacing={}",
                   m_burst_count, double(m_burst_depth_total) / double(m_burst_count),
                   double(m_burst_us_total) / double(m_burst_count) / 1000.0,
                   m_rollback_count != 0 ? m_restore_us_total / m_rollback_count : 0,
                   m_suspend_engaged, m_match_pacing);
    }
  }

  m_stall_samples_us.clear();
  m_rate_window_start = now;
  m_rate_window_tick = m_serve_tick;
  m_rate_window_cycles = cycles_now;
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
  EmitSnapshotChecksumsLocked();
}

// One comparison per (hash tick, peer), from whichever side completes the pair:
// EmitSnapshotChecksumsLocked (remote already arrived) or the MSG_CHECKSUM
// handler (local already emitted). m_frames_lock held by the caller.
//
// Retirement is peer-aware: the tick's entries are only erased once EVERY remote
// peer's checksum for it has been compared (m_crc_seen accumulates their port
// bits until it covers m_remote_mask). Erasing on the first comparison — as the
// 2-peer version did, where "first" and "every" coincided — would have thrown
// away the other peers' still-in-flight checksums and silently skipped 2 of 3
// comparisons at 4 players.
void CEXIMeleeNetplay::CompareChecksumLocked(u32 tick, u8 peer_mask, u32 local_crc, u32 remote_crc)
{
  if (local_crc != remote_crc)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE,
                  "MeleeNetplay: DESYNC at tick {} vs peer ports {:#x} ({:08x} != {:08x})", tick,
                  peer_mask, local_crc, remote_crc);
    Core::DisplayMessage(fmt::format("MeleeNetplay: DESYNC at tick {}", tick), 20000);
    if (!m_desync_dumped && m_dump_armed_tick < 0)
      m_dump_armed_tick = tick;
    if (!m_ring_dumped && m_ring_dump_tick < 0)
      m_ring_dump_tick = tick;
    DumpSeedTraceRing(tick);
  }
  else
  {
    // A matching checksum only demonstrates determinism if the state it
    // hashes is actually changing: a constant crc means the peers are
    // parked on a static screen, not that the cores agree under load.
    INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: checksum ok tick={} peer={:#x} crc={:08x}",
                 tick, peer_mask, remote_crc);
  }

  m_crc_seen[tick] |= peer_mask;
  if (m_crc_seen[tick] != m_remote_mask)
    return;  // other peers' checksums for this tick are still in flight
  m_local_crcs.erase(m_local_crcs.begin(), m_local_crcs.upper_bound(tick));
  m_remote_crcs.erase(m_remote_crcs.begin(), m_remote_crcs.upper_bound(tick));
  m_crc_seen.erase(m_crc_seen.begin(), m_crc_seen.upper_bound(tick));
}

// Write the queued first-mismatch ring dumps (see m_ring_dump_tick). CPU
// thread only — the ring has no lock of its own and Capture runs here.
void CEXIMeleeNetplay::MaybeDumpDesyncRing()
{
  u32 tick = 0;
  {
    std::lock_guard lk(m_frames_lock);
    if (m_ring_dumped || m_ring_dump_tick < 0)
      return;
    m_ring_dumped = true;
    tick = static_cast<u32>(m_ring_dump_tick);
  }
  for (const u32 t : {tick - 1, tick})
  {
    const std::string path = File::GetUserPath(D_USER_IDX) + fmt::format("desync-ring-{}.bin", t);
    const bool ok = m_rollback.DumpSnapshot(t, path);
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: desync ring dump {} -> {}",
                  ok ? "written" : "FAILED (tick left the ring)", path);
  }
}

// Hash + transmit + compare every hash tick the confirmed boundary has
// passed. ring[T] is the pre-state of tick T, so it is FINAL the moment
// every tick < T is confirmed — under prediction that is the validated
// frontier; in lockstep everything served is confirmed. A hash tick whose
// window was corrected by a rollback is covered too: replayed RECVs
// re-capture their slots, and the frontier can only pass T after the replay
// that corrected it ran. m_frames_lock held by the caller.
void CEXIMeleeNetplay::EmitSnapshotChecksumsLocked()
{
  if (!m_rollback.HasHashSpans())
    return;
  const u32 confirmed = m_window != 0 ? m_confirmed_frontier : m_serve_tick;
  while (m_hash_tick + HASH_INTERVAL <= confirmed)
  {
    const u32 tick = m_hash_tick + HASH_INTERVAL;
    const auto parked = m_tick_hashes.find(tick);
    if (parked == m_tick_hashes.end())
    {
      // Not captured YET: the frontier only needs ticks < T to pass T, so
      // emission can run at tick T's own POLL/SEND — before the RECV that
      // captures and hashes T. Wait for it (the very next RECV parks it and
      // emission re-runs every poll); advancing past it here is what starved
      // the oracle to 2-4 comparisons per run (targets 11c/12, and the
      // ring-read predecessor had the same off-by-one in menus).
      break;
    }
    m_hash_tick = tick;
    const u32 crc = parked->second;
    u8 payload[4];
    WriteBE32(payload, crc);
    EnqueueMessage(MSG_CHECKSUM, 0, tick, payload, 4);
    m_local_crcs[tick] = crc;
    const auto remote = m_remote_crcs.find(tick);
    if (remote != m_remote_crcs.end())
    {
      // Compare against every peer whose checksum for this tick already landed.
      // Copy first: CompareChecksumLocked may retire (erase) the tick's map.
      const std::map<u8, u32> arrived = remote->second;
      for (const auto& [peer_mask, peer_crc] : arrived)
        CompareChecksumLocked(tick, peer_mask, crc, peer_crc);
    }
  }
  m_tick_hashes.erase(m_tick_hashes.begin(), m_tick_hashes.upper_bound(m_hash_tick));
}

// Hash ring[tick] the moment it is captured (see m_tick_hashes). Runs on the
// CPU thread right after Capture; replayed re-captures overwrite the parked
// value with the corrected pass's hash.
void CEXIMeleeNetplay::HashCapturedTick(u32 tick)
{
  if (tick == 0 || tick % HASH_INTERVAL != 0 || !m_rollback.HasHashSpans())
    return;
  u32 crc = 0;
  if (m_rollback.SnapshotChecksum(tick, &crc))
    m_tick_hashes[tick] = crc;
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
    // NEVER clobber real data. A tick can have ARRIVED remote inputs that
    // are still latency-INVISIBLE (visible_at in the future) -- predicting
    // into it overwrote the real bytes, and validation then compared the
    // speculation against itself: a wrong input silently false-validated,
    // peers diverged with no rollback, and the failure was a wall-clock
    // race (masked by tracer timing, widened by deeper windows -- smokes
    // #13-#15). Arrived data is a guaranteed-correct prediction: serve it.
    if ((frame.have_mask & (1 << port)) != 0)
    {
      served[port] = frame.pads[port];
      continue;
    }
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
    // A replay burst is only fully paid for once the LAST replayed tick's
    // body has executed (the replay loop is RECV+inject+run with no
    // exchanges) -- the first post-replay POLL is that boundary. Resume
    // wall-clock pacing on a fresh reference (the throttle re-anchored
    // itself continuously while suspended).
    if (m_burst_active && m_replay_serving == 0 && m_pending_replay == 0)
    {
      const u64 burst_end_cycles = static_cast<u64>(m_system.GetCoreTiming().GetTicks());
      m_burst_us_total += static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - m_burst_start)
                                               .count());
      m_burst_cycles_total += burst_end_cycles - m_burst_start_cycles;
      m_burst_count++;
      m_burst_active = false;
      // Segment attribution for the ~7-field emulated cost each rollback
      // carries (pace8 histogram): pre = last normal serve -> REPLAY
      // directive, mid = the replay burst itself. The post-burst origin
      // serve's NoteServeCycles delta = pre + mid + post.
      const double fc = double(m_system.GetSystemTimers().GetTicksPerSecond()) / 60.0;
      INFO_LOG_FMT(EXPANSIONINTERFACE,
                   "MeleeNetplay: burst detail tick={}: pre={:.2f} mid={:.2f} fields",
                   m_serve_tick,
                   m_last_serve_cycles != 0 ?
                       double(m_burst_start_cycles - m_last_serve_cycles) / fc :
                       0.0,
                   double(burst_end_cycles - m_burst_start_cycles) / fc);
    }
    if (m_throttle_suspended && m_match_pacing == 1 && m_replay_serving == 0 &&
        m_pending_replay == 0)
      SuspendThrottle(false);
    // Replay directive outranks readiness: memory has already been restored,
    // so the game must consume the replayed ticks before anything else.
    if (m_pending_replay != 0)
    {
      const u32 k = m_pending_replay;
      m_pending_replay = 0;
      m_replay_serving = k;
      if (m_match_pacing == 1 || m_match_pacing >= 3)
        SuspendThrottle(true);
      m_burst_start = std::chrono::steady_clock::now();
      m_burst_start_cycles = static_cast<u64>(m_system.GetCoreTiming().GetTicks());
      m_burst_active = true;
      m_burst_depth_total += k;
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
        // Restoring while a transfer is IN FLIGHT is a real, prompt desync
        // source (A/B proven: smoke #8 always outwaited transfers = 0
        // desyncs; smoke #9 restored under them = desync at the first
        // rollbacks): the transfer completes after the restore and delivers
        // its payload/callback into the rolled-back state on this peer
        // only. But waiting for GLOBAL quiescence never converges under
        // free-running streaming (smoke #8: seconds parked per rollback,
        // 11 ticks/s). Park until exactly what is in flight NOW completes:
        // each transfer takes ms, and newly-issued traffic does not extend
        // the wait.
        auto& dsp = m_system.GetDSP();
        auto& dvdt = m_system.GetDVDThread();
        auto& dvdi = m_system.GetDVDInterface();
        if (m_rollback_io_defer_streak == 0)
        {
          m_park_aram_target =
              dsp.IsARAMDMAInProgress() ? dsp.GetARAMDMACompletionCount() + 1 : 0;
          m_park_dvd_target = dvdt.GetNonDTKReadsCompleted() + dvdt.GetPendingNonDTKReadCount();
          m_park_di_target =
              dvdi.IsCommandPending() ? dvdi.GetNonDTKCommandsCompleted() + 1 : 0;
        }
        // ARAM must be fully IDLE, not just first-wave-complete: ARQ loads
        // are CASCADES (chunk N's completion callback issues chunk N+1), and
        // restoring between chunks rolls the game's queue bookkeeping back
        // mid-chain -- observed as the lbarq queue-head pointer differing
        // across peers right after a short-park rollback (smoke #10). The
        // game's ARQ interrupt handler keeps running while we park (the EXI
        // spin executes with interrupts enabled), so the cascade drains in
        // a few ms; unlike DVD streaming, ARAM traffic is sparse, so
        // idle-wait converges.
        const bool parked_io_pending =
            dsp.IsARAMDMAInProgress() ||
            (m_park_aram_target != 0 &&
             dsp.GetARAMDMACompletionCount() < m_park_aram_target) ||
            dvdt.GetNonDTKReadsCompleted() < m_park_dvd_target ||
            (m_park_di_target != 0 && dvdi.GetNonDTKCommandsCompleted() < m_park_di_target);
        // Cap tight: a rollback landing during an ARAM scene-load cascade
        // otherwise parks for the whole burst (smoke #17: one park hit the
        // old 1200 cap = 13.7s stall, 17 ticks/s run, gate starved). NOTE
        // every desync previously attributed to restore-under-in-flight-I/O
        // predates the input-clobber fix -- this cap doubles as the
        // experiment: if desyncs stay gone with ~0.5s parks, the io hazard
        // was misattributed and the parks are a politeness, not a shield.
        // NOTE no DVD epoch gate here. It was tried (554e1d1bab) as a
        // refuse-restore on any non-DTK DVD completion since the target's
        // capture, to kill the dvd.o cbForStateBusy livelock wedge (v15
        // requal run 4) -- and immediately desynced (wedgefix run 1,
        // refused_epoch=70+ per peer): the "fights issue zero non-DTK DVD
        // traffic" premise from the v15q run 5 profile is STAGE-DEPENDENT
        // (music streams from disc in chunks on most stages, ~1 read/100
        // ticks), and every refusal is an accepted wrong-input divergence.
        // A refusal storm is strictly worse than the rare wedge it
        // prevents. The durable fix is DVD completion RE-DELIVERY (mirror
        // the ARAM design below + payload rewrite, since DVD copies land at
        // completion time, not issue time); DVDEpochUnchanged and the
        // refused_epoch stat stay wired for it.
        // Pending non-DTK DVD reads hard-block the restore (generous cap):
        // the first-wave target is fixed at park start, so a read the
        // free-running game issued MID-park is invisible to it — and when
        // the ARAM-churn escape valve forces the restore through, that
        // read's payload lands AFTER the restore at an erased-timeline
        // address (v20b round-2 q1: dvd.o executing=DummyCommandBlock
        // frozen 64s, music stream dead from the fight-start load tail;
        // every stream-death run shows refused_io at the cap). Reads drain
        // in ms and are not free-running; 600 polls is a wedged-DVDThread
        // backstop, not a working budget.
        const bool dvd_reads_pending = dvdt.GetPendingNonDTKReadCount() != 0;
        if ((parked_io_pending && m_rollback_io_defer_streak < 60) ||
            (dvd_reads_pending && m_rollback_io_defer_streak < 600))
        {
          m_restore_refused_io++;
          m_rollback_io_defer_streak++;
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
          m_rollback_io_defer_streak = 0;
          m_confirmed_frontier++;  // treat the mismatched tick as confirmed-wrong
        }
        else if ([&] {
                   const auto t0 = std::chrono::steady_clock::now();
                   const bool ok = m_rollback.Restore(m_system, target);
                   m_restore_us_total += static_cast<u64>(
                       std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count());
                   return ok;
                 }())
        {
          // ARAM interrupt re-delivery: the target slot was captured with an
          // ARAM DMA interrupt pending (copy already done -- it is IN the
          // snapshot). If it has since fired, the delivery landed in the
          // timeline this restore just erased: the restored lbArq/SDK queue
          // still says "in flight" and would spin-wait forever on an
          // interrupt the emulator has no reason to re-send (v15 requal
          // run 6, lbArq_80014BD0 wait loop). Re-raise it: the game's own
          // ISR pops the restored queue and re-chains queued transfers
          // (their copies re-execute from state the snapshot holds -- every
          // request in the restored queue was posted before the capture).
          // Still-pending is fine: the scheduled CoreTiming event will
          // deliver it into state that expects it.
          if (m_rollback.ARAMIntPending(target) && !m_system.GetDSP().IsARAMDMAInProgress())
          {
            m_system.GetDSP().RedeliverARAMInterrupt();
            m_aram_redelivered++;
            INFO_LOG_FMT(EXPANSIONINTERFACE,
                         "MeleeNetplay: re-delivered ARAM completion into restored tick {}",
                         target);
          }
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
          if (m_match_pacing == 1 || m_match_pacing >= 3)
            SuspendThrottle(true);
          m_burst_start = std::chrono::steady_clock::now();
          m_burst_start_cycles = static_cast<u64>(m_system.GetCoreTiming().GetTicks());
          m_burst_active = true;
          m_burst_depth_total += depth;
          m_ticks_since_rollback = 0;
          m_rollback_count++;
          m_rollback_depth_max = std::max(m_rollback_depth_max, depth);
          m_rollback_needed = false;
          m_rollback_io_defer_streak = 0;
          return (u32(POLL_REPLAY) << 24) | depth;
        }
        else
        {
          ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: ROLLBACK restore failed at tick {}",
                        m_serve_tick);
          m_rollback_needed = false;
          m_rollback_io_defer_streak = 0;
          m_confirmed_frontier++;
        }
      }
      if (m_rollback_needed)
      {
        // PARK while a rollback is pending (io-deferred above): serving more
        // ticks past a stalled frontier grows the restore depth without
        // bound — past RING_SIZE the snapshot AND the frame history are gone
        // and the rollback becomes impossible by construction (R1 smoke #2:
        // depth 45, frontier deadlocked 24k ticks). Parking bounds depth at
        // ~W, keeps history alive, and the game spinning at POLL still
        // advances CoreTiming, so the in-flight transfer actually drains.
        if (m_match_pacing >= 3)
          SuspendThrottle(true);
        Common::SleepCurrentThread(1);
        return u32(POLL_WAIT) << 24;
      }
    }
    // Barrier speculation drain. Suppressing NEW predictions from the stamp
    // on (the !m_barrier_armed gate below) is not enough: up to W ticks of
    // OUTSTANDING speculation can still be pending behind the stamp, and a
    // mispredict rollback across the tick that set the game's scene-exit
    // request (unk_C, restored gm state) erases the request AFTER the ready
    // stamp went out on the wire irrevocably. The remote then releases at
    // the stamped tick and exits; the rolled-back peer's outer loop cannot
    // (nw_SceneBarrier lives outside the replayed tick body) — one peer
    // leaves the scene, the other re-runs it → permanent split (v16q1:
    // host exited at 13067, client re-set unk_C at 13069; every "match-end
    // wedge" back to pace11 run 2 is this). Park serving until the frontier
    // confirms everything behind the stamp: no unconfirmed tick may remain
    // once a barrier flag is up. Cost: one latency-floor stall per barrier
    // arming, at a scene transition where it is imperceptible.
    // Payload fence: a journaled delivery (async payload landing in RESTORED
    // memory — scene preloads during the GAME!/victory window are the big
    // source) means a rollback right now could orphan an in-flight or
    // just-delivered transfer whose completion flags live in restored heap
    // (payload re-delivery fixes the BYTES; nothing can re-fire the flags).
    // Quiesce prediction and drain speculation while such traffic is
    // landing; renewed per delivery, expires PAYLOAD_FENCE_TICKS after the
    // last one. Music streaming never trips this (devcom buffers are
    // excluded, so those deliveries are not journaled).
    if (m_window != 0 && m_rollback.IsLoaded())
    {
      const u64 seq = m_rollback.DVDDeliverySeq();
      if (seq != m_last_delivery_seq)
      {
        m_last_delivery_seq = seq;
        if (m_serve_tick >= m_payload_fence_until)
        {
          INFO_LOG_FMT(EXPANSIONINTERFACE,
                       "MeleeNetplay: payload fence armed at tick {} (dvd deliveries {})",
                       m_serve_tick, seq);
        }
        m_payload_fence_until = m_serve_tick + PAYLOAD_FENCE_TICKS;
      }
    }
    const bool payload_fenced = m_serve_tick < m_payload_fence_until;
    if ((m_barrier_armed || m_quiesce_stamped || payload_fenced) && m_window != 0)
    {
      std::lock_guard lk(m_frames_lock);
      if (m_confirmed_frontier < m_serve_tick)
      {
        m_barrier_drain_polls++;
        if (m_match_pacing >= 3)
          SuspendThrottle(true);
        Common::SleepCurrentThread(1);
        return u32(POLL_WAIT) << 24;
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
      if (m_match_pacing == 3 && m_throttle_suspended)
        SuspendThrottle(false);
      if (m_match_pacing == 4)
        UpdateSchedulePacing();
      NoteServeCycles();
      return u32(POLL_READY) << 24;
    }
    // Remote inputs late: predict instead of stalling, if allowed. Gates:
    // window budget vs the confirmed frontier; real matches only (see
    // InRealMatch — attract demos pass the crc-evolution test, but their
    // memcard/DVD-heavy scene traffic makes any rollback there a
    // completion-amnesia wedge hazard); local inputs must already be looped
    // back (they always are: the game SENDs tick+delay before polling tick).
    if (m_window != 0 && m_rollback.IsLoaded() && InRealMatch() && !m_barrier_armed &&
        !m_quiesce_stamped && !payload_fenced)
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
        if (m_match_pacing == 3 && m_throttle_suspended)
          SuspendThrottle(false);
        if (m_match_pacing == 4)
          UpdateSchedulePacing();
        NoteServeCycles();
        return u32(POLL_READY) << 24;
      }
    }
    if (!m_stall_active)
    {
      m_stall_start = std::chrono::steady_clock::now();
      m_stall_active = true;
    }
    // Mode 3: the WAIT spin advances the emulated clock (measured 2.3x wall
    // in pace9 -- 6.6 fields burned per 48ms window-edge park), and the
    // throttle then bills those fields against wall time after the park,
    // freezing the game (~123ms observed). That, not the replay burst
    // (mid ~0.4 fields), is the ~7-field-per-rollback pacing cost: both
    // peers' post-park freezes delay their sends, interlocking at ~30
    // ticks/s. Suspending here re-anchors the reference continuously, so
    // spin-burned emulated time is forgiven and serves resume debt-free at
    // the next READY.
    if (m_match_pacing >= 3)
      SuspendThrottle(true);
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
      m_last_exchange_us.store(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
      SendInputs(ReadBE32(buf), buf + 4);
      EnsureSeedTraceArmed("CMD_SEND");
      // Per-tick markers interleave with the MBP stream so each traced write
      // attributes to an exact game tick (checksum lines are 60-coarse and
      // stop entirely at a desync -- exactly where attribution matters most).
      if (m_seed_trace_ring)
        m_seed_trace_ring->Push(0, m_replay_serving, 0, m_serve_tick);
      else if (m_trace_seed_addr != 0)
        NOTICE_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: trace tick={} replaying={}",
                       m_serve_tick, m_replay_serving);
      // Snapshot point: the game is parked in this transaction at the top of
      // tick m_serve_tick, before that tick's inputs are injected or its
      // logic runs — ring[T] is the pre-state of tick T.
      if (m_rollback.IsLoaded())
      {
        // Window mode captures at RECV instead (see CMD_RECV): a rollback's
        // ORIGIN serve tick is captured here during the pass the rollback
        // invalidates, and the replay re-captures only target..serve-1 — the
        // origin slot stayed stale, so a hash tick landing on it compared
        // wrong-pass state (target8: ~94% of intervals flagged with sims
        // byte-identical), and a later rollback TARGETING it restored the
        // invalidated pass. RECV is the same semantic point (pre-inject,
        // nothing but polls between SEND and RECV) and covers normal,
        // replayed, and post-replay-origin serves uniformly.
        if (m_window == 0)
        {
          m_rollback.Capture(m_system, m_serve_tick);
          HashCapturedTick(m_serve_tick);
        }
        if (m_rollback.TotalCaptures() % 600 == 0)
        {
          INFO_LOG_FMT(EXPANSIONINTERFACE,
                       "MeleeRollback: {} captures, {:.2f} MB each, last {} us",
                       m_rollback.TotalCaptures(),
                       m_rollback.SnapshotBytes() / (1024.0 * 1024.0),
                       m_rollback.LastCaptureMicros());
        }
        // Emit before any torture rewind: in torture mode the ORIGINAL pass
        // is the canonical timeline (replay is a fidelity test of it), so
        // ring[T], just captured, is final right now. Under prediction this
        // drains whatever the frontier has confirmed since the last poll.
        {
          std::lock_guard lk(m_frames_lock);
          EmitSnapshotChecksumsLocked();
        }
        MaybeDumpDesyncRing();
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
      for (const auto& watch : m_rollback.Watches())
      {
        INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: watch {}={:08x} tick={}", watch.label,
                     m_rollback.ReadWatch(m_system, watch), tick);
      }
      // The game's live-state hash stays LOCAL-ONLY when the device oracle
      // is active: it hashes memory at submission time, which under
      // prediction covers speculative and render-cadence-dependent bytes —
      // the false-desync class of target run 3 (byte-identical sims,
      // permanently split transmitted hashes). Cross-peer comparison runs on
      // ring-snapshot hashes instead (EmitSnapshotChecksumsLocked); this
      // submission still feeds MatchStateEvolving (the fights-only gate for
      // torture/prediction), the watch log cadence, and the desync dump
      // pacing above. Tables without hash lines fall back to exchanging the
      // game hash — only lockstep runs are honest there, but rollback needs
      // a region table anyway.
      if (!m_rollback.HasHashSpans())
      {
        u8 payload[4];
        WriteBE32(payload, crc);
        EnqueueMessage(MSG_CHECKSUM, 0, tick, payload, 4);
        std::lock_guard lk(m_frames_lock);
        m_local_crcs[tick] = crc;
        const auto remote = m_remote_crcs.find(tick);
        if (remote != m_remote_crcs.end())
        {
          const std::map<u8, u32> arrived = remote->second;
          for (const auto& [peer_mask, peer_crc] : arrived)
            CompareChecksumLocked(tick, peer_mask, crc, peer_crc);
        }
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

bool CEXIMeleeNetplay::InRealMatch()
{
  // GameRouting bytes via the major-scene watch (first watch by generator
  // convention, same one SameScene keys on): byte 0 = current major mode,
  // byte 3 = current minor scene. 0x02 = VS mode; minors 02 = MATCH,
  // 03 = SUDDEN DEATH (gm_803DD9A0_Scenes). Everything else — boot, title,
  // attract (0x18, which MatchStateEvolving alone would pass), menus, CSS,
  // SSS, results — stays lockstep.
  const auto& watches = m_rollback.Watches();
  if (watches.empty())
    return MatchStateEvolving();  // no watch configured: legacy loose gate
  const u32 routing = m_rollback.ReadWatch(m_system, watches.front());
  const u32 major = routing >> 24;
  const u32 minor = routing & 0xFF;
  const bool in_match =
      MatchStateEvolving() && major == 0x02 && (minor == 0x02 || minor == 0x03);
  if (in_match != m_in_match_gate)
  {
    m_in_match_gate = in_match;
    if (m_match_pacing == 2)
      SuspendThrottle(in_match);
    INFO_LOG_FMT(EXPANSIONINTERFACE,
                 "MeleeNetplay: prediction gate {} (routing={:08x} tick={})",
                 in_match ? "OPEN (real match)" : "closed", routing, m_serve_tick);
  }
  return in_match;
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
    else if (!m_rollback.IOEpochUnchanged(m_system, target))
    {
      // A completion was DELIVERED inside the window — invisible to the
      // in-flight predicates above, but restoring would rewind the game's
      // request bookkeeping to "waiting" for a callback that already fired
      // and will never re-fire (HSD synth spin-waits on ARAM loads forever).
      // The slot is permanently unrestorable; skip the interval.
      m_restore_refused_epoch++;
      INFO_LOG_FMT(EXPANSIONINTERFACE,
                   "MeleeRollback: torture skipped at tick {} (async completion in window)",
                   m_serve_tick);
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
    NOTICE_LOG_FMT(EXPANSIONINTERFACE,
                   "MeleeNetplay: game requested handshake; blocking until session resolves");
    // Block until the session resolves — with no deadline. The link layer
    // retries forever, so a timed fallback here races human-paced starts (a
    // joiner boots, the host taps Start >60s later): the game silently drops
    // to an offline boot, the transport then forms the session anyway, and
    // the in-session peer wedges at its first exchange (S4 finding,
    // 2026-07-17). The only exits are resolution or explicit cancellation
    // (quit → m_running cleared; peer link death → m_session_failed).
    while (!m_session_ready && !m_session_failed && m_running.IsSet())
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
    // Session census (v4). Without these the game had to hardcode the
    // participating ports (nw.combined_mask = 0x3, "MVP is 1v1"), which forced
    // ports 2/3 to PAD_ERR_NO_CONTROLLER on every RECV.
    blob[16] = m_combined_mask;
    blob[17] = m_barrier_mask;
    blob[18] = static_cast<u8>(m_players);
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
    // Full-speed fork oracle: log the end-of-previous-tick RNG seed at every
    // serve, normal AND replayed. The MBP write tracer masks the real-time-
    // conditioned fork class (its slowdown gives the DSP time to keep up with
    // replay bursts); reading the watch value per tick costs nothing, so
    // cross-peer alignment pins a roll-count fork to the exact tick and shows
    // whether a replay reproduced the straight-through walk.
    if (m_seed_watch != -2 && m_rollback.IsLoaded())
    {
      if (m_seed_watch < 0)
      {
        const auto& ws = m_rollback.Watches();
        for (size_t i = 0; i < ws.size(); i++)
        {
          if (ws[i].label == "rng-seed")
            m_seed_watch = static_cast<int>(i);
        }
        if (m_seed_watch < 0)
          m_seed_watch = -2;  // no seed watch in this region table: stop looking
      }
      if (m_seed_watch >= 0)
      {
        INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: tickseed tick={} replay={} seed={:08x}",
                     m_serve_tick, m_replay_serving,
                     m_rollback.ReadWatch(m_system, m_rollback.Watches()[m_seed_watch]));
      }
    }
    // Window mode: EVERY serve (normal, replayed, post-replay origin)
    // captures its pre-state here — the one point all three pass through on
    // the timeline that ends up canonical (see the CMD_SEND comment).
    // Lockstep keeps SEND-time captures (torture mode 1 restores ring[serve]
    // between SEND and RECV).
    const u32 replay_tag = m_replay_serving;
    if (m_window != 0 && m_rollback.IsLoaded())
    {
      m_rollback.Capture(m_system, m_serve_tick);
      HashCapturedTick(m_serve_tick);
    }
    if (m_replay_serving != 0)
    {
      if (m_window == 0 && m_rollback.IsLoaded())
      {
        m_rollback.Capture(m_system, m_serve_tick);
        HashCapturedTick(m_serve_tick);
      }
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
        // Cross-peer INPUT oracle (companion to tickseed): hash of the exact
        // 4-port block served for this tick; last occurrence per tick =
        // corrected history. If both peers' streams align through a state
        // fork, the divergence is restore fidelity — not input serving.
        INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: padhash tick={} replay={} hash={:08x}",
                     m_serve_tick, replay_tag,
                     Common::ComputeCRC32(pads, 4 * PAD_BYTES));
        // Keep a rollback window of history — replays re-read these ticks.
        // (Pure lockstep pruned everything up to the serve tick here.)
        // Never prune at/above the confirmed frontier: validation needs the
        // real inputs there, and a pending rollback needs the whole span.
        if (m_serve_tick > MeleeRollbackState::RING_SIZE)
        {
          const u32 prune_to = std::min(
              u32(m_serve_tick - MeleeRollbackState::RING_SIZE), m_confirmed_frontier);
          m_frames.erase(m_frames.begin(), m_frames.upper_bound(prune_to));
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
