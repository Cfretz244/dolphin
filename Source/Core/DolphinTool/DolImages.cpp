// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "DolphinTool/DolImages.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <ranges>
#include <iterator>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <mbedtls/sha256.h>
#include <sqlite3.h>

#include "Common/Swap.h"
#include "Core/Boot/DolReader.h"
#include "Core/PowerPC/AOT/aot_images.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PrimeHack/Patches.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DolphinTool/AotCEmitter.h"
#include "DolphinTool/CfgCommand.h"
#include "DolphinTool/RelFile.h"

extern const char s_aot_runtime_header[];
extern const char s_aot_images_header[];

namespace DolphinTool
{
namespace
{
struct Image
{
  std::string name;
  std::string hash;
  std::unique_ptr<DolReader> dol;  // null for trace-sourced images
  PPCMemoryImage memory;
  std::vector<std::vector<u8>> patched_sections;
  std::optional<size_t> base_image;
  // Executable ranges (addr, size) backing `memory`: DOL text sections, or the
  // contiguous runs of a trace-sourced image.
  std::vector<std::pair<u32, u32>> ranges;
  // Trace-sourced images only: addr -> word, persisted in trace_image_words.
  std::map<u32, u32> trace_words;
  // Real-mode images (translate --real-mode-image): physical-pc blocks
  // discovered statically from the exception vectors of a low-memory dump.
  bool real_mode = false;
  std::map<u32, u32> real_blocks;
};
using DB = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

DB Open(const std::string& path, int flags)
{
  sqlite3* raw = nullptr;
  const int result = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
  DB db(raw, sqlite3_close);
  if (result != SQLITE_OK)
    throw std::runtime_error("Cannot open database: " + path);
  return db;
}
Statement Prepare(sqlite3* db, const char* sql)
{
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  return Statement(raw, sqlite3_finalize);
}
void Exec(sqlite3* db, const char* sql)
{
  if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
}
std::string Text(sqlite3_stmt* stmt, int col)
{
  const auto* text = sqlite3_column_text(stmt, col);
  if (!text)
    throw std::runtime_error("Missing image identity");
  return reinterpret_cast<const char*>(text);
}

// Fused bodies may write RAM, but must not change instruction-cache residency
// or translation. They execute only behind a resident, single-cache-line guard.
bool CanFuseFallthrough(const PPCMemoryImage& memory, u32 pc, u32 count)
{
  for (u32 n = 0; n < count; ++n)
  {
    const auto word = memory.ReadInstruction(pc + n * 4);
    if (!word)
      return false;
    const UGeckoInstruction inst(*word);
    const auto* info = PPCTables::GetOpInfo(inst, pc + n * 4);
    if (!info)
      return false;
    if (inst.OPCD == 16 && !inst.LK)  // A taken edge still exits the native body.
      continue;
    if (info->flags & FL_ENDBLOCK)
      return false;
    switch (info->type)
    {
    case OpType::Integer:
    case OpType::CR:
    case OpType::Load:
    case OpType::Store:
    case OpType::LoadFP:
    case OpType::StoreFP:
    case OpType::LoadPS:
    case OpType::StorePS:
    case OpType::DoubleFP:
    case OpType::SingleFP:
    case OpType::PS:
      break;
    default:
      return false;
    }
  }
  return true;
}

void AddImage(std::vector<Image>& images, std::string name, std::vector<u8> bytes)
{
  u8 digest[32];
  if (mbedtls_sha256_ret(bytes.data(), bytes.size(), digest, 0) != 0)
    throw std::runtime_error("Cannot hash DOL");
  std::string hash;
  for (u8 b : digest)
    hash += fmt::format("{:02x}", b);
  if (std::any_of(images.begin(), images.end(), [&](const Image& i) { return i.hash == hash; }))
    return;
  Image image{std::move(name), hash, std::make_unique<DolReader>(std::move(bytes)), {}, {},
              std::nullopt};
  if (!image.dol->IsValid())
    throw std::runtime_error("Invalid DOL: " + image.name);
  for (int i = 0; i < image.dol->GetNumTextSections(); ++i)
  {
    const auto& section = image.dol->GetTextSection(i);
    const u32 addr = image.dol->GetTextSectionAddress(i);
    if (section.size() > UINT32_MAX - addr)
      throw std::runtime_error("DOL text address overflow");
    image.memory.AddSection(addr, section.data(), static_cast<u32>(section.size()));
    if (!section.empty())
      image.ranges.emplace_back(addr, static_cast<u32>(section.size()));
  }
  images.push_back(std::move(image));
}

// A fixed-address image whose bytes exist only in traces. Every word must be
// observed identically by all supplying snapshots (the pre-fix JIT trace writer
// serialized reordered code; such files must be excluded with --trace-image-from).
void AddTraceImage(std::vector<Image>& images, std::map<u32, u32> words, u32 lo, u32 hi)
{
  if (words.empty())
    throw std::runtime_error(fmt::format("--trace-image {:#x}-{:#x}: no snapshot words", lo, hi));
  Image image;
  image.name = fmt::format("<trace:{:08x}-{:08x}>", lo, hi);
  std::vector<u8> digest_input;
  digest_input.reserve(words.size() * 8);
  for (const auto& [addr, word] : words)
  {
    for (u32 v : {addr, word})
      for (int b = 3; b >= 0; --b)
        digest_input.push_back(static_cast<u8>(v >> (b * 8)));
  }
  u8 digest[32];
  if (mbedtls_sha256_ret(digest_input.data(), digest_input.size(), digest, 0) != 0)
    throw std::runtime_error("Cannot hash trace image");
  for (u8 b : digest)
    image.hash += fmt::format("{:02x}", b);
  // Contiguous runs become sections.
  auto it = words.begin();
  while (it != words.end())
  {
    const u32 start = it->first;
    std::vector<u8> bytes;
    u32 expect = start;
    while (it != words.end() && it->first == expect)
    {
      for (int b = 3; b >= 0; --b)
        bytes.push_back(static_cast<u8>(it->second >> (b * 8)));
      expect += 4;
      ++it;
    }
    image.patched_sections.push_back(std::move(bytes));
    const auto& stored = image.patched_sections.back();
    image.memory.AddSection(start, stored.data(), static_cast<u32>(stored.size()));
    image.ranges.emplace_back(start, static_cast<u32>(stored.size()));
  }
  image.trace_words = std::move(words);
  images.push_back(std::move(image));
}

std::map<u32, u32> CollectTraceWords(const std::vector<TraceSnapshotBlock>& snapshots, u32 lo,
                                     u32 hi)
{
  std::map<u32, u32> words;
  size_t conflicts = 0;
  for (const auto& sb : snapshots)
  {
    for (const auto& snap : sb.snapshots)
    {
      for (size_t i = 0; i < snap.words.size(); ++i)
      {
        const u64 addr = u64(sb.addr) + i * 4;
        if (addr < lo || addr + 4 > hi)
          continue;
        auto [it, inserted] = words.emplace(static_cast<u32>(addr), snap.words[i]);
        if (!inserted && it->second != snap.words[i])
          ++conflicts;
      }
    }
  }
  if (conflicts)
    throw std::runtime_error(fmt::format(
        "--trace-image {:#x}-{:#x}: {} conflicting snapshot words; supply only contiguous "
        "(post-fix) captures with --trace-image-from",
        lo, hi, conflicts));
  return words;
}

// Reconstruct trace-sourced images from the CFG database, in image-id order.
void LoadTraceImages(sqlite3* db, std::vector<Image>& images)
{
  auto exists = Prepare(
      db, "SELECT 1 FROM sqlite_master WHERE name='trace_image_words' AND type='table'");
  if (sqlite3_step(exists.get()) != SQLITE_ROW)
    return;
  auto ranges = Prepare(db, "SELECT image_id,lo,hi FROM trace_images ORDER BY image_id");
  while (sqlite3_step(ranges.get()) == SQLITE_ROW)
  {
    const auto id = sqlite3_column_int64(ranges.get(), 0);
    const u32 lo = static_cast<u32>(sqlite3_column_int64(ranges.get(), 1));
    const u32 hi = static_cast<u32>(sqlite3_column_int64(ranges.get(), 2));
    if (id != static_cast<sqlite3_int64>(images.size()))
      throw std::runtime_error("Trace image order mismatch; regenerate CFG");
    auto rows = Prepare(db, "SELECT addr,word FROM trace_image_words WHERE image_id=? ORDER BY addr");
    sqlite3_bind_int64(rows.get(), 1, id);
    std::map<u32, u32> words;
    while (sqlite3_step(rows.get()) == SQLITE_ROW)
      words.emplace(static_cast<u32>(sqlite3_column_int64(rows.get(), 0)),
                    static_cast<u32>(sqlite3_column_int64(rows.get(), 1)));
    AddTraceImage(images, std::move(words), lo, hi);
  }
}
void Scan(const DiscIO::Volume& volume, const DiscIO::FileInfo& dir, const std::string& parent,
          std::vector<Image>& images)
{
  for (const auto& entry : dir)
  {
    const std::string path = parent + entry.GetName();
    if (entry.IsDirectory())
    {
      Scan(volume, entry, path + "/", images);
      continue;
    }
    std::string name = entry.GetName();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (!name.ends_with(".dol"))
      continue;
    if (entry.GetSize() < 0x100 || entry.GetSize() > 64 * 1024 * 1024)
      throw std::runtime_error("Invalid DOL size: " + path);
    std::vector<u8> bytes(entry.GetSize());
    if (DiscIO::ReadFile(volume, volume.GetGamePartition(), &entry, bytes.data(), bytes.size()) !=
        bytes.size())
      throw std::runtime_error("Cannot read DOL: " + path);
    AddImage(images, path, std::move(bytes));
  }
}
std::vector<Image> Discover(const DiscIO::Volume& volume)
{
  std::vector<Image> images;
  const auto partition = volume.GetGamePartition();
  const auto offset = DiscIO::GetBootDOLOffset(volume, partition);
  const auto size = offset ? DiscIO::GetBootDOLSize(volume, partition, *offset) : std::nullopt;
  if (!size || *size > 64 * 1024 * 1024)
    throw std::runtime_error("Cannot read boot DOL");
  std::vector<u8> bytes(*size);
  if (!volume.Read(*offset, *size, bytes.data(), partition))
    throw std::runtime_error("Cannot read boot DOL");
  AddImage(images, "<boot>", std::move(bytes));
  const auto* fs = volume.GetFileSystem(partition);
  if (!fs || !fs->IsValid())
    throw std::runtime_error("Cannot read disc filesystem");
  Scan(volume, fs->GetRoot(), "", images);
  return images;
}

size_t AddPrimeHackImages(const DiscIO::Volume& volume, std::vector<Image>& images)
{
  if (volume.GetGameID() != "R3ME01" || volume.GetRevision() != 0)
    throw std::runtime_error("PrimeHack AOT variants require Trilogy R3ME01 revision 0");
  std::optional<size_t> source;
  for (size_t id = 0; id < images.size(); ++id)
  {
    const auto& memory = images[id].memory;
    if (memory.ReadInstruction(0x8046d340) != 0x4e800020 ||
        !std::all_of(PrimeHack::MP1_PATCHES.begin(), PrimeHack::MP1_PATCHES.end(),
                     [&](const auto& patch)
                     { return memory.ReadInstruction(patch.address) == patch.original; }))
      continue;
    if (source)
      throw std::runtime_error("Ambiguous PrimeHack MP1 source image");
    source = id;
  }
  if (!source)
    throw std::runtime_error("PrimeHack MP1 discriminator/original patch words do not match disc");
  images.reserve(images.size() + PrimeHack::MP1_PATCH_MASKS.size());
  for (u32 mask : PrimeHack::MP1_PATCH_MASKS)
  {
    const auto& base = images[*source];
    Image patched;
    patched.name = fmt::format("{}#primehack-{:02x}", base.name, mask);
    patched.base_image = *source;
    std::string identity = base.hash + patched.name;
    for (size_t i = 0; i < PrimeHack::MP1_PATCHES.size(); ++i)
    {
      const auto& patch = PrimeHack::MP1_PATCHES[i];
      identity += fmt::format(":{:08x}:{:08x}:{:08x}", patch.address, patch.original,
                              (mask & (1u << i)) ? patch.replacement : patch.original);
    }
    u8 digest[32];
    if (mbedtls_sha256_ret(reinterpret_cast<const u8*>(identity.data()), identity.size(), digest,
                           0))
      throw std::runtime_error("Cannot hash PrimeHack image identity");
    for (u8 byte : digest)
      patched.hash += fmt::format("{:02x}", byte);
    for (int section = 0; section < base.dol->GetNumTextSections(); ++section)
    {
      auto bytes = base.dol->GetTextSection(section);
      const u32 address = base.dol->GetTextSectionAddress(section);
      for (size_t i = 0; i < PrimeHack::MP1_PATCHES.size(); ++i)
      {
        const auto& patch = PrimeHack::MP1_PATCHES[i];
        if ((mask & (1u << i)) && patch.address >= address &&
            u64(patch.address) + 4 <= u64(address) + bytes.size())
        {
          const u32 word = Common::swap32(patch.replacement);
          std::memcpy(bytes.data() + patch.address - address, &word, sizeof(word));
        }
      }
      patched.patched_sections.push_back(std::move(bytes));
      const auto& stored = patched.patched_sections.back();
      patched.memory.AddSection(address, stored.data(), static_cast<u32>(stored.size()));
    }
    images.push_back(std::move(patched));
  }
  return *source;
}

bool MatchesSnapshotWord(const Image& image, u32 pc, u32 word, bool primehack)
{
  if (image.memory.ReadInstruction(pc) == word)
    return true;
  // A trace is only a discovery hint. Recognize the known patch words in either
  // state, then disassemble the verified disc/patch image, never arbitrary traces.
  return primehack && std::any_of(PrimeHack::MP1_PATCHES.begin(), PrimeHack::MP1_PATCHES.end(),
                                  [&](const auto& patch)
                                  {
                                    return patch.address == pc &&
                                           (word == patch.original || word == patch.replacement);
                                  });
}
std::ofstream Output(const std::string& path)
{
  std::ofstream out(path);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  return out;
}
} // namespace

bool WriteDolImageCFG(const DiscIO::Volume& volume,
                      const std::vector<TraceSnapshotBlock>& snapshots, const std::string& path,
                      bool primehack, const std::vector<std::pair<u32, u32>>& trace_ranges,
                      const std::vector<TraceSnapshotBlock>& image_snapshots)
{
  try
  {
    if (snapshots.empty())
      throw std::runtime_error("--dol-images requires v4 instruction snapshots");
    if (std::filesystem::exists(path))
      throw std::runtime_error("Output exists; use a fresh database: " + path);
    auto images = Discover(volume);
    std::optional<size_t> primehack_source;
    if (primehack)
      primehack_source = AddPrimeHackImages(volume, images);
    const size_t first_trace_image = images.size();
    for (const auto& [lo, hi] : trace_ranges)
      AddTraceImage(images, CollectTraceWords(image_snapshots, lo, hi), lo, hi);
    auto db = Open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    Exec(db.get(), "BEGIN; CREATE TABLE dol_images(id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
                   "sha256 TEXT NOT NULL); CREATE TABLE dol_image_blocks(image_id INTEGER, "
                   "pc INTEGER, count INTEGER, PRIMARY KEY(image_id,pc)); "
                   "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT); "
                   "INSERT INTO metadata VALUES('dol_images_version','1');");
    if (primehack)
      Exec(db.get(), "INSERT INTO metadata VALUES('primehack_patch_set','mp1-r3me01-v1');");
    if (!trace_ranges.empty())
    {
      Exec(db.get(), "CREATE TABLE trace_images(image_id INTEGER PRIMARY KEY, lo INTEGER, "
                     "hi INTEGER); CREATE TABLE trace_image_words(image_id INTEGER, addr INTEGER, "
                     "word INTEGER, PRIMARY KEY(image_id,addr));");
      auto range_stmt = Prepare(db.get(), "INSERT INTO trace_images VALUES(?,?,?)");
      auto word_stmt = Prepare(db.get(), "INSERT INTO trace_image_words VALUES(?,?,?)");
      for (size_t id = first_trace_image; id < images.size(); ++id)
      {
        const auto& [lo, hi] = trace_ranges[id - first_trace_image];
        sqlite3_bind_int(range_stmt.get(), 1, static_cast<int>(id));
        sqlite3_bind_int64(range_stmt.get(), 2, lo);
        sqlite3_bind_int64(range_stmt.get(), 3, hi);
        if (sqlite3_step(range_stmt.get()) != SQLITE_DONE)
          throw std::runtime_error(sqlite3_errmsg(db.get()));
        sqlite3_reset(range_stmt.get());
        for (const auto& [addr, word] : images[id].trace_words)
        {
          sqlite3_bind_int(word_stmt.get(), 1, static_cast<int>(id));
          sqlite3_bind_int64(word_stmt.get(), 2, addr);
          sqlite3_bind_int64(word_stmt.get(), 3, word);
          if (sqlite3_step(word_stmt.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db.get()));
          sqlite3_reset(word_stmt.get());
        }
      }
    }
    auto image_stmt = Prepare(db.get(), "INSERT INTO dol_images VALUES(?,?,?)");
    auto block_stmt = Prepare(db.get(), "INSERT INTO dol_image_blocks VALUES(?,?,?)");
    size_t total = 0;
    std::vector<std::map<u32, u32>> discovered(images.size());
    for (size_t id = 0; id < images.size(); ++id)
    {
      const auto& image = images[id];
      sqlite3_bind_int(image_stmt.get(), 1, static_cast<int>(id));
      sqlite3_bind_text(image_stmt.get(), 2, image.name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(image_stmt.get(), 3, image.hash.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(image_stmt.get()) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db.get()));
      sqlite3_reset(image_stmt.get());
      std::set<u32> seeds;
      if (primehack_source && (id == *primehack_source || image.base_image))
      {
        for (const auto& patch : PrimeHack::MP1_PATCHES)
          seeds.insert(patch.address);
      }
      const bool is_trace_image = !image.trace_words.empty();
      const auto& seed_snapshots = is_trace_image ? image_snapshots : snapshots;
      if (image.base_image)
      {
        for (const auto& [pc, count] : discovered[*image.base_image])
          seeds.insert(pc);
      }
      for (const auto& sb : seed_snapshots)
      {
        for (const auto& snap : sb.snapshots)
        {
          if ((sb.addr & 3) || snap.words.empty() || snap.words.size() > (UINT32_MAX - sb.addr) / 4)
            continue;
          if (!is_trace_image && !image.memory.ReadInstruction(sb.addr))
            continue;  // outside this DOL's text: cheap skip
          // Older v4 traces serialized the optimized JIT stream: instructions
          // may be reordered or follow a branch to a noncontiguous address.
          // Use only a matching prefix as a discovery hint, never as code or
          // a block length. Decode original DOL text and its static successors.
          // Runtime guards still verify every instruction before execution.
          size_t matched = 0;
          bool terminated = false;
          for (u32 word : snap.words)
          {
            const u32 pc = sb.addr + static_cast<u32>(matched * 4);
            if (!MatchesSnapshotWord(image, pc, word,
                                     primehack_source &&
                                         (id == *primehack_source || image.base_image)) ||
                !PPCTables::IsValidInstruction(UGeckoInstruction(word), pc))
              break;
            ++matched;
            if (PPCTables::GetOpInfo(UGeckoInstruction(word), pc)->flags & FL_ENDBLOCK)
            {
              terminated = true;
              break;
            }
          }
          if (terminated || matched >= 4 || matched == snap.words.size())
            seeds.insert(sb.addr);
        }
      }
      const auto blocks = DisassembleDolSeeds(image.memory, seeds);
      discovered[id] = blocks;
      size_t written = 0;
      for (const auto& [pc, count] : blocks)
      {
        if (image.base_image)
        {
          // Share unchanged native blocks with the original DOL. A NOP/branch
          // patch can change block boundaries, so compare length as well as words.
          const auto& originals = discovered[*image.base_image];
          const auto original = originals.find(pc);
          if (original != originals.end() && original->second == count)
          {
            bool equal = true;
            for (u32 n = 0; n < count && equal; ++n)
              equal = image.memory.ReadInstruction(pc + n * 4) ==
                      images[*image.base_image].memory.ReadInstruction(pc + n * 4);
            if (equal)
              continue;
          }
        }
        sqlite3_bind_int(block_stmt.get(), 1, static_cast<int>(id));
        sqlite3_bind_int64(block_stmt.get(), 2, pc);
        sqlite3_bind_int64(block_stmt.get(), 3, count);
        if (sqlite3_step(block_stmt.get()) != SQLITE_DONE)
          throw std::runtime_error(sqlite3_errmsg(db.get()));
        sqlite3_reset(block_stmt.get());
        ++written;
      }
      total += written;
      fmt::println(std::cerr, "DOL image {}: {} — {} native block candidates", id, image.name,
                   written);
    }
    if (!total)
      throw std::runtime_error("No snapshots match any DOL");
    Exec(db.get(), "COMMIT");
    fmt::println(std::cerr, "Wrote {} guarded DOL blocks to {}", total, path);
    return true;
  }
  catch (const std::exception& e)
  {
    fmt::println(std::cerr, "DOL images: {}", e.what());
    return false;
  }
}

bool IsDolImageCFG(const std::string& path)
{
  try
  {
    auto db = Open(path, SQLITE_OPEN_READONLY);
    auto stmt =
        Prepare(db.get(), "SELECT 1 FROM sqlite_master WHERE name='dol_images' AND type='table'");
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
  }
  catch (const std::exception&)
  {
    return false; // The ordinary CFG reader reports the missing/unreadable database.
  }
}

bool TranslateDolImages(const DiscIO::Volume& volume, const std::string& cfg,
                        const std::string& output, const std::string& prefix,
                        const std::string& boot_hash)
{
  try
  {
    if (prefix.empty() || std::isdigit(static_cast<unsigned char>(prefix[0])) ||
        !std::all_of(prefix.begin(), prefix.end(),
                     [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
      throw std::runtime_error("Invalid C symbol prefix");
    auto images = Discover(volume);
    auto db = Open(cfg, SQLITE_OPEN_READONLY);
    auto patch_set =
        Prepare(db.get(), "SELECT value FROM metadata WHERE key='primehack_patch_set'");
    if (sqlite3_step(patch_set.get()) == SQLITE_ROW)
    {
      if (Text(patch_set.get(), 0) != "mp1-r3me01-v1")
        throw std::runtime_error("Unsupported PrimeHack patch set; regenerate CFG");
      AddPrimeHackImages(volume, images);
    }
    auto version = Prepare(db.get(), "SELECT value FROM metadata WHERE key='dol_images_version'");
    if (sqlite3_step(version.get()) != SQLITE_ROW || Text(version.get(), 0) != "1")
      throw std::runtime_error("Unsupported DOL image CFG version");
    // Bind every image to its disc bytes, not just the boot DOL or game ID.
    auto identities = Prepare(db.get(), "SELECT id,name,sha256 FROM dol_images ORDER BY id");
    size_t seen = 0;
    while (sqlite3_step(identities.get()) == SQLITE_ROW)
    {
      if (seen >= images.size() ||
          sqlite3_column_int64(identities.get(), 0) != static_cast<sqlite3_int64>(seen) ||
          Text(identities.get(), 1) != images[seen].name ||
          Text(identities.get(), 2) != images[seen].hash)
        throw std::runtime_error("DOL image identity mismatch; regenerate CFG from this disc");
      ++seen;
    }
    if (seen != images.size())
      throw std::runtime_error("DOL image inventory mismatch");
    struct Block
    {
      u32 pc, count, guard_count;
      size_t image;
      std::string symbol;
    };
    std::vector<Block> blocks;
    auto query =
        Prepare(db.get(), "SELECT image_id,pc,count FROM dol_image_blocks ORDER BY pc,image_id");
    while (sqlite3_step(query.get()) == SQLITE_ROW)
    {
      const auto id = sqlite3_column_int64(query.get(), 0);
      const auto pc = sqlite3_column_int64(query.get(), 1);
      const auto count = sqlite3_column_int64(query.get(), 2);
      if (id < 0 || id >= static_cast<sqlite3_int64>(images.size()) || pc < 0 || pc > UINT32_MAX ||
          (pc & 3) || count <= 0 || count > (UINT32_MAX - pc) / 4)
        throw std::runtime_error("Invalid DOL block bounds");
      for (u32 n = 0; n < count; ++n)
        if (!images[id].memory.ReadInstruction(static_cast<u32>(pc) + n * 4))
          throw std::runtime_error("DOL block outside text");
      blocks.push_back({static_cast<u32>(pc), static_cast<u32>(count), static_cast<u32>(count),
                        static_cast<size_t>(id),
                        fmt::format("{}_d{}_block_{:08x}", prefix, id, pc)});
    }
    if (blocks.empty())
      throw std::runtime_error("Empty DOL image CFG");
    if (std::filesystem::exists(output))
    {
      for (const auto& entry : std::filesystem::directory_iterator(output))
      {
        // stack.sh opens its tee log before launching the translator.
        if (entry.path().filename() != "translate.log")
          throw std::runtime_error("Use an empty output directory to avoid stale generated code");
      }
    }
    std::vector<std::unordered_map<u32, u32>> sizes(images.size());
    for (const auto& b : blocks)
      sizes[b.image].emplace(b.pc, b.count);
    size_t fused = 0;
    for (auto& b : blocks)
    {
      // Sparse PrimeHack variants retain their original boundaries. Avoid
      // extending base-image guards over a patch whose clone has another start.
      const u32 line = b.pc & ~31u;
      if (images[b.image].base_image ||
          std::any_of(PrimeHack::MP1_PATCHES.begin(), PrimeHack::MP1_PATCHES.end(),
                      [line](const auto& patch) { return (patch.address & ~31u) == line; }))
        continue;
      const u32 limit = (32 - (b.pc & 31)) / 4;
      u32 current = b.pc, count = b.count;
      while (b.guard_count <= limit && CanFuseFallthrough(images[b.image].memory, current, count))
      {
        const u32 next = b.pc + b.guard_count * 4;
        const auto it = sizes[b.image].find(next);
        if (it == sizes[b.image].end() || it->second > limit - b.guard_count)
          break;
        // The final body must also be safe: nested fallthroughs share the guard.
        if (!CanFuseFallthrough(images[b.image].memory, next, it->second))
          break;
        b.guard_count += it->second;
        current = next;
        count = it->second;
      }
      fused += b.guard_count > b.count;
    }
    fmt::println(std::cerr, "Fused resident-line fallthrough chains at {} native entries", fused);
    std::filesystem::create_directories(output);
    Output(output + "/aot_runtime.h") << s_aot_runtime_header;
    auto header = Output(output + "/" + prefix + "_images.h");
    header << "#include \"aot_runtime.h\"\nint aot_match_code(uint32_t,const uint32_t*,uint32_t);\n"
           << "int aot_match_chain(uint32_t,const uint32_t*,uint32_t);\n";
    header << fmt::format("void {}_dispatch(AOTState*);\n", prefix);
    for (size_t id = 0; id < images.size(); ++id)
      header << fmt::format("#define {}_d{}_dispatch {}_dispatch\n", prefix, id, prefix);
    for (const auto& b : blocks)
      header << fmt::format("void {}(AOTState*);\nextern const uint32_t {}_words[{}];\n", b.symbol,
                            b.symbol, b.guard_count);
    header.close();
    // Chunk files fit the existing stack's *_blocks_*.c compilation convention.
    std::vector<std::set<u32>> known(images.size());
    for (const auto& b : blocks)
      known[b.image].insert(b.pc);
    std::vector<std::unique_ptr<AOTCEmitter>> emitters;
    for (size_t id = 0; id < images.size(); ++id)
    {
      auto emitter = std::make_unique<AOTCEmitter>(images[id].memory, known[id],
                                                   fmt::format("{}_d{}", prefix, id));
      emitter->SetGuardedImages();
      emitter->SetInlineHints(sizes[id], {});
      emitters.push_back(std::move(emitter));
    }
    std::ofstream code;
    for (size_t n = 0; n < blocks.size(); ++n)
    {
      if (n % 512 == 0)
      {
        if (code.is_open())
          code.close();
        code = Output(fmt::format("{}/{}_blocks_{:04d}.c", output, prefix, n / 512));
        code << fmt::format("#include \"{}_images.h\"\n", prefix);
      }
      const auto& b = blocks[n];
      code << fmt::format("const uint32_t {}_words[] = {{", b.symbol);
      for (u32 i = 0; i < b.guard_count; ++i)
        code << fmt::format("{:#x}u,", *images[b.image].memory.ReadInstruction(b.pc + i * 4));
      code << "};\n" << emitters[b.image]->TranslateBlock(b.pc, b.count, true, b.guard_count);
    }
    code.close();
    auto dispatch = Output(output + "/" + prefix + "_dispatch.c");
    dispatch << fmt::format("#include \"{}_images.h\"\n", prefix);
    dispatch << "typedef struct { uint32_t pc,count,guard_count; const uint32_t* words; AOTBlockFunc fn; } "
                "Candidate;\n";
    dispatch << "static const Candidate candidates[] = {\n";
    for (const auto& b : blocks)
      dispatch << fmt::format("{{{:#x}u,{}u,{}u,{}_words,{}}},\n", b.pc, b.count, b.guard_count, b.symbol, b.symbol);
    dispatch << "};\n";
    // The index locates all versions of an address. Remember the last successful
    // version separately, but validate it on every use: DOL reloads, patches,
    // instruction-cache replacement and translation changes can change the winner.
    std::map<u32, std::vector<std::pair<u32, size_t>>> pages;
    for (size_t n = 0; n < blocks.size(); ++n)
      if (n == 0 || blocks[n - 1].pc != blocks[n].pc)
        pages[blocks[n].pc >> 16].emplace_back((blocks[n].pc & 0xffff) >> 2, n + 1);
    dispatch << "static const uint32_t page_ids[65536] = {\n";
    size_t page_id = 0;
    for (const auto& [page, entries] : pages)
      dispatch << fmt::format("[{}]={},\n", page, ++page_id);
    dispatch << "};\nstatic const uint32_t candidate_indices[][16384] = {\n";
    for (const auto& [page, entries] : pages)
    {
      dispatch << "{\n";
      for (const auto& [offset, index] : entries)
        dispatch << fmt::format("[{}]={},\n", offset, index);
      dispatch << "},\n";
    }
    dispatch << "};\n";
    dispatch << "static const Candidate* selected[16384];\n";
    dispatch << "static int matches(const Candidate* c,uint32_t pc) { return "
                "c->guard_count>c->count ? aot_match_chain(pc,c->words,c->guard_count) : "
                "aot_match_code(pc,c->words,c->count); }\n";
    dispatch << "static const Candidate* find_candidate(uint32_t pc) {\n";
    dispatch << "  uint32_t slot=((pc>>2)^(pc>>16))&16383;\n"
                "  const Candidate* cached=selected[slot];\n"
                "  if(cached && cached->pc==pc && matches(cached,pc)) return cached;\n";
    dispatch << "  if (pc&3) return 0; uint32_t page=page_ids[pc>>16]; if(!page) return 0;\n";
    dispatch << "  uint32_t lo=candidate_indices[page-1][(pc&65535)>>2]; if(!lo) return 0; --lo;\n";
    dispatch << fmt::format("  for(;lo<{}u && candidates[lo].pc==pc;++lo) {{\n", blocks.size());
    dispatch << "    const Candidate* c=&candidates[lo]; if(c!=cached && matches(c,pc)) { "
                "selected[slot]=c; return c; }\n  } return 0;\n}\n";
    dispatch << fmt::format("AOTBlockFunc {}_lookup_block(uint32_t pc) {{ const Candidate* "
                            "c=find_candidate(pc); return c?c->fn:0; }}\n",
                            prefix);
    dispatch << "static uint32_t image_block_size(uint32_t pc) { const Candidate* "
                "c=find_candidate(pc); return c?c->count:0; }\n";
    dispatch << "void aot_register_image_block_sizes(const char*,uint32_t (*)(uint32_t));\n";
    dispatch << fmt::format("void {}_dispatch(AOTState* s) {{\n", prefix);
    dispatch << "#if AOT_HARNESS\n  if(aot_single_block_mode) return;\n#endif\n  "
                "if(s->downcount<=0) return;\n";
    dispatch << fmt::format("  AOTBlockFunc fn={}_lookup_block(s->pc);\n", prefix);
    dispatch << "  if(fn){[[clang::musttail]] return fn(s);}\n  [[clang::musttail]] return "
                "aot_interpreter_single_step(s);\n}\n";
    dispatch << fmt::format(
        "__attribute__((constructor)) static void register_images(void) {{\n"
        "  aot_register_game(\"{}\",{}_dispatch,{}_lookup_block,AOT_ABI_VERSION);\n"
        "  aot_register_game_image(\"{}\",\"{}\");\n"
        "  aot_register_image_block_sizes(\"{}\",image_block_size);\n}}\n",
        prefix, prefix, prefix, prefix, boot_hash, prefix);
    dispatch.close();
    auto script = Output(output + "/build.sh");
    script << "#!/bin/bash\nset -euo pipefail\ncd \"$(dirname \"$0\")\"\n";
    script << "export IMAGE_CFLAGS='-Os -flto=thin -arch arm64 -mcpu=apple-a14 -fwrapv "
              "-fno-strict-aliasing -DAOT_HARNESS=1'\n";
    script << fmt::format("printf '%s\\0' {}_blocks_*.c | xargs -0 -n1 -P \"${{AOT_JOBS:-4}}\" sh "
                          "-c 'exec clang -c $IMAGE_CFLAGS -I. \"$1\" -o \"${{1%.c}}.o\"' sh\n",
                          prefix);
    // Importing the dispatch table into every ThinLTO block module makes the
    // optimizer repeatedly process references to every candidate function.
    // Keep that immutable index native; block-to-block optimization stays on.
    script << fmt::format("clang -c $IMAGE_CFLAGS -fno-lto -I. {}_dispatch.c -o {}_dispatch.o\n",
                          prefix, prefix);
    script << fmt::format("ar rcs lib{}_aot.a {}_*.o\n", prefix, prefix);
    script.close();
    fmt::println(std::cerr,
                 "Translated {} content-guarded DOL blocks (experimental; measure guard cost)",
                 blocks.size());
    return true;
  }
  catch (const std::exception& e)
  {
    fmt::println(std::cerr, "DOL images: {}", e.what());
    return false;
  }
}

// ============================================================================
// Trusted multi-image mode
// ============================================================================
//
// Same CFG database as the guarded mode above, opposite runtime contract: no
// per-block instruction guards. Each selected base DOL becomes a normal
// single-image translation (flat table, direct tail calls, chain inlining,
// per-site indirect probes) under its own symbol prefix, and AotImageTracker
// picks the active table after instruction-cache invalidation events by
// comparing sparse discriminator words against RAM. PrimeHack patch variants
// become a small override list the tracker writes into the base table; every
// edge into an overridden pc probes the table instead of binding a symbol.
namespace
{
struct TrustedBlock
{
  u32 pc, count;
};

// Static branch/call targets of a block set, decoded from block terminators.
// Fallthrough successors are not branch-entered (they stay chain-inlinable),
// matching the normal single-image pipeline's criterion.
std::unordered_set<u32> BranchEnteredTargets(const PPCMemoryImage& memory,
                                             const std::map<u32, u32>& blocks)
{
  std::unordered_set<u32> targets;
  for (const auto& [pc, count] : blocks)
  {
    const u32 last = pc + (count - 1) * 4;
    const auto word = memory.ReadInstruction(last);
    if (!word)
      continue;
    const UGeckoInstruction inst(*word);
    if (inst.OPCD == 18)
    {
      const u32 target = inst.AA ? static_cast<u32>(inst.LI << 2) :
                                   last + static_cast<u32>(inst.LI << 2);
      targets.insert(target);
    }
    else if (inst.OPCD == 16)
    {
      const u32 target = inst.AA ? static_cast<u32>(inst.BD << 2) :
                                   last + static_cast<u32>(inst.BD << 2);
      targets.insert(target);
    }
  }
  return targets;
}

// Sparse words identifying `image` against RAM. Every other base image must
// differ (or lack the address) at min_distinct of them, so a partially
// matching sibling can never be selected.
std::vector<AotImageWord> ChooseDiscriminators(const std::vector<Image>& images, size_t self,
                                               const std::set<u32>& excluded_lines)
{
  const auto& image = images[self];
  std::vector<AotImageWord> chosen;
  std::set<u32> used;
  auto add_stride = [&](u32 stride) {
    for (const auto& [start, size] : image.ranges)
    {
      if (size < 4)
        continue;
      std::vector<u32> addrs;
      for (u32 a = start; a < start + size; a += stride)
        addrs.push_back(a);
      addrs.push_back(start + size - 4);
      for (u32 a : addrs)
      {
        if (excluded_lines.contains(a & ~31u) || used.contains(a))
          continue;
        const auto word = image.memory.ReadInstruction(a);
        if (!word)
          continue;
        used.insert(a);
        chosen.push_back({a, *word});
      }
    }
  };
  auto distinct_from = [&](size_t other) {
    u32 n = 0;
    for (const auto& w : chosen)
      if (images[other].memory.ReadInstruction(w.addr) != w.word)
        ++n;
    return n;
  };
  constexpr u32 min_distinct = 8;
  for (u32 stride : {0x4000u, 0x1000u, 0x400u, 0x100u, 0x20u, 0x4u})
  {
    add_stride(stride);
    bool ok = true;
    for (size_t o = 0; o < images.size() && ok; ++o)
      if (o != self && !images[o].base_image && distinct_from(o) < min_distinct)
        ok = false;
    if (ok)
      return chosen;
  }
  // Every text word is now a discriminator (tiny synthetic images); the only
  // remaining failure is a sibling that is byte-identical, which AddImage
  // already deduplicates by hash.
  for (size_t o = 0; o < images.size(); ++o)
    if (o != self && !images[o].base_image && distinct_from(o) == 0)
      throw std::runtime_error("Cannot distinguish DOL image " + image.name + " from a sibling");
  return chosen;
}

// Real-mode (MSR.IR=DR=0) code: the OS exception vectors. A dump of physical
// MEM1 [0, N) taken after OSInit installed them (dolphin-tool diff with
// AOT_DIFF_DUMP_LOWMEM=<file>). Blocks are discovered statically from every
// architected vector entry (0x100-0x1700) holding a valid instruction: walk to
// the first FL_ENDBLOCK instruction (the same boundary rule the JIT uses, so
// mtspr ends a block), follow direct branch targets and fallthroughs, stop at
// rfi / indirect branches; then split blocks at every discovered leader.
constexpr u32 kRealModeLo = 0x100;
constexpr u32 kRealModeHi = 0x3000;

std::map<u32, u32> DiscoverRealModeBlocks(const PPCMemoryImage& memory, u32 hi)
{
  auto valid = [&](u32 pc) -> std::optional<UGeckoInstruction> {
    if (pc < kRealModeLo || pc + 4 > hi)
      return std::nullopt;
    const auto word = memory.ReadInstruction(pc);
    if (!word || *word == 0)
      return std::nullopt;
    const UGeckoInstruction inst(*word);
    if (!PPCTables::IsValidInstruction(inst, pc))
      return std::nullopt;
    return inst;
  };
  std::set<u32> leaders;
  std::vector<u32> work;
  auto add = [&](u32 pc) {
    if (valid(pc) && leaders.insert(pc).second)
      work.push_back(pc);
  };
  for (u32 vector = 0x100; vector <= 0x1700; vector += 0x100)
    add(vector);
  while (!work.empty())
  {
    u32 pc = work.back();
    work.pop_back();
    for (;; pc += 4)
    {
      const auto inst = valid(pc);
      if (!inst)
        break;
      const auto* info = PPCTables::GetOpInfo(*inst, pc);
      if (!(info->flags & FL_ENDBLOCK))
        continue;
      if (inst->OPCD == 18)
      {
        add(u32(SignExt26(inst->LI << 2)) + (inst->AA ? 0 : pc));
        if (inst->LK)
          add(pc + 4);
      }
      else if (inst->OPCD == 16)
      {
        add(u32(SignExt16(s16(inst->BD << 2))) + (inst->AA ? 0 : pc));
        const bool always = (inst->BO & BO_DONT_DECREMENT_FLAG) && (inst->BO & BO_DONT_CHECK_CONDITION);
        if (!always || inst->LK)
          add(pc + 4);
      }
      else if (inst->OPCD == 19 && (inst->SUBOP10 == 16 || inst->SUBOP10 == 528))
      {
        const bool always = (inst->BO_2 & BO_DONT_CHECK_CONDITION) &&
                            (inst->SUBOP10 == 528 || (inst->BO_2 & BO_DONT_DECREMENT_FLAG));
        if (!always || inst->LK_3)
          add(pc + 4);
      }
      else if (!(inst->OPCD == 19 && inst->SUBOP10 == 50))  // everything but rfi resumes
      {
        add(pc + 4);
      }
      break;
    }
  }
  std::map<u32, u32> blocks;
  for (auto it = leaders.begin(); it != leaders.end(); ++it)
  {
    const auto next = std::next(it);
    u32 count = 0;
    for (u32 pc = *it; ; pc += 4)
    {
      if (next != leaders.end() && pc == *next)
        break;
      const auto inst = valid(pc);
      if (!inst)
        break;
      ++count;
      if (PPCTables::GetOpInfo(*inst, pc)->flags & FL_ENDBLOCK)
        break;
    }
    if (count)
      blocks.emplace(*it, count);
  }
  return blocks;
}

// Appends one real-mode image per distinct dump (identical translated words
// are deduplicated: the dumps of DOLs built from the same SDK can agree).
void AddRealModeImages(std::vector<Image>& images, const std::vector<std::string>& dumps)
{
  for (const auto& path : dumps)
  {
    std::ifstream in(path, std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof())
      throw std::runtime_error("Cannot read --real-mode-image " + path);
    if (bytes.size() < 0x200 || bytes.size() > kRealModeHi || (bytes.size() & 3))
      throw std::runtime_error(fmt::format(
          "--real-mode-image {}: expected a dump of physical [0, N) with 0x200 <= N <= {:#x}",
          path, kRealModeHi));
    Image image;
    image.real_mode = true;
    image.patched_sections.push_back(std::move(bytes));
    const auto& stored = image.patched_sections.back();
    image.memory.AddSection(0, stored.data(), static_cast<u32>(stored.size()));
    image.real_blocks = DiscoverRealModeBlocks(image.memory, static_cast<u32>(stored.size()));
    if (image.real_blocks.empty())
      throw std::runtime_error("--real-mode-image " + path + ": no exception vectors found");
    std::vector<u8> digest_input;
    for (const auto& [pc, count] : image.real_blocks)
    {
      for (u32 n = 0; n < count; ++n)
      {
        const u32 addr = pc + n * 4;
        const u32 word = *image.memory.ReadInstruction(addr);
        for (u32 v : {addr, word})
          for (int b = 3; b >= 0; --b)
            digest_input.push_back(static_cast<u8>(v >> (b * 8)));
      }
    }
    u8 digest[32];
    if (mbedtls_sha256_ret(digest_input.data(), digest_input.size(), digest, 0) != 0)
      throw std::runtime_error("Cannot hash real-mode image");
    for (u8 b : digest)
      image.hash += fmt::format("{:02x}", b);
    if (std::any_of(images.begin(), images.end(),
                    [&](const Image& i) { return i.real_mode && i.hash == image.hash; }))
    {
      fmt::println(std::cerr, "Real-mode image {}: identical to an earlier dump, skipped", path);
      continue;
    }
    image.name = "<real:" + std::filesystem::path(path).filename().string() + ">";
    images.push_back(std::move(image));
  }
}

// ---------------------------------------------------------------------------
// Wii-SDK RSO modules (Metroid Prime 2/3 and the Trilogy launcher; design notes:
// research/aot-rso-design.md in aot-dolphin-helper). The game loads each .rso
// whole into a heap buffer and locates it in place at a base that moves, so
// module blocks are emitted base-relative and found at runtime by the
// AotImageTracker (miss-triggered header identification).
// ---------------------------------------------------------------------------
std::vector<u8> ReadDiscFile(const DiscIO::Volume& volume, const std::string& path)
{
  const auto partition = volume.GetGamePartition();
  const auto* fs = volume.GetFileSystem(partition);
  if (!fs || !fs->IsValid())
    throw std::runtime_error("Cannot read disc filesystem");
  const auto info = fs->FindFileInfo(path);
  if (!info || info->IsDirectory())
    throw std::runtime_error("No such disc file: " + path);
  std::vector<u8> bytes(info->GetSize());
  if (DiscIO::ReadFile(volume, partition, info.get(), bytes.data(), bytes.size()) != bytes.size())
    throw std::runtime_error("Cannot read disc file: " + path);
  return bytes;
}

u32 Be32(const std::vector<u8>& d, u32 off)
{
  if (off > d.size() || d.size() - off < 4)
    throw std::runtime_error("RSO: truncated file");
  return (u32(d[off]) << 24) | (u32(d[off + 1]) << 16) | (u32(d[off + 2]) << 8) | d[off + 3];
}

std::string CStr(const std::vector<u8>& d, u32 off)
{
  std::string out;
  while (off < d.size() && d[off])
    out += static_cast<char>(d[off++]);
  return out;
}

struct RsoReloc
{
  u32 offset;  // file offset of the patched field
  u32 index;   // internals: target section; externals: import index
  u8 type;
  u32 addend;
};

struct RsoExport
{
  std::string name;
  u32 offset;   // in its section
  u32 section;  // section index (the static module's own sections for selfile.sel)
};

struct RsoFile
{
  std::string name;  // basename of the build path, e.g. RSO_FishCloud.plf
  std::vector<u8> data;
  std::vector<std::pair<u32, u32>> sections;  // (file offset, size)
  std::vector<RsoReloc> internals, externals;
  std::vector<std::string> imports;
  std::vector<RsoExport> exports;
};

RsoFile ParseRso(std::vector<u8> d)
{
  RsoFile f;
  const u32 nsec = Be32(d, 8), sec_off = Be32(d, 12), name_off = Be32(d, 16);
  // The runtime identifies a located module by 10 <= numSections <= 40 and
  // the section table directly after the 0x58-byte header.
  if (nsec < 10 || nsec > 40 || sec_off != 0x58)
    throw std::runtime_error(fmt::format("RSO: unsupported header (numSections {}, section table "
                                         "at {:#x})",
                                         nsec, sec_off));
  for (u32 i = 0; i < nsec; i++)
  {
    const u32 off = Be32(d, sec_off + i * 8) & ~1u, size = Be32(d, sec_off + i * 8 + 4);
    // Offset 0 = no bytes in the file (bss, empty sections).
    if (off && (off > d.size() || d.size() - off < size))
      throw std::runtime_error(fmt::format("RSO: section {} outside the file", i));
    f.sections.emplace_back(off, size);
  }
  const std::string path = CStr(d, name_off);
  f.name = path.substr(path.find_last_of("\\/") == std::string::npos ? 0 :
                                                                     path.find_last_of("\\/") + 1);
  auto relocs = [&](u32 off, u32 size, std::vector<RsoReloc>& out) {
    for (u32 i = 0; i + 12 <= size; i += 12)
    {
      const u32 info = Be32(d, off + i + 4);
      out.push_back({Be32(d, off + i), info >> 8, static_cast<u8>(info & 0xFF), Be32(d, off + i + 8)});
    }
  };
  relocs(Be32(d, 0x30), Be32(d, 0x34), f.internals);
  relocs(Be32(d, 0x38), Be32(d, 0x3C), f.externals);
  const u32 imp_off = Be32(d, 0x4C), imp_size = Be32(d, 0x50), imp_names = Be32(d, 0x54);
  for (u32 i = 0; i + 12 <= imp_size; i += 12)
    f.imports.push_back(CStr(d, imp_names + Be32(d, imp_off + i)));
  const u32 exp_off = Be32(d, 0x40), exp_size = Be32(d, 0x44), exp_names = Be32(d, 0x48);
  for (u32 i = 0; i + 16 <= exp_size; i += 16)
    f.exports.push_back({CStr(d, exp_names + Be32(d, exp_off + i)), Be32(d, exp_off + i + 4),
                         Be32(d, exp_off + i + 8)});
  f.data = std::move(d);
  return f;
}

// --rso-module IMAGE:DISC_PATH:STATIC_BASES  (selfile.sel = same directory)
struct RsoSpec
{
  size_t image;
  std::string path;
  std::string static_bases;
};

std::vector<RsoSpec> ParseRsoSpecs(const std::vector<std::string>& specs)
{
  std::vector<RsoSpec> out;
  for (const auto& spec : specs)
  {
    const size_t a = spec.find(':'), b = spec.find(':', a + 1);
    if (a == std::string::npos || b == std::string::npos)
      throw std::runtime_error("--rso-module wants IMAGE:DISC_PATH:STATIC_BASES, got " + spec);
    size_t image = 0;
    try
    {
      image = std::stoul(spec.substr(0, a));
    }
    catch (const std::exception&)
    {
      throw std::runtime_error("--rso-module: bad image id in " + spec);
    }
    RsoSpec parsed{image, spec.substr(a + 1, b - a - 1), spec.substr(b + 1)};
    if (std::any_of(out.begin(), out.end(), [&](const RsoSpec& o) { return o.path == parsed.path; }))
      throw std::runtime_error("--rso-module: duplicate module " + parsed.path);
    out.push_back(std::move(parsed));
  }
  return out;
}

// Static-module (selfile.sel) section bases: "SECTION 0xADDRESS" per line, '#'
// comments. Taken from a RAM dump of the located static module (its section
// table); every base must lie inside the owning DOL's text, data or bss.
std::map<u32, u32> ReadStaticBases(const std::string& path, const DolReader& dol)
{
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("--rso-module: cannot open static bases file " + path);
  std::map<u32, u32> bases;
  std::string line;
  while (std::getline(in, line))
  {
    line = line.substr(0, line.find('#'));
    std::istringstream fields(line);
    u32 sect = 0;
    std::string addr;
    if (!(fields >> sect))
      continue;
    if (!(fields >> addr))
      throw std::runtime_error("--rso-module: bad line in " + path + ": " + line);
    const u32 value = static_cast<u32>(std::stoul(addr, nullptr, 16));
    bool inside = value - dol.GetBssAddress() < dol.GetBssSize();
    for (int i = 0; i < dol.GetNumTextSections(); ++i)
      inside |= value - dol.GetTextSectionAddress(i) < dol.GetTextSectionSize(i);
    for (int i = 0; i < dol.GetNumDataSections(); ++i)
      inside |= value - dol.GetDataSectionAddress(i) < dol.GetDataSectionSize(i);
    if (!inside)
      throw std::runtime_error(fmt::format("--rso-module: static section {} base {:#010x} ({}) is "
                                           "not inside the owning DOL",
                                           sect, value, path));
    bases[sect] = value;
  }
  if (bases.empty())
    throw std::runtime_error("--rso-module: empty static bases file " + path);
  return bases;
}

u32 ParseVariantMask(const std::string& name)
{
  const auto pos = name.rfind("#primehack-");
  if (pos == std::string::npos)
    throw std::runtime_error("Unrecognized image variant name: " + name);
  return static_cast<u32>(std::stoul(name.substr(pos + 11), nullptr, 16));
}
}  // namespace

bool TranslateTrustedImages(const DiscIO::Volume& volume, const std::string& cfg,
                            const std::string& output, const std::string& prefix,
                            const std::string& boot_hash, const std::set<size_t>& selected,
                            const std::vector<std::string>& real_mode_dumps,
                            const std::vector<std::string>& rso_module_specs)
{
  try
  {
    if (prefix.empty() || std::isdigit(static_cast<unsigned char>(prefix[0])) ||
        !std::all_of(prefix.begin(), prefix.end(),
                     [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
      throw std::runtime_error("Invalid C symbol prefix");
    auto images = Discover(volume);
    auto db = Open(cfg, SQLITE_OPEN_READONLY);
    bool primehack = false;
    {
      auto patch_set =
          Prepare(db.get(), "SELECT value FROM metadata WHERE key='primehack_patch_set'");
      if (sqlite3_step(patch_set.get()) == SQLITE_ROW)
      {
        if (Text(patch_set.get(), 0) != "mp1-r3me01-v1")
          throw std::runtime_error("Unsupported PrimeHack patch set; regenerate CFG");
        AddPrimeHackImages(volume, images);
        primehack = true;
      }
    }
    LoadTraceImages(db.get(), images);
    {
      auto version =
          Prepare(db.get(), "SELECT value FROM metadata WHERE key='dol_images_version'");
      if (sqlite3_step(version.get()) != SQLITE_ROW || Text(version.get(), 0) != "1")
        throw std::runtime_error("Unsupported DOL image CFG version");
      auto identities = Prepare(db.get(), "SELECT id,name,sha256 FROM dol_images ORDER BY id");
      size_t seen = 0;
      while (sqlite3_step(identities.get()) == SQLITE_ROW)
      {
        if (seen >= images.size() ||
            sqlite3_column_int64(identities.get(), 0) != static_cast<sqlite3_int64>(seen) ||
            Text(identities.get(), 1) != images[seen].name ||
            Text(identities.get(), 2) != images[seen].hash)
          throw std::runtime_error("DOL image identity mismatch; regenerate CFG from this disc");
        ++seen;
      }
      if (seen != images.size())
        throw std::runtime_error("DOL image inventory mismatch");
    }
    const size_t cfg_image_count = images.size();
    AddRealModeImages(images, real_mode_dumps);
    std::vector<std::map<u32, u32>> rows(images.size());
    for (size_t id = cfg_image_count; id < images.size(); ++id)
      rows[id] = images[id].real_blocks;
    {
      auto query = Prepare(db.get(), "SELECT image_id,pc,count FROM dol_image_blocks");
      while (sqlite3_step(query.get()) == SQLITE_ROW)
      {
        const auto id = sqlite3_column_int64(query.get(), 0);
        const auto pc = sqlite3_column_int64(query.get(), 1);
        const auto count = sqlite3_column_int64(query.get(), 2);
        if (id < 0 || id >= static_cast<sqlite3_int64>(cfg_image_count) || pc < 0 ||
            pc > UINT32_MAX || (pc & 3) || count <= 0 || count > (UINT32_MAX - pc) / 4)
          throw std::runtime_error("Invalid DOL block bounds");
        for (u32 n = 0; n < count; ++n)
          if (!images[id].memory.ReadInstruction(static_cast<u32>(pc) + n * 4))
            throw std::runtime_error("DOL block outside text");
        rows[id].emplace(static_cast<u32>(pc), static_cast<u32>(count));
      }
    }
    for (size_t id : selected)
      if (id >= images.size() || images[id].base_image)
        throw std::runtime_error(fmt::format("--images: {} is not a base DOL image", id));
    std::vector<size_t> bases;
    for (size_t id = 0; id < images.size(); ++id)
    {
      // Trace-sourced (auxiliary) and real-mode images are always emitted: the
      // user asked for them.
      if (images[id].base_image || (!selected.empty() && !selected.contains(id) &&
                                    images[id].trace_words.empty() && !images[id].real_mode))
        continue;
      if (rows[id].empty())
        throw std::runtime_error("Selected image has no blocks: " + images[id].name);
      bases.push_back(id);
    }
    if (bases.empty())
      throw std::runtime_error("No base DOL images selected");
    if (std::filesystem::exists(output))
    {
      for (const auto& entry : std::filesystem::directory_iterator(output))
      {
        if (entry.path().filename() != "translate.log")
          throw std::runtime_error("Use an empty output directory to avoid stale generated code");
      }
    }
    std::filesystem::create_directories(output);
    Output(output + "/aot_runtime.h") << s_aot_runtime_header;
    Output(output + "/aot_images.h") << s_aot_images_header;

    // Every base image is loaded at its own text range at some point; the
    // flat table spans [min block, max block] of the base plus variant pcs.
    struct Variant
    {
      size_t image;
      u32 mask;
    };
    struct OverrideOut
    {
      u32 pc = 0;
      u32 base_size = 0;
      bool has_base = false;
      std::vector<std::pair<bool, u32>> by_mask;  // (has variant, size)
    };
    struct BaseOut
    {
      size_t image = 0;
      std::string sym;  // prefix_d<i>
      u32 table_base = 0, table_size = 0;
      std::vector<Variant> variants;
      std::vector<AotImageWord> words;
      std::vector<OverrideOut> overrides;
      std::map<std::string, u32> unhandled;
      size_t translated = 0, variant_blocks = 0, inline_targets = 0;
    };
    std::vector<BaseOut> outs;
    size_t total_table_entries = 0;

    for (size_t b : bases)
    {
      BaseOut out;
      out.image = b;
      out.sym = fmt::format("{}_d{}", prefix, b);
      const auto& blocks = rows[b];
      for (size_t v = 0; v < images.size(); ++v)
        if (images[v].base_image && *images[v].base_image == b)
          out.variants.push_back({v, ParseVariantMask(images[v].name)});

      std::set<u32> known(std::views::keys(blocks).begin(), std::views::keys(blocks).end());
      std::unordered_set<u32> volatile_pcs;
      std::set<u32> excluded_lines;
      for (const auto& var : out.variants)
      {
        for (const auto& [pc, count] : rows[var.image])
        {
          volatile_pcs.insert(pc);
          known.insert(pc);
        }
      }
      if (!out.variants.empty())
      {
        for (const auto& patch : PrimeHack::MP1_PATCHES)
          excluded_lines.insert(patch.address & ~31u);
      }
      u32 lo = UINT32_MAX, hi = 0;
      for (u32 pc : known)
      {
        lo = std::min(lo, pc);
        hi = std::max(hi, pc);
      }
      if (hi >= 0x82000000u)
        throw std::runtime_error("Trusted image block outside MEM1");
      out.table_base = lo & ~3u;
      out.table_size = ((hi - out.table_base) >> 2) + 1;
      total_table_entries += out.table_size;
      if (images[b].real_mode)
      {
        // Every translated word, at its cached virtual alias (aot_images.h).
        for (const auto& [pc, count] : blocks)
          for (u32 n = 0; n < count; ++n)
            out.words.push_back({0x80000000u | (pc + n * 4), *images[b].memory.ReadInstruction(pc + n * 4)});
      }
      else
      {
        out.words = ChooseDiscriminators(images, b, excluded_lines);
      }

      // Inline hints exactly as the single-image pipeline computes them, minus
      // runtime-swapped pcs (never inlined: their body is chosen at runtime).
      const auto branch_entered = BranchEnteredTargets(images[b].memory, blocks);
      std::unordered_map<u32, u32> sizes(blocks.begin(), blocks.end());
      std::unordered_set<u32> inline_targets;
      for (const auto& [pc, count] : blocks)
        if (!branch_entered.contains(pc) && !volatile_pcs.contains(pc))
          inline_targets.insert(pc);
      out.inline_targets = inline_targets.size();

      // Per-image forward declarations (block TUs of this image include it).
      {
        auto fwd = Output(fmt::format("{}/{}_forward_decls.h", output, out.sym));
        fwd << fmt::format("#ifndef {0}_FORWARD_DECLS_H\n#define {0}_FORWARD_DECLS_H\n", out.sym);
        fwd << "#include \"aot_images.h\"\n\n";
        fwd << fmt::format("#define {}_TABLE_BASE {:#010x}u\n", out.sym, out.table_base);
        fwd << fmt::format("#define {}_TABLE_SIZE {}u\n", out.sym, out.table_size);
        fwd << fmt::format("extern AOTBlockFunc {}_fast_table[];\n", out.sym);
        fwd << fmt::format("__attribute__((noinline)) void {}_dispatch(AOTState* s);\n", prefix);
        fwd << fmt::format("#define {}_dispatch {}_dispatch\n\n", out.sym, prefix);
        for (const auto& [pc, count] : blocks)
          fwd << fmt::format("__attribute__((noinline)) void {}_block_{:08x}(AOTState* s);\n",
                             out.sym, pc);
        for (const auto& var : out.variants)
          for (const auto& [pc, count] : rows[var.image])
            fwd << fmt::format(
                "__attribute__((noinline)) void {}_block_{:08x}_v{:02x}(AOTState* s);\n",
                out.sym, pc, var.mask);
        fwd << "#endif\n";
      }

      // Base blocks, grouped by 64KB like the single-image pipeline.
      {
        AOTCEmitter emitter(images[b].memory, known, out.sym);
        emitter.SetInlineHints(sizes, inline_targets);
        emitter.SetVolatileTargets(volatile_pcs);
        if (images[b].real_mode)
          emitter.SetRealMode();
        std::map<u32, std::vector<TrustedBlock>> groups;
        for (const auto& [pc, count] : blocks)
          groups[pc >> 16].push_back({pc, count});
        for (const auto& [group, list] : groups)
        {
          auto file = Output(fmt::format("{}/{}_blocks_d{}_{:04x}.c", output, prefix, b, group));
          file << fmt::format("#include \"{}_forward_decls.h\"\n\n", out.sym);
          for (const auto& tb : list)
          {
            file << emitter.TranslateBlock(tb.pc, tb.count, true) << "\n";
            ++out.translated;
          }
        }
        out.unhandled = emitter.GetUnhandledOpcodes();
      }

      // Variant blocks: compiled from the patched image, no chain inlining,
      // symbol suffixed by mask. Their edges to base blocks bind directly
      // (identical code in both images); edges to other volatile pcs probe.
      if (!out.variants.empty())
      {
        auto file = Output(fmt::format("{}/{}_blocks_d{}_variants.c", output, prefix, b));
        file << fmt::format("#include \"{}_forward_decls.h\"\n\n", out.sym);
        for (const auto& var : out.variants)
        {
          AOTCEmitter emitter(images[var.image].memory, known, out.sym);
          emitter.SetInlineHints(sizes, {});
          emitter.SetVolatileTargets(volatile_pcs);
          for (const auto& [pc, count] : rows[var.image])
          {
            file << emitter.TranslateBlock(pc, count, true, 0,
                                           fmt::format("{}_block_{:08x}_v{:02x}", out.sym, pc,
                                                       var.mask))
                 << "\n";
            ++out.variant_blocks;
          }
          for (const auto& [name, n] : emitter.GetUnhandledOpcodes())
            out.unhandled[name] += n;
        }
        for (u32 pc : std::set<u32>(volatile_pcs.begin(), volatile_pcs.end()))
        {
          OverrideOut ov;
          ov.pc = pc;
          if (auto it = blocks.find(pc); it != blocks.end())
          {
            ov.has_base = true;
            ov.base_size = it->second;
          }
          for (const auto& var : out.variants)
          {
            auto it = rows[var.image].find(pc);
            ov.by_mask.emplace_back(it != rows[var.image].end(),
                                    it != rows[var.image].end() ? it->second : 0);
          }
          out.overrides.push_back(ov);
        }
      }
      fmt::println(std::cerr,
                   "DOL image {}: {} -- {} blocks, {} variant blocks ({} masks), table {:#010x} "
                   "x {} entries ({:.1f} MB), {} discriminators, {} chain-inline targets",
                   b, images[b].name, out.translated, out.variant_blocks, out.variants.size(),
                   out.table_base, out.table_size,
                   out.table_size * sizeof(void*) / (1024.0 * 1024.0), out.words.size(),
                   out.inline_targets);
      outs.push_back(std::move(out));
    }

    // RSO modules (--rso-module): base-relative module blocks bound to the
    // owning DOL image's blocks for external calls; descriptors for the tracker.
    struct RsoOut
    {
      std::string sym;
      std::string name;
      u32 num_sections = 0, text_section = 1, text_size = 0;
      std::vector<u32> section_sizes;
      size_t blocks = 0, word_count = 0;
    };
    std::vector<RsoOut> rso_outs;
    for (const auto& spec : ParseRsoSpecs(rso_module_specs))
    {
      const auto owner = std::find_if(outs.begin(), outs.end(),
                                      [&](const BaseOut& o) { return o.image == spec.image; });
      if (owner == outs.end())
        throw std::runtime_error("--rso-module: image not emitted: " + std::to_string(spec.image));
      RsoFile rso = ParseRso(ReadDiscFile(volume, spec.path));
      const std::string dir = spec.path.substr(0, spec.path.find_last_of('/') + 1);
      RsoFile sel = ParseRso(ReadDiscFile(volume, dir + "selfile.sel"));
      std::unordered_map<std::string, u32> sel_exports;  // name -> export record index
      for (u32 k = 0; k < sel.exports.size(); ++k)
        sel_exports[sel.exports[k].name] = k;
      if (!images[spec.image].dol)
        throw std::runtime_error("--rso-module: image " + std::to_string(spec.image) +
                                 " is not a DOL");
      const std::map<u32, u32> static_bases =
          ReadStaticBases(spec.static_bases, *images[spec.image].dol);
      RsoOut ro;
      ro.sym = fmt::format("{}_rso{}", prefix, rso_outs.size());
      ro.name = rso.name;
      ro.num_sections = static_cast<u32>(rso.sections.size());
      // The code section is the one the module's _prolog lives in (section 1
      // in every Trilogy module; the file carries no executable flag).
      ro.text_section = 1;
      for (const auto& e : rso.exports)
        if (e.name == "_prolog")
          ro.text_section = e.section;
      if (ro.text_section == 0 || ro.text_section >= rso.sections.size())
        throw std::runtime_error("RSO: bad code section index in " + spec.path);
      const auto [text_off, text_size] = rso.sections.at(ro.text_section);
      ro.text_size = text_size;
      if (!text_off || !text_size || (text_off & 3) || (text_size & 3) ||
          text_off + text_size > rso.data.size())
        throw std::runtime_error("RSO: bad text section in " + spec.path);
      if (std::any_of(rso_outs.begin(), rso_outs.end(), [&](const RsoOut& o) {
            return o.name == ro.name && o.num_sections == ro.num_sections &&
                   o.section_sizes == [&] {
                     std::vector<u32> v;
                     for (const auto& sec : rso.sections)
                       v.push_back(sec.second);
                     return v;
                   }();
          }))
        throw std::runtime_error("RSO: two modules named " + ro.name +
                                 " with identical section sizes cannot be told apart");
      for (const auto& sec : rso.sections)
        ro.section_sizes.push_back(sec.second);
      const u32 text_sect = ro.text_section;
      auto in_text = [&](u32 off) { return off >= text_off && off < text_off + text_size; };
      auto synth = [&](u32 file_off) { return (text_sect << 24) | (file_off - text_off); };

      std::vector<u8> text(rso.data.begin() + text_off, rso.data.begin() + text_off + text_size);
      ModuleMode mode;
      mode.fn_prefix = ro.sym;
      mode.base_array = ro.sym + "_base";
      std::set<u32> dol_known(std::views::keys(rows[spec.image]).begin(),
                              std::views::keys(rows[spec.image]).end());
      mode.dol_blocks = &dol_known;
      for (const auto& [off, size] : rso.sections)
        mode.section_sizes.push_back(size);
      std::set<u32> leaders = {text_sect << 24};
      std::vector<AotRsoWord> words;
      auto field16 = [](u8 type, u32 t) -> u16 {
        return type == R_PPC_ADDR16_LO ? static_cast<u16>(t) :
               type == R_PPC_ADDR16_HI ? static_cast<u16>(t >> 16) :
                                         static_cast<u16>((t + 0x8000) >> 16);
      };
      std::set<u32> reloc_words;  // file offsets of words touched by any relocation
      for (const auto* list : {&rso.internals, &rso.externals})
        for (const auto& r : *list)
          reloc_words.insert(r.offset & ~3u);
      for (const auto& r : rso.internals)
      {
        // Code addresses taken anywhere (vtables, function pointers, branches)
        // become block leaders.
        if (r.index == text_sect && r.addend < text_size)
          leaders.insert((text_sect << 24) | (r.addend & ~3u));
        if (!in_text(r.offset))
          continue;
        const u32 site = synth(r.offset & ~3u);
        if (r.type == R_PPC_ADDR16_LO || r.type == R_PPC_ADDR16_HI || r.type == R_PPC_ADDR16_HA)
          mode.imm_relocs[site] =
              ModuleImmReloc{r.type, fmt::format("({}_base[{}]+{:#x}u)", ro.sym, r.index, r.addend)};
        else if (r.type == R_PPC_REL24 && r.index == text_sect)
          mode.branch_overrides[site] = {ModuleBranchOverride::Local, (text_sect << 24) | r.addend};
        else
          mode.force_fallback.insert(site);
      }
      // Located external HA/LO words are base-independent: identity checks that
      // the module was linked against this DOL with these static bases. Keep
      // candidates per static section so every section used is checked.
      std::map<u32, std::vector<AotRsoWord>> ext_words;
      size_t cross_module = 0;
      for (const auto& r : rso.externals)
      {
        if (!in_text(r.offset))
          continue;
        const u32 site = synth(r.offset & ~3u);
        const auto exp = r.index < rso.imports.size() ? sel_exports.find(rso.imports[r.index]) :
                                                        sel_exports.end();
        const auto sect_base =
            exp == sel_exports.end() ? static_bases.end() :
                                       static_bases.find(sel.exports[exp->second].section);
        if (sect_base == static_bases.end())
        {
          // Cross-RSO import (not exported by the static module): single-step
          // the relocated in-RAM word.
          if (exp == sel_exports.end())
            ++cross_module;
          else
            throw std::runtime_error(fmt::format("RSO {}: static section {} has no base in {}",
                                                 ro.name, sel.exports[exp->second].section,
                                                 spec.static_bases));
          mode.force_fallback.insert(site);
          continue;
        }
        const u32 target = sect_base->second + sel.exports[exp->second].offset + r.addend;
        const u32 at = r.offset - text_off;
        if (r.type == R_PPC_REL24)
        {
          mode.branch_overrides[site] = {ModuleBranchOverride::Absolute, target};
        }
        else if (r.type == R_PPC_ADDR16_LO || r.type == R_PPC_ADDR16_HI ||
                 r.type == R_PPC_ADDR16_HA)
        {
          const u16 f = field16(r.type, target);
          text[at] = static_cast<u8>(f >> 8);
          text[at + 1] = static_cast<u8>(f);
          if ((r.offset & 3) == 2)
            ext_words[sect_base->first].push_back(
                {r.offset & ~3u, (u32(text[at - 2]) << 24) | (u32(text[at - 1]) << 16) |
                                     (u32(text[at]) << 8) | text[at + 1]});
        }
        else if (r.type == R_PPC_ADDR32 && (r.offset & 3) == 0)
        {
          for (int b = 0; b < 4; ++b)
            text[at + b] = static_cast<u8>(target >> (24 - 8 * b));
          mode.force_fallback.insert(site);  // data word in text: never executed as code
        }
        else
        {
          mode.force_fallback.insert(site);
        }
      }
      // Up to 12 external checks, round-robin over the static sections used.
      for (size_t round = 0, added = 1; added && words.size() < 12; ++round)
      {
        added = 0;
        for (const auto& [sect, list] : ext_words)
        {
          if (round < list.size() && words.size() < 12)
          {
            // Spread the picks over the module rather than its first function.
            words.push_back(list[(round * 7919) % list.size()]);
            ++added;
          }
        }
      }
      for (const auto& e : rso.exports)
        if (e.section == text_sect && e.offset < text_size)
          leaders.insert((text_sect << 24) | (e.offset & ~3u));
      // Unrelocated text words: must equal the disc bytes in RAM.
      for (u32 k = 0, step = std::max(4u, (text_size / 16) & ~3u); k < text_size && words.size() < 24;
           k += step)
      {
        u32 at = k;
        while (at < text_size && reloc_words.contains(text_off + at))
          at += 4;
        if (at < text_size)
          words.push_back({text_off + at, Be32(rso.data, text_off + at)});
      }
      // Distinct offsets only (a short module can land twice on one word).
      std::sort(words.begin(), words.end(),
                [](const AotRsoWord& a, const AotRsoWord& b) { return a.offset < b.offset; });
      words.erase(std::unique(words.begin(), words.end(),
                              [](const AotRsoWord& a, const AotRsoWord& b) {
                                return a.offset == b.offset;
                              }),
                  words.end());

      PPCMemoryImage mem;
      mem.AddSection(text_sect << 24, text.data(), text_size);
      // Linear sweep: every valid word belongs to a block; leaders at the
      // section start, after every block-ending instruction, at every branch
      // target and at every relocation target / export in .text.
      auto inst_at = [&](u32 pc) -> std::optional<UGeckoInstruction> {
        const auto w = mem.ReadInstruction(pc);
        if (!w || *w == 0 || !PPCTables::IsValidInstruction(UGeckoInstruction(*w), pc))
          return std::nullopt;
        return UGeckoInstruction(*w);
      };
      const u32 end = (text_sect << 24) + text_size;
      for (u32 pc = text_sect << 24; pc < end; pc += 4)
      {
        const auto inst = inst_at(pc);
        if (!inst)
        {
          leaders.insert(pc + 4);
          continue;
        }
        if (!(PPCTables::GetOpInfo(*inst, pc)->flags & FL_ENDBLOCK))
          continue;
        leaders.insert(pc + 4);
        if (auto ov = mode.branch_overrides.find(pc); ov != mode.branch_overrides.end())
        {
          if (ov->second.kind == ModuleBranchOverride::Local)
            leaders.insert(ov->second.target);
        }
        else if (inst->OPCD == 18 && !inst->AA)
          leaders.insert(pc + u32(SignExt26(inst->LI << 2)));
        else if (inst->OPCD == 16 && !inst->AA)
          leaders.insert(pc + u32(SignExt16(s16(inst->BD << 2))));
      }
      std::map<u32, u32> mblocks;
      for (auto it = leaders.begin(); it != leaders.end(); ++it)
      {
        if (*it < (text_sect << 24) || *it >= end)
          continue;
        const auto next = std::next(it);
        u32 count = 0;
        for (u32 pc = *it; pc < end; pc += 4)
        {
          if (next != leaders.end() && pc == *next && pc != *it)
            break;
          const auto inst = inst_at(pc);
          if (!inst)
            break;
          ++count;
          if (PPCTables::GetOpInfo(*inst, pc)->flags & FL_ENDBLOCK)
            break;
        }
        if (count)
          mblocks.emplace(*it, count);
      }
      std::set<u32> mblock_set(std::views::keys(mblocks).begin(), std::views::keys(mblocks).end());
      AOTCEmitter emitter(mem, {}, owner->sym);
      emitter.SetModuleMode(&mode, mblock_set);
      auto file = Output(fmt::format("{}/{}_blocks_rso{}.c", output, prefix, rso_outs.size()));
      file << fmt::format("#include \"{}_forward_decls.h\"\n\n", owner->sym);
      file << fmt::format("uint32_t {}_base[{}];\n\n", ro.sym, ro.num_sections);
      for (const auto& [pc, count] : mblocks)
        file << fmt::format("__attribute__((noinline)) void {}_s{}_{:x}(AOTState* s);\n", ro.sym,
                            pc >> 24, pc & 0x00FFFFFF);
      for (const auto& [pc, count] : mblocks)
        file << emitter.TranslateBlock(pc, count, true) << "\n";
      file << fmt::format("const AOTBlockFunc {}_table[{}] = {{\n", ro.sym, text_size / 4);
      for (u32 e = 0; e < text_size / 4; ++e)
      {
        const u32 pc = (text_sect << 24) | (e << 2);
        file << (mblocks.contains(pc) ? fmt::format("    {}_s{}_{:x},\n", ro.sym, text_sect, e << 2) :
                                        std::string("    0,\n"));
      }
      file << "};\n";
      file << fmt::format("const uint32_t {}_section_sizes[] = {{", ro.sym);
      for (const auto& [off, size] : rso.sections)
        file << fmt::format("{:#x}u,", size);
      file << "};\n";
      file << fmt::format("const AotRsoWord {}_words[] = {{\n", ro.sym);
      for (const auto& w : words)
        file << fmt::format("    {{{:#x}u,{:#010x}u}},\n", w.offset, w.word);
      file << "};\n";
      ro.word_count = words.size();
      file << "#if AOT_HARNESS\n";
      file << fmt::format("const AotImageBlockSize {}_block_sizes[] = {{\n", ro.sym);
      for (const auto& [pc, count] : mblocks)
        file << fmt::format("    {{{:#x}u,{}u}},\n", pc & 0x00FFFFFF, count);
      file << "};\n#endif\n";
      ro.blocks = mblocks.size();
      for (const auto& [name, n] : emitter.GetUnhandledOpcodes())
        fmt::println(std::cerr, "    rso {} fallback [{}]: {}", ro.name, name, n);
      fmt::println(std::cerr,
                   "RSO module {} ({}, {}): {} blocks over {} bytes of text (section {}), {} imm "
                   "relocs, {} branch overrides, {} forced fallbacks ({} cross-module import "
                   "sites), {} identity words, bound to image {}",
                   ro.sym, ro.name, spec.path, ro.blocks, text_size, text_sect,
                   mode.imm_relocs.size(), mode.branch_overrides.size(),
                   mode.force_fallback.size(), cross_module, words.size(), spec.image);
      rso_outs.push_back(std::move(ro));
    }

    // Shared dispatch: tables, descriptors, tracker-driven dispatch, registration.
    {
      auto file = Output(fmt::format("{}/{}_dispatch.c", output, prefix));
      file << "#include \"aot_images.h\"\n";
      for (const auto& out : outs)
        file << fmt::format("#include \"{}_forward_decls.h\"\n", out.sym);
      file << "\n";
      for (const auto& out : outs)
      {
        const auto& blocks = rows[out.image];
        file << fmt::format("AOTBlockFunc {}_fast_table[{}] = {{\n", out.sym, out.table_size);
        auto it = blocks.begin();
        for (u32 e = 0; e < out.table_size; ++e)
        {
          const u32 addr = out.table_base + (e << 2);
          while (it != blocks.end() && it->first < addr)
            ++it;
          if (it != blocks.end() && it->first == addr)
            file << fmt::format("    {}_block_{:08x},\n", out.sym, addr);
          else
            file << "    0,\n";
        }
        file << "};\n";
        file << fmt::format("static const AotImageWord {}_words[] = {{\n", out.sym);
        for (const auto& w : out.words)
          file << fmt::format("    {{{:#010x}u,{:#010x}u}},\n", w.addr, w.word);
        file << "};\n";
        if (!out.variants.empty())
        {
          file << fmt::format("static const AotImagePatchSite {}_patches[] = {{\n", out.sym);
          for (const auto& patch : PrimeHack::MP1_PATCHES)
            file << fmt::format("    {{{:#010x}u,{:#010x}u,{:#010x}u}},\n", patch.address,
                                patch.original, patch.replacement);
          file << "};\n";
          file << fmt::format("static const uint32_t {}_masks[] = {{", out.sym);
          for (const auto& var : out.variants)
            file << fmt::format("{:#x}u,", var.mask);
          file << "};\n";
          for (const auto& ov : out.overrides)
          {
            file << fmt::format("static const AOTBlockFunc {}_ov_{:08x}_fns[] = {{", out.sym,
                                ov.pc);
            for (size_t k = 0; k < out.variants.size(); ++k)
              file << (ov.by_mask[k].first ?
                           fmt::format("{}_block_{:08x}_v{:02x},", out.sym, ov.pc,
                                       out.variants[k].mask) :
                           "0,");
            file << "};\n";
            file << fmt::format("static const uint32_t {}_ov_{:08x}_sizes[] = {{", out.sym,
                                ov.pc);
            for (size_t k = 0; k < out.variants.size(); ++k)
              file << fmt::format("{}u,", ov.by_mask[k].second);
            file << "};\n";
          }
          file << fmt::format("static const AotImageOverride {}_overrides[] = {{\n", out.sym);
          for (const auto& ov : out.overrides)
            file << fmt::format("    {{{:#010x}u,{}u,{},{}_ov_{:08x}_fns,{}_ov_{:08x}_sizes}},\n",
                                ov.pc, ov.base_size,
                                ov.has_base ? fmt::format("{}_block_{:08x}", out.sym, ov.pc) : "0",
                                out.sym, ov.pc, out.sym, ov.pc);
          file << "};\n";
        }
        file << "#if AOT_HARNESS\n";
        file << fmt::format("static const AotImageBlockSize {}_block_sizes[] = {{\n", out.sym);
        for (const auto& [pc, count] : blocks)
          file << fmt::format("    {{{:#010x}u,{}u}},\n", pc, count);
        file << "};\n#endif\n";
      }
      file << fmt::format("static const AotImageDesc {}_images[] = {{\n", prefix);
      for (const auto& out : outs)
      {
        const bool pv = !out.variants.empty();
        file << fmt::format(
            "    {{\"{}\",{:#010x}u,{}u,{}_fast_table,{}_words,{}u,{},{}u,{},{}u,{},{}u,\n",
            images[out.image].name, out.table_base, out.table_size, out.sym, out.sym,
            out.words.size(), pv ? out.sym + "_patches" : "0",
            pv ? PrimeHack::MP1_PATCHES.size() : 0, pv ? out.sym + "_masks" : "0",
            pv ? out.variants.size() : 0, pv ? out.sym + "_overrides" : "0",
            pv ? out.overrides.size() : 0);
        const u32 flags = images[out.image].real_mode         ? AOT_IMAGE_REAL_MODE :
                          !images[out.image].trace_words.empty() ? AOT_IMAGE_AUXILIARY :
                                                                   0u;
        file << "#if AOT_HARNESS\n";
        file << fmt::format("     {}_block_sizes,{}u,{}u}},\n", out.sym, rows[out.image].size(),
                            flags);
        file << fmt::format("#else\n     0,0u,{}u}},\n#endif\n", flags);
      }
      file << "};\n\n";
      file << fmt::format("__attribute__((noinline)) void {}_dispatch(AOTState* s) {{\n", prefix);
      file << "#if AOT_HARNESS\n    if (aot_single_block_mode) return;\n#endif\n";
      file << "    if (s->downcount <= 0) return;\n";
      file << "    if (__builtin_expect(*aot_images_generation != aot_images_seen, 0))\n"
              "        aot_images_rescan();\n";
      file << "    uint32_t idx = (s->pc - aot_active_image.base) >> 2;\n";
      file << "    if (idx < aot_active_image.size) {\n";
      file << "        AOTBlockFunc fn = aot_active_image.table[idx];\n";
      file << "        if (fn) { [[clang::musttail]] return fn(s); }\n    }\n";
      if (std::any_of(outs.begin(), outs.end(),
                      [&](const BaseOut& o) { return !images[o.image].trace_words.empty(); }))
      {
        file << "    idx = (s->pc - aot_active_image_aux.base) >> 2;\n";
        file << "    if (idx < aot_active_image_aux.size) {\n";
        file << "        AOTBlockFunc fn = aot_active_image_aux.table[idx];\n";
        file << "        if (fn) { [[clang::musttail]] return fn(s); }\n    }\n";
      }
      if (std::any_of(outs.begin(), outs.end(),
                      [&](const BaseOut& o) { return images[o.image].real_mode; }))
      {
        // Real-mode slot: physical pcs, MSR.IR clear only (the exception vectors).
        file << "    if (!(s->msr & 0x20u)) {\n";
        file << "        idx = (s->pc - aot_active_image_real.base) >> 2;\n";
        file << "        if (idx < aot_active_image_real.size) {\n";
        file << "            AOTBlockFunc fn = aot_active_image_real.table[idx];\n";
        file << "            if (fn) { [[clang::musttail]] return fn(s); }\n        }\n    }\n";
      }
      file << fmt::format("    [[clang::musttail]] return {}(s);\n}}\n\n",
                          rso_outs.empty() ? "aot_interpreter_single_step" : "aot_rso_dispatch");
      if (!rso_outs.empty())
      {
        for (const auto& ro : rso_outs)
        {
          file << fmt::format("extern uint32_t {0}_base[]; extern const AOTBlockFunc {0}_table[];\n"
                              "extern const uint32_t {0}_section_sizes[]; extern const AotRsoWord "
                              "{0}_words[];\n",
                              ro.sym);
          file << fmt::format("#if AOT_HARNESS\nextern const AotImageBlockSize {}_block_sizes[];\n#endif\n",
                              ro.sym);
        }
        file << fmt::format("static const AotRsoModuleDesc {}_rso_modules[] = {{\n", prefix);
        for (const auto& ro : rso_outs)
        {
          file << fmt::format("    {{\"{1}\",{2}u,{0}_section_sizes,{3}u,{4}u,{0}_table,{0}_base,"
                              "{0}_words,{5}u,\n",
                              ro.sym, ro.name, ro.num_sections, ro.text_section, ro.text_size,
                              ro.word_count);
          file << fmt::format("#if AOT_HARNESS\n     {0}_block_sizes,{1}u}},\n#else\n     0,0u}},\n#endif\n",
                              ro.sym, ro.blocks);
        }
        file << "};\n";
      }
      file << fmt::format("AOTBlockFunc {}_lookup_block(uint32_t pc) {{ return "
                          "aot_images_lookup(pc); }}\n",
                          prefix);
      file << "#if AOT_HARNESS\n";
      file << "void aot_register_image_block_sizes(const char*, uint32_t (*)(uint32_t));\n";
      file << fmt::format("static uint32_t {}_image_block_size(uint32_t pc) {{ return "
                          "aot_images_block_size(pc); }}\n",
                          prefix);
      file << "#endif\n\n";
      file << "__attribute__((constructor))\n";
      file << fmt::format("static void aot_register_{}(void) {{\n", prefix);
      file << fmt::format(
          "    aot_register_game(\"{}\", {}_dispatch, {}_lookup_block, AOT_ABI_VERSION);\n",
          prefix, prefix, prefix);
      file << fmt::format("    aot_register_game_image(\"{}\", \"{}\");\n", prefix, boot_hash);
      file << fmt::format(
          "    aot_register_game_images(\"{}\", {}_images, {}u, AOT_IMAGES_VERSION);\n", prefix,
          prefix, outs.size());
      if (!rso_outs.empty())
        file << fmt::format("    aot_register_game_rso_modules(\"{}\", {}_rso_modules, {}u, "
                            "AOT_IMAGES_VERSION);\n",
                            prefix, prefix, rso_outs.size());
      file << "#if AOT_HARNESS\n";
      file << fmt::format("    aot_register_image_block_sizes(\"{}\", {}_image_block_size);\n",
                          prefix, prefix);
      file << "#endif\n}\n";
    }

    // Build script: identical flags to the single-image pipeline (and to
    // build-aot-ios.sh, which globs the same <prefix>_blocks_*.c names).
    {
      const std::string path = output + "/build.sh";
      auto script = Output(path);
      script << "#!/bin/bash\nset -e\ncd \"$(dirname \"$0\")\"\n";
      script << fmt::format("PREFIX=\"{}\"\n", prefix);
      script << "BLOCK_CFLAGS=\"-Os -flto=thin -arch arm64 -mcpu=apple-a14"
                " -fwrapv -fno-strict-aliasing -DAOT_HARNESS=1\"\n";
      script << "DISPATCH_CFLAGS=\"-O2 -flto=thin -arch arm64 -mcpu=apple-a14"
                " -fwrapv -fno-strict-aliasing -DAOT_HARNESS=1\"\n";
      script << "JOBS=\"${AOT_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}\"\n\n";
      script << "echo \"Compiling AOT blocks with LTO (${JOBS} jobs)...\"\n";
      script << "export BLOCK_CFLAGS\n";
      script << "printf '%s\\0' ${PREFIX}_blocks_*.c | \\\n";
      script << "    xargs -0 -n1 -P \"$JOBS\" sh -c"
                " 'exec clang -c $BLOCK_CFLAGS -I. \"$1\" -o \"${1%.c}.o\"' sh\n";
      script << "clang -c $DISPATCH_CFLAGS -I. \"${PREFIX}_dispatch.c\""
                " -o \"${PREFIX}_dispatch.o\"\n";
      script << "echo \"Creating static library...\"\n";
      script << "ar rcs lib${PREFIX}_aot.a ${PREFIX}_*.o\n";
      script << "echo \"Done: lib${PREFIX}_aot.a\"\n";
      script.close();
      std::filesystem::permissions(path, std::filesystem::perms::owner_exec |
                                             std::filesystem::perms::group_exec |
                                             std::filesystem::perms::others_exec,
                                   std::filesystem::perm_options::add);
    }
    size_t translated = 0;
    std::map<std::string, u32> unhandled;
    for (const auto& out : outs)
    {
      translated += out.translated + out.variant_blocks;
      for (const auto& [name, n] : out.unhandled)
        unhandled[name] += n;
    }
    fmt::println(std::cerr, "Results:");
    fmt::println(std::cerr, "  Translated: {} blocks ({} images, {:.1f} MB of tables)",
                 translated, outs.size(), total_table_entries * sizeof(void*) / (1024.0 * 1024.0));
    fmt::println(std::cerr, "  Skipped (SMC): 0 blocks");
    if (!unhandled.empty())
    {
      fmt::println(std::cerr, "  Unhandled opcodes ({} types, falling back to interpreter):",
                   unhandled.size());
      for (const auto& [name, n] : unhandled)
        fmt::println(std::cerr, "    {}: {} occurrences", name, n);
    }
    fmt::println(std::cerr, "Trusted multi-image library generated in {}/", output);
    return true;
  }
  catch (const std::exception& e)
  {
    fmt::println(std::cerr, "DOL images: {}", e.what());
    return false;
  }
}
} // namespace DolphinTool
