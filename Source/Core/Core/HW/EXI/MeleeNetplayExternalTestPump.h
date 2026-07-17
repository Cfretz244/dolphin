// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Test-only stand-in for the APP side of the external netplay transport
// (MeleeNetplayExternalTransport.h): pumps bytes between the process-global
// link registry and plain TCP sockets, so the full two-instance Mac harness
// can run THROUGH the external-transport code path with zero Apple
// dependencies. This rehearses exactly what the iOS pairing layer does --
// attach links, push inbound bytes, forward the send callback -- before any
// of it touches a phone (S2 in research/adhoc-pairing-design.md).
//
// Enabled by Config MeleeNetplay.ExternalTestTcp when Transport=1:
//   host:   "listen:<port>"  -- accept `links` connections, attach in order
//   client: "<host>:<port>"  -- connect once, attach
// Never set this in production; the app owns the links there.

#pragma once

#include <string>

#include "Common/CommonTypes.h"

namespace ExpansionInterface::MeleeNetplayExternalTestPump
{
void Start(bool is_host, const std::string& spec, u32 links);
void Stop();  // idempotent; also resets the external link registry
}  // namespace ExpansionInterface::MeleeNetplayExternalTestPump
