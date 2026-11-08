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

// Custom modded building benches only. Vanilla Hearthfire is handled separately.
// First test: conservative EditorID-based recognition; unknown keywords are logged and skipped.
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

    // Conservative automatic detection of custom building keywords.
    // Never take ownership of vanilla Hearthfire keywords: Hearthfire.cpp handles those.
    bool IsModdedBuildingKeyword(const RE::BGSKeyword* keyword)
    {
        if (!keyword) return false;
        const char* raw = keyword->GetFormEditorID();
        if (!raw || !*raw) return false;
        std::string name(raw);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (name == "byohbuildingcarpenter" ||
            name.starts_with("byohbuildinginteriorpart") ||
            name.starts_with("byohbuildingdrafting")) return false;

        // Reject obvious non-building crafting stations before accepting any custom keyword.
        if (name.find("forge") != std::string::npos ||
            name.find("smelt") != std::string::npos ||
            name.find("tanning") != std::string::npos ||
            name.find("grindstone") != std::string::npos ||
            name.find("temper") != std::string::npos ||
            name.find("cook") != std::string::npos ||
            name.find("campfire") != std::string::npos ||
            name.find("alchemy") != std::string::npos ||
            name.find("enchant") != std::string::npos) return false;

        return name.find("build") != std::string::npos ||
               name.find("carpenter") != std::string::npos ||
               name.find("construct") != std::string::npos ||
               name.find("furnish") != std::string::npos ||
               name.find("homestead") != std::string::npos ||
               name.find("housepart") != std::string::npos;
    }

    void Restore()
    {
        for (const auto& i : g_ingredients) {
            if (i.entry) i.entry->count = i.original;
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

    void RefreshAll(RE::CraftingSubMenus::ConstructibleObjectMenu* menu)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!menu || !player) return;
        std::unordered_map<RE::TESBoundObject*, std::int32_t> owned;
        owned.reserve(g_byItem.size());
        bool changed = false;
        for (auto& i : g_ingredients) {
            if (!i.entry || !i.item || i.original <= 0) continue;
            auto [it, inserted] = owned.try_emplace(i.item, 0);
            if (inserted) it->second = player->GetItemCount(i.item);
            const auto wanted = (i.protectedRecipe ? i.original : std::clamp(it->second, std::int32_t{0}, i.original));
            if (i.entry->count != wanted) {
                i.entry->count = wanted;
                changed = true;
            }
        }
        SKSE::log::info("[HearthfireModded] {} ingredient entries, {} distinct materials, changed={}",
                        g_ingredients.size(), owned.size(), changed);
        if (changed) menu->UpdateConstructibleList();
    }

    void OnOpen()
    {
        auto* menu = GetMenu();
        if (!menu) return;
        Restore();

        std::size_t matched = 0;
        std::size_t others = 0;
        std::unordered_set<RE::BGSConstructibleObject*> seen;
        std::unordered_set<RE::FormID> reported;
        for (const auto& item : menu->recipes) {
            auto* recipe = item.constructibleObject;
            if (!recipe || !seen.insert(recipe).second) continue;
            const auto* keyword = recipe->benchKeyword;
            const bool recognised = IsModdedBuildingKeyword(keyword);
            if (recognised) ++matched;
            else ++others;
            const auto id = keyword ? keyword->GetFormID() : RE::FormID{0};
            if (reported.insert(id).second) {
                const char* editorID = keyword ? keyword->GetFormEditorID() : nullptr;
                SKSE::log::info("[HearthfireModded][Detect] keyword={:08X} '{}' building={}",
                                id, editorID ? editorID : "<null>", recognised);
            }
        }
        SKSE::log::info("[HearthfireModded][Detect] building recipes={}, other recipes={}", matched, others);
        // Only claim menus made exclusively of recognised custom building recipes.
        // Mixed menus are left unchanged to avoid touching other crafting modules.
        if (!matched || others) return;

        // Hearthfire construction progresses while the menu remains open. Some
        // recipes are hidden until a previous building stage is completed.
        // Capture ALL recipes for the exact bench keywords already seen in this
        // menu, not just the currently visible recipes. This does not bypass
        // recipe conditions or add any recipe to the UI.
        std::unordered_set<RE::FormID> activeKeywords;
        for (const auto& item : menu->recipes) {
            const auto* recipe = item.constructibleObject;
            if (recipe && IsModdedBuildingKeyword(recipe->benchKeyword)) {
                activeKeywords.insert(recipe->benchKeyword->GetFormID());
            }
        }

        seen.clear();
        auto indexRecipe = [&](RE::BGSConstructibleObject* recipe) {
            if (!recipe || !recipe->benchKeyword ||
                !activeKeywords.contains(recipe->benchKeyword->GetFormID()) ||
                !seen.insert(recipe).second) return;
            recipe->requiredItems.ForEachContainerObject([&](RE::ContainerObject& entry) {
                if (entry.obj && entry.count > 0) {
                    const auto index = g_ingredients.size();
                    g_ingredients.push_back({&entry, entry.obj, entry.count, UniversalCraftingFramework::QuestRecipeGuard::IsProtected(recipe) });
                    g_byItem[entry.obj->GetFormID()].push_back(index);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        };

        if (auto* handler = RE::TESDataHandler::GetSingleton()) {
            for (auto* recipe : handler->GetFormArray<RE::BGSConstructibleObject>()) {
                indexRecipe(recipe);
            }
        }
        // Also support runtime-generated recipes not present in the form array.
        for (const auto& item : menu->recipes) {
            indexRecipe(item.constructibleObject);
        }
        g_active = true;
        SKSE::log::info("[HearthfireModded] Custom building menu accepted: {} visible, {} total recipes indexed (including locked stages)", matched, seen.size());
        RefreshAll(menu);
    }

    void ProcessDirty()
    {
        g_queued = false;
        auto pending = std::move(g_dirty);
        g_dirty.clear();
        if (!g_active) return;
        auto* menu = GetMenu();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!menu || !player) return;
        bool changed = false;
        for (auto id : pending) {
            const auto it = g_byItem.find(id);
            if (it == g_byItem.end() || it->second.empty()) continue;
            const auto owned = player->GetItemCount(g_ingredients[it->second.front()].item);
            for (auto index : it->second) {
                auto& i = g_ingredients[index];
                const auto wanted = (i.protectedRecipe ? i.original : std::clamp(owned, std::int32_t{0}, i.original));
                if (i.entry && i.entry->count != wanted) {
                    i.entry->count = wanted;
                    changed = true;
                }
            }
        }
        // Vanilla may reveal the next construction stage after the current
        // inventory event. All same-bench recipes are already prepared above.
        if (changed) menu->UpdateConstructibleList();
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

namespace UniversalCraftingFramework::HearthfireModded
{
    void Initialize()
    {
        if (auto* ui = RE::UI::GetSingleton())
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuListener);
        if (auto* source = RE::ScriptEventSourceHolder::GetSingleton())
            source->AddEventSink<RE::TESContainerChangedEvent>(&g_inventoryListener);
        SKSE::log::info("[HearthfireModded] Custom building workbench module installed; conservative automatic keyword detection, event-driven.");
    }
}
