// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_DeviceMeleeNetplay.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/Random.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
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

  m_fake_latency_ms = std::max(0, Config::Get(Config::MAIN_MELEE_NETPLAY_FAKE_LATENCY_MS));
  m_fake_jitter_ms = std::max(0, Config::Get(Config::MAIN_MELEE_NETPLAY_FAKE_JITTER_MS));
  if (m_fake_latency_ms != 0 || m_fake_jitter_ms != 0)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE,
                 "MeleeNetplay: SIMULATING {} ms one-way latency (+0..{} ms jitter). "
                 "Delay window is {} frames ~= {} ms.",
                 m_fake_latency_ms, m_fake_jitter_ms, m_delay, m_delay * 100 / 6);
  }

  if (m_is_host)
  {
    m_seed = Config::Get(Config::MAIN_MELEE_NETPLAY_SEED);
    if (m_seed == 0)
      m_seed = Common::Random::GenerateValue<u32>();
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
    u8 hello[8];
    hello[0] = MSG_HELLO;
    hello[1] = PROTO_VERSION;
    hello[2] = m_local_mask;
    hello[3] = m_delay;
    WriteBE32(&hello[4], m_seed);
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
    u8 hello[8];
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
  Core::DisplayMessage(fmt::format("MeleeNetplay: session up (local ports {:#x}, delay {})",
                                   m_local_mask, m_delay),
                       8000);

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
    if (m_fake_latency_ms != 0 || m_fake_jitter_ms != 0)
    {
      int hold = m_fake_latency_ms;
      if (m_fake_jitter_ms > 0)
        hold += std::uniform_int_distribution<int>(0, m_fake_jitter_ms)(m_jitter_rng);
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

  std::vector<u32> sorted = m_stall_samples_us;
  std::sort(sorted.begin(), sorted.end());
  const size_t n = sorted.size();
  const auto pct = [&](double p) { return sorted[std::min(n - 1, size_t(p * n))]; };

  // A frame budget at 60Hz is 16667us. Ticks whose stall exceeds that cannot
  // have run at full speed, so this fraction is the "how much slower than
  // realtime" figure a player would actually feel.
  const size_t over_budget = std::count_if(sorted.begin(), sorted.end(),
                                           [](u32 us) { return us > 16667; });

  u64 sum = 0;
  for (u32 us : sorted)
    sum += us;

  INFO_LOG_FMT(EXPANSIONINTERFACE,
               "MeleeNetplay: stalls over {} ticks (us): mean={} p50={} p90={} p99={} max={} "
               "| over-frame-budget={} ({:.1f}%)",
               n, sum / n, pct(0.50), pct(0.90), pct(0.99), sorted[n - 1], over_budget,
               100.0 * double(over_budget) / double(n));

  m_stall_samples_us.clear();
  m_stall_report_tick = m_serve_tick;
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
      return 1;
    }
    if (!m_stall_active)
    {
      m_stall_start = std::chrono::steady_clock::now();
      m_stall_active = true;
    }
    // Be polite to the host CPU while the game spins on us.
    Common::SleepCurrentThread(1);
    return 0;
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
      SendInputs(ReadBE32(buf), buf + 4);
    break;
  case CMD_CHECKSUM:
    if (size >= 8)
    {
      const u32 tick = ReadBE32(buf);
      const u32 crc = ReadBE32(buf + 4);
      SendMessageRaw(MSG_CHECKSUM, 0, tick, buf + 4, 4);
      std::lock_guard lk(m_frames_lock);
      m_local_crcs[tick] = crc;
      const auto remote = m_remote_crcs.find(tick);
      if (remote != m_remote_crcs.end() && remote->second != crc)
      {
        ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: DESYNC at tick {} ({:08x} != {:08x})",
                      tick, crc, remote->second);
        Core::DisplayMessage(fmt::format("MeleeNetplay: DESYNC at tick {}", tick), 20000);
      }
    }
    break;
  default:
    break;
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
    u8 pads[320] = {};
    {
      std::lock_guard lk(m_frames_lock);
      const auto it = m_frames.find(m_serve_tick);
      if (it != m_frames.end())
      {
        for (u32 port = 0; port < 4; port++)
          std::memcpy(pads + port * PAD_BYTES, it->second.pads[port].data(), PAD_BYTES);
        // Prune everything up to and including this tick; lockstep never re-reads.
        m_frames.erase(m_frames.begin(), m_frames.upper_bound(m_serve_tick));
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
