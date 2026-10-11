-- The single reader for engine and external-project .arcmodule descriptors.
local modules_dir = _SCRIPT_DIR

ARC_THIRDPARTY = {
    nlohmann={include="ThirdParty/nlohmann"}, picosha2={include="ThirdParty/picosha2"},
    spdlog={include="ThirdParty/spdlog/include"}, Catch2={include="ThirdParty/Catch2/src",link="Catch2"},
    rapidcheck={include="ThirdParty/rapidcheck/include",link="rapidcheck"},
    rapidcheck_catch={include="ThirdParty/rapidcheck/extras/catch/include"},
    glm={include="ThirdParty/glm"}, stb={include="ThirdParty/stb"},
    miniaudio={include="ThirdParty/miniaudio"}, Astra={include="ThirdParty/Astra/include"},
    enkiTS={include="ThirdParty/enkiTS/src",link="enkiTS"}, tracy={include="ThirdParty/tracy/public",link="tracy"},
    freetype={include="ThirdParty/freetype/include",link="freetype"},
    msdfgen={include="ThirdParty/msdfgen",link="msdfgen"}, NRI={include="ThirdParty/NRI/Include",link="NRI"},
    VulkanHeaders={include="ThirdParty/Vulkan-Headers/include"},
    DirectXHeaders={include="ThirdParty/DirectX-Headers/include"},
    VMA={include="ThirdParty/VMA/include"}, D3D12MA={include="ThirdParty/D3D12MA/include"},
    SDL3={include="@SDL3_INCLUDE_DIR@",link="SDL3"}, imgui={include="ThirdParty/imgui",link="imgui"},
    imguinodeeditor={include="ThirdParty/imgui-node-editor",link="imgui-node-editor"},
    Manifold2D={include="ThirdParty/Manifold2D/include",link="Manifold2D"},
    Mosaic={include="ThirdParty/Mosaic/include"},
    bc7enc_rdo={include="ThirdParty/bc7enc_rdo",link="bc7enc_rdo"},
    cgltf={include="ThirdParty/cgltf"}, meshoptimizer={include="ThirdParty/meshoptimizer/src",link="meshoptimizer"},
}

local function fail(check, detail)
    error("[arcmodule] " .. check .. ": " .. detail, 0)
end

local function keys(value, allowed, file, field)
    if type(value) ~= "table" then fail("schema", file .. ": " .. field .. " must be an object") end
    for key in pairs(value) do
        if not allowed[key] then fail("schema", file .. ": unknown key " .. field .. "." .. tostring(key)) end
    end
end

local function strings(value, file, field)
    if type(value) ~= "table" then fail("schema", file .. ": " .. field .. " must be an array") end
    local count = 0
    for key, item in pairs(value) do
        if type(key) ~= "number" or key < 1 or key % 1 ~= 0 or type(item) ~= "string" or item == "" then
            fail("schema", file .. ": " .. field .. " must be a string array")
        end
        count = count + 1
    end
    if count ~= #value then fail("schema", file .. ": " .. field .. " must be a dense array") end
end

local types = {Runtime=true, Client=true, Developer=true, Editor=true, Game=true, Program=true}
local allowed_edges = {
    Runtime={Runtime=true}, Client={Runtime=true,Client=true},
    Developer={Runtime=true,Client=true,Developer=true,Game=true,Program=true},
    Editor={Runtime=true,Client=true,Developer=true,Editor=true,Game=true,Program=true},
    Game={Runtime=true,Client=true},
    Program={Runtime=true,Client=true,Developer=true,Editor=true,Game=true,Program=true},
}

local function suffix(name)
    -- "ImGui" is one word in the macro (ArcaneImGui -> ARC_IMGUI_API, not ARC_IM_GUI_API).
    local short = (name:match("^Arcane(.+)$") or name):gsub("ImGui", "Imgui")
    return short:gsub("(%l)(%u)", "%1_%2"):gsub("(%u)(%u%l)", "%1_%2"):upper()
end

local function api_macro(mod)
    return mod.apiMacro or ("ARC_" .. suffix(mod.name) .. "_API")
end

