// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/VtxAotCommand.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <OptionParser.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <sqlite3.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "DolphinTool/VertexLoaderCEmitter.h"

namespace DolphinTool
{

// Runtime header emitted alongside the generated C vertex loader files
static const char* VTX_AOT_RUNTIME_HEADER = R"(
// Auto-generated vertex loader AOT runtime header
#ifndef VTX_AOT_RUNTIME_H
#define VTX_AOT_RUNTIME_H

#include <stdint.h>
#include <string.h>

typedef struct VtxLoaderState {
    float (*position_cache)[4];            // [3][4]
    uint32_t* position_matrix_index_cache; // [3]
    float* normal_cache;                   // [4]
    float* tangent_cache;                  // [4]
    float* binormal_cache;                 // [4]
} VtxLoaderState;

typedef int (*VtxLoaderAOTFunc)(
    const uint8_t* src, uint8_t* dst, int count,
    const uint8_t* const* arraybases, const uint32_t* strides,
    VtxLoaderState* zf);

// Portable vertex declaration (mirrors Dolphin's PortableVertexDeclaration)
typedef struct VtxAttrFormat {
    int type;        // ComponentFormat enum value
    int components;
    int offset;
    int enable;
    int integer;
} VtxAttrFormat;

typedef struct VtxPortableDecl {
    int stride;
    VtxAttrFormat position;
    VtxAttrFormat normals[3];
    VtxAttrFormat colors[2];
    VtxAttrFormat texcoords[8];
    VtxAttrFormat posmtx;
} VtxPortableDecl;

// Vertex loader registry entry
typedef struct VtxAotLoaderEntry {
    uint32_t key[5];        // vtx_desc_low, vtx_desc_high, vat_g0, vat_g1, vat_g2
    VtxLoaderAOTFunc func;
    VtxPortableDecl decl;
    uint32_t vertex_size;
    uint32_t native_components;
} VtxAotLoaderEntry;

// Scale factors for dequantization
static const float scale_factors[32] = {
    1.0f / (1u << 0),  1.0f / (1u << 1),  1.0f / (1u << 2),  1.0f / (1u << 3),
    1.0f / (1u << 4),  1.0f / (1u << 5),  1.0f / (1u << 6),  1.0f / (1u << 7),
    1.0f / (1u << 8),  1.0f / (1u << 9),  1.0f / (1u << 10), 1.0f / (1u << 11),
    1.0f / (1u << 12), 1.0f / (1u << 13), 1.0f / (1u << 14), 1.0f / (1u << 15),
    1.0f / (1u << 16), 1.0f / (1u << 17), 1.0f / (1u << 18), 1.0f / (1u << 19),
    1.0f / (1u << 20), 1.0f / (1u << 21), 1.0f / (1u << 22), 1.0f / (1u << 23),
    1.0f / (1u << 24), 1.0f / (1u << 25), 1.0f / (1u << 26), 1.0f / (1u << 27),
    1.0f / (1u << 28), 1.0f / (1u << 29), 1.0f / (1u << 30), 1.0f / (1u << 31),
};

// Inline byte-swap helpers
static inline uint16_t read_u16(const uint8_t* p) {
    uint16_t v; memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}

static inline uint32_t read_u32(const uint8_t* p) {
    uint32_t v; memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static inline float read_float(const uint8_t* p) {
    uint32_t v; memcpy(&v, p, 4);
    v = __builtin_bswap32(v);
    float f; memcpy(&f, &v, 4);
    return f;
}

static inline void write_float(uint8_t* dst, float v) {
    memcpy(dst, &v, 4);
}

static inline void write_u32_le(uint8_t* dst, uint32_t v) {
    memcpy(dst, &v, 4);
}

#endif // VTX_AOT_RUNTIME_H
)";

static std::string MakeLoaderFuncName(const std::string& prefix,
                                      const VertexLoaderCEmitter::FormatConfig& cfg)
{
  return fmt::format("{}_vtx_{:08x}_{:08x}_{:08x}_{:08x}_{:08x}", prefix,
                     cfg.vtx_desc_low, cfg.vtx_desc_high, cfg.vat_g0, cfg.vat_g1, cfg.vat_g2);
}

static std::string EmitAttrFormat(const VertexLoaderCEmitter::AttrFormat& a)
{
  return fmt::format("{{ {}, {}, {}, {}, {} }}", a.type, a.components, a.offset,
                     a.enable ? 1 : 0, a.integer ? 1 : 0);
}

static std::string EmitVertexDecl(const VertexLoaderCEmitter::VertexDecl& d)
{
  std::string out;
  out += fmt::format("    {{ {}, // stride\n", d.stride);
  out += fmt::format("      {}, // position\n", EmitAttrFormat(d.position));
  out += "      { ";
  for (int i = 0; i < 3; i++)
  {
    out += EmitAttrFormat(d.normals[i]);
    out += (i < 2) ? ", " : " }, // normals\n";
  }
  out += "      { ";
  for (int i = 0; i < 2; i++)
  {
    out += EmitAttrFormat(d.colors[i]);
    out += (i < 1) ? ", " : " }, // colors\n";
  }
  out += "      { ";
  for (int i = 0; i < 8; i++)
  {
    out += EmitAttrFormat(d.texcoords[i]);
    out += (i < 7) ? ",\n        " : " }, // texcoords\n";
  }
  out += fmt::format("      {} // posmtx\n", EmitAttrFormat(d.posmtx));
  out += "    }";
  return out;
}

// Reads only the vertex-format records of a .dpht trace file. Layout mirrors
// CfgCommand.cpp ReadTraceFile: header = magic, version, block_count, edge_count,
// smc_count (v2), + vtx_format_count (v3+), + snapshot_section_offset (v4+); then
// blocks (8 B each: addr, size), edges (12 B each: from, to, type, 3 pad), SMC
// records (16 B each: addr, length, u64 counter), then the vertex formats (20 B each:
// vtx_desc_low, vtx_desc_high, vat_g0, vat_g1, vat_g2). v2 traces carry no formats.
static bool ReadTraceVertexFormats(const std::string& path,
                                   std::vector<VertexLoaderCEmitter::FormatConfig>& out)
{
  File::IOFile file(path, "rb");
  if (!file.IsOpen())
  {
    fmt::println(std::cerr, "Error: Cannot open trace file: {}", path);
    return false;
  }

  u32 magic, version, block_count, edge_count, smc_count;
  if (!file.ReadBytes(&magic, 4) || !file.ReadBytes(&version, 4) ||
      !file.ReadBytes(&block_count, 4) || !file.ReadBytes(&edge_count, 4) ||
      !file.ReadBytes(&smc_count, 4))
  {
    fmt::println(std::cerr, "Error: Truncated trace header: {}", path);
    return false;
  }

  if (magic != 0x54485044 || version < 2 || version > 4)
  {
    fmt::println(std::cerr, "Error: Invalid trace file {} (magic={:#x}, version={})", path, magic,
                 version);
    return false;
  }

  u32 vtx_format_count = 0;
  if (version >= 3 && !file.ReadBytes(&vtx_format_count, 4))
    return false;
  u64 header_size = 20;
  if (version >= 3)
    header_size += 4;
  if (version >= 4)
    header_size += 4;  // snapshot_section_offset (unused here)

  const u64 formats_offset = header_size + u64(block_count) * 8 + u64(edge_count) * 12 +
                             u64(smc_count) * 16;
  if (formats_offset + u64(vtx_format_count) * 20 > file.GetSize() ||
      !file.Seek(static_cast<s64>(formats_offset), File::SeekOrigin::Begin))
  {
    fmt::println(std::cerr, "Error: Trace file {} too short for its vertex-format section", path);
    return false;
  }

  for (u32 i = 0; i < vtx_format_count; i++)
  {
    VertexLoaderCEmitter::FormatConfig cfg;
    u32 v[5];
    if (!file.ReadBytes(v, sizeof(v)))
      return false;
    cfg.vtx_desc_low = v[0];
    cfg.vtx_desc_high = v[1];
    cfg.vat_g0 = v[2];
    cfg.vat_g1 = v[3];
    cfg.vat_g2 = v[4];
    out.push_back(cfg);
  }
  return true;
}

int VtxAotCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: dolphin-tool vtxaot [options]");

