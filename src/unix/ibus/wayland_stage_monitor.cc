// Copyright 2026 Mozkey contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "unix/ibus/wayland_stage_monitor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace mozc {
namespace ibus {
namespace {

constexpr char kMutterDisplayConfigService[] = "org.gnome.Mutter.DisplayConfig";
constexpr char kMutterDisplayConfigPath[] = "/org/gnome/Mutter/DisplayConfig";
constexpr char kMutterDisplayConfigInterface[] =
    "org.gnome.Mutter.DisplayConfig";
constexpr int kDbusCallTimeoutMsec = 200;
constexpr absl::Duration kRetryInterval = absl::Seconds(5);
// GNOME Mutter MetaLogicalMonitorLayoutMode::LOGICAL == 1.
constexpr guint32 kMutterLayoutModeLogical = 1;
constexpr guint32 kMutterLayoutModePhysical = 2;

class ScopedGVariant {
 public:
  explicit ScopedGVariant(GVariant* variant = nullptr) : variant_(variant) {}
  ScopedGVariant(const ScopedGVariant&) = delete;
  ScopedGVariant& operator=(const ScopedGVariant&) = delete;
  ~ScopedGVariant() { reset(); }

  GVariant* get() const { return variant_; }
  void reset(GVariant* variant = nullptr) {
    if (variant_ != nullptr) {
      g_variant_unref(variant_);
    }
    variant_ = variant;
  }

