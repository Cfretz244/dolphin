// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Core/HW/EXI/HpsDecoder.h"

namespace
{
void PutBE32(std::vector<u8>& v, std::size_t off, u32 val)
{
  v[off] = u8(val >> 24);
  v[off + 1] = u8(val >> 16);
  v[off + 2] = u8(val >> 8);
  v[off + 3] = u8(val);
}

void PutBE16(std::vector<u8>& v, std::size_t off, u16 val)
{
  v[off] = u8(val >> 8);
  v[off + 1] = u8(val);
}

constexpr std::size_t HEADER_BYTES = 0x10 + 2 * 0x38;
constexpr std::size_t BLOCK_HEADER_BYTES = 0x20;

// Builds the 0x80-byte file header + channel info for a stereo file. Every
// coefficient pair defaults to zero except pair 1 = (2048, 0), i.e. exactly
// +1.0 * hist1 in the Q11 fixed-point format, so tests can exercise history.
std::vector<u8> MakeHeader(u32 sample_rate = 32000)
{
  std::vector<u8> v(0x80, 0);
  std::memcpy(v.data(), " HALPST\0", 8);
  PutBE32(v, 0x08, sample_rate);
  PutBE32(v, 0x0C, 2);
  for (std::size_t ch = 0; ch < 2; ch++)
  {
    const std::size_t coefs = 0x10 + ch * 0x38 + 0x10;
    PutBE16(v, coefs + 1 * 4, 2048);  // pair 1: coef1 = 1.0, coef2 = 0
  }
  return v;
}

// Appends one block whose every ADPCM frame is `frame` repeated
// frames_per_channel times per channel. Returns the block's offset.
u32 AppendBlock(std::vector<u8>& v, u32 frames_per_channel, const u8 (&frame)[8], u32 next,
                s16 hist1 = 0, s16 hist2 = 0)
{
  const u32 offset = static_cast<u32>(v.size());
  const u32 dsp_len = frames_per_channel * 2 * 8;
  std::vector<u8> block(BLOCK_HEADER_BYTES, 0);
  PutBE32(block, 0x00, dsp_len);
  PutBE32(block, 0x08, next);
  for (std::size_t ch = 0; ch < 2; ch++)
  {
    PutBE16(block, 0x0C + ch * 8 + 2, static_cast<u16>(hist1));
    PutBE16(block, 0x0C + ch * 8 + 4, static_cast<u16>(hist2));
  }
  v.insert(v.end(), block.begin(), block.end());
  for (u32 f = 0; f < frames_per_channel * 2; f++)
    v.insert(v.end(), frame, frame + 8);
  return offset;
}

HpsDecoder MakeDecoder(const std::vector<u8>& file)
{
  return HpsDecoder(
      [&file](u64 offset, u64 length, u8* out) {
        if (offset + length > file.size())
          return false;
        std::memcpy(out, file.data() + offset, length);
        return true;
      },
      file.size());
}
}  // namespace

TEST(HpsDecoder, DecodesKnownSamples)
{
  auto file = MakeHeader();
  // Coef pair 0 = (0,0), scale shift 0 => scale 1: each decoded sample is
  // floor(((nibble << 11) + 1024) / 2048) = the nibble value itself.
  const u8 frame[8] = {0x00, 0x12, 0x7F, 0x89, 0xF0, 0x00, 0x00, 0x00};
  AppendBlock(file, 1, frame, 0xFFFFFFFF);

  auto decoder = MakeDecoder(file);
  std::string error;
  ASSERT_TRUE(decoder.Init(&error)) << error;
  EXPECT_EQ(decoder.GetSampleRate(), 32000u);

  s16 out[14 * 2] = {};
  ASSERT_EQ(decoder.DecodeNext(out, 14), 14u);
  // Nibbles of the data bytes, high first, sign-extended from 4 bits.
  const s16 expected[14] = {1, 2, 7, -1, -8, -7, -1, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 14; i++)
  {
    EXPECT_EQ(out[i * 2], expected[i]) << "sample " << i;
    EXPECT_EQ(out[i * 2 + 1], expected[i]) << "sample " << i << " (right)";
  }
  EXPECT_EQ(decoder.DecodeNext(out, 14), 0u);
  EXPECT_TRUE(decoder.HasEnded());
  EXPECT_FALSE(decoder.HasFailed());
}

