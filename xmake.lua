add_rules("mode.debug", "mode.release")

add_repositories("liteldev-repo https://github.com/LiteLDev/xmake-repo.git")

add_requires("levilamina 0.12.1", "simpleini")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("BackupHelper")
    add_cxflags("/EHa", "/utf-8")
    add_defines("NOMINMAX", "UNICODE")
    add_files("src/**.cpp")
    add_includedirs("src")
    add_packages("levilamina", "simpleini")
    add_shflags("/DELAYLOAD:bedrock_server.dll")
    set_exceptions("none")
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")

    after_build(function (target)
        local plugin_packer = import("scripts.after_build")

        local plugin_define = {
            pluginName = target:name(),
            pluginFile = path.filename(target:targetfile()),
        }
        
        plugin_packer.pack_plugin(target,plugin_define)
    end)