 private:
  GVariant* variant_;
};

struct PhysicalSize {
  int width = 0;
  int height = 0;
};

// Reads per-connector current physical mode dimensions from GetCurrentState's
// `monitors` array (`a((ssss)a(siiddada{sv})a{sv})`).
absl::flat_hash_map<std::string, PhysicalSize> ParseCurrentMonitorModes(
    GVariant* monitors_variant) {
  absl::flat_hash_map<std::string, PhysicalSize> result;
  if (monitors_variant == nullptr ||
      !g_variant_is_of_type(monitors_variant, G_VARIANT_TYPE_ARRAY)) {
    return result;
  }

  GVariantIter monitor_iter;
  g_variant_iter_init(&monitor_iter, monitors_variant);
  GVariant* raw_monitor = nullptr;
  while ((raw_monitor = g_variant_iter_next_value(&monitor_iter)) != nullptr) {
    ScopedGVariant monitor(raw_monitor);
    if (g_variant_n_children(monitor.get()) < 2) {
      continue;
    }
    ScopedGVariant spec(g_variant_get_child_value(monitor.get(), 0));
    ScopedGVariant modes(g_variant_get_child_value(monitor.get(), 1));
    if (g_variant_n_children(spec.get()) < 1 ||
        !g_variant_is_of_type(modes.get(), G_VARIANT_TYPE_ARRAY)) {
      continue;
    }

    ScopedGVariant connector_var(g_variant_get_child_value(spec.get(), 0));
    const gchar* connector = g_variant_get_string(connector_var.get(), nullptr);
    if (connector == nullptr || connector[0] == '\0') {
      continue;
    }

    GVariantIter mode_iter;
    g_variant_iter_init(&mode_iter, modes.get());
    GVariant* raw_mode = nullptr;
    while ((raw_mode = g_variant_iter_next_value(&mode_iter)) != nullptr) {
      ScopedGVariant mode(raw_mode);
      if (g_variant_n_children(mode.get()) < 7) {
        continue;
      }
      ScopedGVariant width_var(g_variant_get_child_value(mode.get(), 1));
      ScopedGVariant height_var(g_variant_get_child_value(mode.get(), 2));
      ScopedGVariant props_var(g_variant_get_child_value(mode.get(), 6));

      gboolean is_current = FALSE;
      if (!g_variant_lookup(props_var.get(), "is-current", "b", &is_current) ||
          !is_current) {
        continue;
      }
      const gint32 width = g_variant_get_int32(width_var.get());
      const gint32 height = g_variant_get_int32(height_var.get());
      if (width > 0 && height > 0) {
        result[connector] = PhysicalSize{width, height};
        break;
      }
    }
  }
  return result;
}

int64_t SquaredDistanceToMonitor(const StageMonitor& monitor, int x, int y) {
  if (monitor.width <= 0 || monitor.height <= 0) {
    return std::numeric_limits<int64_t>::max();
  }
  const int64_t dx = std::max<int64_t>({
      static_cast<int64_t>(monitor.x) - x,
      0,
      static_cast<int64_t>(x) - (static_cast<int64_t>(monitor.x) + monitor.width - 1),
  });
  const int64_t dy = std::max<int64_t>({
      static_cast<int64_t>(monitor.y) - y,
      0,
      static_cast<int64_t>(y) - (static_cast<int64_t>(monitor.y) + monitor.height - 1),
  });
  return dx * dx + dy * dy;
}

}  // namespace

std::vector<StageMonitor> ParseMutterDisplayConfigState(GVariant* state) {
  std::vector<StageMonitor> result;
  if (state == nullptr || !g_variant_is_of_type(state, G_VARIANT_TYPE_TUPLE) ||
      g_variant_n_children(state) < 4) {
    return result;
  }

  ScopedGVariant monitors_var(g_variant_get_child_value(state, 1));
  ScopedGVariant logical_monitors_var(g_variant_get_child_value(state, 2));
  ScopedGVariant global_props_var(g_variant_get_child_value(state, 3));

  const absl::flat_hash_map<std::string, PhysicalSize> mode_sizes =
      ParseCurrentMonitorModes(monitors_var.get());
  if (mode_sizes.empty() ||
      !g_variant_is_of_type(logical_monitors_var.get(), G_VARIANT_TYPE_ARRAY)) {
    return result;
  }

  guint32 layout_mode = kMutterLayoutModePhysical;
  if (g_variant_is_of_type(global_props_var.get(),
                           G_VARIANT_TYPE_DICTIONARY)) {
    g_variant_lookup(global_props_var.get(), "layout-mode", "u", &layout_mode);
  }
  const bool logical_layout = (layout_mode == kMutterLayoutModeLogical);

  GVariantIter lm_iter;
  g_variant_iter_init(&lm_iter, logical_monitors_var.get());
  GVariant* raw_lm = nullptr;
  while ((raw_lm = g_variant_iter_next_value(&lm_iter)) != nullptr) {
    ScopedGVariant lm(raw_lm);
    if (g_variant_n_children(lm.get()) < 6) {
      continue;
    }
    ScopedGVariant x_var(g_variant_get_child_value(lm.get(), 0));
    ScopedGVariant y_var(g_variant_get_child_value(lm.get(), 1));
    ScopedGVariant scale_var(g_variant_get_child_value(lm.get(), 2));
    ScopedGVariant transform_var(g_variant_get_child_value(lm.get(), 3));
    ScopedGVariant connectors_var(g_variant_get_child_value(lm.get(), 5));

    const gint32 x = g_variant_get_int32(x_var.get());
    const gint32 y = g_variant_get_int32(y_var.get());
    const gdouble scale = g_variant_get_double(scale_var.get());
    const guint32 transform = g_variant_get_uint32(transform_var.get());
    if (!g_variant_is_of_type(connectors_var.get(), G_VARIANT_TYPE_ARRAY)) {
      continue;
    }

    GVariantIter conn_iter;
    g_variant_iter_init(&conn_iter, connectors_var.get());
    GVariant* raw_spec = nullptr;
    while ((raw_spec = g_variant_iter_next_value(&conn_iter)) != nullptr) {
      ScopedGVariant spec(raw_spec);
      if (g_variant_n_children(spec.get()) < 1) {
        continue;
      }
      ScopedGVariant name_var(g_variant_get_child_value(spec.get(), 0));
      const gchar* connector = g_variant_get_string(name_var.get(), nullptr);
      if (connector == nullptr) {
        continue;
      }
      const auto it = mode_sizes.find(connector);
      if (it == mode_sizes.end()) {
        continue;
      }
      int phys_w = it->second.width;
      int phys_h = it->second.height;
      // Odd Mutter MetaMonitorTransform values (90, 270, flipped-90/270) swap axes.
      if ((transform % 2U) == 1U) {
        std::swap(phys_w, phys_h);
      }
      int logical_w = phys_w;
      int logical_h = phys_h;
      if (logical_layout && scale > 0.0) {
        logical_w = static_cast<int>(std::lround(phys_w / scale));
        logical_h = static_cast<int>(std::lround(phys_h / scale));
      }
      if (logical_w > 0 && logical_h > 0) {
        result.push_back(StageMonitor{connector, x, y, logical_w, logical_h});
        break;
      }
    }
  }
  return result;
}

std::optional<StageMonitor> FindStageMonitorForRect(
    absl::Span<const StageMonitor> monitors,
    const commands::RendererCommand::Rectangle& preedit_rect) {
  if (monitors.empty()) {
    return std::nullopt;
  }
  const int px = preedit_rect.left();
  const int py = preedit_rect.top();
  for (const StageMonitor& monitor : monitors) {
    if (monitor.width <= 0 || monitor.height <= 0) {
      continue;
    }
    if (px >= monitor.x && px < monitor.x + monitor.width && py >= monitor.y &&
        py < monitor.y + monitor.height) {
      return monitor;
    }
  }

  const StageMonitor* best = nullptr;
  int64_t best_distance = std::numeric_limits<int64_t>::max();
  for (const StageMonitor& monitor : monitors) {
    if (monitor.width <= 0 || monitor.height <= 0) {
      continue;
    }
    const int64_t distance = SquaredDistanceToMonitor(monitor, px, py);
    if (distance < best_distance) {
      best_distance = distance;
      best = &monitor;
    }
  }
  if (best == nullptr) {
    return std::nullopt;
  }
  return *best;
}

WaylandStageMonitorResolver::WaylandStageMonitorResolver()
    : fetcher_([this](std::vector<StageMonitor>* monitors) {
        return FetchFromMutterDbus(monitors);
      }) {}

WaylandStageMonitorResolver::WaylandStageMonitorResolver(FetcherFn fetcher)
    : fetcher_(std::move(fetcher)), dbus_init_attempted_(true) {}

WaylandStageMonitorResolver::~WaylandStageMonitorResolver() {
  if (bus_ != nullptr) {
    if (monitors_changed_id_ != 0) {
      g_dbus_connection_signal_unsubscribe(bus_, monitors_changed_id_);
      monitors_changed_id_ = 0;
    }
    g_object_unref(bus_);
    bus_ = nullptr;
  }
}

void WaylandStageMonitorResolver::MarkDirty() { dirty_ = true; }

std::optional<StageMonitor> WaylandStageMonitorResolver::ResolveMonitorForRect(
    const commands::RendererCommand::Rectangle& preedit_rect) {
  RefreshIfNeeded();
  return FindStageMonitorForRect(monitors_, preedit_rect);
}

void WaylandStageMonitorResolver::EnsureSubscribed() {
  if (dbus_init_attempted_) {
    return;
  }
  dbus_init_attempted_ = true;
  GError* error = nullptr;
  bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (bus_ == nullptr) {
    if (!has_logged_dbus_error_) {
      LOG(WARNING) << "Failed to connect to session D-Bus for Mutter "
                      "DisplayConfig: "
                   << (error != nullptr ? error->message : "unknown error");
      has_logged_dbus_error_ = true;
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    return;
  }

  monitors_changed_id_ = g_dbus_connection_signal_subscribe(
      bus_, nullptr, kMutterDisplayConfigInterface, "MonitorsChanged",
      kMutterDisplayConfigPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      &WaylandStageMonitorResolver::OnMonitorsChanged, this, nullptr);
}

void WaylandStageMonitorResolver::RefreshIfNeeded() {
  if (!dirty_) {
    return;
  }
  const absl::Time now = absl::Now();
  if (now < next_retry_time_) {
    return;
  }
  std::vector<StageMonitor> updated;
  if (fetcher_ && fetcher_(&updated)) {
    monitors_ = std::move(updated);
    dirty_ = false;
    next_retry_time_ = absl::InfinitePast();
  } else {
    next_retry_time_ = now + kRetryInterval;
  }
}

bool WaylandStageMonitorResolver::FetchFromMutterDbus(
    std::vector<StageMonitor>* monitors) {
  EnsureSubscribed();
  if (bus_ == nullptr) {
    return false;
  }

  GError* error = nullptr;
  ScopedGVariant response(g_dbus_connection_call_sync(
      bus_, kMutterDisplayConfigService, kMutterDisplayConfigPath,
      kMutterDisplayConfigInterface, "GetCurrentState", nullptr,
      G_VARIANT_TYPE(
          "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})"),
      G_DBUS_CALL_FLAGS_NONE, kDbusCallTimeoutMsec, nullptr, &error));
  if (response.get() == nullptr) {
    if (!has_logged_dbus_error_) {
      LOG(WARNING) << "Mutter DisplayConfig.GetCurrentState failed: "
                   << (error != nullptr ? error->message : "unknown error");
      has_logged_dbus_error_ = true;
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    return false;
  }

  *monitors = ParseMutterDisplayConfigState(response.get());
  return !monitors->empty();
}

void WaylandStageMonitorResolver::OnMonitorsChanged(
    GDBusConnection* connection, const gchar* sender_name,
    const gchar* object_path, const gchar* interface_name,
    const gchar* signal_name, GVariant* parameters, gpointer user_data) {
  auto* self = static_cast<WaylandStageMonitorResolver*>(user_data);
  self->next_retry_time_ = absl::InfinitePast();
  self->dirty_ = true;
}

}  // namespace ibus
}  // namespace mozc
