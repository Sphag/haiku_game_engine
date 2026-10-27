// Scene graph (US-016): local and world transforms, dirty propagation, reparenting, cycle rejection, and child policies.
// CPU-only; these run on every OS.

#include "engine/scene/scene_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <vector>

using engine::ecs::EntityHandle;
using engine::scene::ChildPolicy;
using engine::scene::HierarchyError;
using engine::scene::ReparentMode;
using engine::scene::SceneGraph;
using engine::scene::Transform;

namespace {

Transform posed(glm::dvec3 position, glm::dquat rotation = glm::dquat(1.0, 0.0, 0.0, 0.0),
                glm::dvec3 scale = glm::dvec3(1.0, 1.0, 1.0)) {
    Transform result;
    result.position = position;
    result.rotation = rotation;
    result.scale = scale;
    return result;
}

glm::dquat turn(glm::dvec3 axis, double degrees) {
    return glm::angleAxis(glm::radians(degrees), axis);
}

bool near(glm::dvec3 actual, glm::dvec3 expected, double margin) {
    return glm::all(glm::lessThanEqual(glm::abs(actual - expected), glm::dvec3(margin, margin, margin)));
}

bool near(const glm::dmat4& actual, const glm::dmat4& expected, double margin) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::abs(actual[column][row] - expected[column][row]) > margin) {
                return false;
            }
        }
    }
    return true;
}

EntityHandle make(SceneGraph& graph, const Transform& initial, EntityHandle parent = {}) {
    auto created = graph.create(initial, parent);
    REQUIRE(created.ok());
    return created.value();
}

// The world matrix of a live node, copied so that later changes cannot alter it.
glm::dmat4 world_of(SceneGraph& graph, EntityHandle node) {
    const glm::dmat4* matrix = graph.world_matrix(node);
    REQUIRE(matrix != nullptr);
    return *matrix;
}

glm::dvec3 origin_of(SceneGraph& graph, EntityHandle node) {
    return glm::dvec3(world_of(graph, node)[3]);
}

}  // namespace

TEST_CASE("Scene graph composes local transforms into world matrices", "[unit]") {
    SceneGraph graph;
    const EntityHandle root = make(graph, posed(glm::dvec3(10.0, 0.0, 0.0), turn(glm::dvec3(0.0, 1.0, 0.0), 90.0)));
    const EntityHandle child = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0)), root);

    // The child's local +X is turned into -Z by the root, then moved by the root's translation.
    CHECK(near(origin_of(graph, child), glm::dvec3(10.0, 0.0, -1.0), 1e-12));
    CHECK(near(origin_of(graph, root), glm::dvec3(10.0, 0.0, 0.0), 1e-12));
    CHECK(graph.parent(child) == root);
    REQUIRE(graph.children(root) != nullptr);
    CHECK(graph.children(root)->size() == 1);
}

TEST_CASE("Scene graph scales children through a non-uniform parent", "[unit]") {
    SceneGraph graph;
    const EntityHandle parent = make(graph, posed(glm::dvec3(0.0), glm::dquat(1.0, 0.0, 0.0, 0.0), glm::dvec3(2.0, 3.0, 4.0)));
    // The child turns 90 degrees about Z. Its local +X and +Y then become parent-space +Y and -X, and the parent stretches them.
    const EntityHandle child = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0), turn(glm::dvec3(0.0, 0.0, 1.0), 90.0)), parent);

    const glm::dmat4 world = world_of(graph, child);
    CHECK(near(glm::dvec3(world[0]), glm::dvec3(0.0, 3.0, 0.0), 1e-12));
    CHECK(near(glm::dvec3(world[1]), glm::dvec3(-2.0, 0.0, 0.0), 1e-12));
    CHECK(near(glm::dvec3(world[3]), glm::dvec3(2.0, 0.0, 0.0), 1e-12));
}

