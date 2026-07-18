// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/HpsDecoder.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace
{
u32 ReadBE32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

s16 ReadBE16s(const u8* p)
{
  return static_cast<s16>((u16(p[0]) << 8) | u16(p[1]));
}
}  // namespace

HpsDecoder::HpsDecoder(ReadCallback reader, u64 file_size)
    : m_reader(std::move(reader)), m_file_size(file_size)
{
}

bool HpsDecoder::Fail(std::string reason)
{
  m_failed = true;
  m_ended = true;
  if (m_error.empty())
    m_error = std::move(reason);
  return false;
}

bool HpsDecoder::Init(std::string* error)
{
  // File header (0x10) + one 0x38 channel info block per channel.
  u8 hdr[0x10 + CHANNELS * 0x38];
  if (m_file_size < sizeof(hdr) + BLOCK_HEADER_BYTES)
    Fail("file too small for HPS header");
  else if (!m_reader(0, sizeof(hdr), hdr))
    Fail("read failed on HPS header");
  else if (std::memcmp(hdr, " HALPST\0", 8) != 0)
    Fail("bad HPS magic");
  else if (ReadBE32(hdr + 0x0C) != CHANNELS)
    Fail("unsupported channel count (every Melee track is stereo)");
  else
  {
    m_sample_rate = ReadBE32(hdr + 0x08);
    if (m_sample_rate < 4000 || m_sample_rate > 96000)
      Fail("implausible sample rate");
  }
  if (m_failed)
  {
    if (error)
      *error = m_error;
    return false;
  }

  // Channel info: {u32 largest_block_len, u32, u32 sample_count, u32,
  // s16 coefs[16], u8[8] initial decoder state}. Only the coefficients matter
  // here -- every block carries its own decoder state, which is also what
  // makes looping click-free (the loop target block resets the history).
  for (u32 ch = 0; ch < CHANNELS; ch++)
  {
    const u8* coefs = hdr + 0x10 + ch * 0x38 + 0x10;
    for (u32 pair = 0; pair < COEF_PAIRS; pair++)
    {
      m_coefs[ch][pair][0] = ReadBE16s(coefs + pair * 4);
      m_coefs[ch][pair][1] = ReadBE16s(coefs + pair * 4 + 2);
    }
  }

  m_initialized = true;
  return true;
}

std::size_t HpsDecoder::DecodeNext(s16* out, std::size_t max_frames)
{
  if (!m_initialized || m_failed)
    return 0;

  std::size_t written_frames = 0;
  while (written_frames < max_frames)
  {
    if (m_buffer_pos >= m_buffer.size())
    {
      if (m_ended || !LoadNextBlock())
        break;
    }
    const std::size_t avail_frames = (m_buffer.size() - m_buffer_pos) / CHANNELS;
    const std::size_t take = std::min(avail_frames, max_frames - written_frames);
    std::memcpy(out + written_frames * CHANNELS, m_buffer.data() + m_buffer_pos,
                take * CHANNELS * sizeof(s16));
    m_buffer_pos += take * CHANNELS;
    written_frames += take;
  }
  return written_frames;
}

bool HpsDecoder::LoadNextBlock()
{
  // Loops until a block yields samples (skipping empty blocks with a progress
  // bound) or the stream ends/fails.
  for (;;)
  {
    if (m_ended)
      return false;

    const u64 off = m_next_block_offset;
    if (off + BLOCK_HEADER_BYTES > m_file_size)
      return Fail("block header out of bounds");

    // Block header: {u32 dsp_data_len, u32 unused, u32 next_block_offset,
    // per-channel {u8 ps_hi, u8 ps, s16 hist1, s16 hist2, u16 pad}, u32 pad}.
    u8 hdr[BLOCK_HEADER_BYTES];
    if (!m_reader(off, BLOCK_HEADER_BYTES, hdr))
      return Fail("read failed on block header");

    const u32 dsp_len = ReadBE32(hdr);
    const u32 next = ReadBE32(hdr + 0x08);
    s16 hist[CHANNELS][2];
    for (u32 ch = 0; ch < CHANNELS; ch++)
    {
      hist[ch][0] = ReadBE16s(hdr + 0x0C + ch * 8 + 2);
      hist[ch][1] = ReadBE16s(hdr + 0x0C + ch * 8 + 4);
    }

    if (dsp_len > MAX_BLOCK_DATA_BYTES || dsp_len > m_file_size - off - BLOCK_HEADER_BYTES)
      return Fail("block data out of bounds");

    // Advance the cursor before decoding so a decode failure doesn't re-read
    // the same bad block. next==0 / all-ones is the no-loop end sentinel; a
    // looping track points back INTO the file instead.
    if (next == 0 || next == 0xFFFFFFFF)
      m_ended = true;
    else if (next < FIRST_BLOCK_OFFSET || next >= m_file_size)
      return Fail("bad next-block pointer");
    else
      m_next_block_offset = next;

    const u32 frames_per_channel = (dsp_len / ADPCM_FRAME_BYTES) / CHANNELS;
    if (frames_per_channel == 0)
    {
      if (++m_blocks_without_output > MAX_BLOCKS_WITHOUT_OUTPUT)
        return Fail("no forward progress (degenerate block cycle)");
      continue;
    }
    m_blocks_without_output = 0;

    m_block_data.resize(dsp_len);
    if (!m_reader(off + BLOCK_HEADER_BYTES, dsp_len, m_block_data.data()))
      return Fail("read failed on block data");

    // The first half of the block's ADPCM frames are the left channel, the
    // second half the right; both restart from the block header's history.
    const u32 out_frames = frames_per_channel * SAMPLES_PER_ADPCM_FRAME;
    m_buffer.resize(std::size_t(out_frames) * CHANNELS);
    m_buffer_pos = 0;
    for (u32 ch = 0; ch < CHANNELS; ch++)
    {
      const u8* src = m_block_data.data() + std::size_t(ch) * frames_per_channel * ADPCM_FRAME_BYTES;
      s32 h1 = hist[ch][0];
      s32 h2 = hist[ch][1];
      s16* dst = m_buffer.data() + ch;
      for (u32 f = 0; f < frames_per_channel; f++, src += ADPCM_FRAME_BYTES)
      {
        // Frame: header byte (coef-index nibble | scale-shift nibble) + 7 data
        // bytes = 14 samples. sample = clamp16((((nibble * scale) << 11) +
        // 1024 + coef1*hist1 + coef2*hist2) >> 11), history = last two samples.
        const s32 scale = 1 << (src[0] & 0xF);
        const u32 ci = src[0] >> 4;
        if (ci >= COEF_PAIRS)
          return Fail("coefficient index out of range");
        const s32 c1 = m_coefs[ch][ci][0];
        const s32 c2 = m_coefs[ch][ci][1];
        for (u32 b = 1; b < ADPCM_FRAME_BYTES; b++)
        {
          const s32 nibbles[2] = {static_cast<s8>(src[b]) >> 4,
                                  static_cast<s8>(src[b] << 4) >> 4};
          for (const s32 nibble : nibbles)
          {
            // s64: the s32 worst case ((8<<15)<<11 + two 32767^2 products)
            // overflows on hostile input.
            const s64 acc = (s64(nibble * scale) << 11) + 1024 + s64(c1) * h1 + s64(c2) * h2;
            const s32 sample = static_cast<s32>(std::clamp<s64>(acc >> 11, -32768, 32767));
            h2 = h1;
            h1 = sample;
            *dst = static_cast<s16>(sample);
            dst += CHANNELS;
          }
        }
      }
    }
    return true;
  }
}