TEST(HpsDecoder, AppliesCoefficientHistory)
{
  auto file = MakeHeader();
  // Coef pair 1 = (1.0, 0), so sample = nibble + hist1. With every nibble 1
  // and hist1 seeded to 5 from the block header, the run is 6, 7, 8, ...
  const u8 frame[8] = {0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
  AppendBlock(file, 1, frame, 0xFFFFFFFF, 5, 0);

  auto decoder = MakeDecoder(file);
  ASSERT_TRUE(decoder.Init(nullptr));
  s16 out[14 * 2] = {};
  ASSERT_EQ(decoder.DecodeNext(out, 14), 14u);
  for (int i = 0; i < 14; i++)
    EXPECT_EQ(out[i * 2], 6 + i) << "sample " << i;
}

TEST(HpsDecoder, LoopsToEarlierBlockExactly)
{
  auto file = MakeHeader();
  const u8 frame_a[8] = {0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
  const u8 frame_b[8] = {0x00, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22};
  const u32 first = static_cast<u32>(file.size());
  ASSERT_EQ(first, 0x80u);
  // Block A gets patched to point at B below; B loops back to A. Each block
  // carries its own decoder state, so every loop pass decodes identically.
  AppendBlock(file, 1, frame_a, 0);
  const u32 second = AppendBlock(file, 1, frame_b, first);
  PutBE32(file, first + 0x08, second);

  auto decoder = MakeDecoder(file);
  ASSERT_TRUE(decoder.Init(nullptr));

  constexpr std::size_t PERIOD = 28;  // two blocks x 14 frames
  s16 out[PERIOD * 2 * 3] = {};
  ASSERT_EQ(decoder.DecodeNext(out, PERIOD * 3), PERIOD * 3);
  EXPECT_FALSE(decoder.HasEnded());
  for (std::size_t i = 0; i < PERIOD * 2; i++)
  {
    EXPECT_EQ(out[PERIOD * 2 + i], out[i]) << "loop pass 2 sample " << i;
    EXPECT_EQ(out[PERIOD * 4 + i], out[i]) << "loop pass 3 sample " << i;
  }
}

TEST(HpsDecoder, RejectsBadMagic)
{
  auto file = MakeHeader();
  file[0] = 'X';
  const u8 frame[8] = {};
  AppendBlock(file, 1, frame, 0xFFFFFFFF);
  auto decoder = MakeDecoder(file);
  std::string error;
  EXPECT_FALSE(decoder.Init(&error));
  EXPECT_TRUE(decoder.HasFailed());
}

TEST(HpsDecoder, RejectsMonoChannelCount)
{
  auto file = MakeHeader();
  PutBE32(file, 0x0C, 1);
  const u8 frame[8] = {};
  AppendBlock(file, 1, frame, 0xFFFFFFFF);
  auto decoder = MakeDecoder(file);
  EXPECT_FALSE(decoder.Init(nullptr));
  EXPECT_TRUE(decoder.HasFailed());
}

TEST(HpsDecoder, TruncatedBlockDataFails)
{
  auto file = MakeHeader();
  const u8 frame[8] = {};
  const u32 block = AppendBlock(file, 1, frame, 0xFFFFFFFF);
  PutBE32(file, block, 0x100000);  // dsp_len far beyond EOF
  auto decoder = MakeDecoder(file);
  ASSERT_TRUE(decoder.Init(nullptr));
  s16 out[32] = {};
  EXPECT_EQ(decoder.DecodeNext(out, 16), 0u);
  EXPECT_TRUE(decoder.HasFailed());
}

TEST(HpsDecoder, BadCoefficientIndexFails)
{
  auto file = MakeHeader();
  // Header nibble 0x9 indexes coefficient pair 9 of 8.
  const u8 frame[8] = {0x90, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
  AppendBlock(file, 1, frame, 0xFFFFFFFF);
  auto decoder = MakeDecoder(file);
  ASSERT_TRUE(decoder.Init(nullptr));
  s16 out[32] = {};
  EXPECT_EQ(decoder.DecodeNext(out, 16), 0u);
  EXPECT_TRUE(decoder.HasFailed());
}

TEST(HpsDecoder, EmptySelfLoopTerminates)
{
  auto file = MakeHeader();
  const u8 frame[8] = {};
  const u32 block = AppendBlock(file, 0, frame, 0);
  PutBE32(file, block + 0x08, block);  // zero-sample block pointing at itself
  auto decoder = MakeDecoder(file);
  ASSERT_TRUE(decoder.Init(nullptr));
  s16 out[32] = {};
  // Must terminate (fail), not spin forever.
  EXPECT_EQ(decoder.DecodeNext(out, 16), 0u);
  EXPECT_TRUE(decoder.HasFailed());
}

TEST(HpsDecoder, NextPointerOutOfBoundsFails)
{
  auto file = MakeHeader();
  const u8 frame[8] = {0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
  const u32 block = AppendBlock(file, 1, frame, 0);
  PutBE32(file, block + 0x08, static_cast<u32>(file.size() + 0x1000));
  auto decoder = MakeDecoder(file);
  ASSERT_TRUE(decoder.Init(nullptr));
  s16 out[64] = {};
  EXPECT_EQ(decoder.DecodeNext(out, 32), 0u);
  EXPECT_TRUE(decoder.HasFailed());
}

// Real-corpus smoke test: point HPS_TEST_DIR at a directory of .hps files
// extracted from the disc (they are not committed -- the ISO is a pinned
// local-only asset). Optionally set HPS_TEST_WAV_DIR to dump the first 30s
// of each as a WAV for manual audition.
TEST(HpsDecoder, RealFilesFromEnvDir)
{
  const char* dir = std::getenv("HPS_TEST_DIR");
  if (!dir)
    GTEST_SKIP() << "HPS_TEST_DIR not set";

  int tested = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir))
  {
    if (entry.path().extension() != ".hps")
      continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(file.empty());

    auto decoder = MakeDecoder(file);
    std::string error;
    ASSERT_TRUE(decoder.Init(&error)) << entry.path() << ": " << error;
    EXPECT_EQ(decoder.GetSampleRate(), 32000u) << entry.path();

    // Decode 30 seconds; every Melee BGM track is far longer than its file's
    // linear content only when looping works, so sustained decode past the
    // file length proves the loop path on looping tracks.
    const std::size_t want_frames = decoder.GetSampleRate() * 30;
    std::vector<s16> pcm(want_frames * 2);
    std::size_t got_frames = 0;
    while (got_frames < want_frames)
    {
      const std::size_t got =
          decoder.DecodeNext(pcm.data() + got_frames * 2, want_frames - got_frames);
      if (got == 0)
        break;
      got_frames += got;
    }
    EXPECT_FALSE(decoder.HasFailed()) << entry.path() << ": " << decoder.GetError();
    EXPECT_GT(got_frames, 0u) << entry.path();
    const bool any_nonzero =
        std::any_of(pcm.begin(), pcm.begin() + got_frames * 2, [](s16 s) { return s != 0; });
    EXPECT_TRUE(any_nonzero) << entry.path() << " decoded to pure silence";

    if (const char* wav_dir = std::getenv("HPS_TEST_WAV_DIR"))
    {
      const auto wav_path = std::filesystem::path(wav_dir) /
                            (entry.path().stem().string() + ".wav");
      std::ofstream wav(wav_path, std::ios::binary);
      const u32 data_bytes = static_cast<u32>(got_frames * 2 * sizeof(s16));
      const u32 sample_rate = decoder.GetSampleRate();
      const u32 byte_rate = sample_rate * 2 * sizeof(s16);
      u8 header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                       16,  0,   0,   0,   1, 0, 2, 0, 0,   0,   0,   0,   0,   0,   0,   0,
                       4,   0,   16,  0,   'd', 'a', 't', 'a', 0, 0, 0, 0};
      const u32 riff_size = 36 + data_bytes;
      std::memcpy(header + 4, &riff_size, 4);
      std::memcpy(header + 24, &sample_rate, 4);
      std::memcpy(header + 28, &byte_rate, 4);
      std::memcpy(header + 40, &data_bytes, 4);
      wav.write(reinterpret_cast<const char*>(header), sizeof(header));
      wav.write(reinterpret_cast<const char*>(pcm.data()), data_bytes);
    }
    tested++;
  }
  EXPECT_GT(tested, 0) << "no .hps files in " << dir;
}
