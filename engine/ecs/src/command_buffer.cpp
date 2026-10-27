#include "engine/ecs/command_buffer.hpp"

#include "engine/core/assert.hpp"

#include <map>
#include <set>
#include <utility>

namespace engine::ecs {
namespace {

// Pending entities and live entities use different key spaces, so one table can track both.
constexpr std::uint64_t kPendingBit = std::uint64_t{1} << 63;

// Index and generation together identify a live handle. The world id is checked separately through World::alive().
std::uint64_t live_key(EntityHandle entity) noexcept {
    return (std::uint64_t{entity.index} << 32) | entity.generation;
}

}  // namespace

PendingEntity CommandBuffer::create() {
    commands_.push_back(Command{CommandKind::Create, EntityRef{}, nullptr});
    return PendingEntity{++pending_count_};
}

void CommandBuffer::destroy(EntityRef entity) {
    commands_.push_back(Command{CommandKind::Destroy, entity, nullptr});
}

ApplyReport CommandBuffer::apply(Registry& registry) {
    ApplyReport report;

    // Pass 1: verify every command. Nothing in the registry changes here. The sets below describe what the accepted
    // commands would leave behind, so a later command sees the effects of earlier ones in the same buffer.
    std::vector<bool> accepted(commands_.size(), false);
    std::set<std::uint64_t> destroyed;                // live handles destroyed by an accepted command
    std::vector<bool> pending_alive;                  // indexed by pending id - 1
    std::map<std::pair<std::uint64_t, std::type_index>, bool> presence;  // (entity, type) -> presence after accepted commands

    for (std::size_t i = 0; i < commands_.size(); ++i) {
        const Command& command = commands_[i];
        if (command.kind == CommandKind::Create) {
            pending_alive.push_back(true);
            accepted[i] = true;
            continue;
        }

        const bool pending = command.target.pending_id != 0;
        bool alive = false;
        std::uint64_t key = 0;
        if (pending) {
            key = kPendingBit | command.target.pending_id;
            alive = command.target.pending_id <= pending_alive.size() && pending_alive[command.target.pending_id - 1];
        } else {
            key = live_key(command.target.handle);
            alive = registry.world().alive(command.target.handle) && destroyed.count(key) == 0;
        }
        if (!alive) {
            report.rejected.push_back({i, RejectReason::EntityNotAlive});
            continue;
        }

        if (command.kind == CommandKind::Destroy) {
            if (pending) {
                pending_alive[command.target.pending_id - 1] = false;
            } else {
                destroyed.insert(key);
            }
            accepted[i] = true;
            continue;
        }

        const std::pair<std::uint64_t, std::type_index> slot{key, command.op->type()};
        bool has = false;
        if (const auto known = presence.find(slot); known != presence.end()) {
            has = known->second;
        } else {
            has = !pending && command.op->present(registry, command.target.handle);
        }

        if (command.kind == CommandKind::Add) {
            if (has) {
                report.rejected.push_back({i, RejectReason::ComponentPresent});
                continue;
            }
            presence[slot] = true;
        } else {
            if (!has) {
                report.rejected.push_back({i, RejectReason::ComponentAbsent});
                continue;
            }
            presence[slot] = false;
        }
        accepted[i] = true;
    }

    // Pass 2: apply the accepted commands in recording order. Each pending id maps to the entity its create produced.
    std::vector<EntityHandle> created;  // created[k] is the handle of PendingEntity{k + 1}
    for (std::size_t i = 0; i < commands_.size(); ++i) {
        if (!accepted[i]) {
            continue;
        }
        const Command& command = commands_[i];
        ++report.applied;
        if (command.kind == CommandKind::Create) {
            created.push_back(registry.create());
            continue;
        }

        const EntityHandle entity =
            command.target.pending_id != 0 ? created[command.target.pending_id - 1] : command.target.handle;
        switch (command.kind) {
            case CommandKind::Create:
                break;
            case CommandKind::Destroy: {
                const bool destroyed_now = registry.destroy(entity);
                ENGINE_ASSERT(destroyed_now, "a command accepted by verification must destroy a live entity");
                break;
            }
            case CommandKind::Add:
                command.op->add(registry, entity);
                break;
            case CommandKind::Remove:
                command.op->remove(registry, entity);
                break;
        }
    }

    report.created = std::move(created);
    clear();
    return report;
}

}  // namespace engine::ecs
