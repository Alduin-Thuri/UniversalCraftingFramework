#include "QuestRecipeGuard.h"
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
    constexpr std::string_view kTanningRackKeyword = "CraftingTanningRack";

    struct Ingredient
    {
        RE::ContainerObject* entry{};
        RE::TESBoundObject* item{};
        std::int32_t originalCount{};
        bool protectedRecipe{};
    };

    // One extra recipe; vanilla recipes remain untouched.
    RE::BGSConstructibleObject* g_freeLeatherRecipe = nullptr;
    constexpr RE::FormID kLeatherFormID = 0x000DB5D2;

    std::vector<Ingredient> g_ingredients;
    bool g_modified = false;
    bool g_tanningRackActive = false;
    bool g_refreshQueued = false;
    std::uint64_t g_menuGeneration = 0;
    std::unordered_map<RE::FormID, std::vector<std::size_t>> g_byIngredient;
    std::unordered_set<RE::FormID> g_dirtyItems;


    bool IsTanningRackKeyword(const RE::BGSKeyword* keyword)
    {
        if (!keyword) {
            return false;
        }
        const char* editorID = keyword->GetFormEditorID();
        return editorID && std::string_view(editorID) == kTanningRackKeyword;
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
        SKSE::log::debug("TanningRack: original ingredient counts restored.");
    }

    void RefreshCounts()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            SKSE::log::warn("TanningRack: player unavailable; no change applied.");
            return;
        }

        // No whole-inventory scan: only query ingredient FormIDs used by tanning rack recipes.
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
            ingredient.entry->count = ingredient.protectedRecipe ? ingredient.originalCount : std::clamp(it->second, std::int32_t{ 0 }, ingredient.originalCount);
            if (ingredient.entry->count == ingredient.originalCount) {
                ++vanilla;
            } else {
                ++bypassed;
            }
        }
        g_modified = true;
        SKSE::log::info("TanningRack refresh: {} vanilla, {} bypassed; {} distinct inventory queries.",
            vanilla, bypassed, ownedCounts.size());
    }

    void RegisterFreeLeatherRecipe()
    {
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler || g_freeLeatherRecipe) {
            return;
        }

        // Use an existing leather COBJ as the template so the new record
        // inherits the correct created item, bench keyword and output count.
        RE::BGSConstructibleObject* templateRecipe = nullptr;
        for (auto* recipe : handler->GetFormArray<RE::BGSConstructibleObject>()) {
            if (recipe && IsTanningRackKeyword(recipe->benchKeyword) &&
                recipe->createdItem && recipe->createdItem->GetFormID() == kLeatherFormID) {
                templateRecipe = recipe;
                break;
            }
        }
        if (!templateRecipe) {
            SKSE::log::warn("[TanningRack] No leather recipe found; extra recipe not created.");
            return;
        }

        auto* duplicate = templateRecipe->CreateDuplicateForm(false, nullptr);
        if (!duplicate || duplicate->GetFormType() != RE::FormType::ConstructibleObject) {
            SKSE::log::error("[TanningRack] Could not duplicate the leather recipe.");
            return;
        }
        auto* extra = static_cast<RE::BGSConstructibleObject*>(duplicate);

        // Build the custom record independently. CreateDuplicateForm can leave
        // component fields unpopulated on some runtimes; copy the two form
        // references explicitly from the source without changing the source.
        extra->createdItem = templateRecipe->createdItem;
        extra->benchKeyword = templateRecipe->benchKeyword;
        extra->data.numConstructed = 1;

        // CNTO is a list, not a quantity. ClearDataComponent() does not
        // reliably reset this component after form duplication. Do not free
        // copied pointers: they may still be shared with the source recipe.
        extra->requiredItems.containerObjects = nullptr;
        extra->requiredItems.numContainerObjects = 0;
        extra->requiredItems.blockStolenItems = false;

        // Empty CTDA chain on the new record only. Do not delete nodes that
        // might still be owned by the original recipe.
        extra->conditions.head = nullptr;

        SKSE::log::info("[TanningRack] Clone check: source={:08X}, clone={:08X}, CNTO={}, CTDA={}, CNAM={:08X}, BNAM={:08X}",
            templateRecipe->GetFormID(), extra->GetFormID(),
            extra->requiredItems.numContainerObjects, extra->conditions.head != nullptr,
            extra->createdItem ? extra->createdItem->GetFormID() : 0,
            extra->benchKeyword ? extra->benchKeyword->GetFormID() : 0);

        if (extra == templateRecipe ||
            extra->requiredItems.numContainerObjects != 0 ||
            extra->conditions.head != nullptr ||
            extra->createdItem != templateRecipe->createdItem ||
            extra->benchKeyword != templateRecipe->benchKeyword ||
            !extra->createdItem || !extra->benchKeyword) {
            SKSE::log::error("[TanningRack] Custom Leather clone invalid; refusing to register.");
            return;
        }

        // Register the runtime-created recipe so the crafting menu can find it.
        if (!handler->AddFormToDataHandler(extra)) {
            SKSE::log::error("[TanningRack] Failed to register extra leather recipe.");
            return;
        }
        g_freeLeatherRecipe = extra;
        SKSE::log::info("[TanningRack] Custom Leather COBJ {:08X}: CNTO={}, CTDA={}, output={:08X}, quantity={}; vanilla unchanged.",
            extra->GetFormID(), extra->requiredItems.numContainerObjects,
            extra->conditions.head != nullptr, extra->createdItem->GetFormID(), extra->data.numConstructed);
    }

    void IndexRecipes()
    {
        RestoreOriginalCounts();
        g_ingredients.clear();
        g_byIngredient.clear();
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) {
            SKSE::log::error("TanningRack: TESDataHandler unavailable.");
            return;
        }
        std::size_t recipes = 0;
        for (auto* recipe : handler->GetFormArray<RE::BGSConstructibleObject>()) {
            if (!recipe || !IsTanningRackKeyword(recipe->benchKeyword)) {
                continue;
            }
            ++recipes;
            recipe->requiredItems.ForEachContainerObject([&](RE::ContainerObject& entry) {
                if (entry.obj && entry.count > 0) {
                    const auto index = g_ingredients.size();
                    g_ingredients.push_back({ &entry, entry.obj, entry.count , UniversalCraftingFramework::QuestRecipeGuard::IsProtected(recipe) });
                    g_byIngredient[entry.obj->GetFormID()].push_back(index);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        SKSE::log::info("TanningRack: indexed {} recipes and {} ingredient entries.", recipes, g_ingredients.size());
    }

    // Called once after the menu-open event, on the game task queue.
    // This is an explicit refresh, not a vtable hook.
    void RefreshVisibleTanningRackMenu()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME)) {
            SKSE::log::info("[TanningRack] Crafting menu already closed; skipping.");
            return;
        }

        auto menu = ui->GetMenu<RE::CraftingMenu>();
        if (!menu) {
            SKSE::log::warn("[TanningRack] CraftingMenu unavailable.");
            return;
        }

        auto* subMenu = menu->GetCraftingSubMenu();
        if (!subMenu) {
            SKSE::log::warn("[TanningRack] Crafting submenu not initialized.");
            return;
        }

        if (!subMenu->furniture) {
            SKSE::log::info("[TanningRack] No workbench assigned.");
            return;
        }
        // The FNMK furniture entry-point keywords are NOT the COBJ bench keyword.
        // Detect the actual workbench by inspecting the recipes already loaded
        // in the constructible-object submenu instead.
        const auto benchType = subMenu->furniture->workBenchData.benchType;
        if (benchType != RE::TESFurniture::WorkBenchData::BenchType::kCreateObject) {
            SKSE::log::info("[TanningRack] Not a create-object bench; skipping.");
            return;
        }

        auto* constructible = static_cast<RE::CraftingSubMenus::ConstructibleObjectMenu*>(subMenu);
        std::size_t tanningRackRecipes = 0;
        std::size_t otherRecipes = 0;
        for (const auto& item : constructible->recipes) {
            if (!item.constructibleObject) {
                continue;
            }
            if (IsTanningRackKeyword(item.constructibleObject->benchKeyword)) {
                ++tanningRackRecipes;
            } else {
                ++otherRecipes;
            }
        }
        SKSE::log::info("[TanningRack] Menu recipes: {} tanning rack, {} other.", tanningRackRecipes, otherRecipes);
        if (tanningRackRecipes == 0 || otherRecipes != 0) {
            SKSE::log::info("[TanningRack] TanningRack not confirmed by menu recipes; no changes.");
            return;
        }

        // Runtime-created COBJ forms are not necessarily picked up by the menu's
        // initial recipe enumeration. Add only our extra recipe to this menu's
        // own recipe list, without changing any vanilla entries.
        if (g_freeLeatherRecipe) {
            bool alreadyPresent = false;
            std::uint32_t leatherFilter = 0;
            bool foundLeatherFilter = false;
            for (const auto& item : constructible->recipes) {
                if (item.constructibleObject == g_freeLeatherRecipe) {
                    alreadyPresent = true;
                }
                // Copy the actual menu filter value from an existing Leather
                // recipe, rather than assuming that 0 means "no filter".
                if (item.constructibleObject &&
                    item.constructibleObject->createdItem &&
                    item.constructibleObject->createdItem->GetFormID() == kLeatherFormID &&
                    IsTanningRackKeyword(item.constructibleObject->benchKeyword)) {
                    leatherFilter = item.filterFlag;
                    foundLeatherFilter = true;
                }
            }
            if (!alreadyPresent) {
                RE::CraftingSubMenus::ConstructibleObjectMenu::ItemEntry extraEntry{};
                extraEntry.constructibleObject = g_freeLeatherRecipe;
                extraEntry.filterFlag = leatherFilter;
                constructible->recipes.push_back(extraEntry);
                SKSE::log::info("[TanningRack] Custom Leather {:08X} inserted: filter={}, copiedFromLeather={}, menuSize={}",
                    g_freeLeatherRecipe->GetFormID(), leatherFilter, foundLeatherFilter, constructible->recipes.size());
            }
        }

        g_tanningRackActive = true;
        RefreshCounts();
        // A tanning rack uses the constructible-object submenu.
        SKSE::log::info("[TanningRack] Calling UpdateConstructibleList explicitly.");
        constructible->UpdateConstructibleList();
        bool customStillPresent = false;
        for (const auto& item : constructible->recipes) {
            if (item.constructibleObject == g_freeLeatherRecipe && g_freeLeatherRecipe) {
                customStillPresent = true;
                break;
            }
        }
        SKSE::log::info("[TanningRack] List refresh completed: customPresent={}, menuSize={}",
            customStillPresent, constructible->recipes.size());
    }

    // Only inventory changes involving indexed tanning rack ingredients reach this task.
    // This event is not exclusive to crafting: the menu and the ingredient filter
    // are both required. Actual behaviour is deliberately logged for testing.
    void ProcessIngredientChanges()
    {
        g_refreshQueued = false;
        auto pending = std::move(g_dirtyItems);
        g_dirtyItems.clear();

        auto* ui = RE::UI::GetSingleton();
        if (!g_tanningRackActive || !ui || !ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME)) {
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
                const auto wanted = (ingredient.protectedRecipe ? ingredient.originalCount : std::clamp(owned, std::int32_t{ 0 }, ingredient.originalCount));
                if (ingredient.entry && ingredient.entry->count != wanted) {
                    ingredient.entry->count = wanted;
                    changed = true;
                }
                ++checked;
            }
        }
        if (!changed) return;

        SKSE::log::info("[TanningRack] Ingredient threshold changed: {} entries checked; refreshing once.", checked);
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
            if (!event || !g_tanningRackActive) return RE::BSEventNotifyControl::kContinue;
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
                    g_tanningRackActive = false;
                    SKSE::log::info("[TanningRack] Crafting menu opened; scheduling one-time refresh.");
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        const auto generation = g_menuGeneration;
                        tasks->AddTask([generation]() {
                            if (generation == g_menuGeneration) RefreshVisibleTanningRackMenu();
                        });
                    } else {
                        SKSE::log::error("[TanningRack] Task interface unavailable.");
                    }
                } else {
                    ++g_menuGeneration;
                    g_tanningRackActive = false;
                    g_dirtyItems.clear();
                    RestoreOriginalCounts();
                    SKSE::log::info("[TanningRack] Crafting menu closed.");
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    MenuListener g_listener;

    void InitializeTanningRackInternal()
    {
        RegisterFreeLeatherRecipe();
        IndexRecipes();
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_listener);
        }
        if (auto* source = RE::ScriptEventSourceHolder::GetSingleton()) {
            source->AddEventSink<RE::TESContainerChangedEvent>(&g_inventoryListener);
            SKSE::log::info("[TanningRack] Filtered inventory event listener installed.");
        } else {
            SKSE::log::warn("[TanningRack] Inventory event source unavailable.");
        }
        SKSE::log::info("[TanningRack] Recipe-based tanning rack detection installed; no vtable hooks.");
    }
}

namespace UniversalCraftingFramework::TanningRack
{
    void Initialize()
    {
        InitializeTanningRackInternal();
    }
}