  parser.add_option("-c", "--cfg").action("store").help("Path to CFG SQLite database");
  parser.add_option("-t", "--trace")
      .action("append")
      .help("Trace file (.dpht, v3+) whose recorded vertex formats are added (repeatable)");
  parser.add_option("-o", "--output").action("store").help("Path to output directory");
  parser.add_option("-p", "--prefix").action("store").help("Game ID prefix (e.g. GALE01)");

  const optparse::Values options = parser.parse_args(args);

  const std::vector<std::string> trace_paths =
      options.is_set("trace") ? std::vector<std::string>(options.all("trace").begin(),
                                                         options.all("trace").end()) :
                                std::vector<std::string>{};

  if ((!options.is_set("cfg") && trace_paths.empty()) || !options.is_set("output") ||
      !options.is_set("prefix"))
  {
    parser.print_help();
    return EXIT_FAILURE;
  }

  const std::string cfg_path = options.is_set("cfg") ? options["cfg"] : std::string{};
  const std::string output_dir = options["output"];
  const std::string prefix = options["prefix"];

  // Formats = union of the CFG vertex_formats table (if present) and every trace's
  // vertex-format records, in first-seen order.
  std::vector<VertexLoaderCEmitter::FormatConfig> formats;
  std::set<std::tuple<u32, u32, u32, u32, u32>> seen;
  const auto add_formats = [&](const std::vector<VertexLoaderCEmitter::FormatConfig>& src) {
    for (const auto& f : src)
    {
      if (seen.emplace(f.vtx_desc_low, f.vtx_desc_high, f.vat_g0, f.vat_g1, f.vat_g2).second)
        formats.push_back(f);
    }
  };

