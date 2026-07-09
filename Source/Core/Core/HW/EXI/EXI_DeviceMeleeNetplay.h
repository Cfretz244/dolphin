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
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Core/HW/EXI/EXI_Device.h"

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
    CMD_SEND = 0x02,       // then DMAWrite 64B {u32 tick, PADStatus[4], pad}
    CMD_POLL = 0x03,       // ImmRead 4B: 1 if serve-tick frame ready
    CMD_RECV = 0x04,       // then DMARead 64B {PADStatus[4], pad}; advances serve tick
    CMD_CHECKSUM = 0x05,   // then DMAWrite 32B {u32 tick, u32 crc32, pad}
  };

  static constexpr u32 DEVICE_ID = 0x4D4E4554;  // 'MNET'; unknown to CARD -> "no card"
  static constexpr u8 PROTO_VERSION = 1;
  static constexpr u32 PAD_BYTES = 12;  // sizeof(SDK PADStatus)

  // Wire message types
  enum : u8
  {
    MSG_HELLO = 0x10,   // host->client {u8 ver, u8 host_mask, u8 delay, u8 pad, u32be seed}
    MSG_HELLO_ACK = 0x11,  // client->host {u8 ver, u8 client_mask}
    MSG_INPUTS = 0x01,     // {tick} payload: 12B per set mask bit, ascending port order
    MSG_CHECKSUM = 0x02,   // {tick} payload: 4B crc
  };

  struct Frame
  {
    u8 have_mask = 0;
    std::array<std::array<u8, PAD_BYTES>, 4> pads{};
  };

  void NetThread();
  bool EstablishSession(sf::TcpSocket& sock);
  void HandleMessage(u8 type, u8 mask, u32 tick, const u8* payload, u32 len);
  void SendInputs(u32 tick, const u8* pads_48b);
  void SendMessageRaw(u8 type, u8 mask, u32 tick, const u8* payload, u16 len);
  bool FrameReady(u32 tick);

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

  // --- CPU-thread transaction state
  u8 m_command = CMD_ID;
  u32 m_serve_tick = 0;
  bool m_warned_savestate = false;
};
}  // namespace ExpansionInterface
