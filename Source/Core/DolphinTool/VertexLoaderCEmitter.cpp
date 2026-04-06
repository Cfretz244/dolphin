// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/VertexLoaderCEmitter.h"

#include <fmt/format.h>

namespace DolphinTool
{

// VB_HAS_* flags (must match VideoCommon/NativeVertexFormat.h)
static constexpr uint32_t VB_HAS_POSMTXIDX = (1 << 1);
static constexpr uint32_t VB_HAS_TEXMTXIDX0 = (1 << 2);
static constexpr uint32_t VB_HAS_NORMAL = (1 << 10);
static constexpr uint32_t VB_HAS_TANGENT = (1 << 11);
static constexpr uint32_t VB_HAS_BINORMAL = (1 << 12);
static constexpr uint32_t VB_HAS_COL0 = (1 << 13);
static constexpr uint32_t VB_HAS_UV0 = (1 << 15);

// VertexComponentFormat enum values
static constexpr int VCF_NOT_PRESENT = 0;
static constexpr int VCF_DIRECT = 1;
static constexpr int VCF_INDEX8 = 2;
static constexpr int VCF_INDEX16 = 3;

// ComponentFormat enum values
static constexpr int CF_UBYTE = 0;
static constexpr int CF_BYTE = 1;
static constexpr int CF_USHORT = 2;
static constexpr int CF_SHORT = 3;
static constexpr int CF_FLOAT = 4;

// ColorFormat enum values
static constexpr int CLR_RGB565 = 0;
static constexpr int CLR_RGB888 = 1;
static constexpr int CLR_RGB888x = 2;
static constexpr int CLR_RGBA4444 = 3;
static constexpr int CLR_RGBA6666 = 4;
static constexpr int CLR_RGBA8888 = 5;

// ComponentFormat type for the output PortableVertexDeclaration
static constexpr int PVD_UBYTE = 0;
static constexpr int PVD_FLOAT = 4;

static bool IsIndexed(int vcf)
{
  return vcf == VCF_INDEX8 || vcf == VCF_INDEX16;
}

static int GetElementSize(int format)
{
  switch (format)
  {
  case CF_UBYTE:
  case CF_BYTE:
    return 1;
  case CF_USHORT:
  case CF_SHORT:
    return 2;
  default:
    return 4;
  }
}

static int GetColorSize(int color_format)
{
  switch (color_format)
  {
  case CLR_RGB565:
  case CLR_RGBA4444:
    return 2;
  case CLR_RGB888:
  case CLR_RGBA6666:
    return 3;
  case CLR_RGB888x:
  case CLR_RGBA8888:
    return 4;
  default:
    return 0;
  }
}

static int GetNormalScalingExponent(int format)
{
  switch (format)
  {
  case CF_UBYTE:
    return 7;
  case CF_BYTE:
    return 6;
  case CF_USHORT:
    return 15;
  case CF_SHORT:
    return 14;
  default:
    return 0;
  }
}

VertexLoaderCEmitter::VertexLoaderCEmitter(const FormatConfig& config) : m_config(config)
{
  ParseConfig();
  ComputeLayout();
}

void VertexLoaderCEmitter::ParseConfig()
{
  uint32_t lo = m_config.vtx_desc_low;
  uint32_t hi = m_config.vtx_desc_high;
  uint32_t g0 = m_config.vat_g0;
  uint32_t g1 = m_config.vat_g1;
  uint32_t g2 = m_config.vat_g2;

  m_pos_mat_idx = (lo >> 0) & 1;
  for (int i = 0; i < 8; i++)
    m_tex_mat_idx[i] = (lo >> (1 + i)) & 1;
  m_pos_vcf = (lo >> 9) & 3;
  m_nrm_vcf = (lo >> 11) & 3;
  m_col_vcf[0] = (lo >> 13) & 3;
  m_col_vcf[1] = (lo >> 15) & 3;

  for (int i = 0; i < 8; i++)
    m_tex_vcf[i] = (hi >> (i * 2)) & 3;

  m_pos_elements = ((g0 >> 0) & 1) == 0 ? 2 : 3;
  m_pos_format = (g0 >> 1) & 7;
  m_pos_frac = (g0 >> 4) & 0x1F;
  m_nrm_elements = (g0 >> 9) & 1;
  m_nrm_format = (g0 >> 10) & 7;
  m_col_format[0] = (g0 >> 14) & 7;
  m_col_format[1] = (g0 >> 18) & 7;
  m_byte_dequant = (g0 >> 30) & 1;
  m_normal_index3 = (g0 >> 31) & 1;

  m_tex_elements[0] = (g0 >> 21) & 1;
  m_tex_format[0] = (g0 >> 22) & 7;
  m_tex_frac[0] = (g0 >> 25) & 0x1F;

  m_tex_elements[1] = (g1 >> 0) & 1;
  m_tex_format[1] = (g1 >> 1) & 7;
  m_tex_frac[1] = (g1 >> 4) & 0x1F;
  m_tex_elements[2] = (g1 >> 9) & 1;
  m_tex_format[2] = (g1 >> 10) & 7;
  m_tex_frac[2] = (g1 >> 13) & 0x1F;
  m_tex_elements[3] = (g1 >> 18) & 1;
  m_tex_format[3] = (g1 >> 19) & 7;
  m_tex_frac[3] = (g1 >> 22) & 0x1F;
  m_tex_elements[4] = (g1 >> 27) & 1;
  m_tex_format[4] = (g1 >> 28) & 7;
  m_tex_frac[4] = ((g1 >> 31) & 1) | (((g2 >> 0) & 0xF) << 1);

  m_tex_elements[5] = (g2 >> 5) & 1;
  m_tex_format[5] = (g2 >> 6) & 7;
  m_tex_frac[5] = (g2 >> 9) & 0x1F;
  m_tex_elements[6] = (g2 >> 14) & 1;
  m_tex_format[6] = (g2 >> 15) & 7;
  m_tex_frac[6] = (g2 >> 18) & 0x1F;
  m_tex_elements[7] = (g2 >> 23) & 1;
  m_tex_format[7] = (g2 >> 24) & 7;
  m_tex_frac[7] = (g2 >> 27) & 0x1F;
}

void VertexLoaderCEmitter::ComputeLayout()
{
  if (m_pos_mat_idx)
    m_native_components |= VB_HAS_POSMTXIDX;
  for (int i = 0; i < 8; i++)
    if (m_tex_mat_idx[i])
      m_native_components |= VB_HAS_TEXMTXIDX0 << i;
  if (m_nrm_vcf != VCF_NOT_PRESENT)
  {
    m_native_components |= VB_HAS_NORMAL;
    if (m_nrm_elements == 1)
      m_native_components |= VB_HAS_TANGENT | VB_HAS_BINORMAL;
  }
  for (int i = 0; i < 2; i++)
    if (m_col_vcf[i] != VCF_NOT_PRESENT)
      m_native_components |= VB_HAS_COL0 << i;
  for (int i = 0; i < 8; i++)
    if (m_tex_vcf[i] != VCF_NOT_PRESENT)
      m_native_components |= VB_HAS_UV0 << i;

  // Compute input vertex size
  m_src_ofs = 0;
  if (m_pos_mat_idx)
    m_src_ofs += 1;
  for (int i = 0; i < 8; i++)
    if (m_tex_mat_idx[i])
      m_src_ofs += 1;

  if (m_pos_vcf == VCF_DIRECT)
    m_src_ofs += m_pos_elements * GetElementSize(m_pos_format);
  else if (m_pos_vcf == VCF_INDEX8)
    m_src_ofs += 1;
  else if (m_pos_vcf == VCF_INDEX16)
    m_src_ofs += 2;

  if (m_nrm_vcf == VCF_DIRECT)
  {
    int nrm_count = (m_nrm_elements == 1) ? 3 : 1;
    m_src_ofs += 3 * GetElementSize(m_nrm_format) * nrm_count;
  }
  else if (IsIndexed(m_nrm_vcf))
  {
    int idx_size = (m_nrm_vcf == VCF_INDEX8) ? 1 : 2;
    m_src_ofs += (m_nrm_elements == 1 && m_normal_index3) ? idx_size * 3 : idx_size;
  }

  for (int i = 0; i < 2; i++)
  {
    if (m_col_vcf[i] == VCF_DIRECT)
      m_src_ofs += GetColorSize(m_col_format[i]);
    else if (m_col_vcf[i] == VCF_INDEX8)
      m_src_ofs += 1;
    else if (m_col_vcf[i] == VCF_INDEX16)
      m_src_ofs += 2;
  }

  for (int i = 0; i < 8; i++)
  {
    int elems = (m_tex_elements[i] == 0) ? 1 : 2;
    if (m_tex_vcf[i] == VCF_DIRECT)
      m_src_ofs += elems * GetElementSize(m_tex_format[i]);
    else if (m_tex_vcf[i] == VCF_INDEX8)
      m_src_ofs += 1;
    else if (m_tex_vcf[i] == VCF_INDEX16)
      m_src_ofs += 2;
  }
}

// Emit C code to read one element from a data source and convert to float
static std::string EmitReadElement(int format, const std::string& ptr, int offset)
{
  switch (format)
  {
  case CF_UBYTE:
    return fmt::format("(float)({}[{}])", ptr, offset);
  case CF_BYTE:
    return fmt::format("(float)((int8_t){}[{}])", ptr, offset);
  case CF_USHORT:
    return fmt::format("(float)read_u16({} + {})", ptr, offset);
  case CF_SHORT:
    return fmt::format("(float)(int16_t)read_u16({} + {})", ptr, offset);
  default:  // Float (including InvalidFloat5-7)
    return fmt::format("read_float({} + {})", ptr, offset);
  }
}

// Emit C code that reads index from src, computes data pointer.
// Returns the variable name holding the data pointer and advances src_ofs_var.
static void EmitIndexRead(std::string& out, int vcf, int array_idx, bool is_position,
                          const std::string& src_ofs_var)
{
  if (vcf == VCF_INDEX8)
  {
    out += fmt::format("    uint32_t idx = src[{}];\n", src_ofs_var);
    out += fmt::format("    {} += 1;\n", src_ofs_var);
    if (is_position)
      out += "    vertex_skip = (idx == 0xFFu);\n";
    out += fmt::format("    const uint8_t* adata = arraybases[{}] + idx * strides[{}];\n",
                       array_idx, array_idx);
  }
  else  // VCF_INDEX16
  {
    out += fmt::format("    uint32_t idx = read_u16(src + {});\n", src_ofs_var);
    out += fmt::format("    {} += 2;\n", src_ofs_var);
    if (is_position)
      out += "    vertex_skip = (idx == 0xFFFFu);\n";
    out += fmt::format("    const uint8_t* adata = arraybases[{}] + idx * strides[{}];\n",
                       array_idx, array_idx);
  }
}

// Emit code to read N components, convert to float, apply scale, write to dst, update z-freeze
static void EmitComponentRead(std::string& out, int format, int count, bool dequant, int frac,
                              const std::string& data_ptr, int data_offset, int dst_offset,
                              const char* cache_name, const char* cache_cond)
{
  int elem_size = GetElementSize(format);
  for (int i = 0; i < count; i++)
  {
    std::string read = EmitReadElement(format, data_ptr, data_offset + i * elem_size);

    bool needs_scale = dequant && frac != 0 && format < CF_FLOAT;
    if (needs_scale)
      read = fmt::format("({} * scale_factors[{}])", read, frac);

    out += fmt::format("    {{ float v = {};\n", read);
    if (cache_name)
    {
      out += fmt::format("      if ({}) {}[{}] = v;\n", cache_cond, cache_name, i);
    }
    out += fmt::format("      write_float(dst + {}, v); }}\n", dst_offset + i * 4);
  }
}

std::string VertexLoaderCEmitter::GenerateLoaderFunction(const std::string& func_name) const
{
  m_dst_ofs = 0;
  m_decl = {};

  std::string out;

  out += fmt::format(
      "int {}(const uint8_t* src_start, uint8_t* dst_start, int count,\n"
      "    const uint8_t* const* arraybases, const uint32_t* strides,\n"
      "    VtxLoaderState* zf)\n"
      "{{\n",
      func_name);

  out += "  int skipped = 0;\n";
  out += "  const uint8_t* src = src_start;\n";
  out += "  uint8_t* dst = dst_start;\n";

  // Pre-compute texmatidx source offsets (relative to start of each vertex)
  uint32_t texmatidx_src_ofs[8] = {};
  {
    uint32_t cursor = 0;
    if (m_pos_mat_idx)
      cursor += 1;
    for (int i = 0; i < 8; i++)
    {
      if (m_tex_mat_idx[i])
      {
        texmatidx_src_ofs[i] = cursor;
        cursor++;
      }
    }
  }

  out += fmt::format("  for (int remaining = count - 1; remaining >= 0; remaining--) {{\n");
  out += "    int src_ofs = 0;\n";
  if (IsIndexed(m_pos_vcf))
    out += "    int vertex_skip = 0;\n";

  // Track current src_ofs and dst_ofs for baking offsets
  uint32_t cur_src = 0;
  uint32_t cur_dst = 0;

  // ---- Position Matrix Index ----
  if (m_pos_mat_idx)
  {
    out += "    // Position Matrix Index\n";
    out += fmt::format("    {{ uint32_t pmidx = src[{}] & 0x3Fu;\n", cur_src);
    out += fmt::format("      write_u32_le(dst + {}, pmidx);\n", cur_dst);
    out += "      if (remaining < 3) zf->position_matrix_index_cache[remaining] = pmidx;\n";
    out += "    }\n";

    m_decl.posmtx.components = 4;
    m_decl.posmtx.enable = true;
    m_decl.posmtx.offset = cur_dst;
    m_decl.posmtx.type = PVD_UBYTE;
    m_decl.posmtx.integer = true;
    cur_src += 1;
    cur_dst += 4;
  }

  // ---- Texture Matrix Indices (just skip in src, read later) ----
  for (int i = 0; i < 8; i++)
    if (m_tex_mat_idx[i])
      cur_src += 1;

  // For indexed components, we need a dynamic src_ofs tracker
  // since index reads consume variable amounts
  bool any_indexed = IsIndexed(m_pos_vcf) || IsIndexed(m_nrm_vcf);
  for (int i = 0; i < 2; i++)
    any_indexed |= IsIndexed(m_col_vcf[i]);
  for (int i = 0; i < 8; i++)
    any_indexed |= IsIndexed(m_tex_vcf[i]);

  if (any_indexed)
    out += fmt::format("    src_ofs = {};\n", cur_src);

  // ---- Position ----
  {
    out += "    // Position\n";
    int elem_size = GetElementSize(m_pos_format);

    if (IsIndexed(m_pos_vcf))
    {
      out += "    {\n";
      EmitIndexRead(out, m_pos_vcf, 0, true, "src_ofs");
      EmitComponentRead(out, m_pos_format, m_pos_elements, m_byte_dequant, m_pos_frac,
                        "adata", 0, cur_dst,
                        "zf->position_cache[remaining]", "remaining < 3 && !vertex_skip");
      out += "    }\n";
    }
    else
    {
      EmitComponentRead(out, m_pos_format, m_pos_elements, m_byte_dequant, m_pos_frac,
                        "src", cur_src, cur_dst,
                        "zf->position_cache[remaining]", "remaining < 3");
      cur_src += m_pos_elements * elem_size;
    }

    m_decl.position.components = m_pos_elements;
    m_decl.position.enable = true;
    m_decl.position.offset = cur_dst;
    m_decl.position.type = PVD_FLOAT;
    m_decl.position.integer = false;
    cur_dst += m_pos_elements * 4;
  }

  // ---- Normal ----
  if (m_nrm_vcf != VCF_NOT_PRESENT)
  {
    out += "    // Normal\n";
    int scaling_exp = GetNormalScalingExponent(m_nrm_format);
    int elem_size = GetElementSize(m_nrm_format);
    int load_bytes = elem_size * 3;

    auto emit_normal_group = [&](int nrm_idx, int data_byte_offset, const char* cache_name) {
      m_decl.normals[nrm_idx].components = 3;
      m_decl.normals[nrm_idx].enable = true;
      m_decl.normals[nrm_idx].offset = cur_dst;
      m_decl.normals[nrm_idx].type = PVD_FLOAT;
      m_decl.normals[nrm_idx].integer = false;

      const char* cond = "remaining == 0";
      if (IsIndexed(m_nrm_vcf))
      {
        EmitComponentRead(out, m_nrm_format, 3, true, scaling_exp,
                          "adata", data_byte_offset, cur_dst,
                          fmt::format("zf->{}", cache_name).c_str(), cond);
      }
      else
      {
        EmitComponentRead(out, m_nrm_format, 3, true, scaling_exp,
                          "src", cur_src + data_byte_offset, cur_dst,
                          fmt::format("zf->{}", cache_name).c_str(), cond);
      }
      cur_dst += 3 * 4;
    };

    if (IsIndexed(m_nrm_vcf))
    {
      out += "    {\n";
      EmitIndexRead(out, m_nrm_vcf, 1, false, "src_ofs");
      emit_normal_group(0, 0, "normal_cache");

      if (m_nrm_elements == 1)  // NTB
      {
        if (m_normal_index3)
        {
          out += "    }\n    {\n";
          EmitIndexRead(out, m_nrm_vcf, 1, false, "src_ofs");
        }
        emit_normal_group(1, load_bytes, "tangent_cache");

        if (m_normal_index3)
        {
          out += "    }\n    {\n";
          EmitIndexRead(out, m_nrm_vcf, 1, false, "src_ofs");
        }
        emit_normal_group(2, load_bytes * 2, "binormal_cache");
      }
      out += "    }\n";
    }
    else
    {
      emit_normal_group(0, 0, "normal_cache");
      if (m_nrm_elements == 1)
      {
        emit_normal_group(1, load_bytes, "tangent_cache");
        emit_normal_group(2, load_bytes * 2, "binormal_cache");
      }
      int direct_bytes = load_bytes * (m_nrm_elements == 1 ? 3 : 1);
      cur_src += direct_bytes;
    }
  }

  // ---- Colors ----
  for (int i = 0; i < 2; i++)
  {
    m_decl.colors[i].components = 4;
    m_decl.colors[i].type = PVD_UBYTE;
    m_decl.colors[i].integer = false;

    if (m_col_vcf[i] != VCF_NOT_PRESENT)
    {
      out += fmt::format("    // Color{}\n", i);
      int color_array = 2 + i;

      m_decl.colors[i].enable = true;
      m_decl.colors[i].offset = cur_dst;

      auto emit_color = [&](const std::string& data_ptr, int off) {
        switch (m_col_format[i])
        {
        case CLR_RGB888:
        case CLR_RGB888x:
          out += fmt::format("    {{ uint32_t c; memcpy(&c, {} + {}, 4);\n", data_ptr, off);
          out += "      c |= 0xFF000000u;\n";
          out += fmt::format("      memcpy(dst + {}, &c, 4); }}\n", cur_dst);
          break;
        case CLR_RGBA8888:
          out += fmt::format("    memcpy(dst + {}, {} + {}, 4);\n", cur_dst, data_ptr, off);
          break;
        case CLR_RGB565:
          out += fmt::format("    {{ uint16_t raw = read_u16({} + {});\n", data_ptr, off);
          out += "      uint32_t col = (raw >> 8) & 0xF8u;\n";
          out += "      col |= (raw << 5) & 0x00FC00u;\n";
          out += "      col |= (raw << 19) & 0xF80000u;\n";
          out += "      col |= (col >> 5) & 0x070007u;\n";
          out += "      col |= (col >> 6) & 0x000300u;\n";
          out += "      col |= 0xFF000000u;\n";
          out += fmt::format("      memcpy(dst + {}, &col, 4); }}\n", cur_dst);
          break;
        case CLR_RGBA4444:
          out += fmt::format("    {{ uint16_t raw; memcpy(&raw, {} + {}, 2);\n", data_ptr, off);
          out += "      uint32_t val = raw;\n";
          out += "      uint32_t col = val & 0x00F0u;\n";
          out += "      col |= (val & 0x000Fu) << 12;\n";
          out += "      col |= (val & 0xF000u) << 8;\n";
          out += "      col |= (val & 0x0F00u) << 20;\n";
          out += "      col |= col >> 4;\n";
          out += fmt::format("      memcpy(dst + {}, &col, 4); }}\n", cur_dst);
          break;
        case CLR_RGBA6666:
          out += fmt::format(
              "    {{ const uint8_t* cp = {} + {};\n", data_ptr, off);
          out += "      uint32_t val = ((uint32_t)cp[0] << 16) | ((uint32_t)cp[1] << 8) | cp[2];\n";
          out += "      uint32_t col = (val >> 16) & 0xFCu;\n";
          out += "      col |= (val >> 2) & 0x0000FC00u;\n";
          out += "      col |= (val << 12) & 0x00FC0000u;\n";
          out += "      col |= (val << 26) & 0xFC000000u;\n";
          out += "      col |= (col >> 6) & 0x03030303u;\n";
          out += fmt::format("      memcpy(dst + {}, &col, 4); }}\n", cur_dst);
          break;
        }
      };

      if (IsIndexed(m_col_vcf[i]))
      {
        out += "    {\n";
        EmitIndexRead(out, m_col_vcf[i], color_array, false, "src_ofs");
        emit_color("adata", 0);
        out += "    }\n";
      }
      else
      {
        emit_color("src", cur_src);
        cur_src += GetColorSize(m_col_format[i]);
      }

      cur_dst += 4;
    }
  }

  // ---- Texture Coordinates ----
  for (int i = 0; i < 8; i++)
  {
    m_decl.texcoords[i].offset = cur_dst;
    m_decl.texcoords[i].type = PVD_FLOAT;
    m_decl.texcoords[i].integer = false;

    int tc_elements = (m_tex_elements[i] == 0) ? 1 : 2;

    if (m_tex_vcf[i] != VCF_NOT_PRESENT)
    {
      out += fmt::format("    // TexCoord{}\n", i);
      int tex_array = 4 + i;
      int elem_size = GetElementSize(m_tex_format[i]);

      // The output count: if texmatidx is set, always write at least 2 components
      int out_elements = m_tex_mat_idx[i] ? 2 : tc_elements;

      if (IsIndexed(m_tex_vcf[i]))
      {
        out += "    {\n";
        EmitIndexRead(out, m_tex_vcf[i], tex_array, false, "src_ofs");
        EmitComponentRead(out, m_tex_format[i], tc_elements, m_byte_dequant, m_tex_frac[i],
                          "adata", 0, cur_dst,
                          nullptr, nullptr);
        // If out_elements > tc_elements, pad with zero
        if (out_elements > tc_elements)
        {
          out += fmt::format("    write_float(dst + {}, 0.0f);\n", cur_dst + tc_elements * 4);
        }
        out += "    }\n";
      }
      else
      {
        EmitComponentRead(out, m_tex_format[i], tc_elements, m_byte_dequant, m_tex_frac[i],
                          "src", cur_src, cur_dst,
                          nullptr, nullptr);
        if (out_elements > tc_elements)
        {
          out += fmt::format("    write_float(dst + {}, 0.0f);\n", cur_dst + tc_elements * 4);
        }
        cur_src += tc_elements * elem_size;
      }

      m_decl.texcoords[i].components = out_elements;
      m_decl.texcoords[i].enable = true;
      cur_dst += out_elements * 4;
    }

    if (m_tex_mat_idx[i])
    {
      m_decl.texcoords[i].components = 3;
      m_decl.texcoords[i].enable = true;

      out += fmt::format("    // TexMatIdx{}\n", i);

      if (m_tex_vcf[i] != VCF_NOT_PRESENT)
      {
        // Append matrix index as 3rd float after the texcoord
        out += fmt::format("    write_float(dst + {}, (float)src[{}]);\n",
                           cur_dst, texmatidx_src_ofs[i]);
        cur_dst += 4;
      }
      else
      {
        // No texcoord present: write [0, 0, matidx]
        m_decl.texcoords[i].offset = cur_dst;
        out += fmt::format("    write_float(dst + {}, 0.0f);\n", cur_dst);
        out += fmt::format("    write_float(dst + {}, 0.0f);\n", cur_dst + 4);
        out += fmt::format("    write_float(dst + {}, (float)src[{}]);\n",
                           cur_dst + 8, texmatidx_src_ofs[i]);
        cur_dst += 12;
      }
    }
  }

  m_dst_ofs = cur_dst;

  // ---- Advance pointers ----
  out += fmt::format("    src += {};\n", m_src_ofs);
  if (IsIndexed(m_pos_vcf))
  {
    out += "    if (vertex_skip) { skipped++; }\n";
    out += fmt::format("    else {{ dst += {}; }}\n", m_dst_ofs);
  }
  else
  {
    out += fmt::format("    dst += {};\n", m_dst_ofs);
  }

  out += "  }\n";
  out += "  return count - skipped;\n";
  out += "}\n";

  m_decl.stride = m_dst_ofs;
  return out;
}

}  // namespace DolphinTool
