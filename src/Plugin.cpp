#include "QuestRecipeGuard.h"
#include <SKSE/SKSE.h>

namespace UniversalCraftingFramework::Smelter { void Initialize(); }
namespace UniversalCraftingFramework::SmelterExtraRecipes { void Initialize(); }
namespace UniversalCraftingFramework::TanningRack { void Initialize(); }
namespace UniversalCraftingFramework::Workbench { void Initialize(); }
namespace UniversalCraftingFramework::Grindstone { void Initialize(); }
namespace UniversalCraftingFramework::Forge { void Initialize(); }
namespace UniversalCraftingFramework::Cooking { void Initialize(); }
namespace UniversalCraftingFramework::Campfire { void Initialize(); }
namespace UniversalCraftingFramework::Hearthfire { void Initialize(); }
namespace UniversalCraftingFramework::HearthfireModded { void Initialize(); }
namespace UniversalCraftingFramework::CustomCraftingINI { void Initialize(); }
namespace UniversalCraftingFramework::StaffEnchanter { void Initialize(); }

namespace
{
    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (!message || message->type != SKSE::MessagingInterface::kDataLoaded) {
            return;
        }

        SKSE::log::info("[UCF Diagnostic] kDataLoaded: module initialization beginning");
        UniversalCraftingFramework::QuestRecipeGuard::Initialize();

        SKSE::log::info("[UCF Diagnostic] BEFORE SmelterExtraRecipes");
        UniversalCraftingFramework::SmelterExtraRecipes::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER SmelterExtraRecipes");

        SKSE::log::info("[UCF Diagnostic] BEFORE Smelter");
        UniversalCraftingFramework::Smelter::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER Smelter");

        SKSE::log::info("[UCF Diagnostic] BEFORE TanningRack");
        UniversalCraftingFramework::TanningRack::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER TanningRack");

        SKSE::log::info("[UCF Diagnostic] BEFORE Workbench");
        UniversalCraftingFramework::Workbench::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER Workbench");

        SKSE::log::info("[UCF Diagnostic] BEFORE Grindstone");
        UniversalCraftingFramework::Grindstone::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER Grindstone");

        SKSE::log::info("[UCF Diagnostic] BEFORE Forge");
        UniversalCraftingFramework::Forge::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER Forge");

        SKSE::log::info("[UCF Diagnostic] BEFORE Cooking");
        UniversalCraftingFramework::Cooking::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER Cooking");

        SKSE::log::info("[UCF Diagnostic] BEFORE Campfire");
        UniversalCraftingFramework::Campfire::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER Campfire");

        SKSE::log::info("[UCF Diagnostic] BEFORE Hearthfire");
        UniversalCraftingFramework::Hearthfire::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER Hearthfire");

        SKSE::log::info("[UCF Diagnostic] BEFORE HearthfireModded");
        UniversalCraftingFramework::HearthfireModded::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER HearthfireModded");

        SKSE::log::info("[UCF Diagnostic] BEFORE CustomCraftingINI");
        UniversalCraftingFramework::CustomCraftingINI::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER CustomCraftingINI");

        SKSE::log::info("[UCF Diagnostic] BEFORE StaffEnchanter");
        UniversalCraftingFramework::StaffEnchanter::Initialize();
        SKSE::log::info("[UCF Diagnostic] AFTER StaffEnchanter");

        SKSE::log::info("[UCF Diagnostic] All modules initialized");
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SKSE::log::info("[UCF Diagnostic] SKSE::Init completed");
    auto* messaging = SKSE::GetMessagingInterface();
    SKSE::log::info("[UCF Diagnostic] Messaging interface available: {}", messaging != nullptr);
    if (!messaging) {
        return false;
    }
    const bool registered = messaging->RegisterListener(OnMessage);
    SKSE::log::info("[UCF Diagnostic] RegisterListener result: {}", registered);
    return registered;
}