// Copyright 2026 Mozkey contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOZC_UNIX_IBUS_WAYLAND_STAGE_MONITOR_H_
#define MOZC_UNIX_IBUS_WAYLAND_STAGE_MONITOR_H_

#include <gio/gio.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "absl/time/time.h"
#include "absl/types/span.h"
#include "protocol/renderer_command.pb.h"

namespace mozc {
namespace ibus {

// Logical monitor geometry in GNOME Mutter Stage coordinates.
struct StageMonitor {
  std::string connector;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Parses the response tuple from org.gnome.Mutter.DisplayConfig.GetCurrentState.
std::vector<StageMonitor> ParseMutterDisplayConfigState(GVariant* state);

// Picks the monitor containing `preedit_rect.left()/top()` (or the closest one).
std::optional<StageMonitor> FindStageMonitorForRect(
    absl::Span<const StageMonitor> monitors,
    const commands::RendererCommand::Rectangle& preedit_rect);

// Queries and caches Mutter Stage monitor geometries, invalidating the cache
// when Mutter emits MonitorsChanged on the session D-Bus.
class WaylandStageMonitorResolver {
 public:
  using FetcherFn = std::function<bool(std::vector<StageMonitor>*)>;

  WaylandStageMonitorResolver();
  explicit WaylandStageMonitorResolver(FetcherFn fetcher);
  WaylandStageMonitorResolver(const WaylandStageMonitorResolver&) = delete;
  WaylandStageMonitorResolver& operator=(const WaylandStageMonitorResolver&) =
      delete;
  ~WaylandStageMonitorResolver();

  std::optional<StageMonitor> ResolveMonitorForRect(
      const commands::RendererCommand::Rectangle& preedit_rect);

  void MarkDirty();

 private:
  void EnsureSubscribed();
  void RefreshIfNeeded();
  bool FetchFromMutterDbus(std::vector<StageMonitor>* monitors);

  static void OnMonitorsChanged(GDBusConnection* connection,
                                const gchar* sender_name,
                                const gchar* object_path,
                                const gchar* interface_name,
                                const gchar* signal_name,
                                GVariant* parameters, gpointer user_data);

  FetcherFn fetcher_;
  GDBusConnection* bus_ = nullptr;
  guint monitors_changed_id_ = 0;
  bool dbus_init_attempted_ = false;
  bool dirty_ = true;
  bool has_logged_dbus_error_ = false;
  absl::Time next_retry_time_ = absl::InfinitePast();
  std::vector<StageMonitor> monitors_;
};

}  // namespace ibus
}  // namespace mozc

#endif  // MOZC_UNIX_IBUS_WAYLAND_STAGE_MONITOR_H_
