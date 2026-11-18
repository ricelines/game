#pragma once

#include "catalog/catalog.hpp"

#include "elaborate/elaborate.hpp"
#include "net/net.hpp"
#include "space/space.hpp"
#include "step/step.hpp"

namespace amber {
// Launch configuration only. This value owns no live simulation state.
struct Settings {
  step::Settings clock;
  perm::LandSettings land;
  react::VmSettings scripts;
  space::Settings space;
  net::Settings network;
  observe::Settings perception;
  catalog::RuntimePins pins;
  std::size_t event_queue_max, authors_max, part_batch_max, mailbox_max, actions_page;
  std::size_t record_queue_batches, snapshot_keep;
  int record_zstd, snapshot_zstd;
  std::uint32_t control_max, repl_every, repl_delay_ms;
  double snapshot_seconds;
  double grant_reminder_seconds;
  std::int64_t architect_allowance;
  double architect_period_seconds, architect_expiry_seconds;
  std::size_t deadlines_per_step;
  float light_cell_m, light_source_radius_m;
  std::uint32_t gap_captures;
  float capture_m;
  elaborate::FixtureLimits fixture_limits;
  std::size_t fixture_workers;
  float default_friction, default_restitution, goal_tolerance_m;
};
Settings read_settings(const std::filesystem::path &, std::string_view profile = "production");
} // namespace amber
