#pragma once

#include <cstdint>

#include "../../../state/activity_sdk/runtime.h"
#include "../../activity/activity_sdk_mission_runtime.h"

namespace sunrise::server::ui::activity_host::sdk_mission_view {

/**
 * Draws everything the SDK records about one selected type-43 scene: its resource, its event
 * gates, the cast the host would prepare, and each linked squad with its members and anchors.
 * Also draws the manual start, stop and per-gate send actions.
 * @param view Current SDK and activity binding.
 * @param occurrenceRow Selected scene occurrence.
 * @param slotRow Selected type-43 slot.
 * @param availability The row's last resolved availability; actions are disabled unless ready.
 */
void draw_scene_details(const state::activity_sdk::BoundView& view,
                        std::uint32_t occurrenceRow,
                        std::uint32_t slotRow,
                        server::activity::activity_sdk_mission::SceneStatus availability) noexcept;

/** Forgets action feedback that belonged to an earlier activity client generation. */
void reset_scene_detail_status() noexcept;

} // namespace sunrise::server::ui::activity_host::sdk_mission_view