TEST_CASE("Scene graph keeps a child orbiting a rotated parent", "[unit]") {
    SceneGraph graph;
    const EntityHandle parent = make(graph, posed(glm::dvec3(0.0), turn(glm::dvec3(0.0, 1.0, 0.0), 90.0)));
    const EntityHandle child = make(graph, posed(glm::dvec3(0.0, 2.0, 0.0)), parent);
    const EntityHandle grandchild = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0)), child);

    CHECK(near(origin_of(graph, child), glm::dvec3(0.0, 2.0, 0.0), 1e-12));
    CHECK(near(origin_of(graph, grandchild), glm::dvec3(0.0, 2.0, -1.0), 1e-12));
}

TEST_CASE("Scene graph dirty propagation reaches descendants and recomputes on demand", "[unit]") {
    SceneGraph graph;
    const EntityHandle root = make(graph, posed(glm::dvec3(0.0)));
    const EntityHandle middle = make(graph, posed(glm::dvec3(0.0, 1.0, 0.0)), root);
    const EntityHandle leaf = make(graph, posed(glm::dvec3(0.0, 0.0, 1.0)), middle);

    CHECK(graph.update() == 3);
    CHECK(graph.update() == 0);
    CHECK_FALSE(graph.dirty(leaf));

    REQUIRE(graph.set_local(root, posed(glm::dvec3(5.0, 0.0, 0.0))).ok());
    CHECK(graph.dirty(root));
    CHECK(graph.dirty(middle));
    CHECK(graph.dirty(leaf));

    // Reading one node refreshes it and its dirty ancestors. Its dirty descendants are not touched.
    CHECK(near(origin_of(graph, leaf), glm::dvec3(5.0, 1.0, 1.0), 1e-12));
    CHECK_FALSE(graph.dirty(middle));
    CHECK(graph.update() == 0);
}

TEST_CASE("Scene graph dirty propagation stays inside the changed subtree", "[unit]") {
    SceneGraph graph;
    const EntityHandle root = make(graph, posed(glm::dvec3(0.0)));
    const EntityHandle left = make(graph, posed(glm::dvec3(-1.0, 0.0, 0.0)), root);
    const EntityHandle right = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0)), root);
    const EntityHandle left_leaf = make(graph, posed(glm::dvec3(0.0, 1.0, 0.0)), left);
    const EntityHandle left_leaf_two = make(graph, posed(glm::dvec3(0.0, 2.0, 0.0)), left);
    REQUIRE(graph.update() == 5);

    REQUIRE(graph.set_local(left, posed(glm::dvec3(-3.0, 0.0, 0.0))).ok());
    CHECK_FALSE(graph.dirty(root));
    CHECK_FALSE(graph.dirty(right));
    CHECK(graph.dirty(left));
    CHECK(graph.dirty(left_leaf));
    CHECK(graph.dirty(left_leaf_two));

    // Setting the same node again before an update still recomputes the subtree once.
    REQUIRE(graph.set_local(left, posed(glm::dvec3(-4.0, 0.0, 0.0))).ok());
    CHECK(graph.update() == 3);
    CHECK(near(origin_of(graph, left_leaf_two), glm::dvec3(-4.0, 2.0, 0.0), 1e-12));
}

TEST_CASE("Scene graph reparenting keeps the world transform when asked", "[unit]") {
    SceneGraph graph;
    const EntityHandle first_parent = make(graph, posed(glm::dvec3(0.0), turn(glm::dvec3(0.0, 1.0, 0.0), 90.0)));
    const EntityHandle second_parent =
        make(graph, posed(glm::dvec3(4.0, 0.0, 0.0), turn(glm::dvec3(0.0, 0.0, 1.0), 30.0), glm::dvec3(2.0)));
    const EntityHandle child = make(graph, posed(glm::dvec3(1.0, 2.0, 3.0), turn(glm::dvec3(1.0, 0.0, 0.0), 20.0)), first_parent);
    const glm::dmat4 before = world_of(graph, child);

    REQUIRE(graph.reparent(child, second_parent, ReparentMode::KeepWorldTransform).ok());
    CHECK(graph.parent(child) == second_parent);
    CHECK(near(world_of(graph, child), before, 1e-9));
    REQUIRE(graph.local(child) != nullptr);
    CHECK_FALSE(near(glm::dvec3(graph.local(child)->position), glm::dvec3(1.0, 2.0, 3.0), 1e-6));
}

