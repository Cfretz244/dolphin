// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Host-side background music for Melee netplay ("jukebox", after Slippi's).
// The game cannot stream HPS music from the emulated DVD under rollback (the
// v15..v21 wedge tail: async-completion amnesia in every state split), so
// under netplay the game merely NOTIFIES the device (play/stop/volume over the
// MNET EXI channel) and this class plays the track entirely outside the sim:
// a PRIVATE cloned disc reader (never the mounted volume -- the DVD thread
// owns that cursor), one dedicated decode thread, and a dedicated mixer
// channel. No emulated DVD traffic, no MEM1 writes, nothing for rollback to
// touch; replay-duplicated play events are deduped by track path.

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}
namespace DiscIO
{
class Volume;
}

class MeleeJukebox final
{
public:
  explicit MeleeJukebox(Core::System& system);
  ~MeleeJukebox();  // joins the decode thread; the mixer FIFO fades out

  // All entry points latch a command and return immediately -- safe from the
  // CPU thread inside an EXI transaction (no disk I/O, no blocking).
  void PlayTrack(std::string disc_path);  // e.g. "/audio/hyaku.hps"
  void Stop();
  void SetGameVolume(u8 melee_volume_0_254);  // the game's final music volume

private:
  void ThreadLoop();
  void PlayCurrentTrack(u64 generation);
  bool EnsureDisc();
  void ApplyVolume();

  Core::System& m_system;

  std::thread m_thread;
  std::mutex m_lock;
  std::condition_variable m_cv;
  // Generation bumps on every latched command; the decode loop rechecks it
  // between chunks, so track cutover abandons the old decode within one chunk
  // (<= the FIFO target depth, ~64ms of residue -- accepted).
  u64 m_generation = 0;
  bool m_want_play = false;
  std::string m_want_path;
  std::atomic<bool> m_running{true};

  std::atomic<u8> m_game_volume{254};

  std::unique_ptr<DiscIO::Volume> m_disc;
};
