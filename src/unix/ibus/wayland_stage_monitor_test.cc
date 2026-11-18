// Copyright 2026 Mozkey contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "unix/ibus/wayland_stage_monitor.h"

#include <gio/gio.h>

#include <optional>
#include <vector>

#include "gtest/gtest.h"
#include "protocol/renderer_command.pb.h"

namespace mozc {
namespace ibus {
namespace {

commands::RendererCommand::Rectangle MakeRect(int left, int top, int right,
                                              int bottom) {
  commands::RendererCommand::Rectangle rect;
  rect.set_left(left);
  rect.set_top(top);
  rect.set_right(right);
  rect.set_bottom(bottom);
  return rect;
}

TEST(WaylandStageMonitorTest, ParsesMutterGetCurrentState) {
  // Matches (ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv}).
  GVariantBuilder monitors_b;
  g_variant_builder_init(&monitors_b,
                         G_VARIANT_TYPE("a((ssss)a(siiddada{sv})a{sv})"));

  const struct {
    const char* connector;
    int width;
    int height;
  } kModes[] = {
      {"DP-4", 1920, 1080},
      {"DP-3", 3840, 2160},
      {"HDMI-1", 2560, 1440},
  };
  for (const auto& m : kModes) {
    GVariantBuilder modes_b;
    g_variant_builder_init(&modes_b, G_VARIANT_TYPE("a(siiddada{sv})"));

    GVariantBuilder scales_b;
    g_variant_builder_init(&scales_b, G_VARIANT_TYPE("ad"));
    g_variant_builder_add(&scales_b, "d", 1.0);

    GVariantBuilder mode_props_b;
    g_variant_builder_init(&mode_props_b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&mode_props_b, "{sv}", "is-current",
                          g_variant_new_boolean(TRUE));

    g_variant_builder_add(&modes_b, "(siiddada{sv})", "mode-0", m.width,
                          m.height, 60.0, 1.0, &scales_b, &mode_props_b);

    GVariantBuilder mon_props_b;
    g_variant_builder_init(&mon_props_b, G_VARIANT_TYPE("a{sv}"));

    g_variant_builder_add(&monitors_b, "((ssss)a(siiddada{sv})a{sv})",
                          m.connector, "LG", "Monitor", "123", &modes_b,
                          &mon_props_b);
  }

  GVariantBuilder lms_b;
  g_variant_builder_init(&lms_b, G_VARIANT_TYPE("a(iiduba(ssss)a{sv})"));
  const struct {
    int x;
    int y;
    double scale;
    guint32 transform;
    gboolean primary;
    const char* connector;
  } kLogical[] = {
      {0, 0, 1.0, 0, FALSE, "DP-4"},
      {1920, 0, 1.5, 0, TRUE, "DP-3"},
      // Rotated 90 degrees (transform=1) should swap 2560x1440 -> 1440x2560.
      {4480, 0, 1.0, 1, FALSE, "HDMI-1"},
  };
  for (const auto& lm : kLogical) {
    GVariantBuilder conns_b;
    g_variant_builder_init(&conns_b, G_VARIANT_TYPE("a(ssss)"));
    g_variant_builder_add(&conns_b, "(ssss)", lm.connector, "LG", "Monitor",
                          "123");
    GVariantBuilder lm_props_b;
    g_variant_builder_init(&lm_props_b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&lms_b, "(iiduba(ssss)a{sv})", lm.x, lm.y, lm.scale,
                          lm.transform, lm.primary, &conns_b, &lm_props_b);
  }

  GVariantBuilder global_props_b;
  g_variant_builder_init(&global_props_b, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&global_props_b, "{sv}", "layout-mode",
                        g_variant_new_uint32(1));

  GVariant* state = g_variant_ref_sink(g_variant_new(
      "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})", 7U,
      &monitors_b, &lms_b, &global_props_b));

  const std::vector<StageMonitor> parsed = ParseMutterDisplayConfigState(state);
  g_variant_unref(state);

  ASSERT_EQ(parsed.size(), 3);
  EXPECT_EQ(parsed[0].connector, "DP-4");
  EXPECT_EQ(parsed[0].x, 0);
  EXPECT_EQ(parsed[0].width, 1920);
  EXPECT_EQ(parsed[0].height, 1080);

  EXPECT_EQ(parsed[1].connector, "DP-3");
  EXPECT_EQ(parsed[1].x, 1920);
  EXPECT_EQ(parsed[1].width, 2560);
  EXPECT_EQ(parsed[1].height, 1440);

  EXPECT_EQ(parsed[2].connector, "HDMI-1");
  EXPECT_EQ(parsed[2].x, 4480);
  EXPECT_EQ(parsed[2].width, 1440);
  EXPECT_EQ(parsed[2].height, 2560);
}

TEST(WaylandStageMonitorTest, ResolverCachesAndRefreshesOnDirty) {
  int fetch_count = 0;
  WaylandStageMonitorResolver resolver(
      [&](std::vector<StageMonitor>* monitors) {
        ++fetch_count;
        if (fetch_count == 1) {
          *monitors = {{"DP-4", 0, 0, 1920, 1080},
                       {"DP-3", 1920, 0, 2560, 1440}};
        } else {
          *monitors = {{"HDMI-1", 0, 0, 2560, 1440}};
        }
        return true;
      });

  const std::optional<StageMonitor> first =
      resolver.ResolveMonitorForRect(MakeRect(2500, 500, 2501, 520));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->connector, "DP-3");
  EXPECT_EQ(fetch_count, 1);

  // Cached hit should not re-query.
  const std::optional<StageMonitor> cached =
      resolver.ResolveMonitorForRect(MakeRect(100, 100, 101, 120));
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(cached->connector, "DP-4");
  EXPECT_EQ(fetch_count, 1);

  resolver.MarkDirty();
  const std::optional<StageMonitor> refreshed =
      resolver.ResolveMonitorForRect(MakeRect(2500, 500, 2501, 520));
  ASSERT_TRUE(refreshed.has_value());
  EXPECT_EQ(refreshed->connector, "HDMI-1");
  EXPECT_EQ(fetch_count, 2);
}

}  // namespace
}  // namespace ibus
}  // namespace mozc
