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

// Vanilla Hearthfire carpenter and furnishing workbenches.
// Mod-added COBJ recipes using vanilla Hearthfire keywords are supported.
// Custom modded bench keywords are reserved for HearthfireModded.cpp.
namespace
{
    struct Ingredient {
        RE::ContainerObject* entry{};
        RE::TESBoundObject* item{};
        std::int32_t original{};
    };

    std::vector<Ingredient> g_ingredients;
    std::unordered_map<RE::FormID, std::vector<std::size_t>> g_byItem;
    std::unordered_set<RE::FormID> g_dirty;
    std::unordered_set<RE::BGSConstructibleObject*> g_indexedRecipes;
    std::unordered_set<RE::FormID> g_activeKeywords;
    bool g_active = false;
    bool g_queued = false;
    std::uint64_t g_generation = 0;

    bool IsCarpenterKeyword(const RE::BGSKeyword* keyword)
    {
        if (!keyword) return false;
        const char* raw = keyword->GetFormEditorID();
        if (!raw || !*raw) return false;
        std::string name(raw);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        // Only the original Hearthfire carpenter and furnishing keyword families.
        // Recipes from other plugins using these same keywords are indexed too.
        // Do not claim arbitrary mod-added workbenches here.
        return name == "byohbuildingcarpenter" ||
               name.starts_with("byohbuildinginteriorpart");
    }

    void Restore()
    {
        for (const auto& i : g_ingredients) {
            if (i.entry) i.entry->count = i.original;
        }
        g_ingredients.clear();
        g_byItem.clear();
        g_dirty.clear();
        g_indexedRecipes.clear();
        g_activeKeywords.clear();
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

    // Only index recipes belonging to the Hearthfire keyword families.
    // Quest/stage conditions remain untouched; only material counts are adjusted.
    // The vanilla menu decides which recipes are visible; this code never adds any.
    std::size_t IndexNewRecipes(RE::CraftingSubMenus::ConstructibleObjectMenu* menu)
    {
        if (!menu) return 0;
        std::size_t newlyIndexed = 0;
        std::size_t newKeywords = 0;
        for (const auto& item : menu->recipes) {
            auto* recipe = item.constructibleObject;
            if (!recipe || !IsCarpenterKeyword(recipe->benchKeyword)) continue;
            if (g_activeKeywords.insert(recipe->benchKeyword->GetFormID()).second)
                ++newKeywords;
        }
        auto indexOne = [&](RE::BGSConstructibleObject* recipe) {
            if (!recipe || !recipe->benchKeyword ||
                !g_activeKeywords.contains(recipe->benchKeyword->GetFormID()) ||
                !g_indexedRecipes.insert(recipe).second) return;
            ++newlyIndexed;
            recipe->requiredItems.ForEachContainerObject([&](RE::ContainerObject& entry) {
                if (entry.obj && entry.count > 0) {
                    const auto index = g_ingredients.size();
                    g_ingredients.push_back({ &entry, entry.obj, entry.count });
                    g_byItem[entry.obj->GetFormID()].push_back(index);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        };
        if (newKeywords) {
            if (auto* handler = RE::TESDataHandler::GetSingleton()) {
                for (auto* recipe : handler->GetFormArray<RE::BGSConstructibleObject>())
                    indexOne(recipe);
            }
        }
        for (const auto& item : menu->recipes)
            indexOne(item.constructibleObject);
        if (newlyIndexed || newKeywords)
            SKSE::log::info("[Hearthfire][LiveSync] new keywords={}, new recipes={}, tracked ingredients={}",
                            newKeywords, newlyIndexed, g_ingredients.size());
        return newlyIndexed;
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
            const auto wanted = std::clamp(it->second, std::int32_t{0}, i.original);
            if (i.entry->count != wanted) {
                i.entry->count = wanted;
                changed = true;
            }
        }
        SKSE::log::info("[Hearthfire] {} ingredient entries, {} distinct materials, changed={}",
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
            const bool recognised = IsCarpenterKeyword(keyword);
            if (recognised) ++matched;
            else ++others;
            const auto id = keyword ? keyword->GetFormID() : RE::FormID{0};
            if (reported.insert(id).second) {
                const char* editorID = keyword ? keyword->GetFormEditorID() : nullptr;
                SKSE::log::info("[Hearthfire][Detect] keyword={:08X} '{}' carpenter={}",
                                id, editorID ? editorID : "<null>", recognised);
            }
        }
        SKSE::log::info("[Hearthfire][Detect] carpenter recipes={}, other recipes={}", matched, others);
        // Only claim menus made exclusively of vanilla Hearthfire bench recipes.
        // Mixed menus are left unchanged to avoid touching other crafting modules.
        if (!matched || others) return;

        // Initial index, including later stages for all keywords already visible.
        // New keywords revealed while the menu is open are handled by LiveSync.
        const auto indexed = IndexNewRecipes(menu);
        g_active = true;
        SKSE::log::info("[Hearthfire] Carpenter menu accepted: {} visible, {} total recipes indexed (including locked stages)", matched, indexed);
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
        // Skyrim updates menu->recipes itself after a Hearthfire build.
        // Synchronise our ingredient cache with any newly revealed keywords/recipes.
        const auto newlyIndexed = IndexNewRecipes(menu);
        bool changed = false;
        for (auto id : pending) {
            const auto it = g_byItem.find(id);
            if (it == g_byItem.end() || it->second.empty()) continue;
            const auto owned = player->GetItemCount(g_ingredients[it->second.front()].item);
            for (auto index : it->second) {
                auto& i = g_ingredients[index];
                const auto wanted = std::clamp(owned, std::int32_t{0}, i.original);
                if (i.entry && i.entry->count != wanted) {
                    i.entry->count = wanted;
                    changed = true;
                }
            }
        }
        // New recipe ingredients may use materials that did not trigger this event.
        // Apply the free-crafting rules to those entries immediately.
        if (newlyIndexed) {
            RefreshAll(menu);
        } else if (changed) {
            menu->UpdateConstructibleList();
        }
        SKSE::log::info("[Hearthfire][LiveSync] event processed: new recipes={}, materials changed={}, visible={}",
                        newlyIndexed, changed, menu->recipes.size());
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
            if (!event || !g_active)
                return RE::BSEventNotifyControl::kContinue;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || (event->oldContainer != player->GetFormID() &&
                            event->newContainer != player->GetFormID()))
                return RE::BSEventNotifyControl::kContinue;
            // Any player inventory change can coincide with Skyrim revealing a new
            // modded Hearthfire recipe. Do not filter on the OLD ingredient cache:
            // the newly revealed recipe may introduce a previously unknown material.
            // Events are coalesced into one task, with no per-frame polling.
            if (g_byItem.contains(event->baseObj))
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

namespace UniversalCraftingFramework::Hearthfire
{
    void Initialize()
    {
        if (auto* ui = RE::UI::GetSingleton())
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuListener);
        if (auto* source = RE::ScriptEventSourceHolder::GetSingleton())
            source->AddEventSink<RE::TESContainerChangedEvent>(&g_inventoryListener);
        SKSE::log::info("[Hearthfire] Vanilla Hearthfire carpenter/furnishing module installed; event-driven.");
    }
}