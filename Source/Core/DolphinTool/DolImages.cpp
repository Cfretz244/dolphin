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
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <mbedtls/sha256.h>
#include <sqlite3.h>

#include "Common/Swap.h"
#include "Core/Boot/DolReader.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PrimeHack/Patches.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DolphinTool/AotCEmitter.h"
#include "DolphinTool/CfgCommand.h"

extern const char s_aot_runtime_header[];

namespace DolphinTool
{
namespace
{
struct Image
{
  std::string name;
  std::string hash;
  std::unique_ptr<DolReader> dol;
  PPCMemoryImage memory;
  std::vector<std::vector<u8>> patched_sections;
  std::optional<size_t> base_image;
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
  }
  images.push_back(std::move(image));
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
                      bool primehack)
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
    auto db = Open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    Exec(db.get(), "BEGIN; CREATE TABLE dol_images(id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
                   "sha256 TEXT NOT NULL); CREATE TABLE dol_image_blocks(image_id INTEGER, "
                   "pc INTEGER, count INTEGER, PRIMARY KEY(image_id,pc)); "
                   "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT); "
                   "INSERT INTO metadata VALUES('dol_images_version','1');");
    if (primehack)
      Exec(db.get(), "INSERT INTO metadata VALUES('primehack_patch_set','mp1-r3me01-v1');");
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
      if (image.base_image)
      {
        for (const auto& [pc, count] : discovered[*image.base_image])
          seeds.insert(pc);
      }
      for (const auto& sb : snapshots)
      {
        for (const auto& snap : sb.snapshots)
        {
          if ((sb.addr & 3) || snap.words.empty() || snap.words.size() > (UINT32_MAX - sb.addr) / 4)
            continue;
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
    // Immutable address index: no cached image choice, so reloads, patches and
    // stale direct entries still go through the content guards. Avoid a binary
    // search through every image's blocks for each interpreted instruction.
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
    dispatch << "static const Candidate* find_candidate(uint32_t pc) {\n";
    dispatch << "  if (pc&3) return 0; uint32_t page=page_ids[pc>>16]; if(!page) return 0;\n";
    dispatch << "  uint32_t lo=candidate_indices[page-1][(pc&65535)>>2]; if(!lo) return 0; --lo;\n";
    dispatch << fmt::format("  for(;lo<{}u && candidates[lo].pc==pc;++lo) {{\n", blocks.size());
    dispatch << "    const Candidate* c=&candidates[lo]; if(c->guard_count>c->count ? "
                "aot_match_chain(pc,c->words,c->guard_count) : aot_match_code(pc,c->words,c->count)) "
                "return c;\n  } return 0;\n}\n";
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
} // namespace DolphinTool
