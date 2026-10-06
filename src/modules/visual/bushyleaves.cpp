#include "bushyleaves.hpp"

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <string_view>

namespace {

std::atomic_bool g_enabled{false};
std::atomic_int g_layers{3};
std::atomic<float> g_spread{0.14f};
std::atomic_bool g_sideFaces{true};
std::atomic_bool g_topBottomFaces{true};
std::atomic_bool g_rebuildPending{false};

bool isLeafName(std::string_view raw) {
    constexpr std::string_view ns = "minecraft:";
    if (raw.starts_with(ns)) raw.remove_prefix(ns.size());

    // Vanilla Bedrock leaf blocks, including newer trees.
    return raw == "leaves"
        || raw == "leaves2"
        || raw == "azalea_leaves"
        || raw == "azalea_leaves_flowered"
        || raw == "oak_leaves"
        || raw == "spruce_leaves"
        || raw == "birch_leaves"
        || raw == "jungle_leaves"
        || raw == "acacia_leaves"
        || raw == "dark_oak_leaves"
        || raw == "mangrove_leaves"
        || raw == "cherry_leaves"
        || raw == "pale_oak_leaves"
        || raw.ends_with("_leaves")
        || raw.ends_with("_leaves2");
}

bool isLeaf(const void* block) {
    if (!block) return false;
    const auto* b = static_cast<const bedrocktools::sdk::Block*>(block);
    const auto* name = b->fullName();
    if (!name || name->empty() || name->size() > 256) return false;
    return isLeafName({name->data(), name->size()});
}

BushyLeavesVec3Raw faceNormal(BushyLeavesFace face) {
    switch (face) {
        case BushyLeavesFace::Down:  return {0.f, -1.f, 0.f};
        case BushyLeavesFace::Up:    return {0.f,  1.f, 0.f};
        case BushyLeavesFace::North: return {0.f,  0.f, -1.f};
        case BushyLeavesFace::South: return {0.f,  0.f,  1.f};
        case BushyLeavesFace::West:  return {-1.f, 0.f, 0.f};
        case BushyLeavesFace::East:  return {1.f,  0.f, 0.f};
    }
    return {};
}

BushyLeavesVec3Raw tangentA(BushyLeavesFace face) {
    switch (face) {
        case BushyLeavesFace::Down:
        case BushyLeavesFace::Up:    return {1.f, 0.f, 0.f};
        case BushyLeavesFace::North:
        case BushyLeavesFace::South: return {1.f, 0.f, 0.f};
        case BushyLeavesFace::West:
        case BushyLeavesFace::East:  return {0.f, 0.f, 1.f};
    }
    return {1.f, 0.f, 0.f};
}

BushyLeavesVec3Raw add(const BushyLeavesVec3Raw& p, const BushyLeavesVec3Raw& a, float s) {
    return {p.x + a.x * s, p.y + a.y * s, p.z + a.z * s};
}

BushyLeavesVec3Raw add2(
    const BushyLeavesVec3Raw& p,
    const BushyLeavesVec3Raw& a, float sa,
    const BushyLeavesVec3Raw& b, float sb
) {
    return {
        p.x + a.x * sa + b.x * sb,
        p.y + a.y * sa + b.y * sb,
        p.z + a.z * sa + b.z * sb
    };
}

bool faceAllowed(BushyLeavesFace face) {
    if (face == BushyLeavesFace::Up || face == BushyLeavesFace::Down)
        return g_topBottomFaces.load(std::memory_order_relaxed);
    return g_sideFaces.load(std::memory_order_relaxed);
}

bool rebuildRenderChunks(void* clientInstance) {
    if (!clientInstance) return false;

    void* levelRenderer = bedrocktools::sdk::field<void*>(
        clientInstance, bedrocktools::sdk::offsets::ClientInstance::mLevelRenderer
    );
    if (!levelRenderer) return false;

    void* node = bedrocktools::sdk::field<void*>(
        levelRenderer,
        bedrocktools::sdk::offsets::LevelRenderer::mRenderChunkCoordinators
            + bedrocktools::sdk::offsets::HashTable::mFirstNode
    );

    bool rebuilt = false;
    std::size_t visited = 0;
    while (node && visited++ < bedrocktools::sdk::offsets::RenderChunkCoordinator::MaxNodes) {
        void* next = bedrocktools::sdk::field<void*>(
            node, bedrocktools::sdk::offsets::HashNode::mNext
        );
        void* coordinator = bedrocktools::sdk::field<void*>(
            node, bedrocktools::sdk::offsets::HashNode::mValuePointer
        );
        if (coordinator) {
            using SetAllDirtyFn = void (*)(void*, bool, bool);
            static SetAllDirtyFn setAllDirty = nullptr;
            if (!setAllDirty) {
                setAllDirty = reinterpret_cast<SetAllDirtyFn>(
                    bedrocktools::memory::resolve(
                        bedrocktools::memory::SignatureId::RenderChunkCoordinatorSetAllDirty
                    )
                );
            }
            if (setAllDirty) {
                setAllDirty(coordinator, true, false);
                rebuilt = true;
            }
        }
        node = next;
    }
    return rebuilt;
}

} // namespace

