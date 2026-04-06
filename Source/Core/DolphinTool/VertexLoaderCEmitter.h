// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace DolphinTool
{

// Generates C source code for a vertex loader function that converts GC/Wii
// vertex data (big-endian, mixed formats) to the GPU-friendly output format.
// Each generated function is specialized for a specific vertex format
// configuration defined by the 5 register values (TVtxDesc + VAT).
class VertexLoaderCEmitter
{
public:
  struct FormatConfig
  {
    uint32_t vtx_desc_low;
    uint32_t vtx_desc_high;
    uint32_t vat_g0;
    uint32_t vat_g1;
    uint32_t vat_g2;
  };

  // Output vertex declaration (matches Dolphin's PortableVertexDeclaration)
  struct AttrFormat
  {
    int type;        // ComponentFormat enum value
    int components;
    int offset;
    bool enable;
    bool integer;
  };

  struct VertexDecl
  {
    int stride;
    AttrFormat position;
    AttrFormat normals[3];
    AttrFormat colors[2];
    AttrFormat texcoords[8];
    AttrFormat posmtx;
  };

  explicit VertexLoaderCEmitter(const FormatConfig& config);

  // Generate the C function body. Returns the complete function definition.
  std::string GenerateLoaderFunction(const std::string& func_name) const;

  // Get the computed output vertex declaration.
  const VertexDecl& GetVertexDecl() const { return m_decl; }

  // Get the input vertex size in bytes.
  uint32_t GetVertexSize() const { return m_src_ofs; }

  // Get the VB_HAS_* component bitmask.
  uint32_t GetNativeComponents() const { return m_native_components; }

private:
  // Helper methods that emit C code fragments
  void EmitReadVertex(std::string& out, int vcf, int format, int count_in, int count_out,
                      bool dequantize, int scaling_exponent, const char* cache_name,
                      int cache_cond, AttrFormat* attr, const std::string& data_reg,
                      int data_offset) const;

  void EmitGetVertexAddr(std::string& out, int array_index, int vcf,
                         std::string& out_data_var, int& out_offset) const;

  void EmitReadColor(std::string& out, int vcf, int color_format,
                     const std::string& data_reg, int data_offset) const;

  // Parse register bitfields
  void ParseConfig();

  // Compute vertex size (input) and native components
  void ComputeLayout();

  FormatConfig m_config;

  // Parsed register fields
  bool m_pos_mat_idx;
  std::array<bool, 8> m_tex_mat_idx;
  int m_pos_vcf;   // VertexComponentFormat for position
  int m_nrm_vcf;   // VertexComponentFormat for normal
  std::array<int, 2> m_col_vcf;  // VertexComponentFormat for colors
  std::array<int, 8> m_tex_vcf;  // VertexComponentFormat for texcoords

  int m_pos_format;  // ComponentFormat
  int m_pos_elements;  // 2 or 3
  int m_pos_frac;
  bool m_byte_dequant;
  bool m_normal_index3;

  int m_nrm_format;
  int m_nrm_elements;  // NormalComponentCount: 0=N, 1=NTB

  std::array<int, 2> m_col_format;  // ColorFormat for each color
  std::array<int, 8> m_tex_format;
  std::array<int, 8> m_tex_elements;  // 0=S, 1=ST
  std::array<int, 8> m_tex_frac;

  // Layout tracking
  mutable uint32_t m_src_ofs = 0;
  mutable uint32_t m_dst_ofs = 0;
  mutable VertexDecl m_decl{};
  uint32_t m_native_components = 0;
};

}  // namespace DolphinTool
