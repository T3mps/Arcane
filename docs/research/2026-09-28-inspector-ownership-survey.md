# Inspector ownership survey: who owns the properties panel in single-window editors

2026-09-28. Three parallel research passes (game engines; DCC/creative tools; IDEs, designers and
other single-window pro apps), each ~20 minutes, official docs and issue trackers cited inline;
unverifiable details are flagged where they occur. The synthesis and the decision are in
`docs/superpowers/specs/2026-09-28-inspector-ownership-design.md`.

Headline findings:
- Every product surveyed has ONE global, contextual properties panel; none deliberately shows two
  selection-following inspectors side by side. Embedded per-editor panels exist (Unity Shader Graph,
  Android Studio, IntelliJ GUI designer) and are documented as problems.
- "The active editor drives the inspector" works only with: keying on the part that SUPPLIES a
  selection rather than focus (Eclipse #23862/#43085, Visual Studio's selection context); a page
  contributed by the source into the one panel; and pin + a second instance as the escape hatch.
- Blender splits by charge: global panel = data-block settings; per-area sidebar = that editor's
  view settings, tool and transient item.


---


# Research A: who owns the Inspector in single-window game-engine editors

## Godot 4 (docs precise)
1. GLOBAL + CONTEXTUAL. "The Inspector dock lists all properties of an object, resource, or node"; it follows selection in the Scene dock and also follows resources opened from FileSystem. History: "The '<' and '>' arrows let you navigate through your edited object history", plus a history-list button. https://docs.godotengine.org/en/stable/tutorials/editor/inspector_dock.html
2. Sub-resources are edited IN the same inspector (expand inline, or click into the sub-resource and the inspector navigates to it -- that push is what the history arrows undo). Same page.
3. Hybrid: one Inspector dock + a sibling "Node" dock (signals/groups) in the same tab strip; history arrows; NO lock/pin and NO second inspector (proposals #9265 closed by consolidation, #13684 opened 2025-11-22 still open, #7459 "multiple Inspectors" open, "Needs consensus"). https://github.com/godotengine/godot-proposals/issues/9265 https://github.com/godotengine/godot-proposals/issues/13684 https://github.com/godotengine/godot-proposals/issues/7459 https://github.com/godotengine/godot-proposals/discussions/12709
4. Other editors (Shader, VisualShader, Animation, TileMap, Theme...) are BOTTOM PANELS raised by EditorPlugin::_handles()/_edit()/_make_visible() when the matching object becomes the inspected object -- they are the "big" editor for the object the Inspector is already showing, so there is no second properties panel: the Inspector keeps showing that node/resource (AnimationPlayer, ShaderMaterial, TileMap) while the bottom panel edits its payload. https://docs.godotengine.org/en/stable/classes/class_editorplugin.html  Scene selection is not preserved separately: selecting the resource replaces it in the Inspector (recover via history arrows). Since 4.1 the shader/script editor can "Make Floating" (own OS window); 4.6 made the bottom panel part of the dock layout. https://docs.godotengine.org/en/stable/tutorials/editor/customizing_editor.html
5. Rationale/complaints: pin/lock requests recur since 2022 (drag N nodes into an Array property is the canonical complaint: "selecting anything in the Scene Tree immediately updates the Inspector, making multi-selection drag-and-drop almost unusable"). Godot 4.4 shipped a PIN for the BOTTOM PANEL only (PR #98074, merged 2024-11-11): "if you have the 'Output' tab open, then you select an AnimationPlayer node, having the lock enabled will stop it from switching to the 'Animation' tab"; icon changed from lock to pin because "Locks usually means you can't edit something". https://github.com/godotengine/godot/pull/98074  VisualShader node params: the docs describe inline port/expression editing on nodes; whether node params ever route to the Inspector dock is NOT verified from docs. https://docs.godotengine.org/en/stable/tutorials/shaders/visual_shaders.html

## Unity (docs precise)
1. GLOBAL + CONTEXTUAL: one Inspector window follows the editor-wide Selection, and tool windows deliberately REUSE it -- Animator states: "Select the state in the Animator Controller, to view the properties for the state in the Inspector window" https://docs.unity3d.com/Manual/class-State.html ; Timeline clips edited in the Inspector https://docs.unity3d.com/Packages/com.unity.timeline@1.8/manual/insp-clip-control.html ; VFX Graph: "every Graph Element displays settings both in the Graph ... and in the Inspector when you select a Node" https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@7.1/manual/Blocks.html
2. EMBEDDED exceptions (each a UI-Toolkit-era rewrite): Shader Graph "Graph Inspector" (10.0+): "a floating window that displays settings related to objects you select in the graph"; "All per-node settings that you previously managed by opening a settings (gear icon) sub-menu are now accessible through the Graph Inspector"; tabs Node Settings / Graph Settings; Blackboard "visible by default, and you cannot drag it off the graph". https://docs.unity3d.com/Packages/com.unity.shadergraph@10.1/manual/Upgrade-Guide-10-0-x.html https://docs.unity3d.com/Packages/com.unity.shadergraph@17.3/manual/Internal-Inspector.html  Input Actions editor (1.8+): three panes in ONE window, pane C "Displays the properties of the currently selected Action or Binding from the Actions panel"; same editor is embedded in Project Settings for project-wide actions. https://docs.unity3d.com/Packages/com.unity.inputsystem@1.16/manual/ActionsEditor.html  UI Builder: "The UI Builder Inspector pane is similar to the Unity Inspector window" (own pane inside the Builder). https://docs.unity3d.com/Manual/UIB-interface-overview.html
3. Hybrid mechanisms on the global Inspector: LOCK ("no longer updates if you change the selection"), MULTI-INSTANCE ("You can open as many Inspector windows as you want" via Add Tab > Inspector), FOCUSED inspectors (right-click > Properties / Alt+P: "always display the properties of the item you opened it for, even if you select something else", float or dock, restored across sessions), Debug mode. https://docs.unity3d.com/Manual/InspectorOptions.html https://docs.unity3d.com/Manual/InspectorFocused.html
4. Scene selection: replaced (Selection is global) unless a locked/focused Inspector holds it. In Shader Graph the scene selection is untouched because graph selection never enters editor Selection -- that is the practical effect of the embedded Graph Inspector.
5. Rationale: Unity's docs give none for the Shader Graph move ("no changes to data"). Observable: graph-internal objects are not UnityEngine.Objects, so the global Inspector could not host them without a proxy; UI Toolkit makes an in-window pane cheap. Complaints: Graph Inspector floating pane sticks to cursor / size not saved (issue tracker, changelog fixes); Shader Graph window forgets floating state ("super annoying to always be undocking and moving and resizing"). https://issuetracker.unity3d.com/issues/hdrp-graph-inspector-window-sticks-to-the-mouse-cursor-on-changing-any-dropdown-value-in-node-settings https://forum.unity.com/threads/any-way-to-keep-shader-graph-window-opening-as-a-floating-window.891421/  Lock: "trying to drag items into the inspector, but end up clicking on the object instead, changing the content of the inspector" -- lock is a lesser-known fix. https://discussions.unity.com/t/the-importance-of-the-lock-button-in-unity-editor-inspector-window/887171

## Flax Engine
1. Global Properties window follows scene selection: "shows properties of the selected objects. Used to view and edit the scene objects (scripts, actors)". https://docs.flaxengine.com/manual/editor/interface.html
2. EMBEDDED per asset editor: Material editor "UI consists of a toolstrip, viewport, properties panel and surface graph"; "This panel contains all material asset properties and allows editing them" (+ Add parameter). Anim Graph window has the identical shape ("contains all anim graph asset parameters"). Asset editors open on double-click as docked windows. https://docs.flaxengine.com/manual/graphics/materials/material-editor/index.html https://docs.flaxengine.com/manual/animation/anim-graph/interface.html
3. Two-tier: global Properties for the scene; each asset window carries its own asset-level property panel (node params live on the node).
4. Scene selection stays in the global Properties window while a material window is active (the embedded panel never competes). Lock / multi-instance: not documented.
5. Rationale: undocumented.

## Stride (Game Studio)
1. Global Property Grid: "displays the properties of the asset or entity you select"; Unity mapping "select an asset in the Asset View and edit its properties in the Property Grid". https://doc.stride3d.net/4.0/en/manual/game-studio/index.html https://doc.stride3d.net/4.0/en/manual/stride-for-unity-developers/index.html
2. Dedicated editors (scene, prefab, UI page, sprite sheet, script) "open in a new tab. You can arrange the tabs how you like, or float them as separate windows"; entity components are added in the same Property Grid ("on the right by default") while the scene tab is active. https://doc.stride3d.net/4.0/en/manual/game-studio/add-entities.html
3. Hybrid: single global grid, contextual to the active tab selection, plus an Asset Preview pane.
4. Switching tab replaces the grid content (inferred from "selected asset or entity"; not stated verbatim). No lock documented.
5. None documented.

## Defold
1. Global Outline + Properties, scoped to the ACTIVE DOCUMENT: Outline "shows the content of the file currently being edited"; Properties "shows properties associated with the currently selected item". https://defold.com/manuals/editor/
2. No embedded panels; every document type (collection, GUI, particle FX, tilemap, atlas) drives the same two panes; curve editing is a Tools-pane tab.
3. Mechanism = document-scoped context: "It is possible to open 2 editor views side by side" (Move to Other Tab Pane) but one Properties pane.
4. Switching document tab swaps Outline+Properties to that document; the scene selection is not shown until you return (no lock).
5. None documented.

## O3DE
1. Entity Inspector global + contextual ("Select an entity in the viewport or the Entity Outliner"); PIN: "Always shows the pinned entity even when you select a different entity", pinned inspectors for multiple entities, "multiple inspector instances of the same entity ... helps you compare". https://docs.o3de.org/docs/user-guide/editor/entity-inspector/
2. Non-scene editors are separate tools with their OWN inspectors: Script Canvas (Tools > Script Canvas) has Node Palette, Graph Outliner, Variable Manager and "In the Node Inspector, you can view and modify the properties for a selected node"; Material Editor is a standalone application/exe with its own inspector; input bindings are edited in the Asset Editor tool. https://docs.o3de.org/docs/user-guide/scripting/script-canvas/get-started/editor-interface/ https://docs.o3de.org/docs/atom-guide/look-dev/tools/material-editor/ https://docs.o3de.org/docs/user-guide/interactivity/input/using-player-input/
3. Hybrid: pin + multi-instance on the entity inspector; per-tool inspectors elsewhere (Qt dock widgets, dockable anywhere). https://docs.o3de.org/docs/user-guide/editor/customizing/
4. Entity Inspector keeps the scene selection because graph/material selection never touches it.
5. None documented beyond the compare use-case.

## CryEngine Sandbox (5.x)
1. Properties panel = "the equivalent to Unity's Inspector"; "exposes properties of selected objects"; appears "in the right-hand pane of the main window once the first Entity has been added". https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/26876254 https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/28180926
2. Tool editors (Flow Graph, Material Editor, Schematyc, Particle, Character Tool) are Qt tool windows that dock as tabs anywhere ("as a tab within another window"); their internal property panels were not verifiable (doc pages return only navigation).
3. Hybrid by docking only; lock / multi-instance not verified.
4/5. Not verified.

## Bevy editor efforts (prototype, 2025-26)
1. Contextual: "components of selected entity are shown in the inspector"; the inspector is scoped as a TOOL ("can and should be spun out and shipped as a helpful first-party dev tool"). https://bevyengine.github.io/bevy_editor_prototypes/roadmap.html
2. PR #25885 puts the details panel "next to the entity tree within the inspector tool itself"; reviewers already want it "resizable and wrappable, and ideally movable" and note floating vs docked behaviour differs. https://github.com/bevyengine/bevy/pull/25885
3-5. Multi-editor ownership undecided; discussion #24010 notes prototype inspector panels "cannot currently be shared between different editor implementations without coupling". https://github.com/bevyengine/bevy/discussions/24010

## Hazel (ImGui)
1. Global Properties panel follows Scene Hierarchy selection; asset editors derive from AssetEditorPanel and open as their own ImGui windows ("DOUBLE CLICKING on an asset field ... opens that asset in the relevant editor / window"). https://docs.hazelengine.com/HazelReleaseNotes/Hazel-2023.1
2-5. Whether asset editor windows embed property widgets is not documented publicly; no lock / multi-instance documented.

## GameMaker
1. Global Inspector "shows all the properties associated with the selected elements in the IDE" across editors (rooms, layers, sequences, objects). 3. LOCK ("a locked inspector window does not lock the editing of any values") + MULTI-INSTANCE ("You can open multiple Inspector windows at any time"); dock or float. https://manual.gamemaker.io/lts/en/IDE_Tools/The_Inspector.htm

## Construct 3
1. Global Properties Bar "displays a list of all the settings you can change on whatever is selected" (project, layout, layer, instances, events) shared by Layout View / Event Sheet View. 3. Bars dock/float; no lock documented. https://www.construct.net/en/make-games/manuals/construct-3/interface/bars/properties-bar (403 on fetch; from search snippets)

## Patterns
- Global contextual inspector (selection-follows): Godot, Unity, Flax (scene), Stride, Defold, O3DE (entities), CryEngine, GameMaker, Construct, Hazel, Bevy.
- Lock/pin on the inspector: Unity, O3DE, GameMaker. Godot: pin on the BOTTOM PANEL only (4.4).
- Multi-instance inspector: Unity (Add Tab), Unity focused/Properties window (per-object), O3DE (pinned per entity), GameMaker.
- History back/forward in the inspector: Godot only (+ history list).
- Sub-object navigation in place (inspector pushes into a sub-resource): Godot.
- Document-scoped inspector (properties follow the active tab, one pane): Defold, Stride.
- Embedded per-editor properties pane inside the tool window: Unity Shader Graph (Graph Inspector), Unity Input Actions, Unity UI Builder, Flax material/anim-graph windows, O3DE Script Canvas Node Inspector, O3DE Material Editor.
- Tool window reuses the global inspector (graph selection becomes editor Selection): Unity Animator, Timeline, VFX Graph.
- Companion editor as a bottom panel for the CURRENTLY inspected object (no second properties pane): Godot.
- Mode tabs inside the embedded pane (Node Settings / Graph Settings): Unity Shader Graph.
- Sibling dock beside the inspector for a different facet of the same selection: Godot "Node" dock (signals/groups).

## What fails
- Selection-follows without lock: drag-N-into-array impossible, tuning A while moving B impossible (Godot #9265/#13684/#7459 still open; Unity users needed the lock for the same thing).
- Lock: users forget it is on ("Inspector stuck on one object" threads); a lock icon reads as read-only (Godot renamed to pin).
- Multi-instance: solves compare/copy but multiplies panes; Godot maintainers left #7459 at "Needs consensus".
- History arrows (Godot): recovery only, not concurrency; a sub-resource push replaces the scene selection view.
- Bottom-panel auto-switch (Godot): selecting an AnimationPlayer hijacks the panel you were reading -> 4.4 pin.
- Embedded floating pane (Unity Graph Inspector): pane-in-window bugs (sticks to cursor, size not saved), window layout not persisted, occludes the graph, cannot dock into the main layout.
- Tool windows sharing the global inspector (Unity Animator/Timeline): clicking a state/clip evicts the scene object from the Inspector; only lock / second inspector mitigates.
- Document-scoped panes (Defold/Stride): switching tabs loses the scene selection view entirely.
- Separate exe/tool with its own inspector (O3DE Material Editor): consistent but duplicates inspector code and styling; Bevy notes inspector panels cannot be shared across editors without type coupling.


---

# Research B -- DCC / creative tools: who owns the properties panel in a one-window editor

Legend: (1) global+contextual? (2) per-area embedded? (3) hybrid mechanisms (4) primary selection's properties while working elsewhere (5) rationale / complaints.

## Blender  (the closest analogue -- two-tier, precisely)
- (1) YES, one GLOBAL Properties editor, context-driven. "Properties are grouped into tabs, shown as a vertical list of icons in the Navigation Bar"; which tabs exist depends on the active object ("Some are only shown for certain object types"; Material/Texture "Shown only when relevant"). Context = the ACTIVE object of the window's view layer, not "last click in any editor". Source: manual RST https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/editors/properties_editor.rst (rendered: https://docs.blender.org/manual/en/latest/editors/properties_editor.html).
- (2) YES as well, but with a different CHARGE: every area (3D Viewport, Node Editors, Image, Sequencer...) has a Sidebar region (N). Definition: "The Sidebar (on the right side of the editor area) contains Panels with settings of objects within the editor and the editor itself." https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/interface/window_system/regions.rst
  - 3D Viewport Sidebar: Item tab "displays properties of the active selection" (Transform of the active object/element); Tool tab = "settings for the currently active tool" + workspace; View tab = "settings that affect only the current 3D Viewport". https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/editors/3dview/sidebar.rst
  - Node Editor Sidebar: Node tab = Name / Label / Color / Properties of the ACTIVE NODE ("displayed properties vary depending on the selected node type"); Tool; View; Group/Options. https://docs.blender.org/manual/en/3.6/interface/controls/nodes/sidebar.html
  - Division of responsibility: Properties editor = DATA-BLOCK settings (scene, render, object, mesh, material, modifiers...) keyed off the active object; Sidebar = (a) the editor's own view settings, (b) the active tool, (c) the "item" the editor is showing (active node in a node editor, active object transform in a viewport). Node properties are NEVER in the Properties editor; the material DATA-BLOCK (slots, settings, viewport display, and a flattened read-out of the node tree under "Surface") is in the Properties editor's Material tab. The Shader Editor's Sidebar Options panel deliberately duplicates: it "contains the same settings that are also available in the Material tab in the Properties" (manual/editors/shader_editor.rst) -- so the node editor can be used stand-alone. Community reading: the Properties editor's Surface/Volume sections are "compressed versions of the node editor ... redundant to the node editor" (artisticrender.com, non-official).
- (3) Hybrid mechanisms: PIN in the Properties editor header -- "lock the editor to the current data-block, preventing it from changing when the selection updates"; Shader Editor header PIN -- "Keeps the current material selection visible in the Shader editor even when another object or material is selected elsewhere"; "Sync with Outliner" (Always / Never / Auto = "Only follow when the Properties editor shares a border with the Outliner") decides whether clicking an icon in the Outliner switches the Properties TAB; multiple Properties editors allowed (any area can be a Properties editor; pin each); Ctrl-F property search greys non-matches.
- (4) Selection is window-global (one active object per view layer); clicking a node in the Shader Editor does NOT change the active object, so the Properties editor stays on that object's material tab -- the node's own properties live in the node editor's Sidebar. Switching the active object switches every unpinned Properties editor and every unpinned node editor to the new object's material.
- (5) Complaints: tab reset on selection -- "When you have a properties editor open on the physics tab, selecting a new object will reset that to the object properties tab. Pinning ... doesn't really solve this - the object then can't be changed" (issue #63991 Outliner/Properties syncing, https://projects.blender.org/blender/blender/issues/63991 -- 403 to fetch; quote via search snippet). Sidebar overcrowding by add-ons is the other chronic complaint; Blender's own guidelines steer add-ons: data-block-level UI belongs in the Properties editor category ("If your add-on deals with mesh data, maybe the best place for it is the Object Data category in Properties editor"), tool/scene operators in the Sidebar; "only create new tabs when necessary" (HIG Sidebar Tabs, https://wiki.blender.org/wiki/Human_Interface_Guidelines/Sidebar_Tabs ; https://hackmd.io/@nickberckley/BkdCFoV2T). The devtalk thread "Solution to the sidebar panel design" (https://devtalk.blender.org/t/solution-to-the-sidebar-panel-design/3515) is a multi-year complaint about vertical tab strips in the sidebar.

## Houdini
- (1) Global-contextual by default: the Parameter Editor "lets you edit the parameters of the currently selected node"; "When a node is selected in the network editor, the parameter editor switches to show the parameters for the selected node." https://www.sidefx.com/docs/houdini/basics/panes.html , https://www.sidefx.com/docs/houdini/network/parms.html
- (2) No per-area embedding: parameters are ALWAYS a separate pane type. Any pane tab can be a Parameters pane; you can have many.
- (3) Mechanisms: PIN -- "Click the Pin button at the top of a pane tab to pin the tab to its current network/node"; LINK GROUPS -- right-click the pin: "No link (pinned)", "Last Selected Node (unpinned)", or a numbered channel so a network editor + parameter editor pair follow each other independently of other pairs; documented recipe "Create a new parameter editor pane that always edits at a certain node": new tab -> Parameters -> Pin; floating parameter window per node (RMB node -> Parameters and Channels); floating panels (Window -> New floating panel); Parameter Spreadsheet for many nodes at once. The docs' own caution: "Tab linking can be complicated and unintuitive ... new users can probably ignore it and just use pinning and unpinning" (panes.html, via search snippet).
- (4) Unpinned parameter panes ALL jump to the last selected node, across any network editor in the same link group. Pinned ones hold.
- (5) Forum complaint: pinning locks the NODE, not the TAB; users wanting the same parameter tab across many nodes are told to open two parameter panes and pin one (https://www.sidefx.com/forum/topic/99565/).

## Nuke  (distinct model: a BIN of stacked panels)
- (1) Not selection-driven at all: a node's properties panel opens on double-click / Ctrl-click / Return, into the Properties Bin, and STAYS open until closed. Many panels stack. https://learn.foundry.com/nuke/content/getting_started/using_interface/properties_panels.html
- (2) No per-area embed; one bin (dockable pane) + optional floating panels.
- (3) Mechanisms: max-panels field on the bin ("max nodes in properties bin", default 10) evicts the oldest; LOCK button -- "have all new panels appear in floating windows"; float / close / Alt-click close-all; prefs "reopen acts like new panel", "expand / collapse panels in Properties bin to match selection" (selection collapses unselected panels rather than replacing them). https://learn.foundry.com/nuke/11.1/content/appendices/appendixa/available_preferences_studio.html
- (4) Selecting another node does not evict the open panel; it may collapse it (pref). Working elsewhere = your panel is still there, possibly buried.
- (5) Complaint: "up to 10 properties panels open ... way too many ... clutter of onscreen controls, which could lead to you changing properties in the wrong node"; pros set max to 2 (https://conradolson.com/four-nuke-preferences-i-always-change-immediately).

## Maya
- (1) Global-contextual: the Attribute Editor (AE) shows the selected node; "Tabs across the top of the Attribute Editor let you select nodes connected to the shown node" (shape, transform, shading group, material...). Channel Box is a SECOND global contextual panel with a narrower charge: "the Channel Box provides a more compact view of keyable attributes for animation, the Attribute Editor gives you full graphical controls". https://help.autodesk.com/cloudhelp/2024/ENU/Maya-Basics/files/GUID-67A58D31-4722-4769-B3E6-1A35B5B53BED.htm
- (2) No per-area embed (editors like Hypershade/Node Editor rely on the same AE, docked or floating).
- (3) Mechanisms: "Click the Pin Tab icon to keep the selected tab loaded, even when you make another selection"; "Copy Tab" spawns a detached, non-following copy ("a new window with a non-changing set of attributes", https://lesterbanks.com/2019/08/use-the-copy-tab-in-maya-to-prevent-losing-selections/); Show menu filters attributes (not verified this pass).
- (4) Unpinned AE follows selection wherever it happens; connected-node tabs give a breadcrumb-ish way to reach related nodes without reselecting.
- (5) Complaint thread "Redesign the Attribute Editor" (https://forums.autodesk.com/t5/maya-ideas/redesign-the-attribute-editor/idi-p/7960097, 403 on fetch): Channel Box / AE duplication and the AE's density are perennial.

## Cinema 4D
- (1) Global-contextual Attribute Manager; it "will switch modes automatically when you select a new element in order to display that element's settings"; "can only display one type of settings at a time" (Object / Tool / Snap / Project / View / Camera modes). https://help.maxon.net/c4d/r21/us/html/5824.html
- (2) No per-area embed.
- (3) "New Attribute Manager" + "Lock Element" (lock icon): "displays the parameters of the object permanently, even when other objects are selected"; "Lock Mode" keeps the mode (e.g. Tool) from auto-switching. Multiple managers each lockable.
- (4) Unlocked manager follows selection; locked ones hold. Explicit rationale in the manual: "If your scene has an object that you frequently need to edit, create a new Attribute Manager and lock it".
- (5) No documented complaint found; the lock/new-manager pair is the textbook answer.

## Substance 3D Designer / Painter
- Designer (1) global-contextual Properties (a.k.a. Parameters) panel beside the Graph view: "select the node and open the Properties panel"; click empty graph space or the graph in Explorer to get GRAPH parameters in the same panel (graph = "nothing selected" context). https://experienceleague.adobe.com/en/docs/substance-3d-designer/using/substance-graphs/graph-parameters (2) no per-area embed (3) none documented (no pin found -- unverified) (4) follows selection across all open graph tabs (one panel).
- Painter (1) global-contextual: "The properties window changes depending on your current selection or tool. Clicking on a layer will display its properties. Paint Layers will show the Tool parameters while Fill layers will display the Fill properties." https://helpx.adobe.com/substance-3d-painter/interface/properties.html (3) none: "does not mention multiple simultaneous Properties windows or pinning".

## DaVinci Resolve (per PAGE inspector)
- (1) Each page (Media, Cut, Edit, Fusion, Fairlight) has its own Inspector for that page's selection type: Edit = selected clip (Video/Audio/Effects/Transition/Image/File tabs); Fusion = selected tool ("display and manipulate the parameter of any selected effect or tool in the Node Editor", Tools/Modifiers tabs). Pages are full-screen modes, so two inspectors never coexist on screen. https://documents.blackmagicdesign.com/UserManuals/DaVinci-Resolve-20-Fusion-Visual-Effects.pdf ; https://edits101.com/davinci-resolve-inspector/
- (2) Effectively per-area (per page) but never side by side.
- (3) Fusion: multi-select stacks several tools' controls in one Inspector; per-tool PIN keeps a tool's controls visible while selecting another ("pin one of the tools in the Inspector so that the controls for both tools always remain visible", https://mixinglight.com/color-grading-tutorials/creating-fusion-effects-davinci-resolve17/); Color page has no Inspector at all (palettes instead).
- (4) Switching pages changes the whole surface, including which inspector exists.
- (5) Forum grumbles about Inspector tab/scroll resetting on selection (https://forum.blackmagicdesign.com/viewtopic.php?f=21&t=179034 -- 403, unverified).

## Adobe After Effects / Premiere Pro
- (1) Effect Controls is a VIEWER-type panel following the selected layer/clip; Premiere: "shows all the effects applied to a selected clip" (https://helpx.adobe.com/premiere/desktop/add-video-effects/apply-video-effects/about-effect-controls-panel.html).
- (3) AE viewer LOCK: "Locking a viewer ... prevents the currently displayed item from being replaced when you open or select a new item. When a viewer is locked, and a new item is opened or selected, After Effects creates a new viewer panel" (https://helpx.adobe.com/after-effects/using/workspaces-panels-viewers.html, 403 on fetch; quote via search snippet). Essential Graphics is a separate, template-scoped panel (not verified this pass).
- (4) Locked = a second Effect Controls tab is created for the new selection (tabs inside one panel).
- (5) Community reports of "strange behavior of the Effect Controls panel" (https://community.adobe.com/t5/after-effects/strange-behavior-of-the-effect-controls-panel/td-p/9023626) typically trace to a forgotten lock.

## Figma
- (1) One right sidebar = "the properties panel"; tabs Design / Prototype (view-only: Comment / Properties; Dev Mode replaced Inspect). "When you select a layer, you can view and modify the layer's properties ... The type of layer you select determines which property settings you'll see"; nothing selected = page-level (local styles/variables, canvas color, export). https://help.figma.com/hc/en-us/articles/360039832014-Design-prototype-and-explore-layer-properties-in-the-right-sidebar
- (2)/(3) No per-area embed, no pin, no multiple; MODE tabs inside one panel are the whole mechanism.
- (4) One selection per file tab; the panel follows it.

## Photoshop / Affinity
- Photoshop (1) Properties panel is "context-sensitive, with options changing depending on the kind of layer you're working with" (layer, document, tool); a floating Contextual Task Bar adds next-step actions. https://helpx.adobe.com/photoshop/using/panels-menus.html (403 on fetch; via search snippets). No pin.
- Affinity (1) no single Properties panel: a Context Toolbar (top strip) shows "options based on the context of whatever is selected" and only "the most commonly used options"; deeper settings are spread over ~30 Studio panels (Layers, Character, Stroke...). https://www.affinity.studio/help/workspace-context-bar/
- (4) Both simply follow selection.

## Rhino / Grasshopper (brief)
- Rhino: Properties panel "manages object properties for the selected objects"; selection-specific pages appear as buttons; "If no objects are selected, viewport properties display." https://docs.mcneel.com/rhino/mac/help/en-us/commands/properties.htm
- Grasshopper: no properties panel at all -- component settings via right-click context menu on the component (from memory, unverified this pass).

# Patterns (distinct mechanisms and who uses them)
1. ONE global contextual panel that follows the active selection -- Blender Properties editor, Houdini Parameters, Maya AE/Channel Box, C4D Attribute Manager, Substance, Figma, Photoshop, Rhino. Universal baseline.
2. PIN / LOCK on that panel to freeze it -- Blender (pin data-block), Houdini (pin node), Maya (Pin Tab), C4D (Lock Element / Lock Mode), AE (viewer lock), Fusion (per-tool pin).
3. MULTIPLE INSTANCES of the panel, each pinnable -- Blender (any area -> Properties), Houdini (new Parameters tab / floating window), Maya (Copy Tab), C4D (New Attribute Manager), AE (lock spawns a new viewer tab).
4. LINK GROUPS / channels (which selection source drives which panel) -- Houdini only; its docs call it "complicated and unintuitive".
5. BIN of persistent stacked panels, opened explicitly, evicted by count -- Nuke (Properties Bin, max N, lock -> float), Fusion multi-select stacking.
6. TWO-TIER split by CHARGE, not by editor: global panel = persistent data-block/asset settings; per-area Sidebar = that editor's view settings + active tool + the editor's own active item (node) -- Blender. Cross-duplication is explicit and intentional (Shader Editor Options panel = Material tab).
7. TABS / MODES inside the one panel -- Blender (per-data-block tab column), Maya (connected-node tabs), C4D (Object/Tool/... modes), Figma (Design/Prototype), Resolve Edit (Video/Audio/Effects...), Fusion (Tools/Modifiers).
8. PER-PAGE inspector where pages are exclusive full-screen modes -- DaVinci Resolve; avoids side-by-side by construction.
9. SELECTION-SYNC POLICY knob -- Blender "Sync with Outliner" (Always/Never/Auto-by-adjacency); Nuke "expand/collapse panels to match selection".
10. Quick-strip instead of a panel -- Affinity Context Toolbar, Photoshop Contextual Task Bar (subset of properties, full set elsewhere).

# What fails (documented pain points)
- Tab reset on selection change: Blender #63991 ("selecting a new object will reset that to the object properties tab. Pinning ... doesn't really solve this - the object then can't be changed"); Houdini forum 99565 (pin holds the node, not the tab, so batch-editing the same tab across nodes needs two panes); Resolve Inspector forum threads (unverified).
- Clutter from persistent panels: Nuke's default of 10 stacked panels -> "changing properties in the wrong node"; pros cut to 2.
- Sidebar sprawl: Blender N-panel tab strips overrun by add-ons ("your category gets lost in the chaos"), a years-long devtalk thread; the HIG answer is routing by charge (data -> Properties editor, tools -> Sidebar) and "only create new tabs when necessary".
- Redundancy confusion: Blender Material tab's Surface tree vs the node editor ("redundant to the node editor"); Maya Channel Box vs Attribute Editor.
- Link/channel systems are too clever: Houdini's own docs tell new users to ignore linking and use pin/unpin.
- Forgotten locks: AE Effect Controls "strange behavior" threads; C4D/Maya pinned panels silently showing stale objects (implicit in the manuals' unlock instructions; no complaint thread verified).
- Nothing found (in this pass) documenting a product that shows two selection-following inspectors side by side on purpose; every product either has one follower (+ pinned copies) or a page/mode boundary.

---


# Research C: who owns the properties/inspector panel in single-window multi-editor apps (IDE / designer family)

Legend per entry: (1) global+contextual? (2) per-editor embedded? (3) hybrid mechanisms (4) what happens to the previous editor's selection on tab switch (5) rationale / complaints.

## Xcode (strongest single-window precedent)
- (1) GLOBAL. One Inspector area on the right of the single workspace window; "In the inspector area, you can view and edit information for a selected object in the project editor or for the entire project." "The available inspectors depend on the current selection" -- the segmented inspector bar is rebuilt per selection: File / History / Quick Help always; Identity / Attributes / Size / Connections / Bindings / View Effects / SKNode / Source Control appear only for the matching selection (IB object, SpriteKit node, commit). https://help.apple.com/xcode/mac/current/en.lproj/dev63ccb4d06.html
- (2) No editor embeds its own inspector. Exception that proves the rule: the SwiftUI canvas exposed the same Attributes inspector as a POPOVER ("Show SwiftUI Inspector", Cmd-Ctrl-click) -- which Xcode 26 dropped; see (5).
- (3) Hybrid = segmented tabs inside ONE panel, tab set recomputed from selection; no pin/lock, no multiple instances.
- (4) Not documented verbatim. Observed behaviour (unverified from docs): switching the center editor to a source file collapses the bar to File/History/Quick Help and the object inspectors show "No Selection"; the previous IB selection is not retained.
- (5) Complaint: Xcode 26 removed the SwiftUI attributes inspector entry; devs report "It seems it's gone for good", bug FB21214592 filed, tutorial still references it. https://developer.apple.com/forums/thread/808662

## Visual Studio (Properties window)
- (1) GLOBAL. "Use this window to view and change the design-time properties and events of selected objects that are located in editors and designers ... [also] file, project, and solution properties." Object-name dropdown: "Lists the currently selected object or objects. Only objects from the active editor or designer are visible." Multi-select shows the intersection. https://learn.microsoft.com/en-us/visualstudio/ide/reference/properties-window
- Mechanism: a GLOBAL SELECTION CONTEXT. "Each window in the IDE can have its own selection context object pushed to the global selection context. The IDE updates the global selection context with values from a window when that window has the focus." Windows call STrackSelection/ITrackSelection.OnSelectChange with an ISelectionContainer (SelectableObjects/SelectedObjects); windows may opt out with SELCONTAINER_DONTPROPAGATE, "useful for tool windows that may have to start with an empty selection." https://learn.microsoft.com/en-us/visualstudio/extensibility/internals/selection-context-objects?view=vs-2022 ; https://learn.microsoft.com/en-us/visualstudio/extensibility/exposing-properties-to-the-properties-window?view=vs-2022
- (2) No. But the toolbar of the ONE window is per-editor: Categorized/Alphabetical/Property Pages always; "Events" only "when a form or control designer is active"; "Sort by Property Source", thumbnail and search "Only available when editing XAML files in the designer"; Messages/Overrides only with Class View active in C++. The same window re-skins itself per active designer (WinForms vs XAML vs code). https://learn.microsoft.com/en-us/visualstudio/ide/reference/properties-window
- (3) Hybrid = one window, per-editor toolbar modes + the object dropdown; Property Pages dialog as the escape hatch for "a subset, the same or a superset" of properties. Runtime XAML inspection is a SEPARATE pair (Live Visual Tree + Live Property Explorer), documented as dockable side by side with each other -- VS accepts a second inspector-like window for the debug domain. https://learn.microsoft.com/en-us/visualstudio/xaml-tools/inspect-xaml-properties-while-debugging?view=visualstudio
- (4) Platform rule: "If the window does not surface properties or change global selection context, selection feedback should not remain in the window when it is no longer the active window in the IDE." and "The window should surface properties for the current selection in the Properties window." The global context is replaced by the newly focused window's context; a code editor pushes its hierarchy item (file properties), so the previous designer's control properties are gone. https://learn.microsoft.com/en-us/visualstudio/extensibility/internals/feedback-to-the-user?view=vs-2022
- (5) Rationale stated: "If you only surface the functionality users need and continually provide consistent selection and environment context feedback, you reduce the complexity in the IDE." Friction: Properties content flips/empties whenever focus moves to a window that pushes no container (the walkthrough has you call OnSelectChange with a null container before disposing objects). No pin/lock in the stock window.

## Eclipse (Properties VIEW + selection service) -- the classic model
- (1) GLOBAL. The Properties view is a PageBookView: "The property sheet view has a default page (an instance of PropertySheetPage) which services all parts without a property sheet page of their own." A part may contribute its own IPropertySheetPage via getAdapter (the page-book); pages "must be pages and listen for selection changes in the active part." https://help.eclipse.org/latest/rtopic/org.eclipse.platform.doc.isv/reference/api/org/eclipse/ui/views/properties/PropertySheet.html ; https://www.eclipse.org/articles/Article-Properties-View/properties-view.html
- Selection service: per-window ISelectionService "keeps track of the selection in the currently active part and propagates selection changes to all registered listeners"; "only selections within the active part are propagated"; one selection provider per part (use an intermediate provider for multi-viewer parts). https://www.eclipse.org/articles/Article-WorkbenchSelections/article.html
- (2) No; parts contribute a PAGE into the shared view. The Tabbed Properties View (from IBM RSA/WID) lets a part contribute tabs/sections filtered by selected type, with AdvancedPropertySection to embed the classic table -- per-editor UI INSIDE the global view. https://www.eclipse.org/articles/Article-Tabbed-Properties/tabbed_properties_view.html
- (3) Hybrid = page-book (per-part page), tabbed sections, "Pin to Selection" (PropertySheet.isPinned/setPinned since 3.4) and "New Properties View" (a second instance/pane). Multiple instances of one view arrived in 3.0 after resistance: "Allowing multiple instances of the same view in the same perspective is something we have resisted in the past since we felt it would complicate the UI." (Nick Edgar, 2003). https://help.eclipse.org/latest/topic/org.eclipse.cdt.doc.user/reference/cdt_u_properties_view.htm ; https://bugs.eclipse.org/bugs/show_bug.cgi?id=31612
- (4) DOCUMENTED: "the properties view looks for a property source from the active part; if there is none, it keeps showing the previous properties." (bug 43085, N. Edgar). Bug 23862 "Properties view doesn't update when switching between editors": stale navigator selection shown after clicking an editor tab; intended behaviour per that thread: "show properties for the last active part, if any (excluding the Properties view itself). Optionally, check the active editor if the active view provides no properties." Opened 2002, fixed 2019. https://bugs.eclipse.org/bugs/show_bug.cgi?id=43085 ; https://bugs.eclipse.org/bugs/show_bug.cgi?id=23862
- (5) Complaints: the 17-year stale-content bug above; the proposed fix was a 'currentPropSourcePart' (track the part that SUPPLIES the content, not merely the focused part).

## JetBrains (IntelliJ GUI Designer, Android Studio Layout Editor, Rider)
- IntelliJ GUI Designer: per-editor EMBEDDED. "The property inspector is a section of a form that shows properties for the component currently selected in the form workspace, or the form itself if no components exist or none are selected." (documented fallback-to-container). https://www.jetbrains.com/help/idea/inspector.html
- Android Studio Layout Editor: per-editor, docked beside the design surface: "You can edit view attributes from the Attributes panel in the Layout Editor. This window is available only when the design editor is open" -- it disappears with the editor. https://developer.android.com/studio/views/layout-editor
- Rider Unity/Unreal: no properties panel; the engine editor keeps the inspector, Rider mirrors project structure / Blueprint info (UnrealLink). https://www.jetbrains.com/help/rider/Game_Development.html (no inspector documented; treated as absent).
- (4) The panel belongs to the editor's layout, so it goes away with the tab; no cross-editor state question. (5) No documented rationale.

## Qt Creator / Qt Design Studio
- Qt Widgets Designer: designer-scoped tool window: "The Property Editor always displays properties of the currently selected object on the form." https://doc.qt.io/qt-6/designer-widget-mode.html
- Qt Design Studio Properties view: global contextual; HYBRID extras: "select [+] to edit its properties in another Properties view" (Multi Property Editor plugin) and "select [lock] to prevent the subject node from changing in this Properties view" -- multiple instances + per-instance lock. https://doc.qt.io/qtdesignstudio/qtquick-properties-view.html
- (4)/(5) Not documented.

## VS Code (deliberately no properties panel)
- (1) None. UX surfaces are Activity Bar / Primary+Secondary Sidebar (Views) / Editor / Panel / Status Bar; the guidelines define containers only, no "properties" concept. https://code.visualstudio.com/api/ux-guidelines/overview
- (2) Custom editors (webviews) host their whole UI; state is torn down when hidden unless retainContextWhenHidden ("significant memory overhead so be conservative"). https://code.visualstudio.com/api/extension-guides/custom-editors
- (3) Inspector precedent: the Hex Editor's Data Inspector defaults to "shown just to the right of the data grid" INSIDE the editor; hexeditor.inspectorType makes it a hover or a sidebar VIEW with its own activity-bar entry that auto-reveals when a hex editor opens. https://github.com/microsoft/vscode-hexeditor/blob/main/README.md
- (4) Sidebar mode: the view persists across tab switches (it is a View, not part of the editor). (5) A VS Code core dev (Tyriar, hexeditor #223) on the sidebar inspector: "It hides my explorer or whatever side bar I have displayed at the time, this is not only distracting but I like it to be there to give me context" -> placement made configurable. https://github.com/microsoft/vscode-hexeditor/issues/223 . Request to show the Settings editor in a sidebar: closed not-planned. https://github.com/microsoft/vscode/issues/176378 . The Secondary Side Bar exists so two Views can be open at once. https://code.visualstudio.com/docs/configure/custom-layout

## Microsoft Office (Format pane)
- (1) GLOBAL contextual task pane. The Format pane retitles and re-populates per selection (Format Shape / Format Picture / Format Chart Area / Format Text Effects...); "if you click on a different chart element, the task pane automatically updates to the new chart element." https://support.microsoft.com/en-us/office/format-elements-of-a-chart-b6c787d5-f90a-41d2-a901-9d3ed9f0dbf0 ; task panes dock/undock/resize: https://support.microsoft.com/en-us/office/manage-pane-layout-in-office-apps-56855b51-1713-4fef-abe9-f155ac74bb89
- (2) No. (3) Hybrid = one pane whose icon-tab set (Fill & Line / Effects / Size & Properties / Picture / Text) is recomputed per selection; the pane is opened on demand from the ribbon, not always-on. (4) With nothing selected the pane stays open showing the last title but disabled/empty controls (unverified from docs; community pages such as indezine describe the auto-toggle). (5) Recurring user confusion: "How to find the Format task pane?" threads on Microsoft Q&A -- the pane is discoverable only through the selection's contextual ribbon tab.

## Web design tools
- Webflow: GLOBAL right panel with three tabs -- Style (S), Element settings (D), Interactions -- all "for the selected element"; the Style panel "lets you adjust all CSS properties available for a selected element." https://university.webflow.com/lesson/style-panel-overview ; https://university.webflow.com/glossary/element-settings-panel . (3) segmented tabs; (4) single document per Designer, so no cross-editor issue; (5) none documented.
- Framer: GLOBAL context-based properties panel on the right "showing different properties (e.g., position, layout, effects, styles) depending on the element you have selected." (third-party interface guides; official docs describe per-feature panels only). https://framerbase.io/knowledge-hub/understanding-the-framer-interface (unofficial)
- Penpot: GLOBAL right sidebar with MODE tabs Design / Prototype / Inspect; "The Design Properties sidebar lets you view and edit the attributes of a selected layer ... some properties are always present (size, position), while others are optional (stroke, shadow, blur...)"; Inspect is view-only for devs. https://help.penpot.app/user-guide/the-interface/ ; https://help.penpot.app/user-guide/dev-tools/ . (5) Bug filed on mode-tab switching glitches: https://github.com/penpot/penpot/issues/2750 ; request to resize the right sidebar: https://github.com/penpot/penpot/issues/3833
- Rive: GLOBAL Inspector, MODE-aware. "The Inspector serves as the editor's right sidebar ... selecting an artboard, object, joystick, script, property group, or State Machine item shows different options based on what that element can do." Nothing selected -> document-level settings (background colour per mode, scripting, shader target, tags, default interpolation). Animate mode "updates to show key buttons next to any property that can be animated" -- same panel, decorated per mode, not a second panel. https://rive.app/docs/editor/interface-overview/inspector ; https://help.rive.app/editor/fundamentals/design-vs-animate-mode

## Apple pro apps (Xcode lineage)
- Final Cut Pro: single-window Inspector; "Final Cut Pro provides a number of inspectors you can use to view and change the attributes of selected items" (Video, Audio, Info, Transition, Title, Text, Generator, Library Properties, Share); "The inspectors that are available depend on the item that's selected. For example, you must select a transition to see the Transition inspector." Tab buttons at the top switch inspectors. https://support.apple.com/guide/final-cut-pro/ver15f87af2/mac
- Logic Pro: single inspector column at the left; per Apple's guide, parameters shown depend "on the type of item selected and which working area has key focus"; the inspector is STACKED (Region inspector above, Track inspector below, plus channel strips) so region-, track- and event-level properties coexist without a second panel. https://support.apple.com/guide/logicpro/inspector-interface-lgcpe9cc3b1d/mac (page fetch returned only the TOC; the quoted phrase is from the search snippet -- verify). Key-focus rule = the Logic equivalent of the VS/Eclipse "active part".

## Others
- LabVIEW: the anti-pattern's cousin. Object properties live in a right-click "Properties" DIALOG whose tabs depend on the object type; the only always-on contextual surface is the floating Context Help window (Ctrl-H) which follows the HOVERED element, not the selection. https://labviewwiki.org/wiki/Properties_dialog ; https://labviewwiki.org/wiki/Context_Help_window
- Scratch: per-editor embedded. The Sprite Pane header shows name / x / y / show / size / direction of the selected sprite, editable inline; no separate inspector. https://en.scratch-wiki.info/wiki/Sprite_Pane
- Notion: properties are IN the document (top of page); no sidebar inspector. https://www.notion.com/help/intro-to-databases
- Obsidian: Properties core plugin adds a sidebar view "File properties [that] shows a view of the properties for the active note" (follows the active editor tab) plus an "All properties" vault-wide view; properties also render inline at the top of the note. https://help.obsidian.md/plugins/properties . Complaint: users ask to hide the "All properties" tab from the right sidebar. https://forum.obsidian.md/t/please-consider-adding-an-option-to-not-show-the-all-properties-tab-on-the-right-sidebar/84903
- OBS Studio (the anti-pattern): source Properties, Filters and Transform are separate DIALOG windows. Community demand for docks: ideas.obsproject.com "Dockable inspectors" (https://ideas.obsproject.com/posts/725/dockable-inspectors), and third-party plugins that convert them: obs-properties-dock "adds the Source Properties and Edit Transform dialogs as docks. This way, you can more easily see the effects of your changes live as it happens" (https://github.com/gxalpha/obs-properties-dock) and source-inspector-dock, "a dock for the selected source's properties, transform, and filters" (https://github.com/prgmitchell/source-inspector-dock); reviews say it "should be a default OBS feature". (5) The documented pain: a modal dialog blocks live feedback and cannot follow selection.

# Patterns (distinct mechanisms, who uses them)
1. Global selection context, one panel, replace-on-focus: Visual Studio (global selection context pushed by the focused window), Eclipse (ISelectionService of the active part), Xcode/Final Cut/Logic (selection + key focus), Office Format pane, Webflow/Framer/Penpot/Rive.
2. Page-book / per-editor contribution INTO the shared panel: Eclipse IPropertySheetPage via getAdapter + Tabbed Properties contributors; VS per-editor toolbar modes (Events, Sort by Source, XAML search); Xcode's per-selection inspector bar.
3. Segmented tabs / modes inside the one panel: Xcode (File/History/Quick Help/Identity/Attributes/Size/Connections), Final Cut (Video/Audio/Info...), Webflow (Style/Settings/Interactions), Penpot (Design/Prototype/Inspect), Office (Fill&Line/Effects/Size&Properties), Rive (Design vs Animate decoration).
4. Stacked sub-inspectors in one column: Logic Pro (Region / Track / Event + channel strips); Android Studio (Declared Attributes / Layout / All).
5. Pin / lock + multiple instances: Eclipse "Pin to Selection" + "New Properties View" (3.4/3.0); Qt Design Studio lock + "+" extra Properties view. Neither VS nor Xcode offers a pin.
6. Fallback target when nothing is selected: Rive -> document settings; IntelliJ GUI Designer -> the form itself; Xcode -> File/History/Quick Help only ("No Selection" for object inspectors); Eclipse (pre-2019) -> stale previous contents.
7. Per-editor embedded panel (no global): IntelliJ GUI Designer, Android Studio Layout Editor Attributes, Scratch Sprite Pane, VS Code hex Data Inspector (default 'aside'), Notion.
8. Deliberate absence + optional View: VS Code (no properties surface; extensions choose editor-internal 'aside' vs a sidebar View; Secondary Side Bar for a second always-on View).
9. Separate domain inspector accepted side by side: VS Live Property Explorer (runtime) next to Properties (design-time).
10. Dialog (anti-pattern): OBS source properties, LabVIEW Properties dialog.

# What fails (documented pain)
- Stale or blank content on focus change: Eclipse bug 23862 ("doesn't update when switching between editors", open 2002-2019) and 43085 ("if there is none, it keeps showing the previous properties"); VS content empties when a focused window pushes no selection container (SELCONTAINER_DONTPROPAGATE exists precisely to manage this).
- Focus-vs-selection conflation: both VS and Eclipse key the panel on the ACTIVE part, so clicking a non-selection tool window changes/loses the panel; Eclipse's proposed remedy was tracking the part that SUPPLIES content ('currentPropSourcePart').
- Multiple instances resisted then added: Eclipse resisted ("would complicate the UI") then shipped secondaryId views in 3.0 and pinning in 3.4 -- demand came from wanting two contexts visible at once.
- Sidebar inspector steals the sidebar: VS Code hexeditor #223 ("hides my explorer ... I like it to be there to give me context") -> made configurable (aside/hover/sidebar).
- Editor-embedded panel is invisible when the editor is not: Android Studio Attributes "available only when the design editor is open" (users must switch to Design/Split mode).
- Removing a contextual entry point silently: Xcode 26 dropped "Show SwiftUI Inspector" while tutorials still reference it (forum thread, FB21214592).
- Modal/dialog properties block live feedback and cannot follow selection: OBS (plugins exist to dock them; "should be a default OBS feature"), LabVIEW.
- Mode-tab switching bugs in a shared panel: Penpot Inspect<->Design glitch (#2750).
