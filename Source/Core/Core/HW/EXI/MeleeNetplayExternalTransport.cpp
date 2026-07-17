// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/MeleeNetplayExternalTransport.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "Common/Logging/Log.h"

namespace ExpansionInterface
{
namespace
{
std::mutex s_registry_lock;
std::vector<std::shared_ptr<ExternalLinkStream>> s_links;
// Callback + ctx are set once by the app before the first attach (documented
// contract), so the two separate atomics never tear in practice.
std::atomic<Dolphin_MeleeNetplay_SendFn> s_send_cb{nullptr};
std::atomic<void*> s_send_ctx{nullptr};

std::shared_ptr<ExternalLinkStream> LinkById(int id)
{
  std::lock_guard lk(s_registry_lock);
  if (id < 0 || static_cast<size_t>(id) >= s_links.size())
    return nullptr;
  return s_links[id];
}
}  // namespace

bool ExternalLinkStream::Send(const u8* data, std::size_t len)
{
  {
    std::lock_guard lk(m_lock);
    if (m_closed)
      return false;
  }
  const auto cb = s_send_cb.load(std::memory_order_acquire);
  if (!cb)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE,
                  "MeleeNetplay: external link {} has no send callback registered", m_id);
    return false;
  }
  cb(m_id, data, len, s_send_ctx.load(std::memory_order_acquire));
  return true;
}

bool ExternalLinkStream::Recv(u8* data, std::size_t len, const Common::Flag& running)
{
  std::size_t total = 0;
  std::unique_lock lk(m_lock);
  while (total < len)
  {
    if (!running.IsSet())
      return false;
    if (m_inbound.empty())
    {
      // Drain-then-fail: a closed link still serves the bytes it already has.
      if (m_closed)
        return false;
      // Timed wait so a cleared `running` flag (which has no condvar) is
      // noticed promptly -- mirrors the TCP path's poll-and-sleep loop.
      m_cv.wait_for(lk, std::chrono::milliseconds(50));
      continue;
    }
    const std::size_t n = std::min(len - total, m_inbound.size());
    std::copy_n(m_inbound.begin(), n, data + total);
    m_inbound.erase(m_inbound.begin(), m_inbound.begin() + n);
    total += n;
  }
  return true;
}

void ExternalLinkStream::Close()
{
  std::lock_guard lk(m_lock);
  m_closed = true;
  m_cv.notify_all();
}

void ExternalLinkStream::PushInbound(const u8* data, std::size_t len)
{
  std::lock_guard lk(m_lock);
  if (m_closed)
    return;
  m_inbound.insert(m_inbound.end(), data, data + len);
  m_cv.notify_all();
}

namespace MeleeNetplayExternal
{
std::vector<std::shared_ptr<ExternalLinkStream>> AttachedLinks()
{
  std::lock_guard lk(s_registry_lock);
  return s_links;
}
}  // namespace MeleeNetplayExternal
}  // namespace ExpansionInterface

// ---------------------------------------------------------------------------
// C surface (the app side of the pump)
// ---------------------------------------------------------------------------

using ExpansionInterface::ExternalLinkStream;

extern "C" int Dolphin_MeleeNetplay_ExternalAttachLink(void)
{
  std::lock_guard lk(ExpansionInterface::s_registry_lock);
  const int id = static_cast<int>(ExpansionInterface::s_links.size());
  ExpansionInterface::s_links.push_back(std::make_shared<ExternalLinkStream>(id));
  INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: external link {} attached", id);
  return id;
}

extern "C" void Dolphin_MeleeNetplay_ExternalPushBytes(int link, const uint8_t* data, size_t len)
{
  if (auto stream = ExpansionInterface::LinkById(link))
    stream->PushInbound(data, len);
}

extern "C" void Dolphin_MeleeNetplay_ExternalLinkClosed(int link)
{
  if (auto stream = ExpansionInterface::LinkById(link))
    stream->Close();
}

extern "C" void Dolphin_MeleeNetplay_ExternalSetSendCallback(Dolphin_MeleeNetplay_SendFn cb,
                                                             void* ctx)
{
  ExpansionInterface::s_send_ctx.store(ctx, std::memory_order_release);
  ExpansionInterface::s_send_cb.store(cb, std::memory_order_release);
}

extern "C" void Dolphin_MeleeNetplay_ExternalReset(void)
{
  std::vector<std::shared_ptr<ExternalLinkStream>> stale;
  {
    std::lock_guard lk(ExpansionInterface::s_registry_lock);
    stale.swap(ExpansionInterface::s_links);
  }
  // Close outside the registry lock: a device thread blocked in Recv holds
  // the stream lock, never the registry lock, so this cannot deadlock.
  for (auto& stream : stale)
    stream->Close();
  INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay: external transport reset ({} links dropped)",
               stale.size());
}
