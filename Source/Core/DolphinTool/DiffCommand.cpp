// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// dolphin-tool diff — Block-level comparison of AOT-translated code vs. interpreter.
// Boots the emulator headlessly with AOT core (CPUCore=6) and diff mode enabled.
// AOTCore::Run() detects the diff config and runs the comparison loop.

#include "DolphinTool/DiffCommand.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <OptionParser.h>
#include <fmt/format.h>
#include <fmt/ostream.h>

#include "Common/ScopeGuard.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "UICommon/UICommon.h"

namespace DolphinTool
{

int DiffCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser = optparse::OptionParser().description(
      "Compare AOT-translated PPC blocks against the interpreter, block by block.");

  parser.add_option("--iso").action("store").metavar("<path>").help("Path to game ISO (required)");

  parser.add_option("--cfg")
      .action("store")
      .metavar("<path>")
      .help("Path to CFG SQLite database for block boundaries (required)");

  parser.add_option("--max-blocks")
      .action("store")
      .type("int")
      .set_default("0")
      .metavar("<N>")
      .help("Stop after N block comparisons (0=unlimited)");

  parser.add_option("--max-divergences")
      .action("store")
      .type("int")
      .set_default("1")
      .metavar("<N>")
      .help("Stop after N divergences (default: 1)");

  parser.add_option("--log")
      .action("store")
      .metavar("<path>")
      .help("Divergence log output file (default: stdout)");

  parser.add_option("--filter-min")
      .action("store")
      .set_default("0")
      .metavar("<addr>")
      .help("Only compare blocks with PC >= addr (hex)");

  parser.add_option("--filter-max")
      .action("store")
      .set_default("0xFFFFFFFF")
      .metavar("<addr>")
      .help("Only compare blocks with PC <= addr (hex)");

  parser.add_option("--self-diff")
      .action("store_true")
      .set_default("0")
      .help("Interpreter-vs-interpreter mode (no AOT, for non-determinism baseline)");

  parser.add_option("--compare-ram")
      .action("store_true")
      .set_default("0")
      .help("Also diff RAM contents after each block (3 copies of 24MB)");

  const optparse::Values options = parser.parse_args(args);

  const std::string iso_path = options.get("iso");
  const std::string cfg_path = options.get("cfg");

  if (iso_path.empty())
  {
    fmt::println(std::cerr, "Error: --iso is required");
    parser.print_help();
    return EXIT_FAILURE;
  }
  if (cfg_path.empty())
  {
    fmt::println(std::cerr, "Error: --cfg is required");
    parser.print_help();
    return EXIT_FAILURE;
  }

  // Parse hex addresses
  const u32 filter_min =
      static_cast<u32>(std::stoul(options.get("filter_min"), nullptr, 0));
  const u32 filter_max =
      static_cast<u32>(std::stoul(options.get("filter_max"), nullptr, 0));

  // Initialize UICommon (config system, video backend)
  UICommon::Init();

  // Headless WSI — no window, null video backend
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Headless;
  wsi.display_connection = nullptr;
  wsi.render_window = nullptr;
  wsi.render_surface = nullptr;

  UICommon::InitControllers(wsi);

  Common::ScopeGuard ui_guard([] {
    UICommon::ShutdownControllers();
    UICommon::Shutdown();
  });

  // Set config for AOT diff mode
  Config::SetBaseOrCurrent(Config::MAIN_CPU_CORE, PowerPC::CPUCore::AOT);
  Config::SetBaseOrCurrent(Config::MAIN_GFX_BACKEND, std::string("Null"));
  Config::SetBaseOrCurrent(Config::MAIN_DEBUG_AOT_DIFF_MODE, true);
  Config::SetBaseOrCurrent(Config::MAIN_DEBUG_AOT_SELF_DIFF,
                           static_cast<bool>(options.get("self_diff")));
  Config::SetBaseOrCurrent(Config::MAIN_DEBUG_AOT_COMPARE_RAM,
                           static_cast<bool>(options.get("compare_ram")));
  Config::SetBaseOrCurrent(Config::MAIN_DEBUG_AOT_CFG_DB_PATH, cfg_path);
  Config::SetBaseOrCurrent(Config::MAIN_DEBUG_AOT_DIFF_MAX_BLOCKS,
                           static_cast<u32>(std::stoul(options.get("max_blocks"))));
  Config::SetBaseOrCurrent(Config::MAIN_DEBUG_AOT_DIFF_MAX_DIVERGENCES,
                           static_cast<u32>(std::stoul(options.get("max_divergences"))));
  Config::SetBaseOrCurrent(Config::MAIN_DEBUG_AOT_DIFF_FILTER_MIN, filter_min);
  Config::SetBaseOrCurrent(Config::MAIN_DEBUG_AOT_DIFF_FILTER_MAX, filter_max);

  const std::string log_path = options.get("log");
  if (!log_path.empty())
    Config::SetBaseOrCurrent(Config::MAIN_DEBUG_AOT_DIFF_LOG_PATH, log_path);

  // Create boot parameters
  auto boot = BootParameters::GenerateFromFile(iso_path);
  if (!boot)
  {
    fmt::println(std::cerr, "Error: Failed to create boot parameters from: {}", iso_path);
    return EXIT_FAILURE;
  }

  auto& system = Core::System::GetInstance();

  // Register callback to detect when emulation stops
  auto state_hook = Core::AddOnStateChangedCallback([](const Core::State state) {
    // Nothing needed — we poll in the main loop below
  });

  fmt::println(std::cerr, "AOT Diff: Booting {}...", iso_path);
  fmt::println(std::cerr, "AOT Diff: CFG database: {}", cfg_path);
  fmt::println(std::cerr, "AOT Diff: Max blocks: {}, Max divergences: {}",
               options.get("max_blocks"), options.get("max_divergences"));
  if (options.get("self_diff"))
    fmt::println(std::cerr, "AOT Diff: Self-diff mode (interpreter vs interpreter)");

  if (!BootManager::BootCore(system, std::move(boot), wsi))
  {
    fmt::println(std::cerr, "Error: Failed to boot emulator");
    return EXIT_FAILURE;
  }

  // Main loop — dispatch host jobs until emulation stops.
  // AOTCore::RunDiff() will call cpu.Break() when comparison is done.
  while (Core::IsRunningOrStarting(system))
  {
    Core::HostDispatchJobs(system);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  Core::Stop(system);
  Core::Shutdown(system);

  fmt::println(std::cerr, "AOT Diff: Done.");
  return EXIT_SUCCESS;
}

}  // namespace DolphinTool