local function load_one(file)
    local raw = io.readfile(file)
    if not raw then fail("schema", file .. ": cannot read descriptor") end
    local doc, err = json.decode(raw)
    if not doc then fail("schema", file .. ": " .. tostring(err)) end
    keys(doc, {name=true,type=true,dependencies=true,thirdParty=true,platforms=true,apiMacro=true,apiHeader=true,vendor=true}, file, "descriptor")
    if doc.vendor ~= nil and type(doc.vendor) ~= "boolean" then fail("schema", file .. ": vendor must be boolean") end
    if type(doc.name) ~= "string" or doc.name ~= path.getbasename(file) then
        fail("schema", file .. ": name must equal descriptor stem")
    end
    if not types[doc.type] then fail("schema", file .. ": invalid type " .. tostring(doc.type)) end
    if doc.apiMacro and (doc.name ~= "ArcaneCore" or doc.apiMacro ~= "ARC_CORE_API") then
        fail("schema", file .. ": apiMacro override is reserved for ArcaneCore")
    end
    if doc.apiHeader ~= nil and (type(doc.apiHeader) ~= "string" or doc.apiHeader == "" or path.isabsolute(doc.apiHeader)) then
        fail("schema", file .. ": apiHeader must be a relative path")
    end
    for _, field in ipairs({"dependencies", "thirdParty"}) do
        keys(doc[field], {public=true,private=true}, file, field)
        for _, visibility in ipairs({"public", "private"}) do
            strings(doc[field][visibility], file, field .. "." .. visibility)
        end
    end
    if doc.platforms ~= nil then
        keys(doc.platforms, {windows=true,linux=true,macosx=true}, file, "platforms")
        for platform, settings in pairs(doc.platforms) do
            keys(settings, {links=true,defines=true}, file, "platforms." .. platform)
            for field, list in pairs(settings) do strings(list, file, "platforms." .. platform .. "." .. field) end
        end
    end
    for _, visibility in ipairs({"public", "private"}) do
        for _, vendor in ipairs(doc.thirdParty[visibility]) do
            if not ARC_THIRDPARTY[vendor] then fail("schema", file .. ": unknown thirdParty " .. vendor) end
        end
    end
    doc.file = file
    doc.root = path.getdirectory(file)
    return doc
end

