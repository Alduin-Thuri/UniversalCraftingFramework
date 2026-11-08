#include "QuestRecipeGuard.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Staff crafting using Skyrim's standard COBJ / ConstructibleObjectMenu system.
// Compatible with custom staff benches using recognisable staff-crafting keywords.
// Does not change conditions, create recipes, or intercept scripted custom menus.
namespace
{
    struct Ingredient {
        RE::ContainerObject* entry{};
        RE::TESBoundObject* item{};
        std::int32_t original{};
        bool protectedRecipe{};
    };

    std::vector<Ingredient> g_ingredients;
    std::unordered_map<RE::FormID, std::vector<std::size_t>> g_byItem;
    std::unordered_set<RE::FormID> g_dirty;
    bool g_active = false;
    bool g_queued = false;
    std::uint64_t g_generation = 0;

    bool IsStaffKeyword(const RE::BGSKeyword* keyword)
    {
        if (!keyword) return false;
        const char* raw = keyword->GetFormEditorID();
        if (!raw || !*raw) return false;
        std::string name(raw);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        // Vanilla Dragonborn and conventional modded staff crafting keywords.
        // Deliberately reject broad "enchant" keywords (arcane enchanters are not COBJ staff benches).
        return name == "dlc2staffenchanter" ||
               (name.find("staff") != std::string::npos &&
                (name.find("enchant") != std::string::npos ||
                 name.find("craft") != std::string::npos ||
                 name.find("forge") != std::string::npos ||
                 name.find("make") != std::string::npos));
    }

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

    RE::CraftingSubMenus::ConstructibleObjectMenu* GetMenu()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME)) return nullptr;
        auto menu = ui->GetMenu<RE::CraftingMenu>();
        if (!menu) return nullptr;
        auto* base = menu->GetCraftingSubMenu();
        if (!base || !base->furniture ||
            base->furniture->workBenchData.benchType !=
                RE::TESFurniture::WorkBenchData::BenchType::kCreateObject) return nullptr;
        return static_cast<RE::CraftingSubMenus::ConstructibleObjectMenu*>(base);
    }

    void OnOpen()
    {
        auto* menu = GetMenu();
        if (!menu) return;
        Restore();

        std::size_t matched = 0;
        std::size_t other = 0;
        std::unordered_set<RE::BGSConstructibleObject*> seen;
        for (const auto& entry : menu->recipes) {
            auto* recipe = entry.constructibleObject;
            if (!recipe || !seen.insert(recipe).second) continue;
            if (IsStaffKeyword(recipe->benchKeyword)) ++matched;
            else ++other;
        }
        // Only take ownership of a pure staff crafting menu; never interfere with
        // the other ten UCF modules or a mixed/custom crafting station.
        if (!matched || other) {
            if (matched) SKSE::log::info("[StaffEnchanter] Mixed menu skipped: staff={}, other={}", matched, other);
            return;
        }

        seen.clear();
        // Include recipes not currently visible, so vanilla conditions still decide
        // visibility while ingredient thresholds are already prepared.
        std::unordered_set<RE::FormID> keywords;
        for (const auto& entry : menu->recipes) {
            auto* recipe = entry.constructibleObject;
            if (recipe && IsStaffKeyword(recipe->benchKeyword))
                keywords.insert(recipe->benchKeyword->GetFormID());
        }
        auto indexOne = [&](RE::BGSConstructibleObject* recipe) {
            if (!recipe || !recipe->benchKeyword ||
                !keywords.contains(recipe->benchKeyword->GetFormID()) ||
                !seen.insert(recipe).second) return;
            recipe->requiredItems.ForEachContainerObject([&](RE::ContainerObject& ingredient) {
                if (ingredient.obj && ingredient.count > 0) {
                    const auto index = g_ingredients.size();
                    g_ingredients.push_back({ &ingredient, ingredient.obj, ingredient.count , UniversalCraftingFramework::QuestRecipeGuard::IsProtected(recipe) });
                    g_byItem[ingredient.obj->GetFormID()].push_back(index);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        };
        if (auto* handler = RE::TESDataHandler::GetSingleton()) {
            for (auto* recipe : handler->GetFormArray<RE::BGSConstructibleObject>())
                indexOne(recipe);
        }
        for (const auto& entry : menu->recipes) indexOne(entry.constructibleObject);

        g_active = true;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) { Restore(); return; }
        std::unordered_map<RE::TESBoundObject*, std::int32_t> counts;
        bool changed = false;
        for (auto& ingredient : g_ingredients) {
            auto [it, inserted] = counts.try_emplace(ingredient.item, 0);
            if (inserted) it->second = player->GetItemCount(ingredient.item);
            const auto wanted = (ingredient.protectedRecipe ? ingredient.original : std::clamp(it->second, std::int32_t{0}, ingredient.original));
            if (ingredient.entry->count != wanted) {
                ingredient.entry->count = wanted;
                changed = true;
            }
        }
        SKSE::log::info("[StaffEnchanter] Menu accepted: visible={}, indexed={}, ingredient entries={}, distinct materials={}, changed={}",
                        matched, seen.size(), g_ingredients.size(), counts.size(), changed);
        if (changed) menu->UpdateConstructibleList();
    }

    void ProcessDirty()
    {
        g_queued = false;
        auto pending = std::move(g_dirty);
        g_dirty.clear();
        if (!g_active || pending.empty()) return;
        auto* menu = GetMenu();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!menu || !player) return;

        bool changed = false;
        std::size_t checked = 0;
        for (auto id : pending) {
            const auto it = g_byItem.find(id);
            if (it == g_byItem.end() || it->second.empty()) continue;
            const auto owned = player->GetItemCount(g_ingredients[it->second.front()].item);
            for (auto index : it->second) {
                auto& ingredient = g_ingredients[index];
                const auto wanted = (ingredient.protectedRecipe ? ingredient.original : std::clamp(owned, std::int32_t{0}, ingredient.original));
                if (ingredient.entry && ingredient.entry->count != wanted) {
                    ingredient.entry->count = wanted;
                    changed = true;
                }
                ++checked;
            }
        }
        if (changed) {
            SKSE::log::info("[StaffEnchanter] Material threshold changed: {} entries checked; refreshing once.", checked);
            menu->UpdateConstructibleList();
        }
    }

    class MenuListener final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!event || std::string_view(event->menuName.c_str()) != RE::CraftingMenu::MENU_NAME)
                return RE::BSEventNotifyControl::kContinue;
            ++g_generation;
            if (!event->opening) Restore();
            else if (auto* tasks = SKSE::GetTaskInterface()) {
                const auto token = g_generation;
                tasks->AddTask([token]() { if (token == g_generation) OnOpen(); });
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
                            event->newContainer != player->GetFormID()))
                return RE::BSEventNotifyControl::kContinue;
            g_dirty.insert(event->baseObj);
            if (!g_queued) {
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    g_queued = true;
                    const auto token = g_generation;
                    tasks->AddTask([token]() {
                        if (token == g_generation) ProcessDirty();
                        else g_queued = false;
                    });
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    MenuListener g_menuListener;
    InventoryListener g_inventoryListener;
}

namespace UniversalCraftingFramework::StaffEnchanter
{
    void Initialize()
    {
        if (auto* ui = RE::UI::GetSingleton())
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuListener);
        if (auto* source = RE::ScriptEventSourceHolder::GetSingleton())
            source->AddEventSink<RE::TESContainerChangedEvent>(&g_inventoryListener);
        SKSE::log::info("[StaffEnchanter] Staff COBJ module installed; event-driven, mod keyword support.");
    }
}
