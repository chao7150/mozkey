// Copyright 2026 Mozkey contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOZC_RENDERER_QT_QT_SCREEN_COORDINATES_H_
#define MOZC_RENDERER_QT_QT_SCREEN_COORDINATES_H_

#include <string>

#include "absl/types/span.h"
#include "base/coordinates.h"
#include "protocol/renderer_command.pb.h"

namespace mozc::renderer {

// Snapshot of QScreen geometry metadata in Qt virtual coordinates.
struct QtScreenGeometry {
  std::string name;
  Rect geometry;
  Rect available_geometry;
  double device_pixel_ratio = 1.0;
};

// Translated preedit rectangle and owning monitor working area in Qt virtual
// coordinates.
struct ResolvedScreenRect {
  Rect preedit_rect;
  Rect monitor_rect;
  int screen_index = -1;
};

// Resolved monitor working area and index in the `screens` span.
struct ResolvedMonitorRect {
  Rect monitor_rect;
  int screen_index = -1;
};

// Translates `command.preedit_rectangle()` into Qt virtual coordinates and
// returns the working area (`available_geometry`) of its owning monitor.
ResolvedScreenRect ResolvePreeditAndMonitorRect(
    const commands::RendererCommand& command,
    absl::Span<const QtScreenGeometry> screens, int primary_screen_index);

// Resolves the working area and screen index for positioning `infolist_` next
// to `candidate_window_rect`.
ResolvedMonitorRect ResolveInfolistMonitor(
    const commands::RendererCommand& command, const Rect& candidate_window_rect,
    absl::Span<const QtScreenGeometry> screens, int primary_screen_index);

// Resolves the working area of the monitor for positioning `infolist_` next
// to `candidate_window_rect`.
Rect ResolveInfolistMonitorRect(const commands::RendererCommand& command,
                                const Rect& candidate_window_rect,
                                absl::Span<const QtScreenGeometry> screens,
                                int primary_screen_index);

// Resolves the working area containing `(x, y)` in Qt virtual coordinates,
// falling back to the nearest monitor (or primary on ties) when outside.
Rect ResolveMonitorRectForVirtualPoint(
    const Point& point, absl::Span<const QtScreenGeometry> screens,
    int primary_screen_index);

}  // namespace mozc::renderer

#endif  // MOZC_RENDERER_QT_QT_SCREEN_COORDINATES_H_
