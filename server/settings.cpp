#include "settings.hpp"

#include <toml++/toml.hpp>

namespace amber {
Settings read_settings(const std::filesystem::path &path) {
  const auto document = toml::parse_file(path.string());
  const auto get = [&]<class T>(const char *section, const char *key) {
    const auto value = document[section][key].value<T>();
    if (!value)
      throw std::invalid_argument(std::string("missing or invalid configuration: ") + section +
                                  "." + key);
    return *value;
  };
  const auto size = [&](const char *section, const char *key) {
    const auto value = get.operator()<std::int64_t>(section, key);
    if (value < 0)
      throw std::invalid_argument(std::string("negative configuration: ") + section + "." + key);
    return static_cast<std::uint32_t>(value);
  };
  const auto real = [&](const char *section, const char *key) {
    return get.operator()<double>(section, key);
  };
  const auto f = [&](const char *section, const char *key) {
    return static_cast<float>(real(section, key));
  };
  Settings s{};
  s.clock = {real("clock", "STEP_HZ"), real("clock", "STEP_MARGIN_MS"),
             size("clock", "PHASE_AVERAGE_STEPS")};
  s.scripts = {size("scripts", "SCRIPT_KB"), real("scripts", "DECLARATION_MS"),
               real("scripts", "HANDLER_MS"), size("scripts", "EVENT_FIELDS_MAX")};
  s.event_queue_max = size("scripts", "EVENT_QUEUE_MAX");
  s.authors_max = size("scripts", "AUTHORS_MAX");
  s.part_batch_max = size("scripts", "PART_BATCH_MAX");
  s.mailbox_max = size("permissions", "MAILBOX_MAX");
  s.grant_reminder_seconds = real("permissions", "GRANT_REMINDER_SECONDS");
  s.deadlines_per_step = size("permissions", "DEADLINES_PER_STEP");
  s.gap_captures = size("elaboration", "GAP_CAPTURES");
  s.capture_m = f("elaboration", "CAPTURE_M");
  s.fixture_limits = {size("elaboration", "FIXTURE_TIMEOUT_MS"),
                      size("elaboration", "FIXTURE_DIAGNOSTIC_BYTES"),
                      size("elaboration", "FIXTURE_RESULT_BYTES")};
  s.fixture_workers = size("elaboration", "FIXTURE_WORKERS");
  s.network = {f("network", "CELL_M"), static_cast<int>(size("network", "ENTER_CELLS")),
               static_cast<int>(size("network", "LEAVE_CELLS")), size("network", "SEND_SOFT"),
               size("network", "SEND_HARD")};
  s.control_max = size("network", "CONTROL_MAX");
  s.repl_every = size("network", "REPL_EVERY");
  s.repl_delay_ms = size("network", "REPL_DELAY_MS");
  s.perception = {f("perception", "SIGHT_M"),
                  f("perception", "REACH_M"),
                  f("perception", "FAR_M"),
                  f("perception", "FAR_ANGULAR_DEGREES"),
                  f("perception", "MOVING_M_S"),
                  real("perception", "FAR_SOURCE_LUX"),
                  size("perception", "THINGS_MAX"),
                  size("perception", "FAR_MAX"),
                  size("perception", "EVENTS_PER_RESPONSE")};
  s.actions_page = size("perception", "ACTIONS_PAGE");
  s.light_cell_m = f("perception", "LIGHT_CELL_M");
  s.light_source_radius_m = f("perception", "LIGHT_SOURCE_RADIUS_M");
  s.pins.bake = get.operator()<std::string>("bake", "BAKE_VERSION");
  s.record_queue_batches = size("record", "QUEUE_BATCHES");
  s.record_zstd = size("record", "ZSTD_LEVEL");
  s.snapshot_zstd = size("snapshots", "ZSTD_LEVEL");
  s.snapshot_seconds = real("snapshots", "SNAP_EVERY_S");
  s.snapshot_keep = size("snapshots", "SNAP_KEEP");
  const auto *gravity = document["space"]["GRAVITY_M_S2"].as_array();
  if (!gravity || gravity->size() != 3)
    throw std::invalid_argument("space.GRAVITY_M_S2 requires three numbers");
  s.space.gravity_m_s2 = {static_cast<float>(gravity->get(0)->value<double>().value()),
                          static_cast<float>(gravity->get(1)->value<double>().value()),
                          static_cast<float>(gravity->get(2)->value<double>().value())};
  s.space.max_bodies = size("space", "MAX_BODIES");
  s.space.max_characters = size("space", "MAX_CHARACTERS");
  s.space.max_body_pairs = size("space", "MAX_BODY_PAIRS");
  s.space.max_contacts = size("space", "MAX_CONTACTS");
  s.space.physics_temp_bytes = size("space", "TEMP_BYTES");
  s.space.contact_slop_m = f("space", "CONTACT_SLOP_M");
  s.space.sweep_skin_m = f("space", "SWEEP_SKIN_M");
  s.space.crowd_correction_m = f("space", "CROWD_CORRECTION_M");
  s.space.blocked_after_s = f("space", "BLOCKED_AFTER_S");
  s.default_friction = f("space", "DEFAULT_FRICTION");
  s.default_restitution = f("space", "DEFAULT_RESTITUTION");
  s.goal_tolerance_m = f("space", "GOAL_TOLERANCE_M");
  s.space.crowd_query_radius_multiplier = f("navigation", "CROWD_QUERY_RADIUS_MULTIPLIER");
  s.space.crowd_optimize_radius_multiplier = f("navigation", "CROWD_OPTIMIZE_RADIUS_MULTIPLIER");
  s.space.crowd_separation_weight = f("navigation", "CROWD_SEPARATION_WEIGHT");
  s.space.navigation = {f("navigation", "CELL_M"),
                        f("navigation", "CELL_HEIGHT_M"),
                        size("navigation", "TILE_CELLS"),
                        f("navigation", "RADIUS_M"),
                        f("navigation", "HEIGHT_M"),
                        f("navigation", "CLIMB_M"),
                        f("navigation", "SLOPE_DEGREES"),
                        f("navigation", "MAX_EDGE_M"),
                        f("navigation", "CONTOUR_ERROR_M"),
                        f("navigation", "DETAIL_SAMPLE_M"),
                        f("navigation", "DETAIL_ERROR_M"),
                        size("navigation", "MIN_REGION_CELLS"),
                        size("navigation", "MERGE_REGION_CELLS"),
                        size("navigation", "NODES_PER_QUERY"),
                        size("navigation", "POLYGONS_PER_PATH"),
                        size("navigation", "WORKERS"),
                        size("navigation", "MAX_TILES"),
                        size("navigation", "POLYGONS_PER_TILE")};
  return s;
}
} // namespace amber