TEST_CASE("Scene graph reparenting keeps the local transform when asked", "[unit]") {
    SceneGraph graph;
    const EntityHandle first_parent = make(graph, posed(glm::dvec3(0.0), turn(glm::dvec3(0.0, 1.0, 0.0), 90.0)));
    const EntityHandle second_parent = make(graph, posed(glm::dvec3(4.0, 0.0, 0.0)));
    const EntityHandle child = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0)), first_parent);
    CHECK(near(origin_of(graph, child), glm::dvec3(0.0, 0.0, -1.0), 1e-12));

    REQUIRE(graph.reparent(child, second_parent, ReparentMode::KeepLocalTransform).ok());
    REQUIRE(graph.local(child) != nullptr);
    CHECK(near(glm::dvec3(graph.local(child)->position), glm::dvec3(1.0, 0.0, 0.0), 1e-12));
    CHECK(near(origin_of(graph, child), glm::dvec3(5.0, 0.0, 0.0), 1e-12));
}

TEST_CASE("Scene graph reparenting to a root keeps the world transform when asked", "[unit]") {
    SceneGraph graph;
    const EntityHandle parent = make(graph, posed(glm::dvec3(2.0, 0.0, 0.0), turn(glm::dvec3(0.0, 0.0, 1.0), 90.0)));
    const EntityHandle child = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0), glm::dquat(1.0, 0.0, 0.0, 0.0), glm::dvec3(3.0)), parent);
    const glm::dmat4 before = world_of(graph, child);

    REQUIRE(graph.reparent(child, EntityHandle{}, ReparentMode::KeepWorldTransform).ok());
    CHECK_FALSE(graph.parent(child).valid());
    CHECK(near(world_of(graph, child), before, 1e-9));
    const std::vector<EntityHandle> roots = graph.roots();
    CHECK(roots.size() == 2);
}

TEST_CASE("Scene graph rejects cyclic hierarchies without changing the graph", "[unit]") {
    SceneGraph graph;
    const EntityHandle root = make(graph, posed(glm::dvec3(0.0)));
    const EntityHandle middle = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0)), root);
    const EntityHandle leaf = make(graph, posed(glm::dvec3(0.0, 1.0, 0.0)), middle);

    for (const ReparentMode mode : {ReparentMode::KeepWorldTransform, ReparentMode::KeepLocalTransform}) {
        const auto self = graph.reparent(root, root, mode);
        REQUIRE_FALSE(self.ok());
        CHECK(self.error() == HierarchyError::Cycle);

        const auto into_descendant = graph.reparent(root, leaf, mode);
        REQUIRE_FALSE(into_descendant.ok());
        CHECK(into_descendant.error() == HierarchyError::Cycle);

        const auto child_into_own_child = graph.reparent(middle, leaf, mode);
        REQUIRE_FALSE(child_into_own_child.ok());
        CHECK(child_into_own_child.error() == HierarchyError::Cycle);
    }

    CHECK_FALSE(graph.parent(root).valid());
    CHECK(graph.parent(middle) == root);
    CHECK(graph.parent(leaf) == middle);
    CHECK(near(origin_of(graph, leaf), glm::dvec3(1.0, 1.0, 0.0), 1e-12));
}

