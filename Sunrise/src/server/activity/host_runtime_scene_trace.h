#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>

#include "../../core/logging/log.h"
#include "host_runtime.h"

namespace sunrise::server::activity::host::scene_trace {

/** @return True for the three type-43 outputs: activation, event update and stop. */
[[nodiscard]] inline bool traced(ScriptableOverrideKind kind) noexcept {
    return kind == ScriptableOverrideKind::authoredScene
           || kind == ScriptableOverrideKind::authoredSceneEvent
           || kind == ScriptableOverrideKind::authoredSceneStop;
}

/** @return Short text for one traced kind. */
[[nodiscard]] inline const char* kind_name(ScriptableOverrideKind kind) noexcept {
    return kind == ScriptableOverrideKind::authoredScene        ? "activate"
           : kind == ScriptableOverrideKind::authoredSceneEvent ? "event"
                                                                : "stop";
}

/**
 * Logs one step of a type-43 output with its exact Auth body, so a scripted scene can be
 * compared byte for byte with one driven from the panel. Other kinds log nothing.
 * @param stage Where the output is: encode, transport or cancel.
 * @param result What happened there.
 * @param pending The output; its body is logged up to its byte count.
 * @param scripted True when a mission script's reservation owns the output.
 * @param eventKey The requested event key, or zero.
 */
inline void write(const char* stage,
                  const char* result,
                  const PendingScriptableOverride& pending,
                  bool scripted,
                  std::uint32_t eventKey) noexcept {
    namespace log = core::log;
    if (!traced(pending.kind) || !log::accepts(log::Channel::server, log::Level::info)) {
        return;
    }
    std::array<char, log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=scene_auth stage=%s result=%s kind=%s source=%s slot=%u object=0x%08X key=0x%08X "
        "rev=%llu gen=%llu client_gen=%llu bits=%u body=",
        stage,
        result,
        kind_name(pending.kind),
        scripted ? "script" : "panel",
        static_cast<unsigned>(pending.target.slotIndex),
        static_cast<unsigned>(pending.target.objectTag),
        static_cast<unsigned>(eventKey),
        static_cast<unsigned long long>(pending.revision),
        static_cast<unsigned long long>(pending.generation),
        static_cast<unsigned long long>(pending.expectedActivityClientGeneration),
        static_cast<unsigned>(pending.bitCount));
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    auto length = static_cast<std::size_t>(prefix);
    const std::size_t bytes = (std::min)(static_cast<std::size_t>(pending.byteCount),
                                         pending.body.size());
    (void)log::append_hex(line, length, std::span(pending.body).first(bytes));
    log::write(log::Channel::server, log::Level::info, std::string_view(line.data(), length));
}

} // namespace sunrise::server::activity::host::scene_trace
