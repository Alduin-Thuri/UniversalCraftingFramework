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

**Current version: 1.1.0 Beta**

The core functionality is operational, but testing is ongoing.

Not every crafting station, modded recipe, or quest-related restriction has been fully verified.

Features, behaviour and compatibility may change during development.

Bug reports and feedback are welcome.

## Author

**Alduin-Thuri**
