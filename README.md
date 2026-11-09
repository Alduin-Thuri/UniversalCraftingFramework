# Universal Crafting Framework

**Universal Crafting Framework (UCF)** is an SKSE plugin for Skyrim Special Edition, developed in C++23 using CommonLibSSE-NG.

The plugin allows players to craft items without having all the required materials while preserving normal material consumption whenever sufficient resources are available.

It aims to extend vanilla crafting functionality without requiring an ESP or ESL plugin.

## Features

### Resource-Free Crafting

- Craft items even when the required materials are unavailable.
- Consume materials normally when sufficient quantities are present.
- Bypass missing material requirements without permanently modifying the original recipe records.
- Automatically refresh supported crafting menus when material availability changes.
- Preserve vanilla perk and quest conditions.

### Extra Smelter Recipes

- Adds 14 optional built-in smelter outputs: 12 Soul Gem variants, Dragon Bones and Dragon Scales.
- Supports custom outputs from Skyrim.esm and installed ESP/ESM/ESL plugins through `SmelterExtraRecipes.ini`.
- `IncludeDefaultRecipes` controls only the 14 added outputs; it does not remove vanilla or modded smelter recipes.
- `CraftQuantity` controls the number of items produced per craft (1–32767).
- Extra recipes do not require materials; duplicate custom outputs are skipped.

### Configuration

The DLL automatically creates missing `Data/SKSE/Plugins/SmelterExtraRecipes.ini` and `Data/SKSE/Plugins/CustomCraftingINI.ini` files on launch. Existing INIs are preserved. Restart Skyrim after editing them.

`CustomCraftingINI.ini` requires the user to list the desired plugin names under `[Plugin]`; its generated list is empty by default.

Example `SmelterExtraRecipes.ini`:

```ini
[Settings]
Enabled=true
IncludeDefaultRecipes=true
CraftQuantity=1

[CustomRecipes]
; Skyrim.esm|0005ACE4
; MyItems.esp|00000800
```

### Supported Crafting Stations

- **Forge** – Weapon and armour crafting
- **Grindstone** – Weapon tempering
- **Smelter** – Ore and ingot processing
- **Tanning Rack** – Leather crafting
- **Workbench** – Armour tempering
- **Cooking** – Cooking recipes
- **Campfire** – Campfire-related crafting
- **Hearthfire** – House construction and furnishing
- **Modded Hearthfire** – Additional construction recipes
- **Staff Enchanter** – Staff crafting
- **Custom Crafting INI** – Configurable support for additional crafting stations

Support and behaviour may vary depending on the crafting station and installed mods.

## Quest Recipe Protection

Version 1.1.0 Beta introduces `QuestRecipeGuard`.

- Identifies 230 quest-associated crafting recipes.
- Preserves the original material requirements of protected recipes.
- Does not intentionally bypass quest progression or perk requirements.
- Helps prevent resource-free crafting from affecting identified quest-related recipes.

**Note:** The protection list is based on identified recipe records. Not every quest-related or modded recipe has been verified.

## Performance

Universal Crafting Framework is designed to minimise unnecessary processing.

- Event-driven crafting updates.
- Ingredient indexing and inventory-query optimisations.
- No continuous per-frame inventory scanning.
- Lightweight integration with native Skyrim crafting systems.

## Requirements

- Skyrim Special Edition / Anniversary Edition
- SKSE
- Address Library for SKSE Plugins

**Tested runtime:** Skyrim 1.7.104

Compatibility with other Skyrim runtimes has not yet been verified.

## Installation

1. Install the required dependencies.
2. Download the latest release.
3. Install the archive using Mod Organizer 2 or another compatible mod manager.
4. Enable the mod and launch Skyrim.

No ESP or ESL plugin is required.

## Technical Information

- **Language:** C++23
- **Framework:** CommonLibSSE-NG
- **Plugin system:** SKSE
- **Build system:** xmake
- **Development environment:** Visual Studio 2026
- **Architecture:** x64

## Building from Source

The source code is available in the `src` directory.

CommonLibSSE-NG is required to build the project and is not included in this repository.

The project uses xmake for compilation.

```powershell
xmake f -m release
xmake build
```

The local `lib`, `build`, `.xmake` and `.vs` directories are excluded from version control.

## Development Status

**Current source: post-1.1.0 Beta (unreleased)**

The core functionality is operational, but testing is ongoing. The extra Smelter recipes, crafting quantities and multiple custom item categories have been tested in-game. Automatic INI regeneration and external-plugin custom outputs still require separate verification.

Not every crafting station, modded recipe, or quest-related restriction has been fully verified.

Features, behaviour and compatibility may change during development.

Bug reports and feedback are welcome.

## Author

**Alduin-Thuri**