TEST_CASE("Scene graph rejects a singular parent when keeping the world transform", "[unit]") {
    SceneGraph graph;
    const EntityHandle flattened = make(graph, posed(glm::dvec3(0.0), glm::dquat(1.0, 0.0, 0.0, 0.0), glm::dvec3(0.0, 1.0, 1.0)));
    const EntityHandle child = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0)));

    const auto keep_world = graph.reparent(child, flattened, ReparentMode::KeepWorldTransform);
    REQUIRE_FALSE(keep_world.ok());
    CHECK(keep_world.error() == HierarchyError::SingularParent);
    CHECK_FALSE(graph.parent(child).valid());

    // Keeping the local transform never needs the parent's inverse, so it still works.
    CHECK(graph.reparent(child, flattened, ReparentMode::KeepLocalTransform).ok());
    CHECK(graph.parent(child) == flattened);
}

TEST_CASE("Scene graph refuses a world-preserving reparent that needs shear and changes nothing", "[unit]") {
    SceneGraph graph;
    const EntityHandle stretched = make(graph, posed(glm::dvec3(0.0), glm::dquat(1.0, 0.0, 0.0, 0.0), glm::dvec3(2.0, 1.0, 1.0)));
    // S * R with a non-uniform S and a 45 degree turn has non-orthogonal axes, so no Transform reproduces it.
    const EntityHandle child = make(graph, posed(glm::dvec3(0.0), turn(glm::dvec3(0.0, 0.0, 1.0), 45.0)), stretched);
    const glm::dmat4 before = world_of(graph, child);
    const Transform local_before = *graph.local(child);

    const auto moved = graph.reparent(child, EntityHandle{}, ReparentMode::KeepWorldTransform);
    REQUIRE_FALSE(moved.ok());
    CHECK(moved.error() == HierarchyError::NotRepresentable);
    CHECK(graph.parent(child) == stretched);
    CHECK(near(graph.local(child)->position, local_before.position, 0.0));
    CHECK(near(world_of(graph, child), before, 1e-12));

    // The same move keeping the local transform is always representable.
    CHECK(graph.reparent(child, EntityHandle{}, ReparentMode::KeepLocalTransform).ok());
}

TEST_CASE("Scene graph rejects invalid transforms and stale handles", "[unit]") {
    SceneGraph graph;
    const double nan = std::numeric_limits<double>::quiet_NaN();

    const auto bad_position = graph.create(posed(glm::dvec3(nan, 0.0, 0.0)));
    REQUIRE_FALSE(bad_position.ok());
    CHECK(bad_position.error() == HierarchyError::InvalidTransform);

    const auto bad_rotation = graph.create(posed(glm::dvec3(0.0), glm::dquat(2.0, 0.0, 0.0, 0.0)));
    REQUIRE_FALSE(bad_rotation.ok());
    CHECK(bad_rotation.error() == HierarchyError::InvalidTransform);
    CHECK(graph.size() == 0);

    const EntityHandle node = make(graph, posed(glm::dvec3(0.0)));
    REQUIRE(graph.destroy(node, ChildPolicy::DestroySubtree).ok());
    CHECK_FALSE(graph.alive(node));
    CHECK(graph.world_matrix(node) == nullptr);
    CHECK(graph.local(node) == nullptr);
    CHECK(graph.children(node) == nullptr);

    const auto moved_dead = graph.set_local(node, posed(glm::dvec3(1.0)));
    REQUIRE_FALSE(moved_dead.ok());
    CHECK(moved_dead.error() == HierarchyError::NotAlive);

    const auto created_under_dead = graph.create(posed(glm::dvec3(0.0)), node);
    REQUIRE_FALSE(created_under_dead.ok());
    CHECK(created_under_dead.error() == HierarchyError::NotAlive);

    // This node takes the slot that the destroyed node freed. The old handle must not reach it.
    const EntityHandle other = make(graph, posed(glm::dvec3(9.0)));
    CHECK(other.index == node.index);
    CHECK_FALSE(graph.alive(node));
    CHECK(graph.world_matrix(node) == nullptr);
    CHECK(near(origin_of(graph, other), glm::dvec3(9.0), 1e-12));

    // A stale handle used as a parent must not reach the node that reuses its slot.
    const auto under_stale = graph.reparent(other, node, ReparentMode::KeepLocalTransform);
    REQUIRE_FALSE(under_stale.ok());
    CHECK(under_stale.error() == HierarchyError::NotAlive);
    CHECK_FALSE(graph.parent(other).valid());
}

