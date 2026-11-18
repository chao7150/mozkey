// Copyright 2026 Mozkey contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "renderer/qt/qt_screen_coordinates.h"

#include <vector>

#include "base/coordinates.h"
#include "gtest/gtest.h"
#include "protocol/renderer_command.pb.h"

namespace mozc::renderer {
namespace {

std::vector<QtScreenGeometry> UbuntuWaylandTripleMonitorScreens() {
  // Matches Qt6 QT_QPA_PLATFORM=xcb QScreen geometries under Mutter
  // scale-monitor-framebuffer + xwayland-native-scaling + Xft.dpi=240 (dpr=2.5).
  return {
      {"DP-3", Rect(3840, 0, 2048, 1152), Rect(3840, 0, 2048, 1152), 2.5},
      {"DP-4", Rect(0, 0, 1536, 864), Rect(0, 0, 1536, 864), 2.5},
      {"HDMI-1", Rect(8960, 0, 2048, 1152), Rect(8960, 0, 2048, 1152), 2.5},
  };
}

void SetProtoRect(commands::RendererCommand::Rectangle* out, int left, int top,
                  int right, int bottom) {
  out->set_left(left);
  out->set_top(top);
  out->set_right(right);
  out->set_bottom(bottom);
}

TEST(QtScreenCoordinatesTest, WaylandStageDeadGapOnPrimaryDp3) {
  const auto screens = UbuntuWaylandTripleMonitorScreens();
  commands::RendererCommand command;
  // Wayland Stage (2500, 500) falls inside the virtual dead gap [1536, 3840)
  // if interpreted blindly in X11 coordinates.
  SetProtoRect(command.mutable_preedit_rectangle(), 2500, 500, 2501, 520);
  auto* appinfo = command.mutable_application_info();
  appinfo->set_wayland_stage_monitor_name("DP-3");
  SetProtoRect(appinfo->mutable_wayland_stage_monitor_rect(), 1920, 0, 4480,
               1440);

  const ResolvedScreenRect resolved =
      ResolvePreeditAndMonitorRect(command, screens, /*primary_screen_index=*/0);
  // (2500 - 1920) * (2048 / 2560) = 464 => vx = 3840 + 464 = 4304.
  EXPECT_EQ(resolved.preedit_rect.Left(), 4304);
  EXPECT_EQ(resolved.preedit_rect.Top(), 400);
  EXPECT_EQ(resolved.preedit_rect.Width(), 1);
  EXPECT_EQ(resolved.preedit_rect.Height(), 16);
  EXPECT_EQ(resolved.monitor_rect.Left(), 3840);
  EXPECT_EQ(resolved.monitor_rect.Width(), 2048);
  EXPECT_EQ(resolved.screen_index, 0);
}

TEST(QtScreenCoordinatesTest, WaylandStageOnSecondaryDp4AndHdmi1) {
  const auto screens = UbuntuWaylandTripleMonitorScreens();
  {
    commands::RendererCommand command;
    SetProtoRect(command.mutable_preedit_rectangle(), 100, 100, 101, 120);
    auto* appinfo = command.mutable_application_info();
    appinfo->set_wayland_stage_monitor_name("DP-4");
    SetProtoRect(appinfo->mutable_wayland_stage_monitor_rect(), 0, 0, 1920,
                 1080);

    const ResolvedScreenRect resolved =
        ResolvePreeditAndMonitorRect(command, screens, 0);
    EXPECT_EQ(resolved.preedit_rect.Left(), 80);
    EXPECT_EQ(resolved.preedit_rect.Top(), 80);
    EXPECT_EQ(resolved.preedit_rect.Height(), 16);
    EXPECT_EQ(resolved.monitor_rect.Left(), 0);
    EXPECT_EQ(resolved.monitor_rect.Width(), 1536);
  }
  {
    commands::RendererCommand command;
    SetProtoRect(command.mutable_preedit_rectangle(), 5000, 500, 5001, 520);
    auto* appinfo = command.mutable_application_info();
    appinfo->set_wayland_stage_monitor_name("HDMI-1");
    SetProtoRect(appinfo->mutable_wayland_stage_monitor_rect(), 4480, 0, 7040,
                 1440);

    const ResolvedScreenRect resolved =
        ResolvePreeditAndMonitorRect(command, screens, 0);
    // (5000 - 4480) * 0.8 = 416 => vx = 8960 + 416 = 9376.
    EXPECT_EQ(resolved.preedit_rect.Left(), 9376);
    EXPECT_EQ(resolved.preedit_rect.Top(), 400);
    EXPECT_EQ(resolved.monitor_rect.Left(), 8960);
    EXPECT_EQ(resolved.monitor_rect.Width(), 2048);
  }
}

TEST(QtScreenCoordinatesTest, WaylandStageWithVerticalOffsetOrigin) {
  const std::vector<QtScreenGeometry> screens = {
      {"DP-1", Rect(0, 0, 1920, 1080), Rect(0, 0, 1920, 1080), 2.0},
      {"DP-2", Rect(3840, 2160, 1280, 720), Rect(3840, 2160, 1280, 720), 2.0},
  };
  commands::RendererCommand command;
  SetProtoRect(command.mutable_preedit_rectangle(), 2120, 1280, 2121, 1300);
  auto* appinfo = command.mutable_application_info();
  appinfo->set_wayland_stage_monitor_name("DP-2");
  SetProtoRect(appinfo->mutable_wayland_stage_monitor_rect(), 1920, 1080, 3840,
               2160);

  const ResolvedScreenRect resolved =
      ResolvePreeditAndMonitorRect(command, screens, 0);
  // (200 / 1920) * 1280 = 133; (200 / 1080) * 720 = 133.
  EXPECT_EQ(resolved.preedit_rect.Left(), 3840 + 133);
  EXPECT_EQ(resolved.preedit_rect.Top(), 2160 + 133);
  EXPECT_EQ(resolved.monitor_rect.Top(), 2160);
}

TEST(QtScreenCoordinatesTest, X11NativeCoordinatesAcrossDeadGaps) {
  const auto screens = UbuntuWaylandTripleMonitorScreens();
  // XWayland right half of DP-3: native X11 x = 8000 (screen spans [3840,
  // 8960) in native X11 pixels despite Qt virtual geometry ending at 5888).
  commands::RendererCommand command;
  SetProtoRect(command.mutable_preedit_rectangle(), 8000, 1000, 8002, 1025);

  const ResolvedScreenRect resolved =
      ResolvePreeditAndMonitorRect(command, screens, 0);
  EXPECT_EQ(resolved.preedit_rect.Left(), 5504);
  EXPECT_EQ(resolved.preedit_rect.Top(), 400);
  EXPECT_EQ(resolved.preedit_rect.Height(), 10);
  EXPECT_EQ(resolved.monitor_rect.Left(), 3840);

  // Near DP-4's right boundary just above the screen (y = -2): should snap
  // to DP-4 instead of DP-3.
  commands::RendererCommand edge_cmd;
  SetProtoRect(edge_cmd.mutable_preedit_rectangle(), 3839, -2, 3840, 18);
  const ResolvedScreenRect edge_resolved =
      ResolvePreeditAndMonitorRect(edge_cmd, screens, 0);
  EXPECT_EQ(edge_resolved.monitor_rect.Left(), 0);
}

TEST(QtScreenCoordinatesTest, InfolistMonitorDoesNotSnapToPrimaryOnRightEdge) {
  const auto screens = UbuntuWaylandTripleMonitorScreens();
  // Candidate window sits at right edge of DP-4: Right() == 1536, which is
  // outside QRect(0, 0, 1536, 864)::contains(1536, y).
  const Rect cand_rect(1236, 200, 300, 220);

  // Wayland metadata explicitly selects DP-4.
  commands::RendererCommand wayland_cmd;
  wayland_cmd.mutable_application_info()->set_wayland_stage_monitor_name(
      "DP-4");
  const Rect wayland_mon =
      ResolveInfolistMonitorRect(wayland_cmd, cand_rect, screens, 0);
  EXPECT_EQ(wayland_mon.Left(), 0);
  EXPECT_EQ(wayland_mon.Width(), 1536);

  // Even without Wayland metadata (X11), window center (1386, 310) is inside
  // DP-4 and must return DP-4 instead of falling back to primary DP-3.
  commands::RendererCommand x11_cmd;
  const Rect x11_mon = ResolveInfolistMonitorRect(x11_cmd, cand_rect, screens, 0);
  EXPECT_EQ(x11_mon.Left(), 0);
  EXPECT_EQ(x11_mon.Width(), 1536);
}

}  // namespace
}  // namespace mozc::renderer
