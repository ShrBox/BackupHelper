add_rules("mode.debug", "mode.release")

add_repositories("liteldev-repo https://github.com/LiteLDev/xmake-repo.git")

if is_config("target_type", "server") then
    add_requires("levilamina 26.20.0", {configs = {target_type = "server"}})
else
    add_requires("levilamina 26.20.0", {configs = {target_type = "client"}})
end

add_requires("levibuildscript", "simpleini")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server", "client")
option_end()

target("BackupHelper")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    if is_plat("windows") then
            add_defines("NOMINMAX", "UNICODE")
            set_exceptions("none") -- To avoid conflicts with /EHa.
            add_cxflags( "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
            add_cxflags(
                "/EHs",
                "-Wno-microsoft-cast",
                "-Wno-invalid-offsetof",
                "-Wno-c++2b-extensions",
                "-Wno-microsoft-include",
                "-Wno-overloaded-virtual",
                "-Wno-ignored-qualifiers",
                "-Wno-missing-field-initializers",
                "-Wno-potentially-evaluated-expression",
                "-Wno-pragma-system-header-outside-header",
                {tools = {"clang_cl"}}
            )
            set_toolchains("clang-cl")
        end
    add_packages("levilamina", "simpleini")
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    add_files("src/**.cpp")
    add_includedirs("src")
    after_build(function (target)
        local assetdir = path.join(os.projectdir(), "assets")
        local bindir = path.join(os.projectdir(), "bin")
        local outputdir = path.join(bindir, target:name())
        os.mkdir(outputdir)
        os.cp(path.join(assetdir, "*"), outputdir)
    end)
