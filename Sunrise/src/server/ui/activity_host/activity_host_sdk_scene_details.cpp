#include "activity_host_sdk_scene_details.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <imgui.h>
#include <string_view>
#include <vector>

#include "../../activity/activity_sdk_scene_spawn.h"
#include "activity_host_sdk_mission_text.h"
#include "activity_host_table_layout.h"

namespace sunrise::server::ui::activity_host::sdk_mission_view {
namespace {

namespace mission = server::activity::activity_sdk_mission;
namespace sdk = state::activity_sdk;

/** One squad a scene edge names, in the scene's own state. */
struct LinkedSquad final {
    std::uint32_t edgeRow{sdk::format::kAbsentIndex};
    std::uint32_t squadRow{sdk::format::kAbsentIndex};
};

/** The selection the retained rows below were resolved for. */
struct DetailKey final {
    const sdk::Catalog* catalog{};
    std::uint32_t scenarioRow{sdk::format::kAbsentIndex};
    std::uint32_t occurrenceRow{sdk::format::kAbsentIndex};
    std::uint32_t slotRow{sdk::format::kAbsentIndex};

    [[nodiscard]] bool operator==(const DetailKey&) const noexcept = default;
};

/** What the last manual action on the selected scene was, for the feedback line. */
enum class DetailAction : std::uint8_t {
    none,
    start,
    stop,
    event,
};

DetailKey g_detailKey{};
/** Scanning every squad is too slow per frame, so the linked rows are kept per selection. */
std::vector<LinkedSquad> g_linkedSquads{};
/** Event keys sent since the last manual start or stop; a display aid, not Host state. */
std::vector<std::uint32_t> g_sentKeys{};
/** Static storage keeps the bounded cast off the UI stack. */
mission::SceneSpawnPlan g_cast{};
mission::SceneStatus g_castStatus{mission::SceneStatus::invalidView};
DetailAction g_lastAction{DetailAction::none};
std::uint32_t g_lastEventKey{};
mission::SceneStatus g_lastStatus{mission::SceneStatus::ready};

/** @return The scene's gates; the section is sorted by scene slot, so they are one range. */
[[nodiscard]] std::span<const sdk::format::AuthoredSceneEventKey>
scene_gates(const sdk::Catalog& catalog, std::uint32_t slotRow) noexcept {
    const auto gates = catalog.authored_scene_event_keys();
    const auto first = std::lower_bound(
        gates.begin(), gates.end(), slotRow, [](const auto& row, std::uint32_t index) {
            return row.sceneSlotIndex < index;
        });
    const auto last =
        std::upper_bound(first, gates.end(), slotRow, [](std::uint32_t index, const auto& row) {
            return index < row.sceneSlotIndex;
        });
    return gates.subspan(static_cast<std::size_t>(first - gates.begin()),
                         static_cast<std::size_t>(last - first));
}

/** Resolves the selection's linked squads and cast once, when the selection changes. */
[[nodiscard]] bool materialize(const sdk::BoundView& view,
                               std::uint32_t occurrenceRow,
                               std::uint32_t slotRow) noexcept {
    const DetailKey key{view.catalog.get(), view.scenarioRow, occurrenceRow, slotRow};
    if (key == g_detailKey) {
        return true;
    }
    try {
        g_detailKey = {};
        g_linkedSquads.clear();
        g_sentKeys.clear();
        g_lastAction = DetailAction::none;
        const sdk::Catalog& catalog = *view.catalog;
        const auto occurrences = catalog.occurrences();
        const auto slots = catalog.slots();
        const auto squads = catalog.squads();
        const auto allEdges = catalog.authored_scene_squad_edges();
        const auto edges = sdk::slot_authored_scene_squad_edges(catalog, slots[slotRow]);
        const std::uint32_t stateRow = occurrences[occurrenceRow].stateIndex;
        for (std::size_t squadRow = 0; squadRow < squads.size() && !edges.empty(); ++squadRow) {
            const sdk::format::Squad& squad = squads[squadRow];
            if (squad.scenarioIndex != view.scenarioRow
                || squad.occurrenceIndex >= occurrences.size()
                || occurrences[squad.occurrenceIndex].stateIndex != stateRow) {
                continue;
            }
            for (const sdk::format::AuthoredSceneSquadEdge& edge : edges) {
                if (edge.squadSlotIndex == squad.slotIndex) {
                    g_linkedSquads.push_back({static_cast<std::uint32_t>(&edge - allEdges.data()),
                                              static_cast<std::uint32_t>(squadRow)});
                }
            }
        }
        g_castStatus = mission::resolve_scene_spawn_plan(view, occurrenceRow, slotRow, g_cast);
        g_detailKey = key;
        return true;
    } catch (...) {
        g_detailKey = {};
        g_linkedSquads.clear();
        return false;
    }
}

/** Remembers one manual action's answer for the feedback line. */
void record(DetailAction action, std::uint32_t eventKey, mission::SceneStatus status) noexcept {
    g_lastAction = action;
    g_lastEventKey = eventKey;
    g_lastStatus = status;
}

/** Draws the start and stop buttons and the answer to the last manual action. */
void draw_actions(const sdk::BoundView& view,
                  std::uint32_t occurrenceRow,
                  std::uint32_t slotRow,
                  bool ready) noexcept {
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Start (new generation)")) {
        record(DetailAction::start,
               0,
               mission::activate_authored_scene(view, occurrenceRow, slotRow));
        g_sentKeys.clear();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Starts the scene without preparing its cast, and forgets its events.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        record(DetailAction::stop, 0, mission::stop_authored_scene(view, occurrenceRow, slotRow));
        g_sentKeys.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    switch (g_lastAction) {
    case DetailAction::none:
        ImGui::TextDisabled("no action yet");
        break;
    case DetailAction::start:
        ImGui::TextDisabled("start: %s", mission::status_name(g_lastStatus));
        break;
    case DetailAction::stop:
        ImGui::TextDisabled("stop: %s", mission::status_name(g_lastStatus));
        break;
    case DetailAction::event:
        ImGui::TextDisabled("event 0x%08X: %s",
                            static_cast<unsigned>(g_lastEventKey),
                            mission::status_name(g_lastStatus));
        break;
    }
}

/** Draws the scene graph's gates in order, each with its own send action. */
void draw_gates(const sdk::BoundView& view,
                std::uint32_t occurrenceRow,
                std::uint32_t slotRow,
                bool ready) noexcept {
    const auto gates = scene_gates(*view.catalog, slotRow);
    ImGui::SeparatorText("Events");
    if (gates.empty()) {
        ImGui::TextDisabled("The SDK lists no event gate for this scene.");
        return;
    }
    ImGui::TextDisabled("%zu gates. An event stays set until the scene is started again or "
                        "stopped; ordinal -1 is the gate that opens a scene.",
                        gates.size());
    if (!ImGui::BeginTable(
            "##sdk_scene_gates", 6, kSceneTableFlags, table_layout::size(gates.size()))) {
        return;
    }
    // The first column is the number mission scripts and the Sunrise Scenes editor use: the
    // position in the SDK's event_keys list. The gate ordinal is a different number.
    ImGui::TableSetupColumn("event # (scripts)");
    ImGui::TableSetupColumn("key");
    ImGui::TableSetupColumn("gate ordinal");
    ImGui::TableSetupColumn("gate offset");
    ImGui::TableSetupColumn("sent here");
    ImGui::TableSetupColumn("action");
    table_layout::frozen_headers();
    for (std::size_t index = 0; index < gates.size(); ++index) {
        const sdk::format::AuthoredSceneEventKey& gate = gates[index];
        const bool sent =
            std::find(g_sentKeys.begin(), g_sentKeys.end(), gate.key) != g_sentKeys.end();
        ImGui::PushID(static_cast<int>(index));
        table_layout::next_row();
        ImGui::TableNextColumn();
        ImGui::Text("%zu", index + 1);
        ImGui::TableNextColumn();
        ImGui::Text("0x%08X", static_cast<unsigned>(gate.key));
        ImGui::TableNextColumn();
        ImGui::Text("%d", static_cast<int>(gate.ordinal));
        ImGui::TableNextColumn();
        ImGui::Text("0x%X", static_cast<unsigned>(gate.gateOffset));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sent ? "yes" : "-");
        ImGui::TableNextColumn();
        // The wire reserves zero for "no event" and all-one bits for "not a key".
        const bool sendable = ready && gate.key != 0 && gate.key != 0xFFFFFFFFU;
        ImGui::BeginDisabled(!sendable);
        if (ImGui::Button("Send")) {
            const mission::SceneStatus status =
                mission::signal_authored_scene(view, occurrenceRow, slotRow, gate.key);
            record(DetailAction::event, gate.key, status);
            if (status == mission::SceneStatus::queued && !sent) {
                try {
                    g_sentKeys.push_back(gate.key);
                } catch (...) {
                    g_sentKeys.clear();
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/** Draws the actor controls the host would bind, each with the squad it draws from. */
void draw_cast(const sdk::Catalog& catalog) noexcept {
    ImGui::SeparatorText("Cast the host prepares (activate{spawn = true})");
    if (g_castStatus != mission::SceneStatus::ready) {
        ImGui::TextDisabled("unresolved: %s", mission::status_name(g_castStatus));
        return;
    }
    ImGui::TextDisabled("%zu actor controls paired with a squad; %zu squad participants left to "
                        "the client",
                        g_cast.count,
                        g_cast.omitted);
    if (g_cast.count == 0
        || !ImGui::BeginTable(
            "##sdk_scene_cast", 2, kSceneTableFlags, table_layout::size(g_cast.count))) {
        return;
    }
    ImGui::TableSetupColumn("actor control (type 2)", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("source squad", ImGuiTableColumnFlags_WidthStretch);
    table_layout::frozen_headers();
    const auto slots = catalog.slots();
    const auto squads = catalog.squads();
    for (std::size_t index = 0; index < g_cast.count; ++index) {
        const mission::SceneSpawnPair& pair = g_cast.pairs[index];
        table_layout::next_row();
        ImGui::TableNextColumn();
        if (pair.actorSlotRow < slots.size()) {
            const sdk::format::Slot& actor = slots[pair.actorSlotRow];
            const std::string_view name = display_text(catalog, actor.name);
            ImGui::Text("%.*s\nindex %u / row %u",
                        print_length(name),
                        name.data(),
                        static_cast<unsigned>(actor.slotIndex),
                        static_cast<unsigned>(pair.actorSlotRow));
        } else {
            ImGui::TextDisabled("invalid");
        }
        ImGui::TableNextColumn();
        if (pair.squadRow < squads.size() && squads[pair.squadRow].slotIndex < slots.size()) {
            const sdk::format::Slot& source = slots[squads[pair.squadRow].slotIndex];
            const std::string_view name = display_text(catalog, source.name);
            ImGui::Text("%.*s\nsquad row %u",
                        print_length(name),
                        name.data(),
                        static_cast<unsigned>(pair.squadRow));
        } else {
            ImGui::TextDisabled("invalid");
        }
    }
    ImGui::EndTable();
}

/** Draws one linked squad's members and the points it can be placed at. */
void draw_squad(const sdk::Catalog& catalog, const LinkedSquad& linked) noexcept {
    const auto slots = catalog.slots();
    const auto squads = catalog.squads();
    const auto actorClasses = catalog.actor_classes();
    if (linked.squadRow >= squads.size() || squads[linked.squadRow].slotIndex >= slots.size()) {
        return;
    }
    const sdk::format::Squad& squad = squads[linked.squadRow];
    const sdk::format::Slot& slot = slots[squad.slotIndex];
    const std::string_view name = display_text(catalog, slot.name);
    const auto members = sdk::squad_members(catalog, squad);
    const auto anchors = sdk::squad_anchors(catalog, squad);
    std::array<char, 256> label{};
    std::snprintf(label.data(),
                  label.size(),
                  "%.*s  (%zu members, %zu points)###squad",
                  print_length(name),
                  name.data(),
                  members.size(),
                  anchors.size());
    ImGui::PushID(static_cast<int>(linked.squadRow));
    if (ImGui::TreeNode(label.data())) {
        const std::string_view squadId = display_text(catalog, squad.id);
        ImGui::TextDisabled("%.*s\nsquad row %u / slot index %u / slot row %u",
                            print_length(squadId),
                            squadId.data(),
                            static_cast<unsigned>(linked.squadRow),
                            static_cast<unsigned>(slot.slotIndex),
                            static_cast<unsigned>(squad.slotIndex));
        for (const sdk::format::SquadMember& member : members) {
            const std::string_view actorClass =
                member.actorClassIndex < actorClasses.size()
                    ? display_text(catalog, actorClasses[member.actorClassIndex].id)
                    : std::string_view("unknown actor class");
            ImGui::BulletText("member %u: %.*s, default count %d",
                              static_cast<unsigned>(member.memberOrdinal),
                              print_length(actorClass),
                              actorClass.data(),
                              static_cast<int>(member.defaultCount));
        }
        for (const sdk::format::SquadAnchor& anchor : anchors) {
            ImGui::BulletText("point %u: %.2f, %.2f, %.2f",
                              static_cast<unsigned>(anchor.pointOrdinal),
                              static_cast<double>(std::bit_cast<float>(anchor.positionBits[0])),
                              static_cast<double>(std::bit_cast<float>(anchor.positionBits[1])),
                              static_cast<double>(std::bit_cast<float>(anchor.positionBits[2])));
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

} // namespace

void draw_scene_details(const sdk::BoundView& view,
                        std::uint32_t occurrenceRow,
                        std::uint32_t slotRow,
                        mission::SceneStatus availability) noexcept {
    if (view.catalog == nullptr || occurrenceRow >= view.catalog->occurrences().size()
        || slotRow >= view.catalog->slots().size() || !materialize(view, occurrenceRow, slotRow)) {
        ImGui::TextDisabled("Scene details unavailable.");
        return;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const sdk::format::Slot& slot = catalog.slots()[slotRow];
    const std::string_view name = display_text(catalog, slot.name);
    const std::string_view id = display_text(catalog, slot.id);
    const bool ready = availability == mission::SceneStatus::ready;

    ImGui::SeparatorText("Selected scene");
    ImGui::Text("%.*s", print_length(name), name.data());
    ImGui::TextDisabled("%.*s\noccurrence row %u / slot row %u / availability: %s",
                        print_length(id),
                        id.data(),
                        static_cast<unsigned>(occurrenceRow),
                        static_cast<unsigned>(slotRow),
                        mission::status_name(availability));
    for (const sdk::format::AuthoredSceneResource& resource :
         sdk::slot_authored_scene_resources(catalog, slot)) {
        const std::string_view resourceId = display_text(catalog, resource.id);
        ImGui::TextDisabled("resource 0x%08X / config 0x%08X\n%.*s",
                            static_cast<unsigned>(resource.resourceTag),
                            static_cast<unsigned>(resource.configTag),
                            print_length(resourceId),
                            resourceId.data());
    }
    draw_actions(view, occurrenceRow, slotRow, ready);
    draw_gates(view, occurrenceRow, slotRow, ready);
    draw_cast(catalog);

    const auto edges = sdk::slot_authored_scene_squad_edges(catalog, slot);
    ImGui::SeparatorText("Squads the scene draws actors from");
    ImGui::TextDisabled(
        "%zu squad edges, %zu squads in this state", edges.size(), g_linkedSquads.size());
    for (const LinkedSquad& linked : g_linkedSquads) {
        draw_squad(catalog, linked);
    }
    // An edge whose squad has no occurrence in this state still names its slot.
    for (const sdk::format::AuthoredSceneSquadEdge& edge : edges) {
        const bool found = std::any_of(
            g_linkedSquads.begin(), g_linkedSquads.end(), [&](const LinkedSquad& linked) {
                return linked.squadRow < catalog.squads().size()
                       && catalog.squads()[linked.squadRow].slotIndex == edge.squadSlotIndex;
            });
        const sdk::format::Slot* const target =
            found ? nullptr : sdk::authored_scene_linked_squad_slot(catalog, edge);
        if (target != nullptr) {
            const std::string_view targetName = display_text(catalog, target->name);
            ImGui::BulletText("%.*s: no squad row in this state",
                              print_length(targetName),
                              targetName.data());
        }
    }
}

void reset_scene_detail_status() noexcept {
    g_lastAction = DetailAction::none;
    g_sentKeys.clear();
}

} // namespace sunrise::server::ui::activity_host::sdk_mission_view
