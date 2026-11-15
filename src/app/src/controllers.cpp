#include "toh/app/controllers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "toh/app/session.h"

namespace toh::app {

void Controllers::learn(ControlTarget target, int index) {
    learning_ = true;
    learn_target_ = target;
    learn_index_ = index;
}

const ControlMapping* Controllers::find(ControlTarget target, int index) const noexcept {
    for (const ControlMapping& mapping : mappings_) {
        if (mapping.target == target && mapping.index == index) {
            return &mapping;
        }
    }
    return nullptr;
}

void Controllers::forget(ControlTarget target, int index) {
    mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(),
                                   [&](const ControlMapping& m) { return m.target == target && m.index == index; }),
                    mappings_.end());
}

bool Controllers::handle(Session& session, int channel, int controller, float value, std::vector<Action>& actions) {
    value = std::clamp(value, 0.0f, 1.0f);
    if (learning_) {
        learning_ = false;
        forget(learn_target_, learn_index_);
        mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(),
                                       [&](const ControlMapping& m) {
                                           return m.channel == channel && m.controller == controller;
                                       }),
                        mappings_.end());
        ControlMapping mapping;
        mapping.channel = channel;
        mapping.controller = controller;
        mapping.target = learn_target_;
        mapping.index = learn_index_;
        mapping.last = value;  // a button pressed to learn it does not act yet
        mappings_.push_back(mapping);
        return true;
    }
    for (ControlMapping& mapping : mappings_) {
        if (mapping.channel != channel || mapping.controller != controller) {
            continue;
        }
        const bool pressed = value >= 0.5f && mapping.last < 0.5f;
        mapping.last = value;
        const auto index = static_cast<std::size_t>(std::max(mapping.index, 0));
        switch (mapping.target) {
            case ControlTarget::MasterVolume:
                session.set_master_gain(2.0f * value);
                break;
            case ControlTarget::TrackVolume:
                session.set_track_gain(index, 2.0f * value);
                break;
            case ControlTarget::TrackPan: {
                const float pan = 2.0f * value - 1.0f;
                session.set_track_pan(index, std::fabs(pan) < 0.02f ? 0.0f : pan);
                break;
            }
            case ControlTarget::TrackMute:
                if (pressed && index < session.tracks().size()) {
                    session.set_track_mute(index, !session.tracks()[index].mute);
                }
                break;
            case ControlTarget::TrackSolo:
                if (pressed && index < session.tracks().size()) {
                    session.set_track_solo(index, !session.tracks()[index].solo);
                }
                break;
            case ControlTarget::GroupVolume:
                session.set_group_gain(index, 2.0f * value);
                break;
            case ControlTarget::Action:
                if (pressed && mapping.index >= 0 && mapping.index < static_cast<int>(kActionCount)) {
                    actions.push_back(static_cast<Action>(mapping.index));
                }
                break;
        }
    }
    return false;
}

std::vector<std::string> Controllers::to_lines() const {
    std::vector<std::string> lines;
    for (const ControlMapping& mapping : mappings_) {
        char line[64];
        std::snprintf(line, sizeof(line), "%d|%d|%d|%d", mapping.channel, mapping.controller,
                      static_cast<int>(mapping.target), mapping.index);
        lines.emplace_back(line);
    }
    return lines;
}

void Controllers::from_lines(const std::vector<std::string>& lines) {
    mappings_.clear();
    for (const std::string& line : lines) {
        int channel = 0;
        int controller = 0;
        int target = 0;
        int index = 0;
        if (std::sscanf(line.c_str(), "%d|%d|%d|%d", &channel, &controller, &target, &index) != 4 || channel < 0 ||
            channel > 15 || controller < 0 || controller > 127 || target < 0 ||
            target > static_cast<int>(ControlTarget::Action) || index < 0) {
            continue;
        }
        ControlMapping mapping;
        mapping.channel = channel;
        mapping.controller = controller;
        mapping.target = static_cast<ControlTarget>(target);
        mapping.index = index;
        mappings_.push_back(mapping);
    }
}

std::string control_text(const ControlMapping& mapping) {
    return "CC " + std::to_string(mapping.controller) + ", channel " + std::to_string(mapping.channel + 1);
}

}  // namespace toh::app
