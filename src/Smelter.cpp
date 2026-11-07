#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr std::string_view kSmelterKeyword = "CraftingSmelter";

    struct Ingredient
    {
        RE::ContainerObject* entry{};
        RE::TESBoundObject* item{};
        std::int32_t originalCount{};
    };

    std::vector<Ingredient> g_ingredients;
    bool g_modified = false;
    bool g_smelterActive = false;
    bool g_refreshQueued = false;
    std::uint64_t g_menuGeneration = 0;
    std::unordered_map<RE::FormID, std::vector<std::size_t>> g_byIngredient;
    std::unordered_set<RE::FormID> g_dirtyItems;


    bool IsSmelterKeyword(const RE::BGSKeyword* keyword)
    {
        if (!keyword) {
            return false;
        }
        const char* editorID = keyword->GetFormEditorID();
        return editorID && std::string_view(editorID) == kSmelterKeyword;
    }

    bool IsSmelterMenu(const RE::CraftingSubMenus::ConstructibleObjectMenu* menu)
    {
        if (!menu || !menu->furniture) {
            return false;
        }
        // Determine the actual workbench, not the global crafting-menu name.
        for (const auto& marker : menu->furniture->entryPointDataArray) {
            if (IsSmelterKeyword(marker.keyword)) {
                return true;
            }
        }
        return false;
    }

    void RestoreOriginalCounts()
    {
        if (!g_modified) {
            return;
        }
        for (const auto& ingredient : g_ingredients) {
            if (ingredient.entry) {
                ingredient.entry->count = ingredient.originalCount;
            }
        }
        g_modified = false;
        SKSE::log::debug("Smelter: original ingredient counts restored.");
    }

    void RefreshCounts()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            SKSE::log::warn("Smelter: player unavailable; no change applied.");
            return;
        }

        // No whole-inventory scan: only query ingredient FormIDs used by smelter recipes.
        // Cache one inventory query per unique item for this refresh.
        std::unordered_map<RE::TESBoundObject*, std::int32_t> ownedCounts;
        ownedCounts.reserve(g_ingredients.size());
        std::size_t vanilla = 0;
        std::size_t bypassed = 0;
        for (const auto& ingredient : g_ingredients) {
            if (!ingredient.entry || !ingredient.item || ingredient.originalCount <= 0) {
                continue;
            }
            auto [it, inserted] = ownedCounts.try_emplace(ingredient.item, 0);
            if (inserted) {
                it->second = player->GetItemCount(ingredient.item);
            }
            ingredient.entry->count = std::clamp(it->second, std::int32_t{ 0 }, ingredient.originalCount);
            if (ingredient.entry->count == ingredient.originalCount) {
                ++vanilla;
            } else {
                ++bypassed;
            }
        }
        g_modified = true;
        SKSE::log::info("Smelter refresh: {} vanilla, {} bypassed; {} distinct inventory queries.",
            vanilla, bypassed, ownedCounts.size());
    }

    void IndexRecipes()
    {
        RestoreOriginalCounts();
        g_ingredients.clear();
        g_byIngredient.clear();
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) {
            SKSE::log::error("Smelter: TESDataHandler unavailable.");
            return;
        }
        std::size_t recipes = 0;
        for (auto* recipe : handler->GetFormArray<RE::BGSConstructibleObject>()) {
            if (!recipe || !IsSmelterKeyword(recipe->benchKeyword)) {
                continue;
            }
            ++recipes;
            recipe->requiredItems.ForEachContainerObject([&](RE::ContainerObject& entry) {
                if (entry.obj && entry.count > 0) {
                    const auto index = g_ingredients.size();
                    g_ingredients.push_back({ &entry, entry.obj, entry.count });
                    g_byIngredient[entry.obj->GetFormID()].push_back(index);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        SKSE::log::info("Smelter: indexed {} recipes and {} ingredient entries.", recipes, g_ingredients.size());
    }

    // Called once after the menu-open event, on the game task queue.
    // This is an explicit refresh, not a vtable hook.
    void RefreshVisibleSmelterMenu()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME)) {
            SKSE::log::info("[v0.8] Crafting menu already closed; skipping.");
            return;
        }

        auto menu = ui->GetMenu<RE::CraftingMenu>();
        if (!menu) {
            SKSE::log::warn("[v0.8] CraftingMenu unavailable.");
            return;
        }

        auto* subMenu = menu->GetCraftingSubMenu();
        if (!subMenu) {
            SKSE::log::warn("[v0.8] Crafting submenu not initialized.");
            return;
        }

        if (!subMenu->furniture) {
            SKSE::log::info("[v0.8] No workbench assigned.");
            return;
        }
        // The FNMK furniture entry-point keywords are NOT the COBJ bench keyword.
        // Detect the actual workbench by inspecting the recipes already loaded
        // in the constructible-object submenu instead.
        const auto benchType = subMenu->furniture->workBenchData.benchType;
        if (benchType != RE::TESFurniture::WorkBenchData::BenchType::kCreateObject) {
            SKSE::log::info("[v0.8] Not a create-object bench; skipping.");
            return;
        }

        auto* constructible = static_cast<RE::CraftingSubMenus::ConstructibleObjectMenu*>(subMenu);
        std::size_t smelterRecipes = 0;
        std::size_t otherRecipes = 0;
        for (const auto& item : constructible->recipes) {
            if (!item.constructibleObject) {
                continue;
            }
            if (IsSmelterKeyword(item.constructibleObject->benchKeyword)) {
                ++smelterRecipes;
            } else {
                ++otherRecipes;
            }
        }
        SKSE::log::info("[v0.8] Menu recipes: {} smelter, {} other.", smelterRecipes, otherRecipes);
        if (smelterRecipes == 0 || otherRecipes != 0) {
            SKSE::log::info("[v0.8] Smelter not confirmed by menu recipes; no changes.");
            return;
        }

        g_smelterActive = true;
        RefreshCounts();
        // A smelter uses the constructible-object submenu.
        SKSE::log::info("[v0.8] Calling UpdateConstructibleList explicitly.");
        constructible->UpdateConstructibleList();
        SKSE::log::info("[v0.8] Explicit list refresh completed.");
    }

    // Only inventory changes involving indexed smelter ingredients reach this task.
    // This event is not exclusive to crafting: the menu and the ingredient filter
    // are both required. Actual behaviour is deliberately logged for testing.
    void ProcessIngredientChanges()
    {
        g_refreshQueued = false;
        auto pending = std::move(g_dirtyItems);
        g_dirtyItems.clear();

        auto* ui = RE::UI::GetSingleton();
        if (!g_smelterActive || !ui || !ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME)) {
            return;
        }
        auto menu = ui->GetMenu<RE::CraftingMenu>();
        if (!menu) return;
        auto* base = menu->GetCraftingSubMenu();
        if (!base || !base->furniture ||
            base->furniture->workBenchData.benchType != RE::TESFurniture::WorkBenchData::BenchType::kCreateObject) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        bool changed = false;
        std::size_t checked = 0;
        for (auto id : pending) {
            auto it = g_byIngredient.find(id);
            if (it == g_byIngredient.end() || it->second.empty()) continue;
            const auto& first = g_ingredients[it->second.front()];
            const auto owned = player->GetItemCount(first.item);
            for (auto index : it->second) {
                const auto& ingredient = g_ingredients[index];
                const auto wanted = std::clamp(owned, std::int32_t{ 0 }, ingredient.originalCount);
                if (ingredient.entry && ingredient.entry->count != wanted) {
                    ingredient.entry->count = wanted;
                    changed = true;
                }
                ++checked;
            }
        }
        if (!changed) return;

        SKSE::log::info("[v0.10] Ingredient threshold changed: {} entries checked; refreshing once.", checked);
        auto* constructible = static_cast<RE::CraftingSubMenus::ConstructibleObjectMenu*>(base);
        constructible->UpdateConstructibleList();
    }

    class InventoryListener final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESContainerChangedEvent* event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!event || !g_smelterActive) return RE::BSEventNotifyControl::kContinue;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return RE::BSEventNotifyControl::kContinue;
            const auto playerID = player->GetFormID();
            if (event->oldContainer != playerID && event->newContainer != playerID) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (!g_byIngredient.contains(event->baseObj)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            g_dirtyItems.insert(event->baseObj);
            if (!g_refreshQueued) {
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    g_refreshQueued = true;
                    const auto generation = g_menuGeneration;
                    tasks->AddTask([generation]() {
                        if (generation == g_menuGeneration) ProcessIngredientChanges();
                        else g_refreshQueued = false;
                    });
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    InventoryListener g_inventoryListener;

    class MenuListener final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (event && std::string_view(event->menuName.c_str()) == RE::CraftingMenu::MENU_NAME) {
                if (event->opening) {
                    ++g_menuGeneration;
                    g_smelterActive = false;
                    SKSE::log::info("[v0.8] Crafting menu opened; scheduling one-time refresh.");
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        const auto generation = g_menuGeneration;
                        tasks->AddTask([generation]() {
                            if (generation == g_menuGeneration) RefreshVisibleSmelterMenu();
                        });
                    } else {
                        SKSE::log::error("[v0.8] Task interface unavailable.");
                    }
                } else {
                    ++g_menuGeneration;
                    g_smelterActive = false;
                    g_dirtyItems.clear();
                    RestoreOriginalCounts();
                    SKSE::log::info("[v0.10] Crafting menu closed.");
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    MenuListener g_listener;

    void InitializeSmelterInternal()
    {
        IndexRecipes();
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_listener);
        }
        if (auto* source = RE::ScriptEventSourceHolder::GetSingleton()) {
            source->AddEventSink<RE::TESContainerChangedEvent>(&g_inventoryListener);
            SKSE::log::info("[v0.10] Filtered inventory event listener installed.");
        } else {
            SKSE::log::warn("[v0.10] Inventory event source unavailable.");
        }
        SKSE::log::info("[v0.10] Recipe-based smelter detection installed; no vtable hooks.");
    }
}

namespace UniversalCraftingFramework::Smelter
{
    void Initialize()
    {
        InitializeSmelterInternal();
    }
}
