#pragma once

#include "../Module.hpp"

#include <cstdint>

struct BushyLeavesVec3Raw {
    float x, y, z;
};

using BushyLeavesFaceFn = void (*)(
    void*, void*, const void*, const BushyLeavesVec3Raw*, const void*
);

enum class BushyLeavesFace : std::uint8_t {
    Down = 0,
    Up,
    North,
    South,
    West,
    East
};

// Called by the existing ConnectedGlass face hook.  Bushy Leaves deliberately
// does not install a second hook on the same Minecraft functions.
void BushyLeavesRenderExtras(
    BushyLeavesFaceFn original,
    BushyLeavesFace face,
    void* tessellator,
    void* meshTessellator,
    const void* block,
    const BushyLeavesVec3Raw* position,
    const void* texture
);

class BushyLeavesModule final : public Module {
public:
    BushyLeavesModule();
    ~BushyLeavesModule() override = default;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool enabledByDefault = true;
    int layers = 3;
    float spread = 0.14f;
    bool sideFaces = true;
    bool topBottomFaces = true;

private:
    void applySettings();
};
