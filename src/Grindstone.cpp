#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Weapon-tempering module: use the recipes Skyrim already placed in the menu.
// Never force an improvement or alter perk/quality checks.
// No per-frame polling or vtable hooks.
namespace
{
    struct Ingredient
    {
        RE::ContainerObject* entry{};
        RE::TESBoundObject* item{};
        std::int32_t original{};
    };

    std::vector<Ingredient> g_ingredients;
    std::unordered_map<RE::FormID, std::vector<std::size_t>> g_byItem;
    std::unordered_set<RE::FormID> g_dirty;
    bool g_active = false;
    bool g_queued = false;
    std::uint64_t g_generation = 0;

    void Restore()
    {
        for (const auto& ingredient : g_ingredients) {
            if (ingredient.entry) ingredient.entry->count = ingredient.original;
        }
        g_ingredients.clear();
        g_byItem.clear();
        g_dirty.clear();
        g_active = false;
        g_queued = false;
    }

    RE::CraftingSubMenus::SmithingMenu* GetWeaponMenu()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME)) return nullptr;
        auto menu = ui->GetMenu<RE::CraftingMenu>();
        if (!menu) return nullptr;
        auto* base = menu->GetCraftingSubMenu();
        if (!base || !base->furniture) return nullptr;
        // Check the actual furniture workbench type before casting the submenu.
        if (base->furniture->workBenchData.benchType !=
            RE::TESFurniture::WorkBenchData::BenchType::kSmithingWeapon) return nullptr;
        return static_cast<RE::CraftingSubMenus::SmithingMenu*>(base);
    }

    void RefreshAll(RE::CraftingSubMenus::SmithingMenu* smithing)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!smithing || !player) return;
        std::unordered_map<RE::TESBoundObject*, std::int32_t> owned;
        owned.reserve(g_byItem.size());
        std::size_t changed = 0;
        for (const auto& ingredient : g_ingredients) {
            auto [it, inserted] = owned.try_emplace(ingredient.item, 0);
            if (inserted) it->second = player->GetItemCount(ingredient.item);
            const auto wanted = std::clamp(it->second, std::int32_t{ 0 }, ingredient.original);
            if (ingredient.entry->count != wanted) {
                ingredient.entry->count = wanted;
                ++changed;
            }
        }
        SKSE::log::info("[Grindstone] {} ingredient entries, {} distinct materials, {} adjusted.",
            g_ingredients.size(), owned.size(), changed);
        if (changed) smithing->UpdateSmithingList();
    }

    void OnOpen()
    {
        auto* smithing = GetWeaponMenu();
        if (!smithing) return;
        Restore();
        std::unordered_set<RE::BGSConstructibleObject*> seen;
        std::size_t weaponEntries = 0;
        for (const auto& item : smithing->recipes) {
            if (!item.constructibleObject || !seen.insert(item.constructibleObject).second) continue;
            ++weaponEntries;
            item.constructibleObject->requiredItems.ForEachContainerObject([&](RE::ContainerObject& entry) {
                if (entry.obj && entry.count > 0) {
                    const auto index = g_ingredients.size();
                    g_ingredients.push_back({ &entry, entry.obj, entry.count });
                    g_byItem[entry.obj->GetFormID()].push_back(index);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        g_active = true;
        SKSE::log::info("[Grindstone] Weapon tempering menu: {} entries, {} distinct COBJ recipes, {} ingredient entries.",
            smithing->recipes.size(), weaponEntries, g_ingredients.size());
        RefreshAll(smithing);
    }

    void ProcessDirty()
    {
        g_queued = false;
        if (!g_active) { g_dirty.clear(); return; }
        auto* smithing = GetWeaponMenu();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!smithing || !player) { g_dirty.clear(); return; }
        bool changed = false;
        for (auto id : g_dirty) {
            const auto it = g_byItem.find(id);
            if (it == g_byItem.end() || it->second.empty()) continue;
            auto* item = g_ingredients[it->second.front()].item;
            const auto owned = player->GetItemCount(item);
            for (auto index : it->second) {
                auto& ingredient = g_ingredients[index];
                const auto wanted = std::clamp(owned, std::int32_t{ 0 }, ingredient.original);
                if (ingredient.entry->count != wanted) {
                    ingredient.entry->count = wanted;
                    changed = true;
                }
            }
        }
        g_dirty.clear();
        if (changed) smithing->UpdateSmithingList();
    }

    class MenuListener final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!event || std::string_view(event->menuName.c_str()) != RE::CraftingMenu::MENU_NAME) return RE::BSEventNotifyControl::kContinue;
            ++g_generation;
            if (!event->opening) {
                Restore();
            } else if (auto* tasks = SKSE::GetTaskInterface()) {
                const auto generation = g_generation;
                tasks->AddTask([generation]() {
                    if (generation == g_generation) OnOpen();
                });
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class InventoryListener final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!event || !g_active || !g_byItem.contains(event->baseObj))
                return RE::BSEventNotifyControl::kContinue;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || (event->oldContainer != player->GetFormID() &&
                event->newContainer != player->GetFormID())) return RE::BSEventNotifyControl::kContinue;
            g_dirty.insert(event->baseObj);
            if (!g_queued) {
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    g_queued = true;
                    const auto generation = g_generation;
                    tasks->AddTask([generation]() {
                        if (generation == g_generation) ProcessDirty();
                    });
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    MenuListener g_menuListener;
    InventoryListener g_inventoryListener;
}

namespace UniversalCraftingFramework::Grindstone
{
    void Initialize()
    {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuListener);
        }
        if (auto* source = RE::ScriptEventSourceHolder::GetSingleton()) {
            source->AddEventSink<RE::TESContainerChangedEvent>(&g_inventoryListener);
        }
        SKSE::log::info("[Grindstone] Event-driven weapon tempering module installed; no frame polling.");
    }
}
