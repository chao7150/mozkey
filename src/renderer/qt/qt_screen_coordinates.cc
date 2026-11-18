// Copyright 2026 Mozkey contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "renderer/qt/qt_screen_coordinates.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

#include "absl/types/span.h"
#include "base/coordinates.h"
#include "protocol/renderer_command.pb.h"

namespace mozc::renderer {
namespace {

bool IsValidScreen(const QtScreenGeometry& screen) {
  return screen.geometry.Width() > 0 && screen.geometry.Height() > 0;
}

Rect EffectiveAvailable(const QtScreenGeometry& screen) {
  if (screen.available_geometry.Width() > 0 &&
      screen.available_geometry.Height() > 0) {
    return screen.available_geometry;
  }
  return screen.geometry;
}

Rect FromProtoRect(const commands::RendererCommand::Rectangle& rect) {
  return Rect(rect.left(), rect.top(), rect.right() - rect.left(),
              rect.bottom() - rect.top());
}

bool ContainsHalfOpen(const Rect& rect, int x, int y) {
  return x >= rect.Left() && x < rect.Right() && y >= rect.Top() &&
         y < rect.Bottom();
}

int64_t SquaredDistanceToRect(const Rect& rect, int x, int y) {
  if (rect.Width() <= 0 || rect.Height() <= 0) {
    return std::numeric_limits<int64_t>::max();
  }
  // Right()-1 and Bottom()-1 match QRect's inclusive pixel upper bounds while
  // avoiding overlap across adjacent monitor borders.
  const int64_t dx = std::max<int64_t>({
      static_cast<int64_t>(rect.Left()) - x,
      0,
      static_cast<int64_t>(x) - (rect.Right() - 1),
  });
  const int64_t dy = std::max<int64_t>({
      static_cast<int64_t>(rect.Top()) - y,
      0,
      static_cast<int64_t>(y) - (rect.Bottom() - 1),
  });
  return dx * dx + dy * dy;
}

int ClampPrimaryIndex(absl::Span<const QtScreenGeometry> screens,
                      int primary_screen_index) {
  if (screens.empty()) {
    return -1;
  }
  if (primary_screen_index >= 0 &&
      primary_screen_index < static_cast<int>(screens.size()) &&
      IsValidScreen(screens[primary_screen_index])) {
    return primary_screen_index;
  }
  for (int i = 0; i < static_cast<int>(screens.size()); ++i) {
    if (IsValidScreen(screens[i])) {
      return i;
    }
  }
  return 0;
}

int FindNamedScreenIndex(absl::Span<const QtScreenGeometry> screens,
                         std::string_view name) {
  if (name.empty()) {
    return -1;
  }
  for (int i = 0; i < static_cast<int>(screens.size()); ++i) {
    if (IsValidScreen(screens[i]) && screens[i].name == name) {
      return i;
    }
  }
  return -1;
}

// Reconstructs the screen's native X11 pixel rectangle from Qt6 XCB geometry.
// Under QT_QPA_PLATFORM=xcb, QScreen::geometry() keeps the native X11 top-left
// origin while dividing width and height by `device_pixel_ratio`.
Rect NativeRectForScreen(const QtScreenGeometry& screen) {
  const double dpr =
      screen.device_pixel_ratio > 0.0 ? screen.device_pixel_ratio : 1.0;
  const int width =
      std::max<int>(1, static_cast<int>(std::lround(screen.geometry.Width() * dpr)));
  const int height =
      std::max<int>(1, static_cast<int>(std::lround(screen.geometry.Height() * dpr)));
  return Rect(screen.geometry.Left(), screen.geometry.Top(), width, height);
}

int ScalePositiveDimension(int value, double scale) {
  if (value <= 0) {
    return value;
  }
  return std::max<int>(1, static_cast<int>(std::lround(value * scale)));
}

Rect TranslateNativeToVirtual(const QtScreenGeometry& screen,
                              const Rect& native_rect) {
  const double dpr =
      screen.device_pixel_ratio > 0.0 ? screen.device_pixel_ratio : 1.0;
  const int screen_left = screen.geometry.Left();
  const int screen_top = screen.geometry.Top();
  const int vx = static_cast<int>(
                     std::floor((native_rect.Left() - screen_left) / dpr)) +
                 screen_left;
  const int vy = static_cast<int>(
                     std::floor((native_rect.Top() - screen_top) / dpr)) +
                 screen_top;
  return Rect(vx, vy, ScalePositiveDimension(native_rect.Width(), 1.0 / dpr),
              ScalePositiveDimension(native_rect.Height(), 1.0 / dpr));
}

Rect TranslateWaylandStageToVirtual(const QtScreenGeometry& screen,
                                    const Rect& stage_preedit_rect,
                                    const Rect& stage_monitor_rect) {
  const double scale_x = static_cast<double>(screen.geometry.Width()) /
                         stage_monitor_rect.Width();
  const double scale_y = static_cast<double>(screen.geometry.Height()) /
                         stage_monitor_rect.Height();
  const int vx =
      screen.geometry.Left() +
      static_cast<int>(std::lround(
          (stage_preedit_rect.Left() - stage_monitor_rect.Left()) * scale_x));
  const int vy =
      screen.geometry.Top() +
      static_cast<int>(std::lround(
          (stage_preedit_rect.Top() - stage_monitor_rect.Top()) * scale_y));
  return Rect(vx, vy, ScalePositiveDimension(stage_preedit_rect.Width(), scale_x),
              ScalePositiveDimension(stage_preedit_rect.Height(), scale_y));
}

template <typename RectGetter>
int FindContainingOrNearestScreenIndex(
    absl::Span<const QtScreenGeometry> screens, int primary_screen_index,
    const Point& point, RectGetter get_rect) {
  for (int i = 0; i < static_cast<int>(screens.size()); ++i) {
    if (IsValidScreen(screens[i]) &&
        ContainsHalfOpen(get_rect(screens[i]), point.x, point.y)) {
      return i;
    }
  }
  const int primary_idx = ClampPrimaryIndex(screens, primary_screen_index);
  if (primary_idx < 0) {
    return -1;
  }
  int best_idx = primary_idx;
  int64_t best_distance =
      SquaredDistanceToRect(get_rect(screens[best_idx]), point.x, point.y);
  for (int i = 0; i < static_cast<int>(screens.size()); ++i) {
    if (!IsValidScreen(screens[i])) {
      continue;
    }
    const int64_t distance =
        SquaredDistanceToRect(get_rect(screens[i]), point.x, point.y);
    if (distance < best_distance) {
      best_distance = distance;
      best_idx = i;
    }
  }
  return best_idx;
}

}  // namespace

ResolvedScreenRect ResolvePreeditAndMonitorRect(
    const commands::RendererCommand& command,
    absl::Span<const QtScreenGeometry> screens, int primary_screen_index) {
  const Rect native_preedit_rect = FromProtoRect(command.preedit_rectangle());
  if (screens.empty()) {
    return {native_preedit_rect, native_preedit_rect, -1};
  }

  if (command.has_application_info() &&
      command.application_info().has_wayland_stage_monitor_name() &&
      command.application_info().has_wayland_stage_monitor_rect()) {
    const auto& appinfo = command.application_info();
    const Rect stage_monitor_rect =
        FromProtoRect(appinfo.wayland_stage_monitor_rect());
    if (stage_monitor_rect.Width() > 0 && stage_monitor_rect.Height() > 0) {
      int screen_idx =
          FindNamedScreenIndex(screens, appinfo.wayland_stage_monitor_name());
      if (screen_idx < 0 && screens.size() == 1 && IsValidScreen(screens[0])) {
        screen_idx = 0;
      }
      if (screen_idx >= 0) {
        return {TranslateWaylandStageToVirtual(
                    screens[screen_idx], native_preedit_rect, stage_monitor_rect),
                EffectiveAvailable(screens[screen_idx]), screen_idx};
      }
    }
  }

  const int screen_idx = FindContainingOrNearestScreenIndex(
      screens, primary_screen_index, native_preedit_rect.origin,
      [](const QtScreenGeometry& s) { return NativeRectForScreen(s); });
  if (screen_idx < 0) {
    return {native_preedit_rect, native_preedit_rect, -1};
  }
  return {TranslateNativeToVirtual(screens[screen_idx], native_preedit_rect),
          EffectiveAvailable(screens[screen_idx]), screen_idx};
}

ResolvedMonitorRect ResolveInfolistMonitor(
    const commands::RendererCommand& command, const Rect& candidate_window_rect,
    absl::Span<const QtScreenGeometry> screens, int primary_screen_index) {
  if (screens.empty()) {
    return {Rect(), -1};
  }
  if (command.has_application_info() &&
      command.application_info().has_wayland_stage_monitor_name()) {
    const int screen_idx = FindNamedScreenIndex(
        screens, command.application_info().wayland_stage_monitor_name());
    if (screen_idx >= 0) {
      return {EffectiveAvailable(screens[screen_idx]), screen_idx};
    }
  }

  // Use the candidate window's center instead of its Right() edge: a window
  // aligned against a screen's right boundary has Right() == screen.Right(),
  // which lies outside half-open QScreen::geometry() containment.
  const Point center(
      candidate_window_rect.Left() + candidate_window_rect.Width() / 2,
      candidate_window_rect.Top() + candidate_window_rect.Height() / 2);
  const int screen_idx = FindContainingOrNearestScreenIndex(
      screens, primary_screen_index, center,
      [](const QtScreenGeometry& s) { return s.geometry; });
  if (screen_idx < 0) {
    return {Rect(), -1};
  }
  return {EffectiveAvailable(screens[screen_idx]), screen_idx};
}

Rect ResolveInfolistMonitorRect(const commands::RendererCommand& command,
                                const Rect& candidate_window_rect,
                                absl::Span<const QtScreenGeometry> screens,
                                int primary_screen_index) {
  return ResolveInfolistMonitor(command, candidate_window_rect, screens,
                                primary_screen_index)
      .monitor_rect;
}

Rect ResolveMonitorRectForVirtualPoint(
    const Point& point, absl::Span<const QtScreenGeometry> screens,
    int primary_screen_index) {
  const int screen_idx = FindContainingOrNearestScreenIndex(
      screens, primary_screen_index, point,
      [](const QtScreenGeometry& s) { return s.geometry; });
  if (screen_idx < 0) {
    return Rect();
  }
  return EffectiveAvailable(screens[screen_idx]);
}

}  // namespace mozc::renderer
