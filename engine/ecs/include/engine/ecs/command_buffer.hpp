#pragma once

#include "engine/ecs/entity.hpp"
#include "engine/ecs/registry.hpp"

#include "engine/core/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace engine::ecs {

// An entity that a command buffer created but that does not exist until apply(). The id is 1-based, numbered in creation
// order, and meaningful only inside the buffer that issued it. ApplyReport::created maps it to the real handle.
struct PendingEntity {
    std::uint32_t id = 0;
};

// The entity a command acts on: a handle the world already issued, or a pending entity from the same buffer.
struct EntityRef {
    EntityRef() = default;
    EntityRef(EntityHandle live) noexcept : handle(live) {}
    EntityRef(PendingEntity pending) noexcept : pending_id(pending.id) {}

    EntityHandle handle{};
    std::uint32_t pending_id = 0;  // non-zero when the command targets a pending entity
};

enum class RejectReason {
    EntityNotAlive,    // stale, foreign, destroyed by an earlier accepted command, or a pending entity that is not alive
    ComponentPresent,  // add of a component the entity already has, including one an earlier accepted command added
    ComponentAbsent    // remove of a component the entity does not have at that point in the buffer
};

struct Rejection {
    std::size_t command = 0;  // position in the buffer, in recording order
    RejectReason reason = RejectReason::EntityNotAlive;
};

struct ApplyReport {
    std::vector<EntityHandle> created;  // created[k] is the handle of PendingEntity{k + 1}; it may be stale if destroyed later
    std::vector<Rejection> rejected;    // in recording order
    std::size_t applied = 0;
};

namespace detail {

// Type-erased view of one recorded component operation, so one buffer can hold any component type.
class ComponentOp {
public:
    ComponentOp() = default;
    ComponentOp(const ComponentOp&) = delete;
    ComponentOp& operator=(const ComponentOp&) = delete;
    virtual ~ComponentOp() = default;

    [[nodiscard]] virtual std::type_index type() const noexcept = 0;
    [[nodiscard]] virtual bool present(const Registry& registry, EntityHandle entity) const = 0;
    virtual void add(Registry& registry, EntityHandle entity) = 0;
    virtual void remove(Registry& registry, EntityHandle entity) = 0;
};

// An add holds its value until apply, so a move-only component can be recorded. A remove holds no value.
template <typename T>
class TypedComponentOp final : public ComponentOp {
public:
    TypedComponentOp() = default;

    template <typename... Args>
    explicit TypedComponentOp(std::in_place_t, Args&&... args) : value_(std::in_place, std::forward<Args>(args)...) {}

    [[nodiscard]] std::type_index type() const noexcept override {
        return std::type_index(typeid(T));
    }
    [[nodiscard]] bool present(const Registry& registry, EntityHandle entity) const override {
        return registry.has<T>(entity);
    }
    void add(Registry& registry, EntityHandle entity) override {
        ENGINE_ASSERT(value_.has_value(), "an add command must carry a component value");
        registry.add<T>(entity, std::move(*value_));
    }
    void remove(Registry& registry, EntityHandle entity) override {
        registry.remove<T>(entity);
    }

private:
    std::optional<T> value_;
};

}  // namespace detail

// Records structural changes (create, destroy, add, remove) so that a system can make them while other systems read the
// same registry. Recording never touches the Registry. apply() first verifies the whole buffer against the registry and
// the effects of earlier commands in the buffer, then runs the accepted commands in recording order. Rejected commands
// are reported and skipped; they never leave a partial change behind.
//
// A CommandBuffer is not thread-safe. Each system records into its own buffer, and the owner applies the buffers at a
// synchronization point.
class CommandBuffer {
public:
    CommandBuffer() = default;
    CommandBuffer(CommandBuffer&&) noexcept = default;
    CommandBuffer& operator=(CommandBuffer&&) noexcept = default;
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    ~CommandBuffer() = default;

    // Reserves an entity that exists only after apply(). The returned id can be used by later commands in this buffer.
    [[nodiscard]] PendingEntity create();

    // Destroys the entity and every component it holds.
    void destroy(EntityRef entity);

    // Adds a T built from args. A component value moves into the buffer, so move-only types are supported.
    template <typename T, typename... Args>
    void add(EntityRef entity, Args&&... args) {
        commands_.push_back(Command{CommandKind::Add, entity,
                                    std::make_unique<detail::TypedComponentOp<T>>(std::in_place, std::forward<Args>(args)...)});
    }

    // Removes the entity's T.
    template <typename T>
    void remove(EntityRef entity) {
        commands_.push_back(Command{CommandKind::Remove, entity, std::make_unique<detail::TypedComponentOp<T>>()});
    }

    // Verifies every command, applies the accepted ones in recording order, and empties the buffer.
    [[nodiscard]] ApplyReport apply(Registry& registry);

    // Drops every recorded command without applying it.
    void clear() noexcept {
        commands_.clear();
        pending_count_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return commands_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return commands_.empty();
    }

private:
    enum class CommandKind : std::uint8_t { Create, Destroy, Add, Remove };

    struct Command {
        CommandKind kind = CommandKind::Create;
        EntityRef target;
        std::unique_ptr<detail::ComponentOp> op;  // Add and Remove only
    };

    std::vector<Command> commands_;
    std::uint32_t pending_count_ = 0;
};

}  // namespace engine::ecs
