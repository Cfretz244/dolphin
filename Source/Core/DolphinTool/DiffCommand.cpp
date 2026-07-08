// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// dolphin-tool diff — Block-level comparison of AOT-translated code vs. interpreter.
// Boots the emulator headlessly with AOT core (CPUCore=6) and diff mode enabled.
// AotHarness (via AOTCore::Run) detects the diff config and runs the comparison loop.

#include "DolphinTool/DiffCommand.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <OptionParser.h>
#include <fmt/format.h>
#include <fmt/ostream.h>

#include <unordered_map>

#include "Common/CommonTypes.h"
#include "Common/ScopeGuard.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/PowerPC/AOT/AotHarness.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "UICommon/UICommon.h"

namespace DolphinTool
{
#ifdef DOLPHIN_AOT_HARNESS


static std::atomic<bool> s_diff_shutdown_requested{false};

static void DiffSignalHandler(int)
{
  s_diff_shutdown_requested.store(true);
  AotHarness::RequestShutdown();
}

int DiffCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser = optparse::OptionParser().description(
      "Compare AOT-translated PPC blocks against the interpreter, block by block.");

  parser.add_option("--iso").action("store").metavar("<path>").help("Path to game ISO (required)");

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

  parser.add_option("--savestate")
      .action("store")
      .metavar("<path>")
      .help("Load a savestate after boot before starting comparison");

  const optparse::Values options = parser.parse_args(args);

  const std::string iso_path = options["iso"];

  if (iso_path.empty())
  {
    fmt::println(std::cerr, "Error: --iso is required");
    parser.print_help();
    return EXIT_FAILURE;
  }

  // Parse hex addresses
  const std::string filter_min_str = options["filter_min"];
  const std::string filter_max_str = options["filter_max"];
  const u32 filter_min = static_cast<u32>(std::stoul(filter_min_str, nullptr, 0));
  const u32 filter_max = static_cast<u32>(std::stoul(filter_max_str, nullptr, 0));
  const u32 max_blocks_val = static_cast<u32>(std::stoul(std::string(options["max_blocks"])));
  const u32 max_div_val = static_cast<u32>(std::stoul(std::string(options["max_divergences"])));
  const bool self_diff = static_cast<int>(options.get("self_diff")) != 0;

  // Initialize UICommon (config system, video backend)
  UICommon::SetUserDirectory("");  // Use default user directory
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
  Config::SetCurrent(Config::MAIN_CPU_CORE, PowerPC::CPUCore::AOT);
  Config::SetCurrent(Config::MAIN_GFX_BACKEND, std::string("Null"));
  Config::SetCurrent(Config::MAIN_DEBUG_AOT_DIFF_MODE, true);
  Config::SetCurrent(Config::MAIN_DEBUG_AOT_SELF_DIFF, self_diff);
  Config::SetCurrent(Config::MAIN_DEBUG_AOT_COMPARE_RAM,
                           static_cast<int>(options.get("compare_ram")) != 0);
  Config::SetCurrent(Config::MAIN_DEBUG_AOT_DIFF_MAX_BLOCKS, max_blocks_val);
  Config::SetCurrent(Config::MAIN_DEBUG_AOT_DIFF_MAX_DIVERGENCES, max_div_val);
  Config::SetCurrent(Config::MAIN_DEBUG_AOT_DIFF_FILTER_MIN, filter_min);
  Config::SetCurrent(Config::MAIN_DEBUG_AOT_DIFF_FILTER_MAX, filter_max);

  const std::string log_path = options["log"];
  if (!log_path.empty())
    Config::SetCurrent(Config::MAIN_DEBUG_AOT_DIFF_LOG_PATH, log_path);

  const std::string savestate_path = options["savestate"];
  if (!savestate_path.empty())
    Config::SetCurrent(Config::MAIN_DEBUG_AOT_DIFF_SAVESTATE_PATH, savestate_path);

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
  fmt::println(std::cerr, "AOT Diff: Max blocks: {}, Max divergences: {}", max_blocks_val,
               max_div_val);
  if (self_diff)
    fmt::println(std::cerr, "AOT Diff: Self-diff mode (interpreter vs interpreter)");

  // Install signal handler so Ctrl+C works
  std::signal(SIGINT, DiffSignalHandler);
  std::signal(SIGTERM, DiffSignalHandler);

  if (!BootManager::BootCore(system, std::move(boot), wsi))
  {
    fmt::println(std::cerr, "Error: Failed to boot emulator");
    return EXIT_FAILURE;
  }

  // Main loop — dispatch host jobs until emulation stops.
  // AotHarness::RunDiff() will call cpu.Break() when comparison is done.
  while (Core::IsRunningOrStarting(system))
  {
    Core::HostDispatchJobs(system);
    if (s_diff_shutdown_requested.load())
    {
      fmt::println(std::cerr, "\nAOT Diff: Interrupted by signal, shutting down...");
      Core::Stop(system);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  Core::Stop(system);
  Core::Shutdown(system);

  fmt::println(std::cerr, "AOT Diff: Done.");
  return EXIT_SUCCESS;
}

#else  // !DOLPHIN_AOT_HARNESS

int DiffCommand(const std::vector<std::string>&)
{
  std::cerr << "dolphin-tool diff requires a build with the AOT harness "
               "(configure with AOT libraries on a desktop platform).\n";
  return EXIT_FAILURE;
}

#endif  // DOLPHIN_AOT_HARNESS

}  // namespace DolphinTool
