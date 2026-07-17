// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/MeleeNetplayExternalTestPump.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <SFML/Network.hpp>

#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/HW/EXI/MeleeNetplayExternalTransport.h"

namespace ExpansionInterface::MeleeNetplayExternalTestPump
{
namespace
{
struct PumpLink
{
  int id = -1;
  sf::TcpSocket socket;
  std::mutex send_lock;
  std::thread rx_thread;
};

std::mutex s_links_lock;
std::vector<std::unique_ptr<PumpLink>> s_links;
Common::Flag s_running;
std::thread s_main_thread;

PumpLink* FindLink(int id)
{
  std::lock_guard lk(s_links_lock);
  for (auto& link : s_links)
  {
    if (link->id == id)
      return link.get();
  }
  return nullptr;
}

void SendCallback(int id, const uint8_t* data, size_t len, void*)
{
  PumpLink* link = FindLink(id);
  if (!link)
    return;
  std::lock_guard lk(link->send_lock);
  std::size_t total = 0;
  while (total < len)
  {
    std::size_t sent = 0;
    const auto status = link->socket.send(data + total, len - total, sent);
    if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay(testpump): send failed on link {}", id);
      return;
    }
    total += sent;
  }
}

void RxThread(PumpLink* link)
{
  Common::SetCurrentThreadName("MNETTestPumpRx");
  u8 buf[4096];
  while (s_running.IsSet())
  {
    std::size_t got = 0;
    const auto status = link->socket.receive(buf, sizeof(buf), got);
    if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error)
      break;
    if (got != 0)
      Dolphin_MeleeNetplay_ExternalPushBytes(link->id, buf, got);
    else
      Common::SleepCurrentThread(1);
  }
  Dolphin_MeleeNetplay_ExternalLinkClosed(link->id);
}

// Attach order is join order, which the device turns into the port census --
// same contract the iOS pairing layer follows.
void AdoptSocket(std::unique_ptr<PumpLink> link)
{
  link->id = Dolphin_MeleeNetplay_ExternalAttachLink();
  link->socket.setBlocking(true);
  link->rx_thread = std::thread(RxThread, link.get());
  INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay(testpump): socket adopted as link {}", link->id);
  std::lock_guard lk(s_links_lock);
  s_links.push_back(std::move(link));
}

void MainThread(bool is_host, std::string spec, u32 links)
{
  Common::SetCurrentThreadName("MNETTestPump");
  const auto colon = spec.rfind(':');
  if (colon == std::string::npos)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay(testpump): bad spec '{}'", spec);
    return;
  }
  const u16 port = static_cast<u16>(std::strtoul(spec.c_str() + colon + 1, nullptr, 10));

  if (is_host)
  {
    sf::TcpListener listener;
    if (listener.listen(port) != sf::Socket::Status::Done)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay(testpump): listen({}) failed", port);
      return;
    }
    listener.setBlocking(false);
    u32 accepted = 0;
    while (accepted < links && s_running.IsSet())
    {
      auto link = std::make_unique<PumpLink>();
      if (listener.accept(link->socket) != sf::Socket::Status::Done)
      {
        Common::SleepCurrentThread(5);
        continue;
      }
      AdoptSocket(std::move(link));
      accepted++;
    }
  }
  else
  {
    const auto remote = sf::IpAddress::resolve(spec.substr(0, colon));
    if (!remote)
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeNetplay(testpump): cannot resolve '{}'", spec);
      return;
    }
    auto link = std::make_unique<PumpLink>();
    link->socket.setBlocking(true);
    while (s_running.IsSet())
    {
      if (link->socket.connect(*remote, port, sf::seconds(2)) == sf::Socket::Status::Done)
      {
        AdoptSocket(std::move(link));
        return;
      }
      Common::SleepCurrentThread(500);
    }
  }
}
}  // namespace

void Start(bool is_host, const std::string& spec, u32 links)
{
  if (s_running.IsSet())
    return;
  WARN_LOG_FMT(EXPANSIONINTERFACE,
               "MeleeNetplay: EXTERNAL TEST PUMP active ({} '{}', {} link(s)) -- "
               "test configuration, never production",
               is_host ? "listen" : "connect", spec, links);
  s_running.Set();
  Dolphin_MeleeNetplay_ExternalSetSendCallback(SendCallback, nullptr);
  s_main_thread = std::thread(MainThread, is_host, spec, links);
}

void Stop()
{
  if (!s_running.IsSet())
    return;
  s_running.Clear();
  {
    std::lock_guard lk(s_links_lock);
    for (auto& link : s_links)
      link->socket.disconnect();
  }
  if (s_main_thread.joinable())
    s_main_thread.join();
  std::vector<std::unique_ptr<PumpLink>> stale;
  {
    std::lock_guard lk(s_links_lock);
    stale.swap(s_links);
  }
  for (auto& link : stale)
  {
    if (link->rx_thread.joinable())
      link->rx_thread.join();
  }
  Dolphin_MeleeNetplay_ExternalSetSendCallback(nullptr, nullptr);
  Dolphin_MeleeNetplay_ExternalReset();
}
}  // namespace ExpansionInterface::MeleeNetplayExternalTestPump
