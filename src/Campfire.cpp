#include "QuestRecipeGuard.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Campfire cooking: same ingredient-threshold
// behaviour as Forge.cpp, with no frame polling or vtable hooks.
namespace {
struct Ingredient {
    RE::ContainerObject* entry{};
    RE::TESBoundObject* item{};
    std::int32_t original{};
        bool protectedRecipe{};
};
std::vector<Ingredient> ingredients;
std::unordered_map<RE::FormID, std::vector<std::size_t>> byItem;
std::unordered_set<RE::FormID> dirty;
bool active = false, queued = false;
std::uint64_t generation = 0;

bool IsCampfire(const RE::BGSKeyword* keyword) {
    if (!keyword) return false;
    const char* id = keyword->GetFormEditorID();
    if (!id) return false;
    std::string_view s(id);
    return s == "Camping_CampfireCookingShared";
}
void Restore() {
    for (auto& i : ingredients) if (i.entry) i.entry->count = i.original;
    ingredients.clear(); byItem.clear(); dirty.clear();
    active = false; queued = false;
}
RE::CraftingSubMenus::ConstructibleObjectMenu* GetMenu() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui || !ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME)) {
        SKSE::log::info("[Campfire][Detect] CraftingMenu not open.");
        return nullptr;
    }
    auto menu = ui->GetMenu<RE::CraftingMenu>();
    if (!menu) {
        SKSE::log::info("[Campfire][Detect] CraftingMenu object unavailable.");
        return nullptr;
    }
    auto* base = menu->GetCraftingSubMenu();
    if (!base || !base->furniture) {
        SKSE::log::info("[Campfire][Detect] Submenu or furniture unavailable.");
        return nullptr;
    }
    const auto benchType = base->furniture->workBenchData.benchType;
    if (benchType != RE::TESFurniture::WorkBenchData::BenchType::kCreateObject) {
        SKSE::log::info("[Campfire][Detect] Bench type is not kCreateObject; cooking skipped.");
        return nullptr;
    }
    auto* result = static_cast<RE::CraftingSubMenus::ConstructibleObjectMenu*>(base);
    bool cooking = false, other = false;
    std::unordered_set<RE::FormID> reportedKeywords;
    std::size_t cookingRecipes = 0, otherRecipes = 0;
    for (const auto& item : result->recipes) {
        if (!item.constructibleObject) continue;
        const auto* keyword = item.constructibleObject->benchKeyword;
        const bool recognised = IsCampfire(keyword);
        if (recognised) { cooking = true; ++cookingRecipes; }
        else { other = true; ++otherRecipes; }
        const auto keywordID = keyword ? keyword->GetFormID() : RE::FormID{0};
        if (reportedKeywords.insert(keywordID).second) {
            const char* editorID = keyword ? keyword->GetFormEditorID() : nullptr;
            SKSE::log::info("[Campfire][Detect] Recipe bench keyword: FormID={:08X}, EditorID='{}', recognised={}",
                keywordID, editorID ? editorID : "<null>", recognised);
        }
    }
    SKSE::log::info("[Campfire][Detect] Recipes: cooking={}, other={}, accepted={}",
        cookingRecipes, otherRecipes, cooking && !other);
    return cooking && !other ? result : nullptr;
}
void Refresh(RE::CraftingSubMenus::ConstructibleObjectMenu* menu) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!menu || !player) return;
    std::unordered_map<RE::TESBoundObject*, std::int32_t> owned;
    bool changed = false;
    for (auto& i : ingredients) {
        if (!i.entry || !i.item || i.original <= 0) continue;
        auto [it, inserted] = owned.try_emplace(i.item, 0);
        if (inserted) it->second = player->GetItemCount(i.item);
        auto wanted = (i.protectedRecipe ? i.original : std::clamp(it->second, std::int32_t{0}, i.original));
        if (i.entry->count != wanted) { i.entry->count = wanted; changed = true; }
    }
    SKSE::log::info("[Campfire] {} ingredient entries; {} unique inventory queries; changed={}",
        ingredients.size(), owned.size(), changed);
    if (changed) menu->UpdateConstructibleList();
}
void OnOpen() {
    auto* menu = GetMenu();
    if (!menu) return;
    Restore();
    std::unordered_set<RE::BGSConstructibleObject*> seen;
    for (const auto& item : menu->recipes) {
        auto* recipe = item.constructibleObject;
        if (!recipe || !IsCampfire(recipe->benchKeyword) || !seen.insert(recipe).second) continue;
        recipe->requiredItems.ForEachContainerObject([&](RE::ContainerObject& entry) {
            if (entry.obj && entry.count > 0) {
                auto index = ingredients.size();
                ingredients.push_back({&entry, entry.obj, entry.count, UniversalCraftingFramework::QuestRecipeGuard::IsProtected(recipe) });
                byItem[entry.obj->GetFormID()].push_back(index);
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }
    active = true;
    SKSE::log::info("[Campfire] {} recipes indexed.", seen.size());
    Refresh(menu);
}
void ProcessDirty() {
    queued = false;
    auto pending = std::move(dirty);
    dirty.clear();
    if (!active) return;
    auto* menu = GetMenu();
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!menu || !player) return;
    bool changed = false;
    for (auto id : pending) {
        auto it = byItem.find(id);
        if (it == byItem.end() || it->second.empty()) continue;
        auto owned = player->GetItemCount(ingredients[it->second.front()].item);
        for (auto index : it->second) {
            auto& i = ingredients[index];
            auto wanted = (i.protectedRecipe ? i.original : std::clamp(owned, std::int32_t{0}, i.original));
            if (i.entry && i.entry->count != wanted) {
                i.entry->count = wanted; changed = true;
            }
        }
    }
    if (changed) menu->UpdateConstructibleList();
}
class MenuListener final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (!event || std::string_view(event->menuName.c_str()) != RE::CraftingMenu::MENU_NAME)
            return RE::BSEventNotifyControl::kContinue;
        ++generation;
        if (!event->opening) Restore();
        else if (auto* tasks = SKSE::GetTaskInterface()) {
            auto token = generation;
            tasks->AddTask([token]() { if (token == generation) OnOpen(); });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
class InventoryListener final : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event,
        RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
        if (!event || !active || !byItem.contains(event->baseObj))
            return RE::BSEventNotifyControl::kContinue;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || (event->oldContainer != player->GetFormID() &&
                        event->newContainer != player->GetFormID()))
            return RE::BSEventNotifyControl::kContinue;
        dirty.insert(event->baseObj);
        if (!queued) if (auto* tasks = SKSE::GetTaskInterface()) {
            queued = true;
            auto token = generation;
            tasks->AddTask([token]() {
                if (token == generation) ProcessDirty();
                else queued = false;
            });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
MenuListener menuListener;
InventoryListener inventoryListener;
}
namespace UniversalCraftingFramework::Campfire {
void Initialize() {
    if (auto* ui = RE::UI::GetSingleton())
        ui->AddEventSink<RE::MenuOpenCloseEvent>(&menuListener);
    if (auto* source = RE::ScriptEventSourceHolder::GetSingleton())
        source->AddEventSink<RE::TESContainerChangedEvent>(&inventoryListener);
    SKSE::log::info("[Campfire] Campfire cooking module installed.");
}
}
