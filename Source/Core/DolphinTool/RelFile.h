// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace DolphinTool
{
// Parser for GameCube .rel relocatable modules (the OSModule/OSLink format used
// by games like Wind Waker to load actor and scene code at runtime). Unlike the
// RSO debugger views in Core/Debugger/RSO.h (which read live emulated memory and
// model the named-symbol RSO variant), this parses the on-disc file image: REL
// relocations carry no symbol names, only (target module id, section, addend).

// ELF PowerPC relocation types appearing in REL files, plus Dolphin SDK markers.
enum RelRelocType : u8
{
  R_PPC_NONE = 0,
  R_PPC_ADDR32 = 1,
  R_PPC_ADDR24 = 2,
  R_PPC_ADDR16 = 3,
  R_PPC_ADDR16_LO = 4,
  R_PPC_ADDR16_HI = 5,
  R_PPC_ADDR16_HA = 6,
  R_PPC_ADDR14 = 7,
  R_PPC_ADDR14_BRTAKEN = 8,
  R_PPC_ADDR14_BRNTAKEN = 9,
  R_PPC_REL24 = 10,
  R_PPC_REL14 = 11,
  R_PPC_REL14_BRTAKEN = 12,
  R_PPC_REL14_BRNTAKEN = 13,
  R_DOLPHIN_NOP = 201,      // carries an offset delta only
  R_DOLPHIN_SECTION = 202,  // switches the patch-site section, resets position
  R_DOLPHIN_END = 203,      // terminates one imp entry's stream
  R_DOLPHIN_MRKREF = 204,
};

struct RelSection
{
  u32 file_offset;  // masked of flag bits; 0 => bss (no file content)
  u32 size;
  bool executable;
  std::vector<u8> data;  // empty for bss

  bool IsBss() const { return file_offset == 0 && size > 0; }
};

// One decoded relocation: patch the word at (site_section, site_offset) of this
// module so it refers to target_module's target_section at addend. For
// target_module 0 (the main DOL), addend is an absolute address and
// target_section is meaningless.
struct RelReloc
{
  u8 site_section;
  u32 site_offset;
  u8 type;
  u32 target_module;
  u8 target_section;
  u32 addend;
};

struct RelFile
{
  u32 module_id;
  u32 version;
  std::string name;  // filename, set by the caller (REL names live in framework.str)
  u32 bss_size;
  u8 prolog_section, epilog_section, unresolved_section;
  u32 prolog_offset, epilog_offset, unresolved_offset;
  std::vector<RelSection> sections;
  std::vector<RelReloc> relocs;

  // Parses a decompressed .rel image. Returns nullopt (with stderr diagnostics
  // mentioning `name`) on malformed input.
  static std::optional<RelFile> Parse(const u8* data, size_t size, const std::string& name);
};
}  // namespace DolphinTool