function arc_modules_load(roots)
    local mods = {byName={},order={}}
    local files, seen, relative = {}, {}, {}
    for _, root in ipairs(roots) do
        local found = {}
        if root:match("%.arcmodule$") then found = {root}
        else found = os.matchfiles(path.join(root, "**.arcmodule")) end
        for _, file in ipairs(found) do
            file = path.translate(file, "/")
            if not seen[file] then
                files[#files+1] = file; seen[file] = true
                local rel = root:match("%.arcmodule$") and path.translate(root, "/") or path.getrelative(root, file)
                local root_name = path.getname(path.getabsolute(root))
                if root:match("%.arcmodule$") and path.isabsolute(rel) and ARCANE_SDK then
                    rel = path.getrelative(ARCANE_SDK, rel)
                end
                relative[file] = root:match("%.arcmodule$") and rel or path.translate((root_name == "Source" or root_name == "ThirdParty") and
                    path.join(root_name, rel) or rel, "/")
            end
        end
    end
    table.sort(files)
    for _, file in ipairs(files) do
        local mod = load_one(file)
        if mods.byName[mod.name] then fail("schema", file .. ": duplicate module " .. mod.name) end
        mods.byName[mod.name] = mod
    end
    for _, file in ipairs(files) do
        local mod = mods.byName[path.getbasename(file)]
        local source_type = relative[file]:match("^Source/([^/]+)/")
        local vendor = relative[file]:match("^ThirdParty/")
        if not source_type and not vendor then fail("folder", file .. ": outside Source/<Type>/") end
        if vendor and not mod.vendor then fail("folder", file .. ": ThirdParty descriptor requires vendor=true") end
        if source_type and source_type ~= mod.type then
            fail("folder", file .. ": " .. mod.type .. " does not match Source/" .. source_type)
        end
        for _, visibility in ipairs({"public", "private"}) do
            for _, dep in ipairs(mod.dependencies[visibility]) do
                local target = mods.byName[dep]
                if not target then fail("schema", file .. ": unknown dependency " .. dep) end
                if not allowed_edges[mod.type][target.type] then
                    fail("type-rule", mod.name .. " (" .. mod.type .. ") -> " .. dep .. " (" .. target.type .. ") in " .. file)
                end
            end
        end
    end
    local active, done, stack = {}, {}, {}
    local function visit(name)
        if active[name] then
            local cycle, started = {}, false
            for _, item in ipairs(stack) do
                if item == name then started = true end
                if started then cycle[#cycle+1] = item end
            end
            cycle[#cycle+1] = name
            fail("cycle", table.concat(cycle, " -> "))
        end
        if done[name] then return end
        active[name] = true; stack[#stack+1] = name
        local mod = mods.byName[name]
        for _, visibility in ipairs({"public", "private"}) do
            for _, dep in ipairs(mod.dependencies[visibility]) do visit(dep) end
        end
        stack[#stack] = nil; active[name] = nil; done[name] = true
        mods.order[#mods.order+1] = name
    end
    local names = {}
    for name in pairs(mods.byName) do names[#names+1] = name end
    table.sort(names)
    for _, name in ipairs(names) do visit(name) end
    local template = io.readfile(path.join(modules_dir, "templates/Api.hpp.in"))
    if not template then fail("api-header", "cannot read build/templates/Api.hpp.in") end
    for _, name in ipairs(mods.order) do
        local mod = mods.byName[name]
        if not mod.vendor then
        local count = 0
        for _, cpp in ipairs(os.matchfiles(path.join(mod.root, "src/**.cpp"))) do
            local source = (io.readfile(cpp) or ""):gsub("/%*.-%*/", " "):gsub("//[^\r\n]*", "")
            for declared in source:gmatch("ARC_MODULE%s*%(%s*([%w_]+)%s*%)") do
                if declared == name then count = count + 1 end
            end
        end
        if count ~= 1 then fail("declaration", mod.file .. ": expected exactly one ARC_MODULE(" .. name .. "), found " .. count) end
        local header = mod.apiHeader and path.join(mod.root, mod.apiHeader) or
            path.join(mod.root, "src/Arcane/Modules", name .. "Api.hpp")
        local expected = template:gsub("@MODULE@", suffix(name)):gsub("@API_MACRO@", api_macro(mod))
        local actual = io.readfile(header)
        local function normalized(s) return s and s:gsub("\r\n", "\n") end
        if normalized(actual) ~= normalized(expected) then
            fail("api-header", mod.file .. ": " .. header .. " differs from template")
        end
        end
    end
    return mods
end

function arc_module_closure(mods, name, kind)
    if kind ~= "public" and kind ~= "all" then fail("schema", "closure kind must be public or all") end
    if not mods.byName[name] then fail("schema", "unknown module " .. name) end
    local found = {}
    local function walk(current)
        for _, visibility in ipairs(kind == "public" and {"public"} or {"public", "private"}) do
            for _, dep in ipairs(mods.byName[current].dependencies[visibility]) do
                if not found[dep] then found[dep] = true; walk(dep) end
            end
        end
    end
    walk(name)
    local result = {}
    for _, candidate in ipairs(mods.order) do if found[candidate] then result[#result+1] = candidate end end
    return result
end

local function layout()
    if _OPTIONS.layout == "monolithic" then fail("layout", "monolithic is not wired until P5") end
    return _OPTIONS.layout or "modular"
end

function arc_module_project(mods, name)
    local mod = assert(mods.byName[name], "unknown module " .. name)
    project(name)
    kind(layout() == "monolithic" and "StaticLib" or "SharedLib")
    defines { "ARC_" .. suffix(name) .. "_EXPORTS" }
    if name == "ArcaneCore" then defines { "ARC_CORE_API_EXPORTS" } end
    location(mod.root)
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir("bin/" .. outputdir .. "/%{prj.name}")
    objdir("bin-int/" .. outputdir .. "/%{prj.name}")
    files { "%{prj.location}/src/**.hpp", "%{prj.location}/src/**.cpp" }
    includedirs { "%{prj.location}/src" }
    local root = ARCANE_SDK or "%{wks.location}"
    local vendors = {}
    -- A dependency's PUBLIC thirdParty is linked too: its public headers carry
    -- inline code that calls the vendor (e.g. PhysicsSystem.hpp -> Manifold2D),
    -- and a vendor static lib has no exports to import. The linker only pulls the
    -- objects a module references; the vendor libs linked this way hold no
    -- process-global state (SDL3, which does, is special-cased below).
    local function add_vendor(vendor)
        if vendors[vendor] then return end
        vendors[vendor] = true
        local entry = ARC_THIRDPARTY[vendor]
        local dir
        if vendor == "SDL3" then dir = SDL3_INCLUDE_DIR
        else dir = root .. "/" .. entry.include end
        if dir then includedirs { dir } end
        if vendor == "SDL3" then
            -- ONE SDL instance per process. Off Windows SDL3 is the system's shared
            -- library, so every user links it. On Windows it is vcpkg's SDL3-static:
            -- only ArcaneWindow links it (ARC_MODULE_EXTRAS) and re-exports the SDL
            -- entry points the other modules call -- a second static copy would be a
            -- second SDL with its own window/event/keyboard state.
            filter "system:not windows"
                links { "SDL3" }
                if SDL3_LIB_DIR then libdirs { SDL3_LIB_DIR } end
            filter {}
        elseif entry.link then links { entry.link } end
    end
    for _, dep in ipairs(arc_module_closure(mods, name, "all")) do
        links { dep }
        local dep_mod = mods.byName[dep]
        local dep_root = dep_mod.root
        includedirs { (path.isabsolute(dep_root) and dep_root or (root .. "/" .. dep_root)) .. (dep_mod.vendor and "" or "/src") }
        for _, vendor in ipairs(mods.byName[dep].thirdParty.public) do add_vendor(vendor) end
    end
    for _, visibility in ipairs({"public", "private"}) do
        for _, vendor in ipairs(mod.thirdParty[visibility]) do add_vendor(vendor) end
    end
    for platform, settings in pairs(mod.platforms or {}) do
        filter("system:" .. platform)
        if settings.links then links(settings.links) end
        if settings.defines then defines(settings.defines) end
    end
    filter {}
    filter "system:not windows"
        visibility "Hidden"
    filter "system:linux"
        linkoptions { "-Wl,-rpath,$ORIGIN" }
    filter "system:macosx"
        linkoptions { "-Wl,-rpath,@loader_path" }
    filter {}
    if ARC_MODULE_EXTRAS and ARC_MODULE_EXTRAS[name] then ARC_MODULE_EXTRAS[name]() end
end

function arc_module_stage(mods, names)
    if layout() == "monolithic" then return end
    -- An SDK consumer's own outputdir names ITS layout; the engine's is always -md.
    local out = (not ARCANE_SDK and outputdir) or "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-md"
    local selected = {}
    for _, name in ipairs(names) do
        selected[name] = true
        for _, dep in ipairs(arc_module_closure(mods, name, "all")) do selected[dep] = true end
    end
    local commands = {}
    for _, name in ipairs(mods.order) do
        if selected[name] then
            local extension = os.target() == "windows" and ".dll" or (os.target() == "macosx" and ".dylib" or ".so")
            local prefix = os.target() == "windows" and "" or "lib"
            commands[#commands+1] = '{COPYFILE} "' .. (ARCANE_SDK or "%{wks.location}") .. '/bin/' .. out .. '/' .. name .. '/' .. prefix .. name .. extension .. '" "%{cfg.buildtarget.directory}/' .. prefix .. name .. extension .. '"'
        end
    end
    postbuildcommands(commands)
    -- In-workspace: a staged-but-unlinked module still has to be BUILT first.
    if not ARCANE_SDK then
        local order = {}
        for _, name in ipairs(mods.order) do if selected[name] then order[#order+1] = name end end
        dependson(order)
    end
end

-- Link + include the module closure of `names`. opts.stage: false = do not
-- copy the module binaries beside the target (a loaded game module: the host
-- already carries them); a list = stage that closure instead (a host that maps
-- modules it never links, e.g. ArcaneServer for a game module's full set).
function arc_module_consumer(mods, names, opts)
    opts = opts or {}
    local selected = {}
    for _, name in ipairs(names) do
        selected[name] = true
        for _, dep in ipairs(arc_module_closure(mods, name, "all")) do selected[dep] = true end
    end
    local root = ARCANE_SDK or "%{wks.location}"
    local vendors = {}
    for _, name in ipairs(mods.order) do
        if selected[name] then
            local mod = mods.byName[name]
            links { name }
            if ARCANE_SDK then libdirs { ARCANE_SDK .. "/bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-md/" .. name } end
            includedirs { (path.isabsolute(mod.root) and mod.root or root .. "/" .. mod.root) .. (mod.vendor and "" or "/src") }
            if opts.vendors ~= false then
                for _, vendor in ipairs(mod.thirdParty.public) do vendors[vendor] = true end
            end
        end
    end
    local vendor_names = {}
    for vendor in pairs(vendors) do vendor_names[#vendor_names+1] = vendor end
    table.sort(vendor_names)   -- deterministic include order across generations
    for _, vendor in ipairs(vendor_names) do
        local entry = ARC_THIRDPARTY[vendor]
        if vendor == "SDL3" then includedirs { SDL3_INCLUDE_DIR }
        elseif entry then includedirs { root .. "/" .. entry.include } end
    end
    if opts.stage ~= false then arc_module_stage(mods, type(opts.stage) == "table" and opts.stage or names) end
end

function arc_module_forced_refs(names)
    local options = {}
    for _, name in ipairs(names) do
        local symbol = "ArcModule_" .. name
        if os.target() == "windows" then options[#options+1] = "/INCLUDE:" .. symbol
        elseif os.target() == "macosx" then options[#options+1] = "-Wl,-u,_" .. symbol
        else options[#options+1] = "-Wl,-u," .. symbol end
    end
    linkoptions(options)
end