TEST_CASE("Scene graph destroy with DestroySubtree removes every descendant", "[unit]") {
    SceneGraph graph;
    const EntityHandle root = make(graph, posed(glm::dvec3(0.0)));
    const EntityHandle branch = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0)), root);
    const EntityHandle branch_leaf = make(graph, posed(glm::dvec3(0.0, 1.0, 0.0)), branch);
    const EntityHandle sibling = make(graph, posed(glm::dvec3(-1.0, 0.0, 0.0)), root);
    REQUIRE(graph.size() == 4);

    REQUIRE(graph.destroy(branch, ChildPolicy::DestroySubtree).ok());
    CHECK_FALSE(graph.alive(branch));
    CHECK_FALSE(graph.alive(branch_leaf));
    CHECK(graph.alive(sibling));
    CHECK(graph.size() == 2);
    REQUIRE(graph.children(root) != nullptr);
    CHECK(*graph.children(root) == std::vector<EntityHandle>{sibling});
    CHECK(near(origin_of(graph, sibling), glm::dvec3(-1.0, 0.0, 0.0), 1e-12));
}

TEST_CASE("Scene graph destroy with ReparentToParent keeps the children's world transforms", "[unit]") {
    SceneGraph graph;
    const EntityHandle root = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0), turn(glm::dvec3(0.0, 1.0, 0.0), 90.0)));
    const EntityHandle middle = make(graph, posed(glm::dvec3(0.0, 0.0, 2.0), turn(glm::dvec3(0.0, 0.0, 1.0), 30.0), glm::dvec3(2.0)), root);
    const EntityHandle leaf = make(graph, posed(glm::dvec3(1.0, 0.0, 0.0)), middle);
    const EntityHandle sibling = make(graph, posed(glm::dvec3(0.0, 3.0, 0.0)), middle);
    const glm::dmat4 leaf_before = world_of(graph, leaf);
    const glm::dmat4 sibling_before = world_of(graph, sibling);

    REQUIRE(graph.destroy(middle, ChildPolicy::ReparentToParent).ok());
    CHECK_FALSE(graph.alive(middle));
    CHECK(graph.size() == 3);
    CHECK(graph.parent(leaf) == root);
    CHECK(graph.parent(sibling) == root);
    CHECK(near(world_of(graph, leaf), leaf_before, 1e-9));
    CHECK(near(world_of(graph, sibling), sibling_before, 1e-9));
}

TEST_CASE("Scene graph destroy with ReparentToParent of a root turns its children into roots", "[unit]") {
    SceneGraph graph;
    const EntityHandle root = make(graph, posed(glm::dvec3(0.0)));
    const EntityHandle child = make(graph, posed(glm::dvec3(2.0, 0.0, 0.0)), root);

    REQUIRE(graph.destroy(root, ChildPolicy::ReparentToParent).ok());
    CHECK_FALSE(graph.parent(child).valid());
    CHECK(graph.roots() == std::vector<EntityHandle>{child});
    CHECK(near(origin_of(graph, child), glm::dvec3(2.0, 0.0, 0.0), 1e-12));
}

