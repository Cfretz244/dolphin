// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// App-owned transport for Melee netplay (research/adhoc-pairing-design.md).
//
// The device's SFML transport is IPv4 TCP, which cannot ride Apple's AWDL
// peer-to-peer Wi-Fi (Bonjour discovery + IPv6 link-local endpoints that iOS
// never exposes as raw sockets). So on iOS the APP owns the connections via
// Network.framework and pumps raw protocol bytes through this seam; the
// device consumes them through the same ILinkStream interface the TCP path
// uses. The wire protocol is byte-identical either way -- the game and the
// message layer never know which transport is underneath.
//
// Ordering contract for the app:
//   1. Dolphin_MeleeNetplay_ExternalSetSendCallback(cb, ctx)
//   2. Dolphin_MeleeNetplay_ExternalAttachLink() once per established
//      connection, BEFORE booting the game. Attach order is join order, and
//      join order is the port-census deal order (first attached link = P2).
//   3. Pump inbound bytes with Dolphin_MeleeNetplay_ExternalPushBytes();
//      deliver outbound bytes from the callback (any thread, must not block).
//   4. Dolphin_MeleeNetplay_ExternalReset() after the session ends.
//
// The registry is process-global so links (and early inbound bytes -- e.g. a
// HELLO arriving while the emulator is still booting) exist before the EXI
// device is constructed; the device adopts them at handshake time.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Flag.h"

extern "C"
{
typedef void (*Dolphin_MeleeNetplay_SendFn)(int link, const uint8_t* data, size_t len, void* ctx);

// Returns the new link's id (>= 0). Ids are dense and attach-ordered.
int Dolphin_MeleeNetplay_ExternalAttachLink(void);
void Dolphin_MeleeNetplay_ExternalPushBytes(int link, const uint8_t* data, size_t len);
void Dolphin_MeleeNetplay_ExternalLinkClosed(int link);
// Register BEFORE the first AttachLink. The callback may be invoked from the
// device's per-peer send threads and must hand bytes off without blocking.
void Dolphin_MeleeNetplay_ExternalSetSendCallback(Dolphin_MeleeNetplay_SendFn cb, void* ctx);
void Dolphin_MeleeNetplay_ExternalReset(void);
}

namespace ExpansionInterface
{
// Transport seam for one peer link. Implementations: TcpLinkStream (SFML,
// device-owned socket -- the qualified Mac path) and ExternalLinkStream
// (app-pumped mailbox, below). Both are consumed by the device's per-peer
// send/rx threads and the handshakes.
class ILinkStream
{
public:
  virtual ~ILinkStream() = default;
  // Blocking send-all. false = link dead.
  virtual bool Send(const u8* data, std::size_t len) = 0;
  // Blocking recv-all; bails when `running` clears or the link dies.
  virtual bool Recv(u8* data, std::size_t len, const Common::Flag& running) = 0;
  // Idempotent; unblocks pending Recv/Send.
  virtual void Close() = 0;
};

// Byte mailbox pumped by the app through the C surface above. Inbound bytes
// buffer here (bounded only by the protocol's own send pacing); outbound
// bytes go straight to the app callback on the caller's thread.
class ExternalLinkStream final : public ILinkStream
{
public:
  explicit ExternalLinkStream(int id) : m_id(id) {}

  bool Send(const u8* data, std::size_t len) override;
  bool Recv(u8* data, std::size_t len, const Common::Flag& running) override;
  void Close() override;

  void PushInbound(const u8* data, std::size_t len);
  int Id() const { return m_id; }

private:
  const int m_id;
  std::mutex m_lock;
  std::condition_variable m_cv;
  std::deque<u8> m_inbound;
  bool m_closed = false;
};

namespace MeleeNetplayExternal
{
// Snapshot of the links attached so far, in attach order.
std::vector<std::shared_ptr<ExternalLinkStream>> AttachedLinks();
}  // namespace MeleeNetplayExternal
}  // namespace ExpansionInterface
