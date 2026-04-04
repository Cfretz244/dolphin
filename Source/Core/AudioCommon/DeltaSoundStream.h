// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "AudioCommon/SoundStream.h"
#include "Common/CommonTypes.h"

class DeltaSoundStream final : public SoundStream
{
#ifdef HAVE_DELTA_AUDIO
public:
  DeltaSoundStream();
  ~DeltaSoundStream() override;

  static bool IsValid() { return true; }

  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;

  /// Pull mixed audio samples from Dolphin's Mixer.
  /// Returns the number of stereo sample frames actually mixed.
  /// @param buffer Output buffer for interleaved 16-bit stereo PCM samples.
  /// @param numSamples Maximum number of stereo frames to mix.
  int PullSamples(short* buffer, int numSamples);

private:
  bool m_running = false;
  int m_volume = 100;
#endif
};
