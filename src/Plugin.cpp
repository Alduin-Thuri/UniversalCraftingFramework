#include <SKSE/SKSE.h>

namespace UniversalCraftingFramework::Smelter
{
    void Initialize();
}

namespace UniversalCraftingFramework::TanningRack
{
    void Initialize();
}

namespace UniversalCraftingFramework::Workbench
{
    void Initialize();
}

namespace UniversalCraftingFramework::Grindstone
{
    void Initialize();
}

namespace UniversalCraftingFramework::Forge
{
    void Initialize();
}

namespace
{
    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (!message || message->type != SKSE::MessagingInterface::kDataLoaded) {
            return;
        }

        UniversalCraftingFramework::Smelter::Initialize();
        UniversalCraftingFramework::TanningRack::Initialize();
        UniversalCraftingFramework::Workbench::Initialize();
        UniversalCraftingFramework::Grindstone::Initialize();
        UniversalCraftingFramework::Forge::Initialize();
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    auto* messaging = SKSE::GetMessagingInterface();
    return messaging && messaging->RegisterListener(OnMessage);
}