  if (!cfg_path.empty())
  {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(cfg_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
      fmt::println(std::cerr, "Error: Cannot open database: {}", cfg_path);
      sqlite3_close(db);
      return EXIT_FAILURE;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT vtx_desc_low, vtx_desc_high, vat_g0, vat_g1, vat_g2 "
                      "FROM vertex_formats";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
      sqlite3_close(db);
      if (trace_paths.empty())
      {
        fmt::println(std::cerr, "Error: vertex_formats table not found in {}", cfg_path);
        return EXIT_FAILURE;
      }
      // Multi-image CFG dbs (cfg --dol-images) carry no vertex_formats table; the
      // traces passed with --trace are then the only source.
      fmt::println(std::cerr,
                   "Warning: vertex_formats table not found in {} — using --trace files only",
                   cfg_path);
    }
    else
    {
      std::vector<VertexLoaderCEmitter::FormatConfig> cfg_formats;
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
        VertexLoaderCEmitter::FormatConfig cfg;
        cfg.vtx_desc_low = static_cast<uint32_t>(sqlite3_column_int64(stmt, 0));
        cfg.vtx_desc_high = static_cast<uint32_t>(sqlite3_column_int64(stmt, 1));
        cfg.vat_g0 = static_cast<uint32_t>(sqlite3_column_int64(stmt, 2));
        cfg.vat_g1 = static_cast<uint32_t>(sqlite3_column_int64(stmt, 3));
        cfg.vat_g2 = static_cast<uint32_t>(sqlite3_column_int64(stmt, 4));
        cfg_formats.push_back(cfg);
      }
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      fmt::println(std::cerr, "  cfg {}: {} vertex formats", cfg_path, cfg_formats.size());
      add_formats(cfg_formats);
    }
  }

  for (const std::string& trace_path : trace_paths)
  {
    std::vector<VertexLoaderCEmitter::FormatConfig> trace_formats;
    if (!ReadTraceVertexFormats(trace_path, trace_formats))
      return EXIT_FAILURE;
    fmt::println(std::cerr, "  trace {}: {} vertex formats", trace_path, trace_formats.size());
    add_formats(trace_formats);
  }
  fmt::println(std::cerr, "  union: {} vertex formats", formats.size());

  if (formats.empty())
  {
    fmt::println(std::cerr, "No vertex formats found. "
                 "Ensure trace collection recorded vertex formats (requires trace format v3+).");
    return EXIT_SUCCESS;
  }

  fmt::println(std::cerr, "Generating AOT vertex loaders for {} formats...", formats.size());

  // Create output directory
  File::CreateFullPath(output_dir + "/");

  // 1. Write runtime header
  {
    std::ofstream hdr(output_dir + "/vtx_aot_runtime.h");
    hdr << VTX_AOT_RUNTIME_HEADER;
  }

  // 2. Generate loader functions
  std::string loaders_code;
  loaders_code += "// Auto-generated vertex loader AOT functions\n";
  loaders_code += fmt::format("// Game: {} | {} formats\n\n", prefix, formats.size());
  loaders_code += "#include \"vtx_aot_runtime.h\"\n\n";

  struct LoaderInfo
  {
    std::string func_name;
    VertexLoaderCEmitter::VertexDecl decl;
    uint32_t vertex_size;
    uint32_t native_components;
    VertexLoaderCEmitter::FormatConfig config;
  };
  std::vector<LoaderInfo> infos;

  for (const auto& cfg : formats)
  {
    VertexLoaderCEmitter emitter(cfg);
    std::string func_name = MakeLoaderFuncName(prefix, cfg);

    loaders_code += emitter.GenerateLoaderFunction(func_name);
    loaders_code += "\n";

    LoaderInfo info;
    info.func_name = func_name;
    info.decl = emitter.GetVertexDecl();
    info.vertex_size = emitter.GetVertexSize();
    info.native_components = emitter.GetNativeComponents();
    info.config = cfg;
    infos.push_back(info);

    fmt::println(std::cerr, "  {} -> {} (src={} dst={} components={:#010x})",
                 func_name, "ok", info.vertex_size, info.decl.stride, info.native_components);
  }

  {
    std::ofstream f(output_dir + "/" + prefix + "_vtx_loaders.c");
    f << loaders_code;
  }

  // 3. Generate dispatch file with lookup table and constructor registration
  {
    std::string dispatch;
    dispatch += "// Auto-generated vertex loader AOT dispatch table\n";
    dispatch += fmt::format("// Game: {}\n\n", prefix);
    dispatch += "#include \"vtx_aot_runtime.h\"\n\n";

    // Forward declarations
    for (const auto& info : infos)
    {
      dispatch += fmt::format("extern int {}(const uint8_t*, uint8_t*, int,\n"
                              "    const uint8_t* const*, const uint32_t*, VtxLoaderState*);\n",
                              info.func_name);
    }
    dispatch += "\n";

    // Loader table
    dispatch += fmt::format("static const VtxAotLoaderEntry {}_vtx_table[] = {{\n", prefix);
    for (const auto& info : infos)
    {
      dispatch += fmt::format("  {{ {{ {:#010x}u, {:#010x}u, {:#010x}u, {:#010x}u, {:#010x}u }},\n",
                              info.config.vtx_desc_low, info.config.vtx_desc_high,
                              info.config.vat_g0, info.config.vat_g1, info.config.vat_g2);
      dispatch += fmt::format("    {},\n", info.func_name);
      dispatch += EmitVertexDecl(info.decl) + ",\n";
      dispatch += fmt::format("    {}, {:#010x}u }},\n", info.vertex_size, info.native_components);
    }
    dispatch += "};\n\n";

    dispatch += fmt::format(
        "static const int {}_vtx_table_size = sizeof({}_vtx_table) / sizeof({}_vtx_table[0]);\n\n",
        prefix, prefix, prefix);

    // Registration function (called before main via __attribute__((constructor)))
    dispatch += "extern void vtx_aot_register_loader(\n"
                "    const char* game_id,\n"
                "    uint32_t desc_low, uint32_t desc_high,\n"
                "    uint32_t g0, uint32_t g1, uint32_t g2,\n"
                "    VtxLoaderAOTFunc func,\n"
                "    const VtxPortableDecl* decl,\n"
                "    uint32_t vertex_size, uint32_t native_components);\n\n";

    dispatch += fmt::format(
        "__attribute__((constructor))\n"
        "static void {}_vtx_register(void) {{\n"
        "  for (int i = 0; i < {}_vtx_table_size; i++) {{\n"
        "    const VtxAotLoaderEntry* e = &{}_vtx_table[i];\n"
        "    vtx_aot_register_loader(\"{}\",\n"
        "        e->key[0], e->key[1], e->key[2], e->key[3], e->key[4],\n"
        "        e->func, &e->decl, e->vertex_size, e->native_components);\n"
        "  }}\n"
        "}}\n",
        prefix, prefix, prefix, prefix);

    std::ofstream f(output_dir + "/" + prefix + "_vtx_dispatch.c");
    f << dispatch;
  }

  // 4. Generate / append build script
  {
    std::string build_name = output_dir + "/build_vtx.sh";
    std::ofstream f(build_name);
    f << "#!/bin/bash\n";
    f << "# Auto-generated vertex loader AOT build script\n";
    f << "set -e\n\n";
    f << "CLANG=${CLANG:-clang}\n";
    f << "CFLAGS=\"-c -O2 -flto=thin -arch arm64 -I.\"\n\n";

    f << fmt::format("$CLANG $CFLAGS {}_vtx_loaders.c -o {}_vtx_loaders.o\n", prefix, prefix);
    f << fmt::format("$CLANG $CFLAGS {}_vtx_dispatch.c -o {}_vtx_dispatch.o\n", prefix, prefix);
    f << "\n";
    f << fmt::format("# Add to existing archive or create new one:\n");
    f << fmt::format("ar rcs lib{}_aot.a {}_vtx_loaders.o {}_vtx_dispatch.o\n",
                     prefix, prefix, prefix);
    f << fmt::format("echo \"Built lib{}_aot.a with {} vertex loaders\"\n",
                     prefix, formats.size());
  }

  fmt::println(std::cerr, "Generated {} vertex loaders -> {}/", formats.size(), output_dir);
  fmt::println(std::cerr, "  {}_vtx_loaders.c", prefix);
  fmt::println(std::cerr, "  {}_vtx_dispatch.c", prefix);
  fmt::println(std::cerr, "  vtx_aot_runtime.h");
  fmt::println(std::cerr, "  build_vtx.sh");

  return EXIT_SUCCESS;
}

}  // namespace DolphinTool
