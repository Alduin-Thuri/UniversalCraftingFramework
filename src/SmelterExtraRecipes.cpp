#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace UniversalCraftingFramework::SmelterExtraRecipes
{
    namespace
    {
        struct RecipeDefinition
        {
            RE::FormID localID{};
            std::string plugin; // Empty means Skyrim.esm (vanilla FormID).
            std::string label;
        };

        constexpr std::array<std::pair<RE::FormID, std::string_view>, 14> kDefaultRecipes{{
            { 0x0002E4E2, "Petty Soul Gem (Empty)" },
            { 0x0002E4E3, "Petty Soul Gem (Filled)" },
            { 0x0002E4E4, "Lesser Soul Gem (Empty)" },
            { 0x0002E4E5, "Lesser Soul Gem (Filled)" },
            { 0x0002E4E6, "Common Soul Gem (Empty)" },
            { 0x0002E4F3, "Common Soul Gem (Filled)" },
            { 0x0002E4F4, "Greater Soul Gem (Empty)" },
            { 0x0002E4FB, "Greater Soul Gem (Filled)" },
            { 0x0002E4FC, "Grand Soul Gem (Empty)" },
            { 0x0002E4FF, "Grand Soul Gem (Filled)" },
            { 0x0002E500, "Black Soul Gem (Empty)" },
            { 0x0002E504, "Black Soul Gem (Filled)" },
            { 0x0003ADA4, "Dragon Bone" },
            { 0x0003ADA3, "Dragon Scales" }
        }};

        constexpr auto kIniPath = "Data/SKSE/Plugins/SmelterExtraRecipes.ini";
        bool g_initialized = false;

        struct Settings
        {
            bool enabled = true;
            bool includeDefaults = true;
            std::uint16_t craftQuantity = 1;
            std::vector<RecipeDefinition> custom;
        };

        std::string Trim(std::string_view s)
        {
            const auto start = s.find_first_not_of(" \t\r\n");
            if (start == std::string_view::npos) return {};
            const auto end = s.find_last_not_of(" \t\r\n");
            return std::string(s.substr(start, end - start + 1));
        }

        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        }

        bool ParseHex(std::string s, RE::FormID& result)
        {
            s = Trim(s);
            if (s.starts_with("0x") || s.starts_with("0X")) s.erase(0, 2);
            if (s.empty() || s.size() > 8) return false;
            std::uint32_t parsed = 0;
            const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), parsed, 16);
            if (error != std::errc{} || end != s.data() + s.size() || parsed == 0) return false;
            result = parsed;
            return true;
        }

        Settings ReadSettings()
        {
            Settings settings;
            // Create a user-editable configuration on first launch only.
            // Never overwrite an existing file, including an unreadable one.
            std::error_code ec;
            if (!std::filesystem::exists(kIniPath, ec) && !ec) {
                std::filesystem::create_directories(std::filesystem::path(kIniPath).parent_path(), ec);
                if (!ec) {
                    std::ofstream created(kIniPath, std::ios::out | std::ios::trunc);
                    if (created) {
                        created << "; Universal Crafting Framework - Smelter Extra Recipes\n"
                                   "; Generated automatically. Restart Skyrim after editing.\n\n"
                                   "[Settings]\n"
                                   "; Enable all extra recipes\nEnabled=true\n"
                                   "; Include 12 Soul Gems and 2 dragon materials\nIncludeDefaultRecipes=true\n"
                                   "; Items produced per craft (1..32767)\nCraftQuantity=1\n\n"
                                   "[CustomRecipes]\n"
                                   "; PluginName|LocalFormID (one per line)\n"
                                   "; Skyrim.esm|0005ACE4\n"
                                   "; MyItems.esp|00000800\n";
                        created.close();
                        if (created) SKSE::log::info("[SmelterExtraRecipes] Generated default INI: {}", kIniPath);
                    }
                }
                if (ec) SKSE::log::error("[SmelterExtraRecipes] Cannot create INI directory: {}", ec.message());
            }
            std::ifstream file(kIniPath);
            if (!file) {
                SKSE::log::warn("[SmelterExtraRecipes] Cannot read/create {}; using built-in defaults.", kIniPath);
                return settings;
            }

            std::string section;
            std::string line;
            std::size_t lineNumber = 0;
            while (std::getline(file, line)) {
                ++lineNumber;
                auto cleaned = Trim(line);
                if (cleaned.empty() || cleaned[0] == ';' || cleaned[0] == '#') continue;
                if (cleaned.front() == '[' && cleaned.back() == ']') {
                    section = Lower(Trim(std::string_view(cleaned).substr(1, cleaned.size() - 2)));
                    continue;
                }
                if (section == "customrecipes") {
                    // Each line: PluginName.esp|LocalFormID
                    // Skyrim.esm|0002E4E2 is also supported.
                    const auto comment = cleaned.find_first_of(";#");
                    if (comment != std::string::npos) cleaned.erase(comment);
                    const auto separator = cleaned.rfind('|');
                    if (separator == std::string::npos) {
                        SKSE::log::warn("[SmelterExtraRecipes] Invalid custom recipe at line {} (expected Plugin.esp|FormID).", lineNumber);
                        continue;
                    }
                    auto plugin = Trim(std::string_view(cleaned).substr(0, separator));
                    auto idText = Trim(std::string_view(cleaned).substr(separator + 1));
                    RE::FormID localID{};
                    if (plugin.empty() || !ParseHex(idText, localID)) {
                        SKSE::log::warn("[SmelterExtraRecipes] Invalid custom recipe at line {}.", lineNumber);
                        continue;
                    }
                    settings.custom.push_back({ localID, std::move(plugin), "Custom" });
                    continue;
                }
                if (section != "settings") continue;
                const auto equal = cleaned.find('=');
                if (equal == std::string::npos) continue;
                auto key = Lower(Trim(std::string_view(cleaned).substr(0, equal)));
                auto value = Lower(Trim(std::string_view(cleaned).substr(equal + 1)));
                const auto comment = value.find_first_of(";#");
                if (comment != std::string::npos) value = Trim(std::string_view(value).substr(0, comment));
                if (key == "enabled") settings.enabled = value == "true" || value == "1" || value == "yes";
                else if (key == "includedefaultrecipes") settings.includeDefaults = value == "true" || value == "1" || value == "yes";
                else if (key == "craftquantity") {
                    std::uint32_t quantity = 0;
                    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), quantity, 10);
                    if (error == std::errc{} && end == value.data() + value.size() && quantity >= 1 && quantity <= 32767) {
                        settings.craftQuantity = static_cast<std::uint16_t>(quantity);
                    } else {
                        SKSE::log::warn("[SmelterExtraRecipes] CraftQuantity must be 1..32767; using {}.", settings.craftQuantity);
                    }
                }
            }
            return settings;
        }

        bool IsSmelterRecipe(const RE::BGSConstructibleObject* recipe)
        {
            if (!recipe || !recipe->benchKeyword) return false;
            const auto* id = recipe->benchKeyword->GetFormEditorID();
            return id && std::string_view(id) == "CraftingSmelter";
        }

        RE::BGSConstructibleObject* FindTemplate(RE::TESDataHandler* handler)
        {
            for (auto* recipe : handler->GetFormArray<RE::BGSConstructibleObject>()) {
                if (IsSmelterRecipe(recipe) && recipe->createdItem) return recipe;
            }
            return nullptr;
        }

        RE::TESBoundObject* ResolveOutput(const RecipeDefinition& definition, RE::TESDataHandler* handler)
        {
            if (definition.plugin.empty() || Lower(definition.plugin) == "skyrim.esm") {
                return RE::TESForm::LookupByID<RE::TESBoundObject>(definition.localID);
            }
            return handler->LookupForm<RE::TESBoundObject>(definition.localID, definition.plugin.c_str());
        }
    }

    void Initialize()
    {
        if (g_initialized) return;
        g_initialized = true;

        const auto settings = ReadSettings();
        if (!settings.enabled) {
            SKSE::log::info("[SmelterExtraRecipes] Disabled by INI.");
            return;
        }
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) {
            SKSE::log::error("[SmelterExtraRecipes] TESDataHandler unavailable.");
            return;
        }
        auto* source = FindTemplate(handler);
        if (!source) {
            SKSE::log::error("[SmelterExtraRecipes] No smelter COBJ template found.");
            return;
        }

        std::vector<RecipeDefinition> definitions;
        if (settings.includeDefaults) {
            for (const auto& [id, label] : kDefaultRecipes) definitions.push_back({ id, "Skyrim.esm", std::string(label) });
        }
        definitions.insert(definitions.end(), settings.custom.begin(), settings.custom.end());

        std::unordered_set<RE::FormID> seen;
        std::size_t added = 0;
        for (const auto& definition : definitions) {
            auto* output = ResolveOutput(definition, handler);
            if (!output) {
                SKSE::log::warn("[SmelterExtraRecipes] Cannot resolve {}|{:08X}.", definition.plugin, definition.localID);
                continue;
            }
            if (!seen.insert(output->GetFormID()).second) {
                SKSE::log::warn("[SmelterExtraRecipes] Duplicate output {:08X}; skipping.", output->GetFormID());
                continue;
            }

            auto* duplicate = source->CreateDuplicateForm(false, nullptr);
            if (!duplicate || duplicate->GetFormType() != RE::FormType::ConstructibleObject) {
                SKSE::log::error("[SmelterExtraRecipes] Failed to duplicate template for {:08X}.", output->GetFormID());
                continue;
            }
            auto* recipe = static_cast<RE::BGSConstructibleObject*>(duplicate);
            recipe->benchKeyword = source->benchKeyword;
            recipe->createdItem = output;
            recipe->data.numConstructed = settings.craftQuantity;
            recipe->requiredItems.containerObjects = nullptr;
            recipe->requiredItems.numContainerObjects = 0;
            recipe->requiredItems.blockStolenItems = false;
            recipe->conditions.head = nullptr;

            if (recipe == source || !recipe->benchKeyword || recipe->createdItem != output ||
                recipe->requiredItems.numContainerObjects != 0 || recipe->conditions.head != nullptr) {
                SKSE::log::error("[SmelterExtraRecipes] Invalid duplicate for {:08X}.", output->GetFormID());
                continue;
            }
            if (!handler->AddFormToDataHandler(recipe)) {
                SKSE::log::error("[SmelterExtraRecipes] Registration failed for {:08X}.", output->GetFormID());
                continue;
            }
            ++added;
            SKSE::log::info("[SmelterExtraRecipes] Added {}|{:08X} -> {} x{}.",
                definition.plugin, definition.localID, output->GetName(), settings.craftQuantity);
        }
        SKSE::log::info("[SmelterExtraRecipes] Registered {}/{} recipes; quantity={}.",
            added, definitions.size(), settings.craftQuantity);
    }
}