#pragma once
#include <RE/Skyrim.h>

namespace UniversalCraftingFramework::QuestRecipeGuard {
    // Read-only recipe classification. Protected recipes keep original ingredient counts.
    void Initialize();
    bool IsProtected(const RE::BGSConstructibleObject* recipe);
}
