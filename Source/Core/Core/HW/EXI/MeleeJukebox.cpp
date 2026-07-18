// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/MeleeJukebox.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/HW/EXI/HpsDecoder.h"
#include "Core/System.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace
{
Mixer* GetMixer(Core::System& system)
{
  SoundStream* stream = system.GetSoundStream();
  return stream ? stream->GetMixer() : nullptr;
}
}  // namespace

MeleeJukebox::MeleeJukebox(Core::System& system) : m_system(system)
{
  m_thread = std::thread(&MeleeJukebox::ThreadLoop, this);
}

MeleeJukebox::~MeleeJukebox()
{
  m_running.store(false);
  {
    std::lock_guard lk(m_lock);
    m_generation++;
  }
  m_cv.notify_all();
  if (m_thread.joinable())
    m_thread.join();
}

void MeleeJukebox::PlayTrack(std::string disc_path)
{
  {
    std::lock_guard lk(m_lock);
    // Same-track dedup: vanilla Melee never restarts the playing song
    // (lbAudioAx same-song strcmp), so a duplicate play -- e.g. re-fired by a
    // replayed tick after rollback -- is a no-op, not a restart.
    if (m_want_play && m_want_path == disc_path)
      return;
    m_want_play = true;
    m_want_path = std::move(disc_path);
    m_generation++;
  }
  m_cv.notify_all();
}

void MeleeJukebox::Stop()
{
  {
    std::lock_guard lk(m_lock);
    if (!m_want_play)
      return;
    m_want_play = false;
    m_want_path.clear();
    m_generation++;
  }
  m_cv.notify_all();
  INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: stop");
}

void MeleeJukebox::SetGameVolume(u8 melee_volume_0_254)
{
  if (m_game_volume.exchange(melee_volume_0_254) != melee_volume_0_254)
    INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: game volume {}", melee_volume_0_254);
  ApplyVolume();
}

void MeleeJukebox::ApplyVolume()
{
  Mixer* mixer = GetMixer(m_system);
  if (!mixer)
    return;
  // melee final volume (0..254, already folds the sound-menu setting, pause
  // ducking and Starman) x the Dolphin-side jukebox volume (0..100, default
  // 80 -- Slippi's 0.8 keeps BGM under SFX). The FIFO's own volume is the
  // only music-vs-SFX balance knob on iOS (the app owns master volume).
  const int config_volume = std::clamp(Config::Get(Config::MAIN_MELEE_JUKEBOX_VOLUME), 0, 100);
  const u32 effective = static_cast<u32>(
      std::lround(255.0 * (m_game_volume.load() / 254.0) * (config_volume / 100.0)));
  mixer->SetJukeboxVolume(effective, effective);
}

bool MeleeJukebox::EnsureDisc()
{
  if (m_disc)
    return true;
  // Clone lazily: the device (and in debug-track mode this thread) can exist
  // before the boot process has mounted the disc. The clone opens a private
  // blob reader; the only race is against disc EJECT, which never happens in
  // a netplay session.
  auto& dvd_thread = m_system.GetDVDThread();
  for (int i = 0; i < 300 && m_running.load(); i++)
  {
    if (dvd_thread.HasDisc())
    {
      m_disc = dvd_thread.CloneDiscForHostReads();
      return m_disc != nullptr;
    }
    Common::SleepCurrentThread(100);
  }
  return false;
}

void MeleeJukebox::ThreadLoop()
{
  Common::SetCurrentThreadName("Melee Jukebox");
  u64 seen_generation = 0;
  while (m_running.load())
  {
    u64 generation;
    bool play;
    {
      std::unique_lock lk(m_lock);
      m_cv.wait(lk, [&] { return m_generation != seen_generation || !m_running.load(); });
      if (!m_running.load())
        return;
      seen_generation = m_generation;
      generation = m_generation;
      play = m_want_play;
    }
    if (play)
      PlayCurrentTrack(generation);
    // Stop: just cease pushing; the mixer FIFO fades the tail to silence.
  }
}

void MeleeJukebox::PlayCurrentTrack(u64 generation)
{
  std::string path;
  {
    std::lock_guard lk(m_lock);
    path = m_want_path;
  }

  if (!EnsureDisc())
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: no disc to read {} from", path);
    return;
  }

  const DiscIO::FileSystem* fs = m_disc->GetFileSystem(DiscIO::PARTITION_NONE);
  if (!fs)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: disc has no filesystem");
    return;
  }
  std::string_view lookup = path;
  while (!lookup.empty() && lookup.front() == '/')
    lookup.remove_prefix(1);
  const std::unique_ptr<DiscIO::FileInfo> file = fs->FindFileInfo(lookup);
  if (!file || file->IsDirectory())
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: track not found on disc: {}", path);
    return;
  }
  const u64 file_offset = file->GetOffset();
  const u64 file_size = file->GetSize();

  HpsDecoder decoder(
      [this, file_offset](u64 offset, u64 length, u8* out) {
        return m_disc->Read(file_offset + offset, length, out, DiscIO::PARTITION_NONE);
      },
      file_size);
  std::string error;
  if (!decoder.Init(&error))
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: {}: {}", path, error);
    return;
  }

  Mixer* mixer = GetMixer(m_system);
  if (!mixer)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: no mixer");
    return;
  }
  mixer->SetJukeboxInputSampleRate(decoder.GetSampleRate());
  ApplyVolume();
  NOTICE_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: playing {} ({} Hz, {} bytes)", path,
                 decoder.GetSampleRate(), file_size);

  // Pace decoding against wall-clock playback via the mixer queue depth, NOT
  // emulation ticks: a rollback replay burst then neither double-pushes nor
  // starves the stream. Keep the target below the mixer's configured buffer
  // depth or its late-queue playhead jump would eat the backlog.
  const std::size_t target_frames = decoder.GetSampleRate() * 64 / 1000;  // ~64ms
  constexpr std::size_t CHUNK_FRAMES = 512;
  s16 chunk[CHUNK_FRAMES * 2];
  u64 pushed_frames = 0;

  while (m_running.load())
  {
    {
      std::lock_guard lk(m_lock);
      if (m_generation != generation)
        return;  // new command (cutover or stop) -- abandon this track
    }
    if (mixer->GetJukeboxQueuedFrames() >= target_frames)
    {
      Common::SleepCurrentThread(5);
      continue;
    }
    const std::size_t got = decoder.DecodeNext(chunk, CHUNK_FRAMES);
    if (got == 0)
    {
      if (decoder.HasFailed())
        ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: {}: decode stopped: {}", path,
                      decoder.GetError());
      else
        INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: {}: track ended (no loop)", path);
      return;
    }
    mixer->PushJukeboxSamples(chunk, got);
    pushed_frames += got;
    if (pushed_frames % (48000 * 30) < CHUNK_FRAMES)
      INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeJukebox: {} frames pushed of {}", pushed_frames, path);
  }
}
