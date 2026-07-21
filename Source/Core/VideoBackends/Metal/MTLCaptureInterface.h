// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// C interface for the zero-copy offscreen frame capture used by embedding
// frontends (the Delta iOS app). Implemented in MTLGfx.mm; kept plain C so an
// ObjC++/ARC bridge can include it without pulling in Dolphin C++ headers.
//
// The frontend supplies a ring of IOSurfaces matching the render surface
// (CAMetalLayer drawable) in size and format (BGRA8). Each present, the video
// backend blits the drawable into the next ring slot on the GPU and publishes
// the slot index from the command buffer's completion handler — the frame
// never touches the CPU. The frontend takes the latest published slot and
// displays it directly (e.g. via CIImage(ioSurface:)).

#include <IOSurface/IOSurfaceRef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Register the capture ring; enables capture (the backend stops presenting
// on-screen) until cleared with (NULL, 0). The surfaces are CFRetained until
// replaced or cleared. Use >= 3 surfaces: the consumer may still be displaying
// slot N while the GPU writes slot N+1 — with 3, a slot is only rewritten two
// presents after it was published.
void Dolphin_SetFrameCaptureSurfaces(const IOSurfaceRef* surfaces, int count);

// Return the index of the most recently completed frame's ring slot and
// consume it (each frame is reported at most once), or -1 if no new frame
// completed since the last take. Safe to call from any thread.
int Dolphin_TakeLatestFrameSurface(void);

#ifdef __cplusplus
}
#endif