void BushyLeavesRenderExtras(
    BushyLeavesFaceFn original,
    BushyLeavesFace face,
    void* tessellator,
    void* meshTessellator,
    const void* block,
    const BushyLeavesVec3Raw* position,
    const void* texture
) {
    if (!original || !position || !block || !isLeaf(block)) return;
    if (!g_enabled.load(std::memory_order_relaxed) || !faceAllowed(face)) return;

    const int layerCount = std::clamp(g_layers.load(std::memory_order_relaxed), 1, 5);
    const float spread = std::clamp(g_spread.load(std::memory_order_relaxed), 0.02f, 0.30f);
    const BushyLeavesVec3Raw n = faceNormal(face);
    const BushyLeavesVec3Raw t = tangentA(face);

    // The vanilla face is already emitted by ConnectedGlass. Add several
    // offset copies around it. This gives transparent leaf textures real
    // depth without changing collision or the leaf texture itself.
    for (int i = 1; i <= layerCount; ++i) {
        const float d = spread * (static_cast<float>(i) / static_cast<float>(layerCount));

        const BushyLeavesVec3Raw pOut = add(*position, n, d);
        const BushyLeavesVec3Raw pIn  = add(*position, n, -d);
        original(tessellator, meshTessellator, block, &pOut, texture);
        original(tessellator, meshTessellator, block, &pIn, texture);

        // One small tangential offset per layer breaks up the perfectly flat
        // stack and makes the foliage look fuller from oblique angles.
        const float side = (i & 1) ? d * 0.55f : -d * 0.55f;
        const BushyLeavesVec3Raw pSide = add2(*position, n, d * 0.35f, t, side);
        original(tessellator, meshTessellator, block, &pSide, texture);
    }
}

BushyLeavesModule::BushyLeavesModule()
    : Module(
          "Bushy Leaves",
          "Adds dense, fluffy 3D geometry to vanilla leaf blocks."
      ) {
}

void BushyLeavesModule::applySettings() {
    layers = std::clamp(layers, 1, 5);
    spread = std::clamp(spread, 0.02f, 0.30f);
    g_layers.store(layers, std::memory_order_relaxed);
    g_spread.store(spread, std::memory_order_relaxed);
    g_sideFaces.store(sideFaces, std::memory_order_relaxed);
    g_topBottomFaces.store(topBottomFaces, std::memory_order_relaxed);
}

void BushyLeavesModule::onInit() {
    applySettings();

    // The face hooks are owned by ConnectedGlass because installing another
    // hook on the same six Minecraft functions would conflict with it.
    // Bushy Leaves piggybacks on that existing hook safely.
    if (enabledByDefault && !masterEnabled) {
        masterEnabled = true;
        updateEnabledState();
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>(
        [](auto& event) {
            if (!g_rebuildPending.load(std::memory_order_acquire)) return;
            if (rebuildRenderChunks(event.clientInstance)) {
                g_rebuildPending.store(false, std::memory_order_release);
            }
        }
    );
}

void BushyLeavesModule::onEnable() {
    applySettings();
    g_enabled.store(true, std::memory_order_release);
    g_rebuildPending.store(true, std::memory_order_release);
}

void BushyLeavesModule::onDisable() {
    g_enabled.store(false, std::memory_order_release);
    g_rebuildPending.store(true, std::memory_order_release);
}

void BushyLeavesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    enabledByDefault = j.value("enabledByDefault", enabledByDefault);
    layers = j.value("layers", layers);
    spread = j.value("spread", spread);
    sideFaces = j.value("sideFaces", sideFaces);
    topBottomFaces = j.value("topBottomFaces", topBottomFaces);
    applySettings();
}

void BushyLeavesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["enabledByDefault"] = enabledByDefault;
    j["layers"] = layers;
    j["spread"] = spread;
    j["sideFaces"] = sideFaces;
    j["topBottomFaces"] = topBottomFaces;
}
