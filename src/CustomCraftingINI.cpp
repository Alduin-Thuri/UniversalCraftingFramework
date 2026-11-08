#include "QuestRecipeGuard.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Explicit opt-in for crafting recipes DEFINED by the plugins listed in the INI.
// Does not bypass COBJ conditions or insert recipes into Skyrim's menu.
namespace
{
    struct Ingredient {
        RE::ContainerObject* entry{};
        RE::TESBoundObject* item{};
        std::int32_t original{};
        bool protectedRecipe{};
    };

    std::unordered_set<std::string> g_plugins;
    std::unordered_set<RE::FormID> g_keywords;
    std::unordered_set<RE::BGSConstructibleObject*> g_indexed;
    std::vector<Ingredient> g_ingredients;
    std::unordered_map<RE::FormID, std::vector<std::size_t>> g_byItem;
    std::unordered_set<RE::FormID> g_dirtyItems;
    bool g_active = false;
    bool g_queued = false;
    std::uint64_t g_generation = 0;

    std::string Trim(std::string s)
    {
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const auto last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    void LoadConfig()
    {
        constexpr auto path = "Data/SKSE/Plugins/CustomCraftingINI.ini";
        std::ifstream file(path);
        if (!file) {
            SKSE::log::warn("[CustomCraftingINI] INI not found: {}. Module inactive.", path);
            return;
        }
        std::string section;
        bool enabled = true;
        std::string line;
        while (std::getline(file, line)) {
            // UTF-8 BOM on first line is allowed.
            if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
            const auto comment = line.find_first_of(";#");
            if (comment != std::string::npos) line.erase(comment);
            line = Trim(line);
            if (line.empty()) continue;
            if (line.front() == '[' && line.back() == ']') {
                section = Lower(Trim(line.substr(1, line.size() - 2)));
                continue;
            }
            if (section == "settings") {
                const auto equal = line.find('=');
                if (equal == std::string::npos) continue;
                if (Lower(Trim(line.substr(0, equal))) == "enabled") {
                    const auto value = Lower(Trim(line.substr(equal + 1)));
                    enabled = value == "true" || value == "1" || value == "yes";
                }
            } else if (section == "plugin" || section == "plugins") {
                const auto name = Lower(line);
                if (name.ends_with(".esp") || name.ends_with(".esm") || name.ends_with(".esl"))
                    g_plugins.insert(name);
                else SKSE::log::warn("[CustomCraftingINI] Ignored invalid plugin entry: {}", line);
            }
        }
        if (!enabled) g_plugins.clear();
        SKSE::log::info("[CustomCraftingINI] enabled={}, configured plugins={}", enabled, g_plugins.size());
    }

    bool IsSelected(const RE::BGSConstructibleObject* recipe)
    {
        if (!recipe) return false;
        const auto* file = recipe->GetFile(0); // Defining plugin, not a later override.
        return file && file->fileName && g_plugins.contains(Lower(file->fileName));
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

    void Restore()
    {
        for (const auto& ingredient : g_ingredients)
            if (ingredient.entry) ingredient.entry->count = ingredient.original;
        g_ingredients.clear();
        g_byItem.clear();
        g_indexed.clear();
        g_keywords.clear();
        g_dirtyItems.clear();
        g_active = false;
        g_queued = false;
    }

    // Index later construction stages, but only for keywords currently revealed by
    // Skyrim and recipes defined by a configured plugin.
    std::size_t IndexNew(RE::CraftingSubMenus::ConstructibleObjectMenu* menu)
    {
        if (!menu) return 0;
        bool newKeyword = false;
        for (const auto& item : menu->recipes) {
            const auto* recipe = item.constructibleObject;
            if (IsSelected(recipe) && recipe->benchKeyword)
                newKeyword |= g_keywords.insert(recipe->benchKeyword->GetFormID()).second;
        }
        std::size_t added = 0;
        auto indexOne = [&](RE::BGSConstructibleObject* recipe) {
            if (!IsSelected(recipe) || !recipe->benchKeyword ||
                !g_keywords.contains(recipe->benchKeyword->GetFormID()) ||
                !g_indexed.insert(recipe).second) return;
            ++added;
            recipe->requiredItems.ForEachContainerObject([&](RE::ContainerObject& entry) {
                if (entry.obj && entry.count > 0) {
                    const auto index = g_ingredients.size();
                    g_ingredients.push_back({ &entry, entry.obj, entry.count , UniversalCraftingFramework::QuestRecipeGuard::IsProtected(recipe) });
                    g_byItem[entry.obj->GetFormID()].push_back(index);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        };
        if (newKeyword) {
            if (auto* handler = RE::TESDataHandler::GetSingleton())
                for (auto* recipe : handler->GetFormArray<RE::BGSConstructibleObject>())
                    indexOne(recipe);
        }
        for (const auto& item : menu->recipes) indexOne(item.constructibleObject);
        if (added) SKSE::log::info("[CustomCraftingINI] Indexed {} new recipes; total={}", added, g_indexed.size());
        return added;
    }

    // Initial opening: query each unique material once. Later updates use
    // ApplyChanged() instead, avoiding a full material pass on every event.
    bool Apply(RE::CraftingSubMenus::ConstructibleObjectMenu* menu)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!menu || !player) return false;
        std::unordered_map<RE::TESBoundObject*, std::int32_t> counts;
        bool changed = false;
        for (auto& ingredient : g_ingredients) {
            if (!ingredient.entry || !ingredient.item) continue;
            auto [it, inserted] = counts.try_emplace(ingredient.item, 0);
            if (inserted) it->second = player->GetItemCount(ingredient.item);
            const auto wanted = (ingredient.protectedRecipe ? ingredient.original : std::clamp(it->second, std::int32_t{0}, ingredient.original));
            if (ingredient.entry->count != wanted) {
                ingredient.entry->count = wanted;
                changed = true;
            }
        }
        if (changed) menu->UpdateConstructibleList();
        return changed;
    }

    // Refresh only entries whose material actually changed, plus entries
    // belonging to recipes newly discovered during this inventory event.
    bool ApplyChanged(RE::CraftingSubMenus::ConstructibleObjectMenu* menu,
        const std::unordered_set<RE::FormID>& dirty, std::size_t firstNewEntry)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!menu || !player) return false;
        std::unordered_map<RE::TESBoundObject*, std::int32_t> counts;
        bool changed = false;
        auto update = [&](std::size_t index) {
            auto& ingredient = g_ingredients[index];
            if (!ingredient.entry || !ingredient.item) return;
            auto [it, inserted] = counts.try_emplace(ingredient.item, 0);
            if (inserted) it->second = player->GetItemCount(ingredient.item);
            const auto wanted = (ingredient.protectedRecipe ? ingredient.original : std::clamp(it->second, std::int32_t{0}, ingredient.original));
            if (ingredient.entry->count != wanted) {
                ingredient.entry->count = wanted;
                changed = true;
            }
        };

        for (auto id : dirty) {
            const auto it = g_byItem.find(id);
            if (it == g_byItem.end()) continue;
            for (auto index : it->second) update(index);
        }
        for (std::size_t index = firstNewEntry; index < g_ingredients.size(); ++index) {
            // Newly added entries may already have been covered by dirty IDs.
            if (!dirty.contains(g_ingredients[index].item->GetFormID())) update(index);
        }
        if (changed) menu->UpdateConstructibleList();
        return changed;
    }

    void OnOpen()
    {
        auto* menu = GetMenu();
        if (!menu || g_plugins.empty()) return;
        Restore();
        std::size_t selected = 0;
        std::size_t other = 0;
        for (const auto& item : menu->recipes) {
            if (!item.constructibleObject) continue;
            if (IsSelected(item.constructibleObject)) ++selected;
            else ++other;
        }
        // Avoid overlapping existing vanilla/forge/cooking/custom modules.
        // An exclusively selected COBJ menu is an explicit opt-in.
        if (!selected || other) {
            SKSE::log::info("[CustomCraftingINI] Menu skipped: selected={}, other={}", selected, other);
            return;
        }
        g_active = true;
        IndexNew(menu);
        Apply(menu);
        SKSE::log::info("[CustomCraftingINI] Menu accepted: {} visible recipes, {} indexed, {} ingredients",
            selected, g_indexed.size(), g_ingredients.size());
    }

    void OnInventory()
    {
        g_queued = false;
        auto dirty = std::move(g_dirtyItems);
        g_dirtyItems.clear();
        if (!g_active) return;
        auto* menu = GetMenu();
        if (!menu) return;

        // Skyrim may reveal another building stage after an inventory event.
        // Inspect visible recipes, but only index all COBJ records if a NEW
        // bench keyword appears (existing IndexNew() behaviour).
        const auto firstNewEntry = g_ingredients.size();
        const auto added = IndexNew(menu);
        if (dirty.empty() && firstNewEntry == g_ingredients.size()) return;

        const auto changed = ApplyChanged(menu, dirty, firstNewEntry);
        if (added || changed) {
            SKSE::log::info("[CustomCraftingINI] LiveSync: new recipes={}, material counts changed={}", added, changed);
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
            else if (!g_plugins.empty()) {
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    const auto token = g_generation;
                    tasks->AddTask([token]() { if (token == g_generation) OnOpen(); });
                }
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
            if (!event || !g_active) return RE::BSEventNotifyControl::kContinue;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || (event->oldContainer != player->GetFormID() &&
                            event->newContainer != player->GetFormID()))
                return RE::BSEventNotifyControl::kContinue;
            if (g_byItem.contains(event->baseObj)) g_dirtyItems.insert(event->baseObj);
            if (g_queued) return RE::BSEventNotifyControl::kContinue;
            if (auto* tasks = SKSE::GetTaskInterface()) {
                g_queued = true;
                const auto token = g_generation;
                tasks->AddTask([token]() {
                    if (token == g_generation) OnInventory();
                    else g_queued = false;
                });
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    MenuListener g_menuListener;
    InventoryListener g_inventoryListener;
}

namespace UniversalCraftingFramework::CustomCraftingINI
{
    void Initialize()
    {
        LoadConfig();
        if (g_plugins.empty()) return;
        if (auto* ui = RE::UI::GetSingleton())
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuListener);
        if (auto* source = RE::ScriptEventSourceHolder::GetSingleton())
            source->AddEventSink<RE::TESContainerChangedEvent>(&g_inventoryListener);
        SKSE::log::info("[CustomCraftingINI] Plugin-list crafting module installed; event-driven.");
    }
}