TEST_CASE("Scene graph destroy with ReparentToParent fails with no change when a child would need shear", "[unit]") {
    SceneGraph graph;
    const EntityHandle stretched = make(graph, posed(glm::dvec3(0.0), glm::dquat(1.0, 0.0, 0.0, 0.0), glm::dvec3(2.0, 1.0, 1.0)));
    const EntityHandle child = make(graph, posed(glm::dvec3(0.0), turn(glm::dvec3(0.0, 0.0, 1.0), 45.0)), stretched);
    const glm::dmat4 before = world_of(graph, child);

    const auto destroyed = graph.destroy(stretched, ChildPolicy::ReparentToParent);
    REQUIRE_FALSE(destroyed.ok());
    CHECK(destroyed.error() == HierarchyError::NotRepresentable);
    CHECK(graph.alive(stretched));
    CHECK(graph.parent(child) == stretched);
    CHECK(near(world_of(graph, child), before, 1e-12));
    CHECK(graph.size() == 2);
}

TEST_CASE("Scene graph handles a 10000-level deep hierarchy without recursion", "[unit]") {
    constexpr std::size_t kDepth = 10000;
    SceneGraph graph;
    std::vector<EntityHandle> chain;
    chain.reserve(kDepth);
    chain.push_back(make(graph, posed(glm::dvec3(0.0))));
    for (std::size_t level = 1; level < kDepth; ++level) {
        chain.push_back(make(graph, posed(glm::dvec3(0.0, 1.0, 0.0)), chain.back()));
    }

    const EntityHandle leaf = chain.back();
    CHECK(near(origin_of(graph, leaf), glm::dvec3(0.0, static_cast<double>(kDepth - 1), 0.0), 1e-9));
    CHECK(graph.update() == 0);

    // Moving the top of the chain must reach the bottom through every level.
    REQUIRE(graph.set_local(chain.front(), posed(glm::dvec3(0.0, 5.0, 0.0))).ok());
    CHECK(graph.update() == kDepth);
    CHECK(near(origin_of(graph, leaf), glm::dvec3(0.0, 5.0 + static_cast<double>(kDepth - 1), 0.0), 1e-9));

    // Reparenting the leaf to the root keeps its world position, which runs through the whole chain to the new parent.
    REQUIRE(graph.reparent(leaf, EntityHandle{}, ReparentMode::KeepWorldTransform).ok());
    CHECK(near(origin_of(graph, leaf), glm::dvec3(0.0, 5.0 + static_cast<double>(kDepth - 1), 0.0), 1e-9));

    REQUIRE(graph.destroy(chain.front(), ChildPolicy::DestroySubtree).ok());
    CHECK(graph.size() == 1);
}

TEST_CASE("Scene graph keeps millimetre precision far from the origin", "[unit]") {
    // Doubles resolve about 1e-9 m at 5e6 m. A 1 mm child offset must survive composition, reparenting, and decomposition.
    SceneGraph graph;
    const EntityHandle far_parent = make(graph, posed(glm::dvec3(5.0e6, 0.0, -3.0e6), turn(glm::dvec3(0.0, 1.0, 0.0), 90.0)));
    const EntityHandle child = make(graph, posed(glm::dvec3(0.001, 0.0, 0.0)), far_parent);
    const EntityHandle leaf = make(graph, posed(glm::dvec3(0.0, 0.002, 0.0)), child);

    // The parent turns local +X into -Z. Its position is added to the offsets, which stay in double precision.
    CHECK(near(origin_of(graph, child), glm::dvec3(5.0e6, 0.0, -3.0e6 - 0.001), 1e-6));
    CHECK(near(origin_of(graph, leaf), glm::dvec3(5.0e6, 0.002, -3.0e6 - 0.001), 1e-6));

    const glm::dmat4 before = world_of(graph, leaf);
    REQUIRE(graph.reparent(leaf, EntityHandle{}, ReparentMode::KeepWorldTransform).ok());
    CHECK(near(world_of(graph, leaf), before, 1e-6));
    CHECK(near(origin_of(graph, leaf), glm::dvec3(5.0e6, 0.002, -3.0e6 - 0.001), 1e-6));
}
