set_project("UniversalCraftingFramework")
set_version("1.0.0")
set_languages("c++23")
set_arch("x64")

add_rules("mode.debug", "mode.release")

includes("lib/commonlibsse-ng")

target("UniversalCraftingFramework")
    set_kind("shared")
    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "UniversalCraftingFramework",
        author = "Alduin",
        description = "Universal Skyrim crafting diagnostics"
    })
    add_files("src/*.cpp")
