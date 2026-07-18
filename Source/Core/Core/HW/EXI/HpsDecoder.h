// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Streaming decoder for HAL's HPS ("HALPST") DSP-ADPCM music format, used for
// every background music track in Super Smash Bros. Melee. Layout reference:
// DarylPinto/hps_decode HPS-LAYOUT.md, cross-checked against the GALE01 disc
// (all 98 tracks: stereo, 32kHz, 0x10000-byte blocks, loop = last block's
// next-pointer aimed back into the file).
//
// Standalone on purpose: no Core dependencies beyond CommonTypes, so it is
// unit-testable (Source/UnitTests/Core/HpsDecoderTest.cpp) and reads through a
// caller-supplied callback instead of touching any emulated device. Malformed
// input must stop-and-log via the failure state, never crash: every offset and
// length is bounds-checked against the file size, and a degenerate block cycle
// that produces no samples terminates instead of spinning.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

class HpsDecoder final
{
public:
  // Reads `length` bytes at `offset` (relative to the start of the HPS file)
  // into `out`; returns false on failure.
  using ReadCallback = std::function<bool(u64 offset, u64 length, u8* out)>;

  HpsDecoder(ReadCallback reader, u64 file_size);

  // Parses the file header and channel info. Must succeed before DecodeNext.
  bool Init(std::string* error);

  u32 GetSampleRate() const { return m_sample_rate; }

  // Decodes up to max_frames interleaved stereo sample frames (L,R pairs) into
  // out, following block links and looping forever for looping tracks.
  // Returns the number of frames produced; 0 means the track ended (no loop)
  // or decoding failed -- distinguish with HasFailed().
  std::size_t DecodeNext(s16* out, std::size_t max_frames);

  bool HasEnded() const { return m_ended; }
  bool HasFailed() const { return m_failed; }
  const std::string& GetError() const { return m_error; }

private:
  bool LoadNextBlock();
  bool Fail(std::string reason);

  static constexpr u64 FIRST_BLOCK_OFFSET = 0x80;
  static constexpr u32 BLOCK_HEADER_BYTES = 0x20;
  static constexpr u32 CHANNELS = 2;
  static constexpr u32 SAMPLES_PER_ADPCM_FRAME = 14;
  static constexpr u32 ADPCM_FRAME_BYTES = 8;
  static constexpr u32 COEF_PAIRS = 8;
  // A real block is <= largest_block_length (0x10000 on the GALE01 disc); this
  // only bounds a corrupt header before the file-size check would.
  static constexpr u32 MAX_BLOCK_DATA_BYTES = 8 * 1024 * 1024;
  // Blocks with no decodable frames make no forward progress; a chain of them
  // longer than this is a degenerate cycle, not a playlist.
  static constexpr u32 MAX_BLOCKS_WITHOUT_OUTPUT = 1024;

  ReadCallback m_reader;
  u64 m_file_size;

  u32 m_sample_rate = 0;
  s16 m_coefs[CHANNELS][COEF_PAIRS][2]{};

  u64 m_next_block_offset = FIRST_BLOCK_OFFSET;
  u32 m_blocks_without_output = 0;

  std::vector<u8> m_block_data;  // scratch: current block's raw ADPCM bytes
  std::vector<s16> m_buffer;     // decoded interleaved LR samples of current block
  std::size_t m_buffer_pos = 0;  // consumed samples (not frames) in m_buffer

  bool m_initialized = false;
  bool m_ended = false;
  bool m_failed = false;
  std::string m_error;
};
