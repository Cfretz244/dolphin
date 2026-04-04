// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAVE_DELTA_AUDIO

#include "AudioCommon/DeltaSoundStream.h"

#include "AudioCommon/Mixer.h"

DeltaSoundStream::DeltaSoundStream() = default;
DeltaSoundStream::~DeltaSoundStream() = default;

bool DeltaSoundStream::Init()
{
  // Unlike NullSound, do NOT call SetSampleRate(0).
  // The SoundStream constructor creates a Mixer with 48kHz output rate,
  // which is exactly what Delta expects.
  return true;
}

bool DeltaSoundStream::SetRunning(bool running)
{
  m_running = running;
  return true;
}

void DeltaSoundStream::SetVolume(int volume)
{
  m_volume = volume;
}

int DeltaSoundStream::PullSamples(short* buffer, int numSamples)
{
  if (!m_running || !m_mixer)
    return 0;

  return m_mixer->Mix(buffer, numSamples);
}

#endif  // HAVE_DELTA_AUDIO
