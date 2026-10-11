# Mechanical include-edge sweep

Run: `python .superpowers/sdd/2026-10-11-modularization/recheck.py` from repository root. Paths below are repository-relative. Counts are direct preprocessor include sites, with current-tree paths and ownership after s1.5 moves. Dependency comparison uses the literal s1.1-s1.3 columns; an omitted Core edge remains missing even when transitive.

Engine files scanned: 437. Program/game files scanned: 714. Resolved cross-module include sites: 546.

## UNASSIGNED

None.

### Assignment inventory

Every scanned engine file appears once below. The current file path supplies the file:1 evidence; ownership applies the s1.5 move without changing source paths.

#### ArcaneAssets (32 files)

- `ArcaneCore/src/Arcane/Assets/ArtifactReader.cpp:1`
- `ArcaneCore/src/Arcane/Assets/ArtifactReader.hpp:1`
- `ArcaneCore/src/Arcane/Assets/AssetCache.hpp:1`
- `ArcaneCore/src/Arcane/Assets/AssetId.hpp:1`
- `ArcaneCore/src/Arcane/Assets/Assets.cpp:1`
- `ArcaneCore/src/Arcane/Assets/Assets.hpp:1`
- `ArcaneCore/src/Arcane/Assets/AssetsSettings.cpp:1`
- `ArcaneCore/src/Arcane/Assets/AssetsSettings.hpp:1`
- `ArcaneCore/src/Arcane/Assets/IdentityFieldRule.hpp:1`
- `ArcaneCore/src/Arcane/Assets/ImageCompare.cpp:1`
- `ArcaneCore/src/Arcane/Assets/ImageCompare.hpp:1`
- `ArcaneCore/src/Arcane/Assets/ImageIo.hpp:1`
- `ArcaneCore/src/Arcane/Assets/StbImpl.cpp:1`
- `ArcaneCore/src/Arcane/Material/GlobalParams.hpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialAsset.cpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialAsset.hpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialBlendMode.hpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialGraph.cpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialGraph.hpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialInstance.cpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialInstance.hpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialSource.cpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialSource.hpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialTemplate.cpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialTemplate.hpp:1`
- `ArcaneCore/src/Arcane/Material/MaterialTypes.hpp:1`
- `ArcaneCore/src/Arcane/Mesh/MeshAsset.cpp:1`
- `ArcaneCore/src/Arcane/Mesh/MeshAsset.hpp:1`
- `ArcaneCore/src/Arcane/Mesh/MeshBuilder.cpp:1`
- `ArcaneCore/src/Arcane/Mesh/MeshBuilder.hpp:1`
- `ArcaneCore/src/Arcane/Sprite/SpriteAsset.cpp:1`
- `ArcaneCore/src/Arcane/Sprite/SpriteAsset.hpp:1`

#### ArcaneAudio (5 files)

- `ArcaneClient/src/Arcane/Audio/AudioDevice.cpp:1`
- `ArcaneClient/src/Arcane/Audio/AudioDevice.hpp:1`
- `ArcaneClient/src/Arcane/Audio/AudioSettings.cpp:1`
- `ArcaneClient/src/Arcane/Audio/AudioSettings.hpp:1`
- `ArcaneClient/src/Arcane/Audio/AudioTypes.hpp:1`

#### ArcaneClient (32 files)

- `ArcaneClient/src/Arcane/Base/Api.hpp:1`
- `ArcaneClient/src/Arcane/Client/Api.hpp:1`
- `ArcaneClient/src/Arcane/Client/ClientRuntime.cpp:1`
- `ArcaneClient/src/Arcane/Client/ClientRuntime.hpp:1`
- `ArcaneClient/src/Arcane/Client/RuntimePresentation.hpp:1`
- `ArcaneClient/src/Arcane/Host/BootSequence.cpp:1`
- `ArcaneClient/src/Arcane/Host/BootSequence.hpp:1`
- `ArcaneClient/src/Arcane/Host/BootSplashWindow.cpp:1`
- `ArcaneClient/src/Arcane/Host/BootSplashWindow.hpp:1`
- `ArcaneClient/src/Arcane/Host/EarlyConfig.cpp:1`
- `ArcaneClient/src/Arcane/Host/EarlyConfig.hpp:1`
- `ArcaneClient/src/Arcane/Host/ExclusionList.cpp:1`
- `ArcaneClient/src/Arcane/Host/ExclusionList.hpp:1`
- `ArcaneClient/src/Arcane/Host/FramePerf.hpp:1`
- `ArcaneClient/src/Arcane/Host/GpuContext.cpp:1`
- `ArcaneClient/src/Arcane/Host/GpuContext.hpp:1`
- `ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp:1`
- `ArcaneClient/src/Arcane/Host/HostConfig.cpp:1`
- `ArcaneClient/src/Arcane/Host/HostConfig.hpp:1`
- `ArcaneClient/src/Arcane/Host/HostOverlay.cpp:1`
- `ArcaneClient/src/Arcane/Host/HostOverlay.hpp:1`
- `ArcaneClient/src/Arcane/Host/HostSettings.cpp:1`
- `ArcaneClient/src/Arcane/Host/HostSettings.hpp:1`
- `ArcaneClient/src/Arcane/Host/HostVerifyHooks.cpp:1`
- `ArcaneClient/src/Arcane/Host/HostVerifyHooks.hpp:1`
- `ArcaneClient/src/Arcane/Host/ProjectBoot.cpp:1`
- `ArcaneClient/src/Arcane/Host/ProjectBoot.hpp:1`
- `ArcaneClient/src/Arcane/Host/RuntimeSettings.cpp:1`
- `ArcaneClient/src/Arcane/Host/RuntimeSettings.hpp:1`
- `ArcaneClient/src/Arcane/Host/SceneRenderResolver.cpp:1`
- `ArcaneClient/src/Arcane/Host/SceneRenderResolver.hpp:1`
- `ArcaneClient/src/Arcane/Host/SettleBound.hpp:1`

#### ArcaneCore (112 files)

- `ArcaneCore/src/Arcane/Base/Assert.cpp:1`
- `ArcaneCore/src/Arcane/Base/Assert.hpp:1`
- `ArcaneCore/src/Arcane/Base/CrashArena.cpp:1`
- `ArcaneCore/src/Arcane/Base/CrashArena.hpp:1`
- `ArcaneCore/src/Arcane/Base/DiagEnvelope.cpp:1`
- `ArcaneCore/src/Arcane/Base/DiagEnvelope.hpp:1`
- `ArcaneCore/src/Arcane/Base/Diagnostics.cpp:1`
- `ArcaneCore/src/Arcane/Base/Diagnostics.hpp:1`
- `ArcaneCore/src/Arcane/Base/DiagnosticsInternal.hpp:1`
- `ArcaneCore/src/Arcane/Base/DiagnosticsSettings.cpp:1`
- `ArcaneCore/src/Arcane/Base/DiagnosticsSettings.hpp:1`
- `ArcaneCore/src/Arcane/Base/DiagnosticsSettingsData.hpp:1`
- `ArcaneCore/src/Arcane/Base/Engine.cpp:1`
- `ArcaneCore/src/Arcane/Base/Engine.hpp:1`
- `ArcaneCore/src/Arcane/Base/ForeignModules.cpp:1`
- `ArcaneCore/src/Arcane/Base/ForeignModules.hpp:1`
- `ArcaneCore/src/Arcane/Base/Log.cpp:1`
- `ArcaneCore/src/Arcane/Base/Log.hpp:1`
- `ArcaneCore/src/Arcane/Base/LogFileSettings.cpp:1`
- `ArcaneCore/src/Arcane/Base/LogFileSettings.hpp:1`
- `ArcaneCore/src/Arcane/Base/ModuleTable.cpp:1`
- `ArcaneCore/src/Arcane/Base/ModuleTable.hpp:1`
- `ArcaneCore/src/Arcane/Base/PortableStack.cpp:1`
- `ArcaneCore/src/Arcane/Base/PortableStack.hpp:1`
- `ArcaneCore/src/Arcane/Base/Posix/DiagnosticsPosix.cpp:1`
- `ArcaneCore/src/Arcane/Base/Posix/MinidumpWriter.cpp:1`
- `ArcaneCore/src/Arcane/Base/Posix/PosixCrashSupport.cpp:1`
- `ArcaneCore/src/Arcane/Base/Posix/PosixCrashSupport.hpp:1`
- `ArcaneCore/src/Arcane/Base/ReporterSettings.cpp:1`
- `ArcaneCore/src/Arcane/Base/ReporterSettings.hpp:1`
- `ArcaneCore/src/Arcane/Base/ReporterSettingsData.hpp:1`
- `ArcaneCore/src/Arcane/Base/ServiceThread.cpp:1`
- `ArcaneCore/src/Arcane/Base/ServiceThread.hpp:1`
- `ArcaneCore/src/Arcane/Build/BuildToolSettings.cpp:1`
- `ArcaneCore/src/Arcane/Build/BuildToolSettings.hpp:1`
- `ArcaneCore/src/Arcane/Build/Toolchain.cpp:1`
- `ArcaneCore/src/Arcane/Build/Toolchain.hpp:1`
- `ArcaneCore/src/Arcane/Cli/Cli.cpp:1`
- `ArcaneCore/src/Arcane/Cli/Cli.hpp:1`
- `ArcaneCore/src/Arcane/Config/Bindings/AstraBinding.cpp:1`
- `ArcaneCore/src/Arcane/Config/Bindings/AstraBinding.hpp:1`
- `ArcaneCore/src/Arcane/Config/Bindings/JobsBinding.cpp:1`
- `ArcaneCore/src/Arcane/Config/Bindings/JobsBinding.hpp:1`
- `ArcaneCore/src/Arcane/Config/Bindings/LogBinding.cpp:1`
- `ArcaneCore/src/Arcane/Config/Bindings/LogBinding.hpp:1`
- `ArcaneCore/src/Arcane/Config/Bindings/Physics2DBinding.cpp:1`
- `ArcaneCore/src/Arcane/Config/Bindings/Physics2DBinding.hpp:1`
- `ArcaneCore/src/Arcane/Config/Config.cpp:1`
- `ArcaneCore/src/Arcane/Config/Config.hpp:1`
- `ArcaneCore/src/Arcane/Config/ConsoleModel.cpp:1`
- `ArcaneCore/src/Arcane/Config/ConsoleModel.hpp:1`
- `ArcaneCore/src/Arcane/Config/ConsoleSettings.cpp:1`
- `ArcaneCore/src/Arcane/Config/ConsoleSettings.hpp:1`
- `ArcaneCore/src/Arcane/Config/CVarConfig.cpp:1`
- `ArcaneCore/src/Arcane/Config/CVarConfig.hpp:1`
- `ArcaneCore/src/Arcane/Config/CVarDecl.hpp:1`
- `ArcaneCore/src/Arcane/Config/CVarFormat.cpp:1`
- `ArcaneCore/src/Arcane/Config/CVarFormat.hpp:1`
- `ArcaneCore/src/Arcane/Config/CVarHandle.hpp:1`
- `ArcaneCore/src/Arcane/Config/CVarModule.hpp:1`
- `ArcaneCore/src/Arcane/Config/CVarRef.hpp:1`
- `ArcaneCore/src/Arcane/Config/CVarRegistry.cpp:1`
- `ArcaneCore/src/Arcane/Config/CVarRegistry.hpp:1`
- `ArcaneCore/src/Arcane/Config/CVarTypes.hpp:1`
- `ArcaneCore/src/Arcane/Config/PlayerSettings.cpp:1`
- `ArcaneCore/src/Arcane/Config/PlayerSettings.hpp:1`
- `ArcaneCore/src/Arcane/Config/PreferenceScope.cpp:1`
- `ArcaneCore/src/Arcane/Config/PreferenceScope.hpp:1`
- `ArcaneCore/src/Arcane/Config/RemoteCVarService.cpp:1`
- `ArcaneCore/src/Arcane/Config/RemoteCVarService.hpp:1`
- `ArcaneCore/src/Arcane/Config/Settings.hpp:1`
- `ArcaneCore/src/Arcane/Config/SettingsField.hpp:1`
- `ArcaneCore/src/Arcane/Config/UiSettings.cpp:1`
- `ArcaneCore/src/Arcane/Config/UiSettings.hpp:1`
- `ArcaneCore/src/Arcane/Config/UiSettingsData.hpp:1`
- `ArcaneCore/src/Arcane/Core/AbiVersion.hpp:1`
- `ArcaneCore/src/Arcane/Core/Api.hpp:1`
- `ArcaneCore/src/Arcane/Core/Constant.hpp:1`
- `ArcaneCore/src/Arcane/Core/ModuleApi.hpp:1`
- `ArcaneCore/src/Arcane/Core/NetMode.cpp:1`
- `ArcaneCore/src/Arcane/Core/NetMode.hpp:1`
- `ArcaneCore/src/Arcane/Core/PhysicsDebugTypes.hpp:1`
- `ArcaneCore/src/Arcane/Guid.cpp:1`
- `ArcaneCore/src/Arcane/Guid.hpp:1`
- `ArcaneCore/src/Arcane/Jobs/ArcaneWorkScheduler.hpp:1`
- `ArcaneCore/src/Arcane/Jobs/JobSystem.cpp:1`
- `ArcaneCore/src/Arcane/Jobs/JobSystem.hpp:1`
- `ArcaneCore/src/Arcane/Jobs/TaskExecutor.hpp:1`
- `ArcaneCore/src/Arcane/Math/Aabb.hpp:1`
- `ArcaneCore/src/Arcane/Math/NormalMatrix.hpp:1`
- `ArcaneCore/src/Arcane/Platform/LaunchPath.cpp:1`
- `ArcaneCore/src/Arcane/Platform/LaunchPath.hpp:1`
- `ArcaneCore/src/Arcane/Platform/NativeWindow.cpp:1`
- `ArcaneCore/src/Arcane/Platform/NativeWindow.hpp:1`
- `ArcaneCore/src/Arcane/Platform/Paths.cpp:1`
- `ArcaneCore/src/Arcane/Platform/Paths.hpp:1`
- `ArcaneCore/src/Arcane/Platform/Platform.hpp:1`
- `ArcaneCore/src/Arcane/Platform/Process.cpp:1`
- `ArcaneCore/src/Arcane/Platform/Process.hpp:1`
- `ArcaneCore/src/Arcane/Platform/SecureRandom.cpp:1`
- `ArcaneCore/src/Arcane/Platform/SecureRandom.hpp:1`
- `ArcaneCore/src/Arcane/Reflection.hpp:1`
- `ArcaneCore/src/Arcane/Util/CharConv.hpp:1`
- `ArcaneCore/src/Arcane/Util/FunctionRef.hpp:1`
- `ArcaneCore/src/Arcane/Util/Logger.hpp:1`
- `ArcaneCore/src/Arcane/Util/LogServerSettings.cpp:1`
- `ArcaneCore/src/Arcane/Util/LogServerSettings.hpp:1`
- `ArcaneCore/src/Arcane/Util/LogServerSettingsData.hpp:1`
- `ArcaneCore/src/Arcane/Util/LruCache.hpp:1`
- `ArcaneCore/src/Arcane/Util/SkylinePacker.hpp:1`
- `ArcaneCore/src/Arcane/Version.hpp:1`
- `ArcaneCore/src/ArcaneCore.cpp:1`

#### ArcaneCrypto (5 files)

- `ArcaneCore/src/Arcane/Crypto/Crypto.cpp:1`
- `ArcaneCore/src/Arcane/Crypto/Crypto.hpp:1`
- `ArcaneCore/src/Arcane/Crypto/CryptoModule.cpp:1`
- `ArcaneCore/src/Arcane/Crypto/CryptoSettings.cpp:1`
- `ArcaneCore/src/Arcane/Crypto/CryptoSettings.hpp:1`

#### ArcaneEdit (13 files)

- `ArcaneClient/src/Arcane/Edit/Command.hpp:1`
- `ArcaneClient/src/Arcane/Edit/CommandStack.cpp:1`
- `ArcaneClient/src/Arcane/Edit/CommandStack.hpp:1`
- `ArcaneClient/src/Arcane/Edit/ComponentEditCommand.cpp:1`
- `ArcaneClient/src/Arcane/Edit/ComponentEditCommand.hpp:1`
- `ArcaneClient/src/Arcane/Edit/EntityOps.cpp:1`
- `ArcaneClient/src/Arcane/Edit/EntityOps.hpp:1`
- `ArcaneClient/src/Arcane/Edit/Gizmo.cpp:1`
- `ArcaneClient/src/Arcane/Edit/Gizmo.hpp:1`
- `ArcaneClient/src/Arcane/Edit/RegistryStateCommand.cpp:1`
- `ArcaneClient/src/Arcane/Edit/RegistryStateCommand.hpp:1`
- `ArcaneClient/src/Arcane/Edit/UndoPayload.cpp:1`
- `ArcaneClient/src/Arcane/Edit/UndoPayload.hpp:1`

#### ArcaneEngine (36 files)

- `ArcaneCore/src/Arcane/Base/ProcessContext.cpp:1`
- `ArcaneCore/src/Arcane/Base/ProcessContext.hpp:1`
- `ArcaneCore/src/Arcane/Base/Runtime.cpp:1`
- `ArcaneCore/src/Arcane/Base/Runtime.hpp:1`
- `ArcaneCore/src/Arcane/Plugin/ClientHooks.hpp:1`
- `ArcaneCore/src/Arcane/Plugin/GameComponents.hpp:1`
- `ArcaneCore/src/Arcane/Plugin/GameModule.hpp:1`
- `ArcaneCore/src/Arcane/Plugin/GameSystems.hpp:1`
- `ArcaneCore/src/Arcane/Plugin/Module.cpp:1`
- `ArcaneCore/src/Arcane/Plugin/Module.hpp:1`
- `ArcaneCore/src/Arcane/Plugin/Plugin.cpp:1`
- `ArcaneCore/src/Arcane/Plugin/Plugin.hpp:1`
- `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:1`
- `ArcaneCore/src/Arcane/Plugin/PluginHost.cpp:1`
- `ArcaneCore/src/Arcane/Plugin/PluginHost.hpp:1`
- `ArcaneCore/src/Arcane/Plugin/PluginSettings.cpp:1`
- `ArcaneCore/src/Arcane/Plugin/PluginSettings.hpp:1`
- `ArcaneCore/src/Arcane/Plugin/SystemFactory.cpp:1`
- `ArcaneCore/src/Arcane/Plugin/SystemFactory.hpp:1`
- `ArcaneCore/src/Arcane/Project/AppSplashSettings.cpp:1`
- `ArcaneCore/src/Arcane/Project/AppSplashSettings.hpp:1`
- `ArcaneCore/src/Arcane/Project/AssetRegistry.cpp:1`
- `ArcaneCore/src/Arcane/Project/AssetRegistry.hpp:1`
- `ArcaneCore/src/Arcane/Project/MountTable.cpp:1`
- `ArcaneCore/src/Arcane/Project/MountTable.hpp:1`
- `ArcaneCore/src/Arcane/Project/Project.cpp:1`
- `ArcaneCore/src/Arcane/Project/Project.hpp:1`
- `ArcaneCore/src/Arcane/Project/ProjectHost.hpp:1`
- `ArcaneCore/src/Arcane/Project/ProjectManifest.cpp:1`
- `ArcaneCore/src/Arcane/Project/ProjectManifest.hpp:1`
- `ArcaneCore/src/Arcane/Project/ProjectOpenOptions.hpp:1`
- `ArcaneCore/src/Arcane/Project/ProjectPaths.cpp:1`
- `ArcaneCore/src/Arcane/Project/ProjectPaths.hpp:1`
- `ArcaneCore/src/Arcane/Scene/EngineResourceTypes.cpp:1`
- `ArcaneCore/src/Arcane/Scene/EngineResourceTypes.hpp:1`
- `ArcaneCore/src/Arcane/Scene/EngineRoster.hpp:1`

#### ArcaneImGui (12 files)

- `ArcaneClient/src/Arcane/ImGui/ConsoleInputLine.cpp:1`
- `ArcaneClient/src/Arcane/ImGui/ConsoleInputLine.hpp:1`
- `ArcaneClient/src/Arcane/ImGui/ImGuiHostHooks.cpp:1`
- `ArcaneClient/src/Arcane/ImGui/ImGuiHostHooks.hpp:1`
- `ArcaneClient/src/Arcane/ImGui/ImGuiLayer.cpp:1`
- `ArcaneClient/src/Arcane/ImGui/ImGuiLayer.hpp:1`
- `ArcaneClient/src/Arcane/ImGui/ImGuiNri.cpp:1`
- `ArcaneClient/src/Arcane/ImGui/ImGuiNri.hpp:1`
- `ArcaneClient/src/Arcane/ImGui/ImGuiNriNode.cpp:1`
- `ArcaneClient/src/Arcane/ImGui/ImGuiNriNode.hpp:1`
- `ArcaneClient/src/Arcane/ImGui/OffscreenImGuiLayer.cpp:1`
- `ArcaneClient/src/Arcane/ImGui/OffscreenImGuiLayer.hpp:1`

#### ArcaneInput (18 files)

- `ArcaneClient/src/Arcane/Input/GameInput.hpp:1`
- `ArcaneClient/src/Arcane/Input/InputActionAsset.cpp:1`
- `ArcaneClient/src/Arcane/Input/InputActionAsset.hpp:1`
- `ArcaneClient/src/Arcane/Input/InputActions.cpp:1`
- `ArcaneClient/src/Arcane/Input/InputActions.hpp:1`
- `ArcaneClient/src/Arcane/Input/InputBindingProfile.cpp:1`
- `ArcaneClient/src/Arcane/Input/InputBindingProfile.hpp:1`
- `ArcaneClient/src/Arcane/Input/InputDevices.cpp:1`
- `ArcaneClient/src/Arcane/Input/InputDevices.hpp:1`
- `ArcaneClient/src/Arcane/Input/InputRebindOperation.cpp:1`
- `ArcaneClient/src/Arcane/Input/InputRebindOperation.hpp:1`
- `ArcaneClient/src/Arcane/Input/InputSettings.cpp:1`
- `ArcaneClient/src/Arcane/Input/InputSettings.hpp:1`
- `ArcaneClient/src/Arcane/Input/InputSnapshot.hpp:1`
- `ArcaneClient/src/Arcane/Input/KeyLayout.cpp:1`
- `ArcaneClient/src/Arcane/Input/KeyLayout.hpp:1`
- `ArcaneClient/src/Arcane/Input/LocalInputUser.cpp:1`
- `ArcaneClient/src/Arcane/Input/LocalInputUser.hpp:1`

#### ArcaneNet (6 files)

- `ArcaneCore/src/Arcane/Net/NetModule.cpp:1`
- `ArcaneCore/src/Arcane/Net/NetSettings.cpp:1`
- `ArcaneCore/src/Arcane/Net/NetSettings.hpp:1`
- `ArcaneCore/src/Arcane/Net/Protocol.hpp:1`
- `ArcaneCore/src/Arcane/Net/RateLimiter.hpp:1`
- `ArcaneCore/src/Arcane/Net/TcpSocket.hpp:1`

#### ArcanePhysics2D (14 files)

- `ArcaneCore/src/Arcane/Physics2D.hpp:1`
- `ArcaneCore/src/Arcane/Physics2DFwd.hpp:1`
- `ArcaneCore/src/Arcane/Scene/Physics2D.cpp:1`
- `ArcaneCore/src/Arcane/Scene/Physics2DDetail.hpp:1`
- `ArcaneCore/src/Arcane/Scene/Physics2DHandles.hpp:1`
- `ArcaneCore/src/Arcane/Scene/Physics2DWorld.hpp:1`
- `ArcaneCore/src/Arcane/Scene/PhysicsComponents.hpp:1`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:1`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.hpp:1`
- `ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.cpp:1`
- `ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.hpp:1`
- `ArcaneCore/src/Arcane/Scene/PhysicsQuerySettings.cpp:1`
- `ArcaneCore/src/Arcane/Scene/PhysicsQuerySettings.hpp:1`
- `ArcaneCore/src/Arcane/Scene/PhysicsSystem.hpp:1`

#### ArcaneRender (113 files)

- `ArcaneClient/src/Arcane/Render/AgilitySdk.hpp:1`
- `ArcaneClient/src/Arcane/Render/Batcher2D.cpp:1`
- `ArcaneClient/src/Arcane/Render/Batcher2D.hpp:1`
- `ArcaneClient/src/Arcane/Render/DeviceCreationD3D12.cpp:1`
- `ArcaneClient/src/Arcane/Render/DeviceCreationD3D12.hpp:1`
- `ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.cpp:1`
- `ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.hpp:1`
- `ArcaneClient/src/Arcane/Render/DeviceRemovedD3D12Stub.cpp:1`
- `ArcaneClient/src/Arcane/Render/DeviceRemovedObservers.hpp:1`
- `ArcaneClient/src/Arcane/Render/FramePacing.cpp:1`
- `ArcaneClient/src/Arcane/Render/FramePacing.hpp:1`
- `ArcaneClient/src/Arcane/Render/GpuBreadcrumbs.cpp:1`
- `ArcaneClient/src/Arcane/Render/GpuBreadcrumbs.hpp:1`
- `ArcaneClient/src/Arcane/Render/GpuCrashD3D12.cpp:1`
- `ArcaneClient/src/Arcane/Render/GpuCrashReport.cpp:1`
- `ArcaneClient/src/Arcane/Render/GpuCrashReport.hpp:1`
- `ArcaneClient/src/Arcane/Render/GpuFaultInjector.hpp:1`
- `ArcaneClient/src/Arcane/Render/GpuInstrumentation.cpp:1`
- `ArcaneClient/src/Arcane/Render/GpuInstrumentation.hpp:1`
- `ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp:1`
- `ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp:1`
- `ArcaneClient/src/Arcane/Render/GraphicsBackend.cpp:1`
- `ArcaneClient/src/Arcane/Render/GraphicsBackend.hpp:1`
- `ArcaneClient/src/Arcane/Render/IGpuCrashBackend.hpp:1`
- `ArcaneClient/src/Arcane/Render/MeshCache.cpp:1`
- `ArcaneClient/src/Arcane/Render/MeshCache.hpp:1`
- `ArcaneClient/src/Arcane/Render/MeshMaterialCache.cpp:1`
- `ArcaneClient/src/Arcane/Render/MeshMaterialCache.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/AssetsCookSettings.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/AssetsCookSettings.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/BindlessTable.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/BindlessTable.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/GpuScene.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/GpuScene.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/Graveyard.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/Graveyard.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/MeshResidencyBudget.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GpuSceneSyncNode.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshCullNode.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshCullNode.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriCommon.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriCommon.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriDeviceCaps.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriDiagnostics.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriDiagnostics.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriMeshBufferCache.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriMeshBufferCache.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriPipelineCache.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriPipelineCache.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriSwapChain.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriSwapChain.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriTextureCache.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriTextureCache.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriUploadRing.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/NriUploadRing.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/RenderGraphExec.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/RenderOverlayNode.cpp:1`
- `ArcaneClient/src/Arcane/Render/Nri/RenderOverlayNode.hpp:1`
- `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.cpp:1`
- `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.hpp:1`
- `ArcaneClient/src/Arcane/Render/PhysicsDebugSettings.cpp:1`
- `ArcaneClient/src/Arcane/Render/PhysicsDebugSettings.hpp:1`
- `ArcaneClient/src/Arcane/Render/PickEmit.cpp:1`
- `ArcaneClient/src/Arcane/Render/PickEmit.hpp:1`
- `ArcaneClient/src/Arcane/Render/PostChainCache.cpp:1`
- `ArcaneClient/src/Arcane/Render/PostChainCache.hpp:1`
- `ArcaneClient/src/Arcane/Render/RenderBudgetSettings.cpp:1`
- `ArcaneClient/src/Arcane/Render/RenderBudgetSettings.hpp:1`
- `ArcaneClient/src/Arcane/Render/RenderDeviceDesc.hpp:1`
- `ArcaneClient/src/Arcane/Render/RenderDeviceSettings.cpp:1`
- `ArcaneClient/src/Arcane/Render/RenderDeviceSettings.hpp:1`
- `ArcaneClient/src/Arcane/Render/RenderErrorLatch.cpp:1`
- `ArcaneClient/src/Arcane/Render/RenderErrorLatch.hpp:1`
- `ArcaneClient/src/Arcane/Render/RenderGraphConfig.hpp:1`
- `ArcaneClient/src/Arcane/Render/RenderLookSettings.cpp:1`
- `ArcaneClient/src/Arcane/Render/RenderLookSettings.hpp:1`
- `ArcaneClient/src/Arcane/Render/RenderOutlineSettings.cpp:1`
- `ArcaneClient/src/Arcane/Render/RenderOutlineSettings.hpp:1`
- `ArcaneClient/src/Arcane/Render/RenderShaderSettings.cpp:1`
- `ArcaneClient/src/Arcane/Render/RenderShaderSettings.hpp:1`
- `ArcaneClient/src/Arcane/Render/RenderSystems.hpp:1`
- `ArcaneClient/src/Arcane/Render/ShaderCompiler.cpp:1`
- `ArcaneClient/src/Arcane/Render/ShaderCompiler.hpp:1`
- `ArcaneClient/src/Arcane/Render/ShaderConventions.hpp:1`
- `ArcaneClient/src/Arcane/Render/ShaderPaths.cpp:1`
- `ArcaneClient/src/Arcane/Render/ShaderPaths.hpp:1`
- `ArcaneClient/src/Arcane/Render/ShaderSourceProvider.cpp:1`
- `ArcaneClient/src/Arcane/Render/ShaderSourceProvider.hpp:1`
- `ArcaneClient/src/Arcane/Render/SpriteCache.cpp:1`
- `ArcaneClient/src/Arcane/Render/SpriteCache.hpp:1`
- `ArcaneClient/src/Arcane/Render/SpriteGeometry.hpp:1`
- `ArcaneClient/src/Arcane/Render/SpriteMaterialCache.cpp:1`
- `ArcaneClient/src/Arcane/Render/SpriteMaterialCache.hpp:1`
- `ArcaneClient/src/Arcane/Render/StbImpl.cpp:1`
- `ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp:1`
- `ArcaneClient/src/Arcane/Render/VulkanDispatchStorage.cpp:1`

#### ArcaneScene (20 files)

- `ArcaneCore/src/Arcane/Ecs.hpp:1`
- `ArcaneCore/src/Arcane/EcsFwd.hpp:1`
- `ArcaneCore/src/Arcane/Scene/BoundsSystem.hpp:1`
- `ArcaneCore/src/Arcane/Scene/Components.hpp:1`
- `ArcaneCore/src/Arcane/Scene/Frustum.hpp:1`
- `ArcaneCore/src/Arcane/Scene/RenderViewSettings.cpp:1`
- `ArcaneCore/src/Arcane/Scene/RenderViewSettings.hpp:1`
- `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp:1`
- `ArcaneCore/src/Arcane/Scene/SceneModule.hpp:1`
- `ArcaneCore/src/Arcane/Scene/SceneResources.hpp:1`
- `ArcaneCore/src/Arcane/Scene/SpriteGeometry.hpp:1`
- `ArcaneCore/src/Arcane/Scene/TransformSystems.hpp:1`
- `ArcaneCore/src/Arcane/Scene/ViewTransform.hpp:1`
- `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp:1`
- `ArcaneCore/src/Arcane/Serialization/RegistrySnapshot.hpp:1`
- `ArcaneCore/src/Arcane/Serialization/ResourceSerialization.cpp:1`
- `ArcaneCore/src/Arcane/Serialization/ResourceSerialization.hpp:1`
- `ArcaneCore/src/Arcane/Serialization/SceneAsset.cpp:1`
- `ArcaneCore/src/Arcane/Serialization/SceneAsset.hpp:1`
- `ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp:1`

#### ArcaneSim (7 files)

- `ArcaneCore/src/Arcane/Sim/NetDriver.hpp:1`
- `ArcaneCore/src/Arcane/Sim/RunLoop.hpp:1`
- `ArcaneCore/src/Arcane/Sim/SimSettings.cpp:1`
- `ArcaneCore/src/Arcane/Sim/SimSettings.hpp:1`
- `ArcaneCore/src/Arcane/Sim/SimSettingsData.hpp:1`
- `ArcaneCore/src/Arcane/Sim/SystemSchedulers.hpp:1`
- `ArcaneCore/src/Arcane/Sim/Time.hpp:1`

#### ArcaneVerify (9 files)

- `ArcaneClient/src/Arcane/Host/OffscreenVehicle.cpp:1`
- `ArcaneClient/src/Arcane/Host/OffscreenVehicle.hpp:1`
- `ArcaneClient/src/Arcane/Host/ReferenceImages.cpp:1`
- `ArcaneClient/src/Arcane/Host/ReferenceImages.hpp:1`
- `ArcaneClient/src/Arcane/Host/Verdict.cpp:1`
- `ArcaneClient/src/Arcane/Host/Verdict.hpp:1`
- `ArcaneClient/src/Arcane/Host/VerifyHooks.hpp:1`
- `ArcaneClient/src/Arcane/Host/VerifyReport.cpp:1`
- `ArcaneClient/src/Arcane/Host/VerifyReport.hpp:1`

#### ArcaneWindow (3 files)

- `ArcaneClient/src/Arcane/Platform/RenderWindowSettings.hpp:1`
- `ArcaneClient/src/Arcane/Platform/Window.cpp:1`
- `ArcaneClient/src/Arcane/Platform/Window.hpp:1`


### Unresolved or ambiguous include targets

These sites are excluded from resolved module edges.
None.

## MISSING DEP

None.
## TYPE VIOLATIONS

None.
## CYCLES

None.
## VENDOR INCLUDES

Flags compare direct includes with vendor names explicitly granted by the spec's s1 dependency cells. `Vulkan-Headers` and similar SDK headers are shown because they are direct external includes; this comparison does not infer transitive permission.

### ArcaneAssets

#### glm — NOT DECLARED (3 sites)

- `ArcaneCore/src/Arcane/Mesh/MeshBuilder.cpp:3` → `glm/gtc/constants.hpp`
- `ArcaneCore/src/Arcane/Mesh/MeshBuilder.hpp:41` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Sprite/SpriteAsset.hpp:17` → `glm/glm.hpp`

#### nlohmann — NOT DECLARED (9 sites)

- `ArcaneCore/src/Arcane/Assets/ArtifactReader.cpp:3` → `Json.hpp`
- `ArcaneCore/src/Arcane/Assets/Assets.cpp:11` → `Json.hpp`
- `ArcaneCore/src/Arcane/Assets/Assets.hpp:16` → `Json.hpp`
- `ArcaneCore/src/Arcane/Assets/IdentityFieldRule.hpp:36` → `Json.hpp`
- `ArcaneCore/src/Arcane/Material/MaterialAsset.cpp:6` → `Json.hpp`
- `ArcaneCore/src/Arcane/Material/MaterialAsset.hpp:23` → `Json.hpp`
- `ArcaneCore/src/Arcane/Material/MaterialGraph.hpp:27` → `Json.hpp`
- `ArcaneCore/src/Arcane/Mesh/MeshAsset.cpp:5` → `Json.hpp`
- `ArcaneCore/src/Arcane/Sprite/SpriteAsset.cpp:5` → `Json.hpp`

#### spdlog — NOT DECLARED (1 sites)

- `ArcaneCore/src/Arcane/Mesh/MeshAsset.cpp:6` → `spdlog/fmt/fmt.h`

#### stb — NOT DECLARED (4 sites)

- `ArcaneCore/src/Arcane/Assets/Assets.cpp:12` → `stb_image.h`
- `ArcaneCore/src/Arcane/Assets/Assets.cpp:13` → `stb_image_write.h`
- `ArcaneCore/src/Arcane/Assets/StbImpl.cpp:13` → `stb_image_write.h`
- `ArcaneCore/src/Arcane/Assets/StbImpl.cpp:8` → `stb_image.h`

### ArcaneAudio

#### miniaudio — NOT DECLARED (1 sites)

- `ArcaneClient/src/Arcane/Audio/AudioDevice.cpp:15` → `miniaudio.h`

### ArcaneClient

#### Astra — NOT DECLARED (4 sites)

- `ArcaneClient/src/Arcane/Client/ClientRuntime.cpp:16` → `Astra/Core/TypeContext.hpp`
- `ArcaneClient/src/Arcane/Client/ClientRuntime.cpp:17` → `Astra/Registry/Registry.hpp`
- `ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp:28` → `Astra/Registry/Registry.hpp`
- `ArcaneClient/src/Arcane/Host/SceneRenderResolver.cpp:21` → `Astra/Registry/Registry.hpp`

#### glm — NOT DECLARED (2 sites)

- `ArcaneClient/src/Arcane/Client/ClientRuntime.hpp:23` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Client/RuntimePresentation.hpp:18` → `glm/glm.hpp`

#### nlohmann — NOT DECLARED (2 sites)

- `ArcaneClient/src/Arcane/Host/ExclusionList.cpp:3` → `Json.hpp`
- `ArcaneClient/src/Arcane/Host/ProjectBoot.hpp:30` → `Json.hpp`

### ArcaneCore

#### Astra — allowed (11 sites)

- `ArcaneCore/src/Arcane/Config/Bindings/AstraBinding.hpp:14` → `Astra/Core/WorkScheduler.hpp`
- `ArcaneCore/src/Arcane/Config/Bindings/AstraBinding.hpp:15` → `Astra/Registry/Registry.hpp`
- `ArcaneCore/src/Arcane/Config/Settings.hpp:36` → `Astra/Core/TypeID.hpp`
- `ArcaneCore/src/Arcane/Config/SettingsField.hpp:19` → `Astra/Core/TypeID.hpp`
- `ArcaneCore/src/Arcane/Config/SettingsField.hpp:20` → `Astra/Reflection/Attribute.hpp`
- `ArcaneCore/src/Arcane/Config/SettingsField.hpp:21` → `Astra/Reflection/EnumInfo.hpp`
- `ArcaneCore/src/Arcane/Config/SettingsField.hpp:22` → `Astra/Reflection/MetaRegistry.hpp`
- `ArcaneCore/src/Arcane/Config/SettingsField.hpp:23` → `Astra/Reflection/TypeMeta.hpp`
- `ArcaneCore/src/Arcane/Jobs/JobSystem.hpp:11` → `Astra/Core/WorkScheduler.hpp`
- `ArcaneCore/src/Arcane/Reflection.hpp:19` → `Astra/Reflection/Attribute.hpp`
- `ArcaneCore/src/Arcane/Reflection.hpp:20` → `Astra/Reflection/Reflection.hpp`

#### Mosaic — NOT DECLARED (5 sites)

- `ArcaneCore/src/Arcane/Base/Assert.hpp:11` → `Mosaic/Assert.hpp`
- `ArcaneCore/src/Arcane/Base/Log.cpp:12` → `Mosaic/Assert.hpp`
- `ArcaneCore/src/Arcane/Base/Log.cpp:13` → `Mosaic/Log.hpp`
- `ArcaneCore/src/Arcane/Base/Log.hpp:16` → `Mosaic/Log.hpp`
- `ArcaneCore/src/Arcane/Jobs/ArcaneWorkScheduler.hpp:23` → `Mosaic/Jobs/WorkScheduler.hpp`

#### enkiTS — allowed (2 sites)

- `ArcaneCore/src/Arcane/Config/Bindings/JobsBinding.cpp:3` → `TaskScheduler.h`
- `ArcaneCore/src/Arcane/Jobs/JobSystem.cpp:7` → `TaskScheduler.h`

#### glm — allowed (2 sites)

- `ArcaneCore/src/Arcane/Math/Aabb.hpp:12` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Math/NormalMatrix.hpp:7` → `glm/glm.hpp`

#### nlohmann — NOT DECLARED (4 sites)

- `ArcaneCore/src/Arcane/Base/DiagEnvelope.cpp:5` → `Json.hpp`
- `ArcaneCore/src/Arcane/Base/Diagnostics.cpp:18` → `Json.hpp`
- `ArcaneCore/src/Arcane/Config/CVarConfig.hpp:10` → `Json.hpp`
- `ArcaneCore/src/Arcane/Config/Config.hpp:13` → `Json.hpp`

#### picosha2 — NOT DECLARED (1 sites)

- `ArcaneCore/src/Arcane/Guid.cpp:5` → `picosha2.hpp`

#### spdlog — NOT DECLARED (10 sites)

- `ArcaneCore/src/Arcane/Base/Assert.cpp:6` → `spdlog/spdlog.h`
- `ArcaneCore/src/Arcane/Base/Log.cpp:3` → `spdlog/sinks/base_sink.h`
- `ArcaneCore/src/Arcane/Base/Log.cpp:4` → `spdlog/sinks/basic_file_sink.h`
- `ArcaneCore/src/Arcane/Base/Log.cpp:5` → `spdlog/sinks/dist_sink.h`
- `ArcaneCore/src/Arcane/Base/Log.cpp:6` → `spdlog/sinks/stdout_color_sinks.h`
- `ArcaneCore/src/Arcane/Base/Log.hpp:14` → `spdlog/spdlog.h`
- `ArcaneCore/src/Arcane/Util/Logger.hpp:31` → `spdlog/spdlog.h`
- `ArcaneCore/src/Arcane/Util/Logger.hpp:32` → `spdlog/sinks/stdout_color_sinks.h`
- `ArcaneCore/src/Arcane/Util/Logger.hpp:33` → `spdlog/sinks/rotating_file_sink.h`
- `ArcaneCore/src/Arcane/Util/Logger.hpp:34` → `spdlog/fmt/fmt.h`

### ArcaneCrypto

#### picosha2 — NOT DECLARED (1 sites)

- `ArcaneCore/src/Arcane/Crypto/Crypto.hpp:13` → `picosha2.hpp`

### ArcaneEdit

#### Astra — NOT DECLARED (10 sites)

- `ArcaneClient/src/Arcane/Edit/CommandStack.hpp:13` → `Astra/Entity/Entity.hpp`
- `ArcaneClient/src/Arcane/Edit/ComponentEditCommand.cpp:3` → `Astra/Component/Component.hpp`
- `ArcaneClient/src/Arcane/Edit/ComponentEditCommand.cpp:4` → `Astra/Registry/Registry.hpp`
- `ArcaneClient/src/Arcane/Edit/ComponentEditCommand.cpp:5` → `Astra/Serialization/BinaryReader.hpp`
- `ArcaneClient/src/Arcane/Edit/ComponentEditCommand.cpp:6` → `Astra/Serialization/BinaryWriter.hpp`
- `ArcaneClient/src/Arcane/Edit/ComponentEditCommand.hpp:12` → `Astra/Entity/Entity.hpp`
- `ArcaneClient/src/Arcane/Edit/EntityOps.cpp:12` → `Astra/Core/TypeID.hpp`
- `ArcaneClient/src/Arcane/Edit/EntityOps.cpp:13` → `Astra/Registry/Registry.hpp`
- `ArcaneClient/src/Arcane/Edit/EntityOps.hpp:13` → `Astra/Entity/Entity.hpp`
- `ArcaneClient/src/Arcane/Edit/RegistryStateCommand.hpp:27` → `Astra/Entity/Entity.hpp`

#### glm — NOT DECLARED (6 sites)

- `ArcaneClient/src/Arcane/Edit/EntityOps.hpp:15` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Edit/Gizmo.cpp:5` → `glm/gtc/matrix_access.hpp`
- `ArcaneClient/src/Arcane/Edit/Gizmo.cpp:6` → `glm/gtc/matrix_transform.hpp`
- `ArcaneClient/src/Arcane/Edit/Gizmo.cpp:7` → `glm/gtc/quaternion.hpp`
- `ArcaneClient/src/Arcane/Edit/Gizmo.hpp:18` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Edit/Gizmo.hpp:19` → `glm/gtc/quaternion.hpp`

#### nlohmann — NOT DECLARED (1 sites)

- `ArcaneClient/src/Arcane/Edit/EntityOps.hpp:17` → `Json.hpp`

### ArcaneEngine

#### Astra — NOT DECLARED (13 sites)

- `ArcaneCore/src/Arcane/Base/ProcessContext.cpp:4` → `Astra/Core/TypeContext.hpp`
- `ArcaneCore/src/Arcane/Base/Runtime.cpp:33` → `Astra/Registry/Registry.hpp`
- `ArcaneCore/src/Arcane/Base/Runtime.cpp:34` → `Astra/Component/ComponentModule.hpp`
- `ArcaneCore/src/Arcane/Base/Runtime.cpp:35` → `Astra/Component/ComponentRegistry.hpp`
- `ArcaneCore/src/Arcane/Base/Runtime.cpp:36` → `Astra/Core/TypeContext.hpp`
- `ArcaneCore/src/Arcane/Base/Runtime.cpp:37` → `Astra/Core/WorkScheduler.hpp`
- `ArcaneCore/src/Arcane/Base/Runtime.cpp:38` → `Astra/Serialization/SerializationError.hpp`
- `ArcaneCore/src/Arcane/Plugin/GameSystems.hpp:27` → `Astra/Core/TypeID.hpp`
- `ArcaneCore/src/Arcane/Plugin/PluginHost.cpp:20` → `Astra/Serialization/BinaryReader.hpp`
- `ArcaneCore/src/Arcane/Plugin/PluginHost.cpp:21` → `Astra/Serialization/BinaryWriter.hpp`
- `ArcaneCore/src/Arcane/Project/ProjectHost.hpp:37` → `Astra/Component/ComponentRegistry.hpp`
- `ArcaneCore/src/Arcane/Project/ProjectHost.hpp:38` → `Astra/Core/TypeID.hpp`
- `ArcaneCore/src/Arcane/Scene/EngineResourceTypes.cpp:7` → `Astra/Core/TypeID.hpp`

#### glm — NOT DECLARED (1 sites)

- `ArcaneCore/src/Arcane/Base/Runtime.hpp:25` → `glm/glm.hpp`

#### imgui — NOT DECLARED (1 sites)

- `ArcaneCore/src/Arcane/Plugin/GameModule.hpp:51` → `imgui.h`

#### nlohmann — NOT DECLARED (4 sites)

- `ArcaneCore/src/Arcane/Project/AssetRegistry.cpp:7` → `Json.hpp`
- `ArcaneCore/src/Arcane/Project/Project.cpp:12` → `Json.hpp`
- `ArcaneCore/src/Arcane/Project/ProjectManifest.cpp:6` → `Json.hpp`
- `ArcaneCore/src/Arcane/Project/ProjectManifest.hpp:14` → `Json.hpp`

### ArcaneImGui

#### NRI — NOT DECLARED (5 sites)

- `ArcaneClient/src/Arcane/ImGui/ImGuiNri.cpp:11` → `Extensions/NRIHelper.h`
- `ArcaneClient/src/Arcane/ImGui/ImGuiNri.cpp:9` → `NRI.h`
- `ArcaneClient/src/Arcane/ImGui/ImGuiNri.hpp:122` → `NRI.h`
- `ArcaneClient/src/Arcane/ImGui/ImGuiNriNode.cpp:9` → `NRI.h`
- `ArcaneClient/src/Arcane/ImGui/ImGuiNriNode.hpp:42` → `NRI.h`

#### SDL3 — NOT DECLARED (1 sites)

- `ArcaneClient/src/Arcane/ImGui/ImGuiLayer.cpp:9` → `SDL3/SDL.h`

#### glm — NOT DECLARED (1 sites)

- `ArcaneClient/src/Arcane/ImGui/OffscreenImGuiLayer.hpp:33` → `glm/glm.hpp`

#### imgui — allowed (7 sites)

- `ArcaneClient/src/Arcane/ImGui/ConsoleInputLine.cpp:5` → `imgui.h`
- `ArcaneClient/src/Arcane/ImGui/ImGuiHostHooks.cpp:6` → `imgui.h`
- `ArcaneClient/src/Arcane/ImGui/ImGuiLayer.cpp:6` → `imgui.h`
- `ArcaneClient/src/Arcane/ImGui/ImGuiLayer.cpp:7` → `backends/imgui_impl_sdl3.h`
- `ArcaneClient/src/Arcane/ImGui/ImGuiNri.cpp:24` → `imgui.h`
- `ArcaneClient/src/Arcane/ImGui/ImGuiNriNode.cpp:11` → `ImGuiNriNode.hpp`
- `ArcaneClient/src/Arcane/ImGui/OffscreenImGuiLayer.cpp:6` → `imgui.h`

### ArcaneInput

#### SDL3 — NOT DECLARED (17 sites)

- `ArcaneClient/src/Arcane/Input/InputActions.cpp:10` → `SDL3/SDL_keyboard.h`
- `ArcaneClient/src/Arcane/Input/InputActions.cpp:11` → `SDL3/SDL_keycode.h`
- `ArcaneClient/src/Arcane/Input/InputActions.cpp:12` → `SDL3/SDL_scancode.h`
- `ArcaneClient/src/Arcane/Input/InputDevices.cpp:10` → `SDL3/SDL_mouse.h`
- `ArcaneClient/src/Arcane/Input/InputDevices.cpp:11` → `SDL3/SDL_scancode.h`
- `ArcaneClient/src/Arcane/Input/InputDevices.cpp:5` → `SDL3/SDL_events.h`
- `ArcaneClient/src/Arcane/Input/InputDevices.cpp:6` → `SDL3/SDL_gamepad.h`
- `ArcaneClient/src/Arcane/Input/InputDevices.cpp:7` → `SDL3/SDL_init.h`
- `ArcaneClient/src/Arcane/Input/InputDevices.cpp:8` → `SDL3/SDL_keyboard.h`
- `ArcaneClient/src/Arcane/Input/InputDevices.cpp:9` → `SDL3/SDL_keycode.h`
- `ArcaneClient/src/Arcane/Input/InputRebindOperation.cpp:4` → `SDL3/SDL_keyboard.h`
- `ArcaneClient/src/Arcane/Input/InputRebindOperation.cpp:5` → `SDL3/SDL_scancode.h`
- `ArcaneClient/src/Arcane/Input/KeyLayout.cpp:3` → `SDL3/SDL_keyboard.h`
- `ArcaneClient/src/Arcane/Input/KeyLayout.cpp:4` → `SDL3/SDL_keycode.h`
- `ArcaneClient/src/Arcane/Input/KeyLayout.cpp:5` → `SDL3/SDL_scancode.h`
- `ArcaneClient/src/Arcane/Input/LocalInputUser.cpp:5` → `SDL3/SDL_filesystem.h`
- `ArcaneClient/src/Arcane/Input/LocalInputUser.cpp:6` → `SDL3/SDL_stdinc.h`

#### glm — NOT DECLARED (2 sites)

- `ArcaneClient/src/Arcane/Input/InputActions.cpp:14` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Input/InputActions.hpp:16` → `glm/glm.hpp`

#### nlohmann — NOT DECLARED (3 sites)

- `ArcaneClient/src/Arcane/Input/InputActionAsset.hpp:9` → `Json.hpp`
- `ArcaneClient/src/Arcane/Input/InputActions.cpp:8` → `Json.hpp`
- `ArcaneClient/src/Arcane/Input/InputActions.hpp:14` → `Json.hpp`

### ArcaneNet

#### nlohmann — NOT DECLARED (1 sites)

- `ArcaneCore/src/Arcane/Net/Protocol.hpp:12` → `Json.hpp`

### ArcanePhysics2D

#### Manifold2D — allowed (17 sites)

- `ArcaneCore/src/Arcane/Scene/Physics2D.cpp:4` → `Manifold2D/Physics/PhysicsWorld.hpp`
- `ArcaneCore/src/Arcane/Scene/Physics2DDetail.hpp:10` → `Manifold2D/Physics/Fixture.hpp`
- `ArcaneCore/src/Arcane/Scene/Physics2DDetail.hpp:11` → `Manifold2D/Physics/PhysicsWorld.hpp`
- `ArcaneCore/src/Arcane/Scene/Physics2DDetail.hpp:12` → `Manifold2D/Physics/Shapes.hpp`
- `ArcaneCore/src/Arcane/Scene/Physics2DHandles.hpp:8` → `Manifold2D/Physics/Fixture.hpp`
- `ArcaneCore/src/Arcane/Scene/Physics2DHandles.hpp:9` → `Manifold2D/Physics/PhysicsTypes.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:12` → `Manifold2D/Physics/Broadphase/Broadphase.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:13` → `Manifold2D/Physics/Broadphase/DynamicTree.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:14` → `Manifold2D/Physics/Broadphase/SpatialGrid.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:15` → `Manifold2D/Physics/Narrowphase/NarrowphaseTrace.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:16` → `Manifold2D/Physics/PhysicsTypes.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:17` → `Manifold2D/Physics/PhysicsWorld.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:18` → `Manifold2D/Physics/Shapes.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:19` → `Manifold2D/Physics/Solver/Solver.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.hpp:5` → `Manifold2D/Physics/PhysicsTypes.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.cpp:3` → `Manifold2D/Physics/Events.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.cpp:4` → `Manifold2D/Physics/PhysicsWorld.hpp`

#### glm — NOT DECLARED (7 sites)

- `ArcaneCore/src/Arcane/Scene/PhysicsComponents.hpp:11` → `glm/vec2.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:6` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.cpp:7` → `glm/trigonometric.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.hpp:6` → `glm/vec2.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry.hpp:7` → `glm/vec4.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.hpp:12` → `glm/vec2.hpp`
- `ArcaneCore/src/Arcane/Scene/PhysicsSystem.hpp:11` → `glm/vec2.hpp`

### ArcaneRender

#### Astra — NOT DECLARED (9 sites)

- `ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp:27` → `Astra/Registry/Registry.hpp`
- `ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp:13` → `Astra/Container/FlatMap.hpp`
- `ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp:14` → `Astra/Core/Tick.hpp`
- `ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp:15` → `Astra/Entity/Entity.hpp`
- `ArcaneClient/src/Arcane/Render/PickEmit.cpp:11` → `Astra/Registry/Registry.hpp`
- `ArcaneClient/src/Arcane/Render/PickEmit.hpp:23` → `Astra/Entity/Entity.hpp`
- `ArcaneClient/src/Arcane/Render/RenderSystems.hpp:37` → `Astra/Registry/Registry.hpp`
- `ArcaneClient/src/Arcane/Render/RenderSystems.hpp:38` → `Astra/System/System.hpp`
- `ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp:30` → `Astra/Registry/Registry.hpp`

#### DXC — NOT DECLARED (2 sites)

- `ArcaneClient/src/Arcane/Render/ShaderCompiler.cpp:168` → `dxcapi.h`
- `ArcaneClient/src/Arcane/Render/ShaderCompiler.cpp:177` → `dxcapi.h`

#### NRI — allowed (78 sites)

- `ArcaneClient/src/Arcane/Render/Nri/BindlessTable.cpp:8` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/BindlessTable.hpp:36` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/GpuScene.cpp:2` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/GpuScene.hpp:38` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/GpuScene.hpp:39` → `Extensions/NRIDeviceCreation.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriCommon.cpp:3` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriCommon.cpp:4` → `Extensions/NRIDeviceCreation.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriCommon.cpp:6` → `NriCommon.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriCommon.hpp:17` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriCommon.hpp:18` → `Extensions/NRIDeviceCreation.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp:14` → `Extensions/NRIRayTracing.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp:15` → `Extensions/NRIWrapperVK.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp:17` → `NriDevice.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp:19` → `NriCommon.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp:4` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp:6` → `Extensions/NRIDeviceCreation.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp:9` → `Extensions/NRIWrapperD3D12.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.hpp:21` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriDevice.hpp:22` → `Extensions/NRIDeviceCreation.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriDiagnostics.cpp:11` → `NriCommon.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriDiagnostics.cpp:6` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriDiagnostics.cpp:9` → `NriDiagnostics.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriDiagnostics.hpp:51` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp:10` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp:12` → `Extensions/NRISwapChain.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp:14` → `NriGraphContext.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp:16` → `NriCommon.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp:222` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriMeshBufferCache.cpp:10` → `Extensions/NRIHelper.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriMeshBufferCache.cpp:12` → `NriMeshBufferCache.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriMeshBufferCache.cpp:9` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriMeshBufferCache.hpp:47` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriMeshBufferCache.hpp:48` → `Extensions/NRIHelper.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriPipelineCache.cpp:11` → `NriPipelineCache.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriPipelineCache.cpp:13` → `NriCommon.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriPipelineCache.cpp:9` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriPipelineCache.hpp:83` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriSwapChain.cpp:11` → `NriCommon.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriSwapChain.cpp:5` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriSwapChain.cpp:7` → `Extensions/NRISwapChain.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriSwapChain.cpp:9` → `NriSwapChain.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriSwapChain.hpp:78` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriSwapChain.hpp:79` → `Extensions/NRISwapChain.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriTextureCache.cpp:11` → `NriTextureCache.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriTextureCache.cpp:8` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriTextureCache.cpp:9` → `Extensions/NRIHelper.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriTextureCache.hpp:43` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriTextureCache.hpp:44` → `Extensions/NRIHelper.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriUploadRing.cpp:10` → `NriCommon.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriUploadRing.cpp:6` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriUploadRing.cpp:8` → `NriUploadRing.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/NriUploadRing.hpp:41` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp:45` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/RenderGraphExec.cpp:29` → `NriCommon.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/RenderGraphExec.cpp:30` → `NriDevice.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/RenderGraphExec.cpp:31` → `NriSwapChain.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/RenderOverlayNode.hpp:3` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:10` → `Extensions/NRIHelper.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:8` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.hpp:106` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.cpp:6` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.cpp:8` → `Extensions/NRIHelper.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.hpp:68` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GpuSceneSyncNode.cpp:2` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp:18` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp:19` → `Extensions/NRIDeviceCreation.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.cpp:6` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.cpp:7` → `Extensions/NRIHelper.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.hpp:69` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshCullNode.cpp:1` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshCullNode.hpp:3` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshCullNode.hpp:5` → `Extensions/NRIDeviceCreation.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp:10` → `Extensions/NRIHelper.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp:8` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp:127` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp:129` → `Extensions/NRIDeviceCreation.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp:9` → `NRI.h`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.hpp:110` → `NRI.h`

#### SDL3 — NOT DECLARED (3 sites)

- `ArcaneClient/src/Arcane/Render/GpuInstrumentation.cpp:7` → `SDL3/SDL_timer.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp:30` → `SDL3/SDL_timer.h`
- `ArcaneClient/src/Arcane/Render/Nri/NriSwapChain.cpp:21` → `SDL3/SDL_timer.h`

#### Vulkan-Headers — NOT DECLARED (4 sites)

- `ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.cpp:24` → `vulkan/vulkan.hpp`
- `ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.cpp:25` → `vulkan/vulkan_beta.h`
- `ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.hpp:22` → `vulkan/vulkan.hpp`
- `ArcaneClient/src/Arcane/Render/VulkanDispatchStorage.cpp:8` → `vulkan/vulkan.hpp`

#### glm — NOT DECLARED (13 sites)

- `ArcaneClient/src/Arcane/Render/Batcher2D.hpp:21` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp:17` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.hpp:78` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp:144` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.hpp:123` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.hpp:124` → `glm/mat4x4.hpp`
- `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.hpp:32` → `glm/vec2.hpp`
- `ArcaneClient/src/Arcane/Render/PickEmit.cpp:13` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Render/PickEmit.hpp:25` → `glm/mat4x4.hpp`
- `ArcaneClient/src/Arcane/Render/PickEmit.hpp:26` → `glm/vec2.hpp`
- `ArcaneClient/src/Arcane/Render/PickEmit.hpp:27` → `glm/vec3.hpp`
- `ArcaneClient/src/Arcane/Render/RenderSystems.hpp:40` → `glm/glm.hpp`
- `ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp:32` → `glm/glm.hpp`

#### stb — NOT DECLARED (1 sites)

- `ArcaneClient/src/Arcane/Render/StbImpl.cpp:20` → `stb_image.h`

### ArcaneScene

#### Astra — NOT DECLARED (40 sites)

- `ArcaneCore/src/Arcane/Ecs.hpp:19` → `Astra/Component/ComponentModule.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:20` → `Astra/Core/Result.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:21` → `Astra/Core/Tick.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:22` → `Astra/Core/TypeContext.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:23` → `Astra/Registry/Query.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:24` → `Astra/Registry/Registry.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:25` → `Astra/Serialization/BinaryReader.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:26` → `Astra/Serialization/BinaryWriter.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:27` → `Astra/Serialization/SerializationError.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:28` → `Astra/System/System.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:29` → `Astra/System/SystemParam.hpp`
- `ArcaneCore/src/Arcane/Ecs.hpp:30` → `Astra/System/SystemScheduler.hpp`
- `ArcaneCore/src/Arcane/Scene/BoundsSystem.hpp:27` → `Astra/Registry/Registry.hpp`
- `ArcaneCore/src/Arcane/Scene/BoundsSystem.hpp:28` → `Astra/System/System.hpp`
- `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp:20` → `Astra/Registry/Registry.hpp`
- `ArcaneCore/src/Arcane/Scene/SceneModule.hpp:10` → `Astra/Component/ComponentRegistry.hpp`
- `ArcaneCore/src/Arcane/Scene/SceneModule.hpp:11` → `Astra/Registry/Registry.hpp`
- `ArcaneCore/src/Arcane/Scene/SceneModule.hpp:12` → `Astra/Serialization/SerializationError.hpp`
- `ArcaneCore/src/Arcane/Scene/SceneResources.hpp:14` → `Astra/Container/FlatMap.hpp`
- `ArcaneCore/src/Arcane/Scene/TransformSystems.hpp:54` → `Astra/Container/FlatMap.hpp`
- `ArcaneCore/src/Arcane/Scene/TransformSystems.hpp:55` → `Astra/Container/FlatSet.hpp`
- `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp:48` → `Astra/Reflection/FieldVisitor.hpp`
- `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp:49` → `Astra/Reflection/FieldInfo.hpp`
- `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp:50` → `Astra/Reflection/MetaRegistry.hpp`
- `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp:51` → `Astra/Core/TypeID.hpp`
- `ArcaneCore/src/Arcane/Serialization/RegistrySnapshot.hpp:19` → `Astra/Core/Result.hpp`
- `ArcaneCore/src/Arcane/Serialization/RegistrySnapshot.hpp:20` → `Astra/Serialization/BinaryReader.hpp`
- `ArcaneCore/src/Arcane/Serialization/RegistrySnapshot.hpp:21` → `Astra/Serialization/BinaryWriter.hpp`
- `ArcaneCore/src/Arcane/Serialization/RegistrySnapshot.hpp:22` → `Astra/Serialization/SerializationError.hpp`
- `ArcaneCore/src/Arcane/Serialization/ResourceSerialization.cpp:5` → `Astra/Core/TypeID.hpp`
- `ArcaneCore/src/Arcane/Serialization/ResourceSerialization.cpp:6` → `Astra/Entity/Entity.hpp`
- `ArcaneCore/src/Arcane/Serialization/ResourceSerialization.hpp:24` → `Astra/Core/Result.hpp`
- `ArcaneCore/src/Arcane/Serialization/ResourceSerialization.hpp:25` → `Astra/Registry/Registry.hpp`
- `ArcaneCore/src/Arcane/Serialization/ResourceSerialization.hpp:26` → `Astra/Serialization/BinaryReader.hpp`
- `ArcaneCore/src/Arcane/Serialization/ResourceSerialization.hpp:27` → `Astra/Serialization/BinaryWriter.hpp`
- `ArcaneCore/src/Arcane/Serialization/ResourceSerialization.hpp:28` → `Astra/Serialization/SerializationError.hpp`
- `ArcaneCore/src/Arcane/Serialization/SceneAsset.hpp:29` → `Astra/Registry/Registry.hpp`
- `ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp:37` → `Astra/Component/ComponentRegistry.hpp`
- `ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp:38` → `Astra/Reflection/MetaRegistry.hpp`
- `ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp:39` → `Astra/Registry/Registry.hpp`

#### glm — NOT DECLARED (15 sites)

- `ArcaneCore/src/Arcane/Scene/BoundsSystem.hpp:30` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Scene/Components.hpp:14` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Scene/Components.hpp:15` → `glm/gtc/quaternion.hpp`
- `ArcaneCore/src/Arcane/Scene/Frustum.hpp:20` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Scene/Frustum.hpp:21` → `glm/gtc/matrix_access.hpp`
- `ArcaneCore/src/Arcane/Scene/RenderViewSettings.hpp:15` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp:22` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp:23` → `glm/gtc/matrix_transform.hpp`
- `ArcaneCore/src/Arcane/Scene/SceneResources.hpp:16` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Scene/SpriteGeometry.hpp:27` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Scene/TransformSystems.hpp:57` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Scene/ViewTransform.hpp:15` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Scene/ViewTransform.hpp:16` → `glm/gtc/matrix_transform.hpp`
- `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp:53` → `glm/glm.hpp`
- `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp:54` → `glm/gtc/quaternion.hpp`

#### nlohmann — NOT DECLARED (3 sites)

- `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp:55` → `Json.hpp`
- `ArcaneCore/src/Arcane/Serialization/SceneAsset.hpp:31` → `Json.hpp`
- `ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp:41` → `Json.hpp`

### ArcaneSim

#### Astra — allowed (4 sites)

- `ArcaneCore/src/Arcane/Sim/RunLoop.hpp:27` → `Astra/Registry/Registry.hpp`
- `ArcaneCore/src/Arcane/Sim/SystemSchedulers.hpp:11` → `Astra/System/SystemScheduler.hpp`
- `ArcaneCore/src/Arcane/Sim/SystemSchedulers.hpp:12` → `Astra/System/SystemExecutor.hpp`
- `ArcaneCore/src/Arcane/Sim/SystemSchedulers.hpp:13` → `Astra/Core/WorkScheduler.hpp`

### ArcaneVerify

#### nlohmann — NOT DECLARED (1 sites)

- `ArcaneClient/src/Arcane/Host/VerifyReport.hpp:44` → `Json.hpp`

### ArcaneWindow

#### SDL3 — NOT DECLARED (4 sites)

- `ArcaneClient/src/Arcane/Platform/Window.cpp:10` → `SDL3/SDL_metal.h`
- `ArcaneClient/src/Arcane/Platform/Window.cpp:31` → `SDL3/SDL_system.h`
- `ArcaneClient/src/Arcane/Platform/Window.cpp:7` → `SDL3/SDL.h`
- `ArcaneClient/src/Arcane/Platform/Window.cpp:8` → `SDL3/SDL_dialog.h`

#### stb — NOT DECLARED (1 sites)

- `ArcaneClient/src/Arcane/Platform/Window.cpp:13` → `stb_image.h`

## PROGRAM/GAME MODULE USE

Direct includes only; module closure is deliberately not inferred.

### ArcaneEditor/src

#### ArcaneAssets (48 sites)

- `ArcaneEditor/src/App/EditorApp.cpp:55` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:56` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:67` → `Arcane/Assets/AssetId.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:73` → `Arcane/Sprite/SpriteAsset.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:28` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:80` → `Arcane/Assets/Assets.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:44` → `Arcane/Assets/Assets.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:45` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:43` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:44` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:47` → `Arcane/Assets/AssetId.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:49` → `Arcane/Sprite/SpriteAsset.hpp`
- `ArcaneEditor/src/Documents/MaterialSpherePreview.hpp:23` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.cpp:23` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.hpp:69` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:40` → `Arcane/Assets/Assets.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:48` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:49` → `Arcane/Assets/AssetId.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:51` → `Arcane/Material/MaterialGraph.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:41` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:42` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:43` → `Arcane/Material/MaterialInstance.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:44` → `Arcane/Material/MaterialTemplate.hpp`
- `ArcaneEditor/src/Documents/ShaderGraphCategoryColors.hpp:13` → `Arcane/Material/MaterialGraph.hpp`
- `ArcaneEditor/src/Documents/SpriteDocument.cpp:9` → `Arcane/Assets/Assets.hpp`
- `ArcaneEditor/src/Documents/SpriteDocument.hpp:29` → `Arcane/Sprite/SpriteAsset.hpp`
- `ArcaneEditor/src/Panels/AssetGraphViewModel.hpp:51` → `Arcane/Assets/Assets.hpp`
- `ArcaneEditor/src/Panels/AssetInspectorSource.cpp:12` → `Arcane/Assets/AssetId.hpp`
- `ArcaneEditor/src/Panels/AssetPanelCommon.cpp:17` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneEditor/src/Panels/AssetPanelCommon.cpp:18` → `Arcane/Assets/AssetId.hpp`
- `ArcaneEditor/src/Panels/AssetPanelModel.hpp:24` → `Arcane/Assets/Assets.hpp`
- `ArcaneEditor/src/Panels/AssetPanelModel.hpp:26` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneEditor/src/Panels/AssetPanelModel.hpp:28` → `Arcane/Assets/IdentityFieldRule.hpp`
- `ArcaneEditor/src/Panels/AssetReferenceIndex.hpp:34` → `Arcane/Assets/Assets.hpp`
- `ArcaneEditor/src/Panels/CreateAssetDialog.hpp:30` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneEditor/src/Panels/CreateAssetDialog.hpp:31` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:10` → `Arcane/Material/GlobalParams.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:11` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:12` → `Arcane/Material/MaterialInstance.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:13` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:14` → `Arcane/Material/MaterialTemplate.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:15` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:17` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:6` → `Arcane/Assets/Assets.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:7` → `Arcane/Assets/ImageIo.hpp`
- `ArcaneEditor/src/Project/MeshImportWave.hpp:17` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneEditor/src/Project/MeshImportWave.hpp:19` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneEditor/src/Widgets/MaterialParamWidgets.hpp:8` → `Arcane/Material/MaterialTypes.hpp`

#### ArcaneAudio (1 sites)

- `ArcaneEditor/src/App/EditorAppFrame.cpp:46` → `Arcane/Audio/AudioDevice.hpp`

#### ArcaneClient (24 sites)

- `ArcaneEditor/src/App/EditorApp.cpp:42` → `Arcane/Host/GpuSceneHost.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:43` → `Arcane/Host/ProjectBoot.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:18` → `Arcane/Host/HostConfig.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:19` → `Arcane/Host/GpuContext.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:20` → `Arcane/Host/FramePerf.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:21` → `Arcane/Host/BootSequence.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:22` → `Arcane/Host/BootSplashWindow.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:23` → `Arcane/Host/ProjectBoot.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:25` → `Arcane/Host/SettleBound.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:83` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:89` → `Arcane/Host/SceneRenderResolver.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:58` → `Arcane/Host/GpuSceneHost.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:71` → `Arcane/Host/HostSettings.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:52` → `Arcane/Host/HostSettings.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:53` → `Arcane/Host/ProjectBoot.hpp`
- `ArcaneEditor/src/App/HarnessRules.hpp:8` → `Arcane/Host/HostConfig.hpp`
- `ArcaneEditor/src/App/HostPresentation.hpp:20` → `Arcane/Host/HostConfig.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.cpp:19` → `Arcane/Host/HostConfig.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:38` → `Arcane/Host/HostConfig.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.hpp:82` → `Arcane/Host/HostConfig.hpp`
- `ArcaneEditor/src/main.cpp:14` → `Arcane/Host/BootSplashWindow.hpp`
- `ArcaneEditor/src/main.cpp:15` → `Arcane/Host/EarlyConfig.hpp`
- `ArcaneEditor/src/main.cpp:16` → `Arcane/Host/HostConfig.hpp`
- `ArcaneEditor/src/main.cpp:20` → `Arcane/Host/ProjectBoot.hpp`

#### ArcaneCore (272 sites)

- `ArcaneEditor/src/App/EditorApp.cpp:25` → `Arcane/Platform/Platform.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:46` → `Arcane/Base/Assert.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:47` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:48` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:49` → `Arcane/Base/Engine.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:50` → `Arcane/Base/ForeignModules.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:51` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:53` → `Arcane/Jobs/JobSystem.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:57` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:62` → `Arcane/Platform/Paths.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:37` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/App/EditorAppAssetOps.cpp:14` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/App/EditorAppAssetOps.cpp:15` → `Arcane/Platform/Paths.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:48` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:50` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:51` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:52` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:53` → `Arcane/Platform/Paths.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:54` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:99` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:40` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:41` → `Arcane/Build/Toolchain.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:42` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:45` → `Arcane/Platform/Paths.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:51` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/App/EditorAppScene.cpp:17` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/App/PlayMode.cpp:3` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/App/PlayMode.hpp:33` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/App/UndoSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Documents/CrashReportDocument.cpp:13` → `Arcane/Base/DiagnosticsSettings.hpp`
- `ArcaneEditor/src/Documents/CrashReportDocument.cpp:14` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Documents/CrashReportDocument.cpp:15` → `Arcane/Config/UiSettings.hpp`
- `ArcaneEditor/src/Documents/CrashReportDocument.cpp:16` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Documents/CrashReportDocument.hpp:58` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneEditor/src/Documents/CrashReportDocument.hpp:59` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Documents/DocumentHost.cpp:3` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Documents/DocumentHost.hpp:12` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Documents/EditorDocument.hpp:11` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocument.cpp:10` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocument.cpp:11` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocument.cpp:9` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocument.hpp:9` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp:12` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp:19` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Documents/InputActionsJson.hpp:3` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Documents/InputActionsRows.hpp:10` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Documents/InputPendingAdd.hpp:8` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Documents/InputSelectionKey.hpp:11` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Documents/MaterialSpherePreview.hpp:21` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Documents/MaterialSpherePreview.hpp:22` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Documents/MaterialSpherePreview.hpp:31` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.cpp:21` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.cpp:41` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.hpp:67` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.hpp:68` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:41` → `Arcane/Base/Assert.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:42` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:43` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:45` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:73` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:47` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:56` → `Arcane/Util/FunctionRef.hpp`
- `ArcaneEditor/src/Documents/ShaderGraphPinLegend.cpp:11` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Documents/ShaderGraphPinLegend.cpp:12` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Documents/ShaderGraphPinLegend.cpp:16` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Documents/SpriteDocument.cpp:10` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Documents/SpriteDocument.hpp:28` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Input/EditorActions.cpp:4` → `Arcane/Base/Assert.hpp`
- `ArcaneEditor/src/Input/EditorActions.cpp:5` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Input/EditorActions.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Input/EditorActions.cpp:7` → `Arcane/Config/CVarModule.hpp`
- `ArcaneEditor/src/Input/EditorActions.hpp:10` → `Arcane/Config/CVarHandle.hpp`
- `ArcaneEditor/src/Panels/AssetActivityLog.cpp:4` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/AssetActivityLog.hpp:22` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetBrowserPanel.cpp:15` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/AssetBrowserPanel.cpp:27` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/AssetBrowserPanel.hpp:15` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetFileOpDialogs.hpp:23` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetGraphPanel.cpp:26` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/AssetGraphPanel.cpp:27` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/AssetGraphPanel.cpp:28` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetGraphPanel.hpp:28` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetGraphPanel.hpp:34` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/AssetGraphViewModel.cpp:4` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/AssetGraphViewModel.hpp:52` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetInspectorSource.cpp:11` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/AssetInspectorSource.cpp:24` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/AssetInspectorSource.hpp:15` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetPanelCommon.cpp:14` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Panels/AssetPanelCommon.cpp:15` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/AssetPanelCommon.cpp:16` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetPanelCommon.cpp:28` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/AssetPanelCommon.hpp:19` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/AssetPanelCommon.hpp:9` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetPanelModel.hpp:25` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetPanelModel.hpp:41` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/AssetReferenceField.hpp:11` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetReferenceIndex.hpp:35` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetStatusPanel.cpp:12` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/AssetStatusPanel.cpp:13` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/AssetStatusPanel.cpp:26` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/ConsoleBuffer.hpp:15` → `Arcane/Util/FunctionRef.hpp`
- `ArcaneEditor/src/Panels/ConsoleModel.cpp:6` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/ConsoleModel.hpp:7` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/Panels/CreateAssetDialog.cpp:10` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/CreateAssetDialog.cpp:24` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/CreateAssetDialog.hpp:29` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/CreateAssetDialog.hpp:42` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/DiagnosticStore.hpp:8` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:10` → `Arcane/Config/UiSettings.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:11` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:37` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:38` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:7` → `Arcane/Config/ConsoleModel.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:8` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.hpp:13` → `Arcane/Config/ConsoleModel.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.hpp:18` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.hpp:19` → `Arcane/Util/FunctionRef.hpp`
- `ArcaneEditor/src/Panels/InspectorFields.cpp:23` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/InspectorFields.hpp:7` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/InspectorHost.cpp:4` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/InspectorHost.hpp:27` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/InspectorView.cpp:13` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Panels/InspectorView.cpp:15` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/InspectorWindows.hpp:21` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Panels/LayoutLibrary.cpp:3` → `Arcane/Platform/Paths.hpp`
- `ArcaneEditor/src/Panels/LocatorRoute.hpp:8` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/Panels/LocatorRoute.hpp:9` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Panels/ProblemsPanel.cpp:8` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Panels/SeverityStyle.hpp:6` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/Panels/TextureMetaPanel.cpp:3` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Project/AssetFileOps.cpp:2` → `Arcane/Platform/Platform.hpp`
- `ArcaneEditor/src/Project/AssetFileOps.cpp:8` → `Arcane/Base/Assert.hpp`
- `ArcaneEditor/src/Project/AssetFileOps.cpp:9` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneEditor/src/Project/AssetFileOps.hpp:15` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Project/ClassTemplates.cpp:137` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Project/ClassTemplates.hpp:31` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Project/IdeLaunch.cpp:5` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Project/IdeLaunch.cpp:6` → `Arcane/Build/BuildToolSettings.hpp`
- `ArcaneEditor/src/Project/IdeLaunch.cpp:7` → `Arcane/Build/Toolchain.hpp`
- `ArcaneEditor/src/Project/IdeLaunch.cpp:8` → `Arcane/Platform/LaunchPath.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:8` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:9` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.hpp:81` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Project/MeshImportWave.cpp:14` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Project/MeshImportWave.cpp:4` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Project/ModuleBuild.cpp:3` → `Arcane/Platform/LaunchPath.hpp`
- `ArcaneEditor/src/Project/RecentProjects.cpp:4` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Project/RecentProjects.cpp:5` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Project/RecentProjects.cpp:6` → `Arcane/Platform/Paths.hpp`
- `ArcaneEditor/src/Project/RuntimeLaunch.cpp:3` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Project/RuntimeLaunch.cpp:4` → `Arcane/Platform/LaunchPath.hpp`
- `ArcaneEditor/src/Project/RuntimeLaunch.hpp:18` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Project/SceneRecents.cpp:4` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Project/SceneRecents.cpp:5` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Project/SceneRecents.cpp:6` → `Arcane/Platform/Paths.hpp`
- `ArcaneEditor/src/Project/SceneRecents.hpp:23` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Project/ServerLaunch.cpp:5` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Project/SourceIncludes.hpp:8` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Scene/EditGesture.hpp:16` → `Arcane/Util/FunctionRef.hpp`
- `ArcaneEditor/src/Scene/SceneSession.hpp:17` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Scene/SceneSession.hpp:21` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Settings/AssetBrowserSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/AssetBrowserSettings.cpp:4` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/AssetGraphSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/AssetGraphSettings.cpp:4` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/AssetGraphSettings.hpp:19` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/AssetsSpriteSettings.cpp:3` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/AssetsSpriteSettings.hpp:11` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/AxisColors.cpp:8` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/DocumentSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/DocumentSettings.cpp:4` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/DocumentSettings.hpp:17` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/EditorConsoleSettings.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/EditorConsoleSettings.cpp:7` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorConsoleSettings.cpp:8` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/EditorDocumentUiSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorDocumentUiSettings.cpp:4` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/EditorDocumentUiSettings.hpp:13` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/EditorGridSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorGridSettings.cpp:4` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/EditorGridSettings.hpp:13` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/EditorPerfSettings.hpp:8` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorPlaySettings.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/EditorPlaySettings.cpp:7` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorRestart.cpp:5` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Settings/EditorRestart.cpp:6` → `Arcane/Platform/Process.hpp`
- `ArcaneEditor/src/Settings/EditorSnapshotSettings.cpp:4` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/EditorSnapshotSettings.hpp:15` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorThemeSettings.cpp:5` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorThemeSettings.cpp:6` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/EditorThemeSettings.hpp:12` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/EditorThemeSettings.hpp:18` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Settings/EditorThumbnailSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorThumbnailSettings.cpp:4` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/EditorUiSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorUiSettings.cpp:4` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/EditorUiStyleSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorUiStyleSettings.cpp:4` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/EditorViewportSettings.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/EditorViewportSettings.cpp:7` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/EditorViewportSettings.hpp:15` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/EditorViewportSettings.hpp:17` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/FontsPage.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/FontsPage.cpp:7` → `Arcane/Platform/Paths.hpp`
- `ArcaneEditor/src/Settings/FontsPage.hpp:12` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/GraphCanvasSettings.cpp:4` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Settings/GraphCanvasSettings.cpp:5` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/GraphCanvasSettings.cpp:6` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/GraphCanvasSettings.cpp:7` → `Arcane/Util/CharConv.hpp`
- `ArcaneEditor/src/Settings/GraphCanvasSettings.hpp:20` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/GraphThemeSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/GraphThemeSettings.cpp:4` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/GraphThemeSettings.hpp:25` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/GraphThemeSettings.hpp:29` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Settings/InspectorSettings.cpp:3` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/InspectorSettings.cpp:4` → `Arcane/Reflection.hpp`
- `ArcaneEditor/src/Settings/LayoutPage.cpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/LayoutSettings.hpp:6` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneEditor/src/Settings/ProjectSettingsPage.hpp:9` → `Arcane/Guid.hpp`
- `ArcaneEditor/src/Settings/SettingsApply.hpp:13` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneEditor/src/Settings/SettingsApply.hpp:14` → `Arcane/Config/CVarRef.hpp`
- `ArcaneEditor/src/Settings/SettingsApply.hpp:15` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/SettingsEdit.cpp:5` → `Arcane/Config/CVarFormat.hpp`
- `ArcaneEditor/src/Settings/SettingsEdit.hpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/SettingsHost.cpp:6` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Settings/SettingsHost.cpp:7` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneEditor/src/Settings/SettingsHost.cpp:8` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneEditor/src/Settings/SettingsHost.cpp:9` → `Arcane/Platform/Paths.hpp`
- `ArcaneEditor/src/Settings/SettingsHost.hpp:14` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/SettingsModel.hpp:13` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/SettingsModel.hpp:14` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/SettingsRows.hpp:14` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/SettingsWindow.hpp:21` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/ShortcutsPage.cpp:8` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneEditor/src/Settings/ShortcutsPage.cpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/ShortcutsPage.hpp:11` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/Settings/ThemePage.cpp:11` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Settings/ThemePage.cpp:12` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Settings/ThemePresets.cpp:5` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneEditor/src/Viewport/EditorCamera.cpp:5` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Viewport/EditorCamera.cpp:6` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Viewport/ViewportGrid.cpp:15` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Viewport/ViewportGrid.cpp:8` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Viewport/ViewportGrid.hpp:48` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Viewport/ViewportSettings.cpp:5` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Viewport/ViewportSettings.hpp:40` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Widgets/EditorFonts.cpp:5` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/Widgets/EditorFonts.cpp:6` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Widgets/EditorTheme.hpp:54` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Widgets/EditorWidgets.cpp:11` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Widgets/EditorWidgets.cpp:29` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Widgets/EditorWidgets.hpp:17` → `Arcane/Util/FunctionRef.hpp`
- `ArcaneEditor/src/Widgets/GraphCanvasBackdrop.hpp:21` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Widgets/GraphCanvasStyle.hpp:35` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Widgets/GraphFit.cpp:17` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Widgets/GraphFit.cpp:20` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Widgets/GraphGridPhase.hpp:20` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Widgets/GraphNodeLod.hpp:26` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Widgets/GraphNodeLod.hpp:27` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Widgets/GraphWire.hpp:21` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Widgets/GraphWire.hpp:28` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Widgets/GraphZoomLevels.hpp:35` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/Widgets/UiMetrics.hpp:8` → `Arcane/Core/Constant.hpp`
- `ArcaneEditor/src/Widgets/UiScale.hpp:15` → `Arcane/Config/Settings.hpp`
- `ArcaneEditor/src/main.cpp:10` → `Arcane/Base/Log.hpp`
- `ArcaneEditor/src/main.cpp:11` → `Arcane/Platform/Platform.hpp`
- `ArcaneEditor/src/main.cpp:13` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneEditor/src/main.cpp:7` → `Arcane/Base/Assert.hpp`
- `ArcaneEditor/src/main.cpp:8` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneEditor/src/main.cpp:9` → `Arcane/Base/DiagnosticsSettings.hpp`

#### ArcaneEdit (31 sites)

- `ArcaneEditor/src/App/EditorApp.hpp:85` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:86` → `Arcane/Edit/Gizmo.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:55` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:56` → `Arcane/Edit/Gizmo.hpp`
- `ArcaneEditor/src/App/EditorAppScene.cpp:18` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneEditor/src/App/UndoSettings.hpp:7` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Documents/InputActionsEditorModel.cpp:5` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.cpp:22` → `Arcane/Edit/Command.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:46` → `Arcane/Edit/Command.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:47` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Documents/SpriteDocument.cpp:11` → `Arcane/Edit/Command.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:40` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.hpp:14` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.hpp:16` → `Arcane/Edit/Gizmo.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.hpp:17` → `Arcane/Edit/RegistryStateCommand.hpp`
- `ArcaneEditor/src/Panels/EntityList.cpp:3` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneEditor/src/Panels/InspectorView.cpp:14` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Panels/SceneInspectorSource.cpp:4` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneEditor/src/Project/AssetFileOps.cpp:10` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Project/AssetFileOps.hpp:13` → `Arcane/Edit/Command.hpp`
- `ArcaneEditor/src/Project/AssetFileOps.hpp:14` → `Arcane/Edit/UndoPayload.hpp`
- `ArcaneEditor/src/Scene/EditGesture.hpp:15` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Scene/SceneSession.hpp:16` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Scene/SelectionOps.hpp:10` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneEditor/src/Scene/UndoGate.hpp:7` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Settings/AxisColors.hpp:16` → `Arcane/Edit/Gizmo.hpp`
- `ArcaneEditor/src/Settings/EditorViewportSettings.hpp:16` → `Arcane/Edit/Gizmo.hpp`
- `ArcaneEditor/src/Settings/SettingsEdit.hpp:10` → `Arcane/Edit/Command.hpp`
- `ArcaneEditor/src/Settings/SettingsRows.hpp:15` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Settings/SettingsWindow.hpp:22` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneEditor/src/Viewport/GizmoOverlay.hpp:13` → `Arcane/Edit/Gizmo.hpp`

#### ArcaneEngine (39 sites)

- `ArcaneEditor/src/App/EditorApp.cpp:63` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:68` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:81` → `Arcane/Base/ProcessContext.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:82` → `Arcane/Base/Runtime.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:88` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneEditor/src/App/EditorAppAssetOps.cpp:16` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:62` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:46` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:48` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/App/EditorAppScene.cpp:19` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/App/PlayMode.cpp:4` → `Arcane/Base/Runtime.hpp`
- `ArcaneEditor/src/App/PlayMode.cpp:5` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneEditor/src/App/PlayMode.cpp:6` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneEditor/src/App/PlayMode.hpp:32` → `Arcane/Base/Runtime.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:44` → `Arcane/Base/Runtime.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:50` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/Panels/AssetFileOpDialogs.hpp:24` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/Panels/AssetInspectorSource.cpp:13` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/Panels/AssetPanelCommon.cpp:19` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/Panels/AssetPanelModel.cpp:3` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneEditor/src/Panels/AssetPanelModel.hpp:27` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneEditor/src/Panels/AssetReferenceField.cpp:12` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/Panels/CreateAssetDialog.cpp:11` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:39` → `Arcane/Base/Runtime.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:41` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/Project/AssetFileOps.hpp:16` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneEditor/src/Project/ClassTemplates.cpp:154` → `Arcane/Plugin/GameComponents.hpp`
- `ArcaneEditor/src/Project/ClassTemplates.cpp:224` → `Arcane/Plugin/GameSystems.hpp`
- `ArcaneEditor/src/Project/ClassTemplates.hpp:26` → `Arcane/Plugin/SystemFactory.hpp`
- `ArcaneEditor/src/Project/ContentDiscovery.cpp:5` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneEditor/src/Project/ContentDiscovery.cpp:6` → `Arcane/Project/MountTable.hpp`
- `ArcaneEditor/src/Project/EditorRecents.cpp:3` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneEditor/src/Project/EditorRecents.cpp:4` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/Settings/AssetBrowserSettings.hpp:15` → `Arcane/Project/ProjectOpenOptions.hpp`
- `ArcaneEditor/src/Settings/EditorSnapshotSettings.cpp:3` → `Arcane/Base/Runtime.hpp`
- `ArcaneEditor/src/Settings/ProjectSettingsPage.cpp:8` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneEditor/src/Settings/ProjectSettingsPage.cpp:9` → `Arcane/Project/Project.hpp`
- `ArcaneEditor/src/Settings/SettingsModel.cpp:4` → `Arcane/Project/ProjectManifest.hpp`
- `ArcaneEditor/src/main.cpp:12` → `Arcane/Project/Project.hpp`

#### ArcaneImGui (3 sites)

- `ArcaneEditor/src/App/EditorApp.hpp:87` → `Arcane/ImGui/OffscreenImGuiLayer.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:12` → `Arcane/ImGui/ConsoleInputLine.hpp`
- `ArcaneEditor/src/main.cpp:18` → `Arcane/ImGui/ImGuiHostHooks.hpp`

#### ArcaneInput (18 sites)

- `ArcaneEditor/src/App/EditorApp.cpp:34` → `Arcane/Input/KeyLayout.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:52` → `Arcane/Input/InputActions.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:84` → `Arcane/Input/InputActionAsset.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:47` → `Arcane/Input/InputActionAsset.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:60` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocument.hpp:10` → `Arcane/Input/InputRebindOperation.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocument.hpp:11` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp:13` → `Arcane/Input/InputActions.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp:14` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneEditor/src/Documents/InputActionsEditorModel.cpp:6` → `Arcane/Input/InputActions.hpp`
- `ArcaneEditor/src/Documents/InputActionsEditorModel.hpp:6` → `Arcane/Input/InputActionAsset.hpp`
- `ArcaneEditor/src/Documents/InputActionsInspectorPage.cpp:10` → `Arcane/Input/InputSettings.hpp`
- `ArcaneEditor/src/Documents/InputActionsInspectorPage.cpp:9` → `Arcane/Input/InputActions.hpp`
- `ArcaneEditor/src/Documents/InputActionsRows.cpp:5` → `Arcane/Input/InputActions.hpp`
- `ArcaneEditor/src/Documents/InputActionsRows.cpp:6` → `Arcane/Input/InputSettings.hpp`
- `ArcaneEditor/src/Input/EditorActions.cpp:8` → `Arcane/Input/KeyLayout.hpp`
- `ArcaneEditor/src/Input/EditorActions.hpp:11` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneEditor/src/Settings/ShortcutsPage.cpp:10` → `Arcane/Input/KeyLayout.hpp`

#### ArcanePhysics2D (1 sites)

- `ArcaneEditor/src/App/EditorAppFrame.cpp:69` → `Arcane/Scene/PhysicsSystem.hpp`

#### ArcaneRender (42 sites)

- `ArcaneEditor/src/App/EditorApp.cpp:69` → `Arcane/Render/GraphicsBackend.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:70` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:71` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:72` → `Arcane/Render/Nri/NriCommon.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:90` → `Arcane/Render/GpuFaultInjector.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:91` → `Arcane/Render/GpuSceneTypes.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:92` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:93` → `Arcane/Render/PickEmit.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:94` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:95` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:63` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:64` → `Arcane/Render/Nri/nodes/PickOutlineNodes.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:65` → `Arcane/Render/PhysicsDebugDraw.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:66` → `Arcane/Render/RenderLookSettings.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:67` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneEditor/src/App/EditorAppProject.cpp:54` → `Arcane/Render/Nri/NriDiagnostics.hpp`
- `ArcaneEditor/src/Documents/CrashReportDocument.cpp:18` → `Arcane/Render/IGpuCrashBackend.hpp`
- `ArcaneEditor/src/Documents/MaterialSpherePreview.hpp:24` → `Arcane/Render/Nri/nodes/MeshNode.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.cpp:16` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.cpp:17` → `Arcane/Render/Nri/nodes/MeshNode.hpp`
- `ArcaneEditor/src/Documents/MeshDocument.cpp:18` → `Arcane/Render/Nri/RenderOverlayNode.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:36` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:37` → `Arcane/Render/Nri/RenderOverlayNode.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:52` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:53` → `Arcane/Render/ShaderConventions.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:48` → `Arcane/Render/GraphicsBackend.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:53` → `Arcane/Render/PostChainCache.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:54` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:55` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:16` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:18` → `Arcane/Render/MeshMaterialCache.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:19` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:20` → `Arcane/Render/PostChainCache.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:21` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:22` → `Arcane/Render/ShaderConventions.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:23` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.hpp:83` → `Arcane/Render/GraphicsBackend.hpp`
- `ArcaneEditor/src/Project/RuntimeLaunch.hpp:19` → `Arcane/Render/GraphicsBackend.hpp`
- `ArcaneEditor/src/Settings/AxisColors.cpp:9` → `Arcane/Render/Nri/nodes/GridNode.hpp`
- `ArcaneEditor/src/Settings/EditorGridSettings.hpp:14` → `Arcane/Render/Nri/nodes/GridNode.hpp`
- `ArcaneEditor/src/Viewport/ViewportGrid.cpp:9` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneEditor/src/main.cpp:19` → `Arcane/Render/AgilitySdk.hpp`

#### ArcaneScene (22 sites)

- `ArcaneEditor/src/App/EditorApp.cpp:74` → `Arcane/Scene/Components.hpp`
- `ArcaneEditor/src/App/EditorAppAssetOps.cpp:17` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:68` → `Arcane/Scene/Components.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:73` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:74` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneEditor/src/App/EditorAppScene.cpp:20` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneEditor/src/App/EditorAppScene.cpp:21` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneEditor/src/App/PlayMode.cpp:7` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneEditor/src/Documents/MaterialSpherePreview.hpp:25` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:42` → `Arcane/Scene/Components.hpp`
- `ArcaneEditor/src/Panels/EntityList.cpp:4` → `Arcane/Scene/Components.hpp`
- `ArcaneEditor/src/Project/ClassTemplates.cpp:178` → `Arcane/Ecs.hpp`
- `ArcaneEditor/src/Project/ClassTemplates.cpp:179` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneEditor/src/Project/ClassTemplates.cpp:208` → `Arcane/Ecs.hpp`
- `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:24` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneEditor/src/Scene/EditModeSchedule.cpp:3` → `Arcane/Scene/BoundsSystem.hpp`
- `ArcaneEditor/src/Scene/EditModeSchedule.cpp:4` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneEditor/src/Scene/SelectionOps.hpp:11` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneEditor/src/Viewport/EditorCamera.cpp:7` → `Arcane/Scene/Components.hpp`
- `ArcaneEditor/src/Viewport/EditorCamera.cpp:8` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneEditor/src/Viewport/EditorCamera.hpp:53` → `Arcane/Scene/ViewTransform.hpp`
- `ArcaneEditor/src/Viewport/ViewportGrid.hpp:49` → `Arcane/Scene/ViewTransform.hpp`

#### ArcaneSim (4 sites)

- `ArcaneEditor/src/App/EditorAppFrame.cpp:70` → `Arcane/Sim/SimSettings.hpp`
- `ArcaneEditor/src/App/PlayMode.cpp:8` → `Arcane/Sim/SimSettings.hpp`
- `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp:15` → `Arcane/Sim/SimSettings.hpp`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:43` → `Arcane/Sim/RunLoop.hpp`

#### ArcaneVerify (7 sites)

- `ArcaneEditor/src/App/EditorApp.cpp:44` → `Arcane/Host/ReferenceImages.hpp`
- `ArcaneEditor/src/App/EditorApp.cpp:45` → `Arcane/Host/VerifyReport.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:24` → `Arcane/Host/OffscreenVehicle.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:26` → `Arcane/Host/ReferenceImages.hpp`
- `ArcaneEditor/src/App/EditorApp.hpp:27` → `Arcane/Host/VerifyReport.hpp`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:59` → `Arcane/Host/ReferenceImages.hpp`
- `ArcaneEditor/src/main.cpp:17` → `Arcane/Host/VerifyHooks.hpp`

#### ImGui (76 sites)

- `ArcaneEditor/src/App/EditorApp.cpp:81` → `imgui.h`
- `ArcaneEditor/src/App/EditorApp.cpp:86` → `imgui_internal.h`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:83` → `imgui.h`
- `ArcaneEditor/src/App/EditorAppFrame.cpp:84` → `imgui_internal.h`
- `ArcaneEditor/src/Documents/CrashReportDocument.cpp:20` → `imgui.h`
- `ArcaneEditor/src/Documents/DocumentHost.cpp:5` → `imgui.h`
- `ArcaneEditor/src/Documents/DocumentPageSelection.hpp:10` → `imgui.h`
- `ArcaneEditor/src/Documents/DocumentPageSelection.hpp:11` → `imgui_internal.h`
- `ArcaneEditor/src/Documents/InputActionsDocument.cpp:13` → `imgui.h`
- `ArcaneEditor/src/Documents/InputActionsDocument.cpp:14` → `imgui_internal.h`
- `ArcaneEditor/src/Documents/InputActionsDocument.hpp:13` → `imgui.h`
- `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp:14` → `imgui.h`
- `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp:16` → `imgui.h`
- `ArcaneEditor/src/Documents/InputActionsInspectorPage.cpp:12` → `imgui.h`
- `ArcaneEditor/src/Documents/MeshDocument.cpp:30` → `imgui.h`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:57` → `imgui.h`
- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:61` → `imgui_internal.h`
- `ArcaneEditor/src/Documents/ShaderGraphCategoryColors.hpp:14` → `imgui.h`
- `ArcaneEditor/src/Documents/ShaderGraphPinLegend.hpp:26` → `imgui.h`
- `ArcaneEditor/src/Documents/ShaderGraphPinTypes.hpp:29` → `imgui.h`
- `ArcaneEditor/src/Documents/SpriteDocument.cpp:15` → `imgui.h`
- `ArcaneEditor/src/Input/EditorActions.cpp:10` → `imgui.h`
- `ArcaneEditor/src/Input/EditorActions.cpp:11` → `imgui_internal.h`
- `ArcaneEditor/src/Panels/AssetBrowserPanel.cpp:17` → `imgui.h`
- `ArcaneEditor/src/Panels/AssetBrowserPanel.cpp:18` → `imgui_internal.h`
- `ArcaneEditor/src/Panels/AssetFileOpDialogs.cpp:7` → `imgui.h`
- `ArcaneEditor/src/Panels/AssetGraphPanel.cpp:30` → `imgui.h`
- `ArcaneEditor/src/Panels/AssetGraphPanel.hpp:30` → `imgui.h`
- `ArcaneEditor/src/Panels/AssetInspectorSource.cpp:14` → `imgui.h`
- `ArcaneEditor/src/Panels/AssetPanelCommon.cpp:21` → `imgui.h`
- `ArcaneEditor/src/Panels/AssetPanelCommon.hpp:11` → `imgui.h`
- `ArcaneEditor/src/Panels/AssetReferenceField.cpp:14` → `imgui.h`
- `ArcaneEditor/src/Panels/AssetReferenceField.cpp:15` → `imgui_internal.h`
- `ArcaneEditor/src/Panels/AssetStatusPanel.cpp:15` → `imgui.h`
- `ArcaneEditor/src/Panels/CreateAssetDialog.cpp:13` → `imgui.h`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:47` → `imgui.h`
- `ArcaneEditor/src/Panels/EditorPanels.cpp:48` → `imgui_internal.h`
- `ArcaneEditor/src/Panels/EditorPanels.hpp:15` → `imgui.h`
- `ArcaneEditor/src/Panels/InspectorView.cpp:20` → `imgui.h`
- `ArcaneEditor/src/Panels/InspectorView.cpp:21` → `imgui_internal.h`
- `ArcaneEditor/src/Panels/InspectorWindows.cpp:10` → `imgui.h`
- `ArcaneEditor/src/Panels/InspectorWindows.cpp:11` → `imgui_internal.h`
- `ArcaneEditor/src/Panels/ProblemsPanel.cpp:9` → `imgui.h`
- `ArcaneEditor/src/Panels/TextureImportSettings.cpp:9` → `imgui.h`
- `ArcaneEditor/src/Scene/EditGesture.cpp:40` → `imgui.h`
- `ArcaneEditor/src/Scene/EditGesture.cpp:41` → `imgui_internal.h`
- `ArcaneEditor/src/Settings/AppearanceApplier.hpp:13` → `imgui.h`
- `ArcaneEditor/src/Settings/AxisColors.hpp:19` → `imgui.h`
- `ArcaneEditor/src/Settings/FontsPage.cpp:9` → `imgui.h`
- `ArcaneEditor/src/Settings/LayoutPage.cpp:11` → `imgui.h`
- `ArcaneEditor/src/Settings/ProjectSettingsPage.cpp:11` → `imgui.h`
- `ArcaneEditor/src/Settings/SettingsRows.cpp:11` → `imgui.h`
- `ArcaneEditor/src/Settings/SettingsRows.cpp:12` → `imgui_internal.h`
- `ArcaneEditor/src/Settings/SettingsWindow.cpp:8` → `imgui.h`
- `ArcaneEditor/src/Settings/SettingsWindow.cpp:9` → `imgui_internal.h`
- `ArcaneEditor/src/Settings/ShortcutsPage.cpp:12` → `imgui.h`
- `ArcaneEditor/src/Settings/ThemePage.hpp:8` → `imgui.h`
- `ArcaneEditor/src/Viewport/GizmoOverlay.hpp:15` → `imgui.h`
- `ArcaneEditor/src/Viewport/ViewportSettings.cpp:7` → `imgui.h`
- `ArcaneEditor/src/Widgets/CanvasEditScope.hpp:34` → `imgui.h`
- `ArcaneEditor/src/Widgets/ColorPickerPopup.cpp:6` → `imgui_internal.h`
- `ArcaneEditor/src/Widgets/ColorPickerPopup.hpp:36` → `imgui.h`
- `ArcaneEditor/src/Widgets/EditorFonts.cpp:8` → `imgui.h`
- `ArcaneEditor/src/Widgets/EditorTheme.hpp:52` → `imgui.h`
- `ArcaneEditor/src/Widgets/EditorWidgets.cpp:13` → `imgui.h`
- `ArcaneEditor/src/Widgets/EditorWidgets.cpp:14` → `imgui_internal.h`
- `ArcaneEditor/src/Widgets/EditorWidgets.hpp:21` → `imgui.h`
- `ArcaneEditor/src/Widgets/GraphCanvasBackdrop.hpp:23` → `imgui.h`
- `ArcaneEditor/src/Widgets/GraphCanvasStyle.hpp:37` → `imgui.h`
- `ArcaneEditor/src/Widgets/GraphFit.hpp:17` → `imgui.h`
- `ArcaneEditor/src/Widgets/GraphGridPhase.hpp:13` → `imgui.h`
- `ArcaneEditor/src/Widgets/GraphLegend.hpp:22` → `imgui.h`
- `ArcaneEditor/src/Widgets/GraphPinDot.hpp:23` → `imgui.h`
- `ArcaneEditor/src/Widgets/GraphWire.hpp:23` → `imgui.h`
- `ArcaneEditor/src/Widgets/PropertyGrid.cpp:7` → `imgui_internal.h`
- `ArcaneEditor/src/Widgets/PropertyGrid.hpp:50` → `imgui.h`

#### ImGuiNodeEditor (9 sites)

- `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:62` → `imgui_node_editor.h`
- `ArcaneEditor/src/Panels/AssetGraphPanel.cpp:34` → `imgui_node_editor.h`
- `ArcaneEditor/src/Widgets/CanvasEditScope.hpp:35` → `imgui_node_editor.h`
- `ArcaneEditor/src/Widgets/CanvasPopupScope.hpp:21` → `imgui_node_editor.h`
- `ArcaneEditor/src/Widgets/GraphCanvasBackdrop.hpp:24` → `imgui_node_editor.h`
- `ArcaneEditor/src/Widgets/GraphCanvasStyle.hpp:38` → `imgui_node_editor.h`
- `ArcaneEditor/src/Widgets/GraphFit.cpp:10` → `imgui_node_editor_internal.h`
- `ArcaneEditor/src/Widgets/GraphWire.hpp:24` → `imgui_node_editor.h`
- `ArcaneEditor/src/Widgets/GraphZoomLevels.hpp:37` → `imgui_node_editor.h`

### ArcaneRuntime/src

#### ArcaneAssets (8 sites)

- `ArcaneRuntime/src/RuntimeApp.cpp:15` → `Arcane/Assets/Assets.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:16` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:24` → `Arcane/Assets/AssetId.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:12` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:26` → `Arcane/Material/GlobalParams.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:7` → `Arcane/Assets/Assets.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:33` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:40` → `Arcane/Material/GlobalParams.hpp`

#### ArcaneAudio (1 sites)

- `ArcaneRuntime/src/RuntimeFrame.cpp:8` → `Arcane/Audio/AudioDevice.hpp`

#### ArcaneClient (24 sites)

- `ArcaneRuntime/src/RuntimeApp.cpp:10` → `Arcane/Host/GpuSceneHost.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:11` → `Arcane/Host/ProjectBoot.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:13` → `Arcane/Host/HostConfig.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:14` → `Arcane/Host/GpuContext.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:17` → `Arcane/Host/FramePerf.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:18` → `Arcane/Host/SceneRenderResolver.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:19` → `Arcane/Host/SettleBound.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:20` → `Arcane/Host/BootSequence.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:21` → `Arcane/Host/BootSplashWindow.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:22` → `Arcane/Host/ProjectBoot.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:25` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:17` → `Arcane/Host/HostSettings.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:18` → `Arcane/Host/RuntimeSettings.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:19` → `Arcane/Host/GpuSceneHost.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:34` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:35` → `Arcane/Host/FramePerf.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:36` → `Arcane/Host/GpuContext.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:37` → `Arcane/Host/HostConfig.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:38` → `Arcane/Host/SceneRenderResolver.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:39` → `Arcane/Host/SettleBound.hpp`
- `ArcaneRuntime/src/main.cpp:12` → `Arcane/Host/BootSplashWindow.hpp`
- `ArcaneRuntime/src/main.cpp:13` → `Arcane/Host/EarlyConfig.hpp`
- `ArcaneRuntime/src/main.cpp:14` → `Arcane/Host/HostConfig.hpp`
- `ArcaneRuntime/src/main.cpp:19` → `Arcane/Host/ProjectBoot.hpp`

#### ArcaneCore (21 sites)

- `ArcaneRuntime/src/RuntimeApp.cpp:12` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:17` → `Arcane/Base/Assert.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:18` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:19` → `Arcane/Base/Engine.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:20` → `Arcane/Base/ForeignModules.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:21` → `Arcane/Base/Log.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:22` → `Arcane/Guid.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:23` → `Arcane/Platform/Paths.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:10` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:11` → `Arcane/Config/ConsoleModel.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:12` → `Arcane/Config/ConsoleSettings.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:13` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:14` → `Arcane/Config/PlayerSettings.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:16` → `Arcane/Base/Log.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:9` → `Arcane/Base/Assert.hpp`
- `ArcaneRuntime/src/main.cpp:10` → `Arcane/Platform/Platform.hpp`
- `ArcaneRuntime/src/main.cpp:11` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneRuntime/src/main.cpp:6` → `Arcane/Base/Assert.hpp`
- `ArcaneRuntime/src/main.cpp:7` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneRuntime/src/main.cpp:8` → `Arcane/Base/DiagnosticsSettings.hpp`
- `ArcaneRuntime/src/main.cpp:9` → `Arcane/Base/Log.hpp`

#### ArcaneEngine (5 sites)

- `ArcaneRuntime/src/RuntimeApp.cpp:25` → `Arcane/Project/Project.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:23` → `Arcane/Base/ProcessContext.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:24` → `Arcane/Base/Runtime.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:27` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:41` → `Arcane/Plugin/PluginHost.hpp`

#### ArcaneImGui (2 sites)

- `ArcaneRuntime/src/RuntimeFrame.cpp:15` → `Arcane/ImGui/ConsoleInputLine.hpp`
- `ArcaneRuntime/src/main.cpp:16` → `Arcane/ImGui/ImGuiHostHooks.hpp`

#### ArcaneInput (2 sites)

- `ArcaneRuntime/src/RuntimeFrame.cpp:21` → `Arcane/Input/InputActions.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:22` → `Arcane/Input/InputSnapshot.hpp`

#### ArcaneRender (21 sites)

- `ArcaneRuntime/src/RuntimeApp.cpp:26` → `Arcane/Render/GraphicsBackend.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:27` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:28` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:29` → `Arcane/Render/PickEmit.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:28` → `Arcane/Render/GpuFaultInjector.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:29` → `Arcane/Render/GpuSceneTypes.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:30` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:31` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:32` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:23` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:24` → `Arcane/Render/GraphicsBackend.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:25` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:26` → `Arcane/Render/Nri/NriDiagnostics.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:27` → `Arcane/Render/PickEmit.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:28` → `Arcane/Render/RenderLookSettings.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:42` → `Arcane/Render/GpuFaultInjector.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:43` → `Arcane/Render/GpuSceneTypes.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:44` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:45` → `Arcane/Render/PickEmit.hpp`
- `ArcaneRuntime/src/RuntimeFrame.hpp:46` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneRuntime/src/main.cpp:17` → `Arcane/Render/AgilitySdk.hpp`

#### ArcaneScene (3 sites)

- `ArcaneRuntime/src/RuntimeApp.cpp:30` → `Arcane/Scene/Components.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:31` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:29` → `Arcane/Scene/SceneCamera.hpp`

#### ArcaneSim (1 sites)

- `ArcaneRuntime/src/RuntimeFrame.cpp:30` → `Arcane/Sim/SimSettings.hpp`

#### ArcaneVerify (6 sites)

- `ArcaneRuntime/src/RuntimeApp.cpp:13` → `Arcane/Host/VerifyReport.hpp`
- `ArcaneRuntime/src/RuntimeApp.cpp:14` → `Arcane/Host/ReferenceImages.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:15` → `Arcane/Host/ReferenceImages.hpp`
- `ArcaneRuntime/src/RuntimeApp.hpp:16` → `Arcane/Host/OffscreenVehicle.hpp`
- `ArcaneRuntime/src/RuntimeFrame.cpp:20` → `Arcane/Host/VerifyReport.hpp`
- `ArcaneRuntime/src/main.cpp:15` → `Arcane/Host/VerifyHooks.hpp`

#### ImGui (2 sites)

- `ArcaneRuntime/src/RuntimeApp.cpp:42` → `imgui.h`
- `ArcaneRuntime/src/RuntimeFrame.cpp:32` → `imgui.h`

### ArcaneServer/src

#### ArcaneCore (13 sites)

- `ArcaneServer/src/ServerAdminConsole.cpp:3` → `Arcane/Base/Log.hpp`
- `ArcaneServer/src/ServerAdminConsole.hpp:10` → `Arcane/Config/RemoteCVarService.hpp`
- `ArcaneServer/src/ServerAdminConsole.hpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneServer/src/ServerApp.cpp:3` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneServer/src/ServerApp.cpp:4` → `Arcane/Base/Log.hpp`
- `ArcaneServer/src/ServerApp.cpp:5` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneServer/src/ServerApp.hpp:22` → `Arcane/Config/RemoteCVarService.hpp`
- `ArcaneServer/src/ServerConfig.cpp:3` → `Arcane/Cli/Cli.hpp`
- `ArcaneServer/src/ServerReport.hpp:18` → `Arcane/Core/Constant.hpp`
- `ArcaneServer/src/main.cpp:12` → `Arcane/Base/Assert.hpp`
- `ArcaneServer/src/main.cpp:13` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneServer/src/main.cpp:14` → `Arcane/Base/Engine.hpp`
- `ArcaneServer/src/main.cpp:15` → `Arcane/Base/Log.hpp`

#### ArcaneEngine (7 sites)

- `ArcaneServer/src/ServerApp.cpp:6` → `Arcane/Plugin/SystemFactory.hpp`
- `ArcaneServer/src/ServerApp.cpp:7` → `Arcane/Project/Project.hpp`
- `ArcaneServer/src/ServerApp.cpp:8` → `Arcane/Project/ProjectHost.hpp`
- `ArcaneServer/src/ServerApp.hpp:20` → `Arcane/Base/ProcessContext.hpp`
- `ArcaneServer/src/ServerApp.hpp:21` → `Arcane/Base/Runtime.hpp`
- `ArcaneServer/src/ServerApp.hpp:23` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneServer/src/main.cpp:16` → `Arcane/Plugin/PluginABI.hpp`

#### ArcanePhysics2D (1 sites)

- `ArcaneServer/src/ServerApp.cpp:9` → `Arcane/Scene/PhysicsSystem.hpp`

#### ArcaneScene (1 sites)

- `ArcaneServer/src/ServerApp.cpp:10` → `Arcane/Scene/TransformSystems.hpp`

#### ArcaneSim (1 sites)

- `ArcaneServer/src/ServerConfig.cpp:4` → `Arcane/Sim/SimSettings.hpp`

### ArcaneTests/src

#### ArcaneAssets (123 sites)

- `ArcaneTests/src/ArtifactReaderTest.cpp:23` → `Arcane/Assets/ArtifactReader.hpp`
- `ArcaneTests/src/ArtifactSizeContractTest.cpp:13` → `Arcane/Assets/ArtifactReader.hpp`
- `ArcaneTests/src/AssetCacheTest.cpp:10` → `Arcane/Assets/AssetCache.hpp`
- `ArcaneTests/src/AssetFileOpsTest.cpp:16` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/AssetFileOpsTest.cpp:17` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneTests/src/AssetFileOpsTest.cpp:18` → `Arcane/Sprite/SpriteAsset.hpp`
- `ArcaneTests/src/AssetIdTest.cpp:7` → `Arcane/Assets/AssetId.hpp`
- `ArcaneTests/src/AssetReferencesTest.cpp:12` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/AssetReferencesTest.cpp:13` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneTests/src/AssetRegistryTest.cpp:12` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/AssetStatusPanelClickTest.cpp:85` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/AssetsGraphCanvasTest.cpp:40` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/AssetsPixelsTest.cpp:23` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/AssetsPixelsTest.cpp:24` → `Arcane/Assets/ImageIo.hpp`
- `ArcaneTests/src/AssetsPixelsTest.cpp:25` → `Arcane/Assets/AssetId.hpp`
- `ArcaneTests/src/AssetsTest.cpp:33` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/AudioDeviceTest.cpp:2` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/BoundsSystemTest.cpp:12` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/ContentDiscoveryTest.cpp:41` → `Arcane/Assets/AssetId.hpp`
- `ArcaneTests/src/DisplayImageIoTest.cpp:18` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/DisplayImageIoTest.cpp:19` → `Arcane/Assets/ImageIo.hpp`
- `ArcaneTests/src/EditorCameraTest.cpp:30` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/EditorDocumentHostTest.cpp:11` → `Arcane/Sprite/SpriteAsset.hpp`
- `ArcaneTests/src/EditorDocumentLiveRefsTest.cpp:12` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/EditorDocumentLiveRefsTest.cpp:13` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/EditorDocumentLiveRefsTest.cpp:14` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneTests/src/EditorDocumentLiveRefsTest.cpp:15` → `Arcane/Sprite/SpriteAsset.hpp`
- `ArcaneTests/src/EditorWitnessTest.cpp:13` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/EditorWitnessTest.cpp:14` → `Arcane/Material/MaterialGraph.hpp`
- `ArcaneTests/src/ExternalBufferPathTest.cpp:10` → `Arcane/Assets/ArtifactReader.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:11` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:7` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:8` → `Arcane/Assets/ImageIo.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:46` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:18` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/GraphCanvasHeadlessTest.cpp:21` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/GraphCanvasHeadlessTest.cpp:22` → `Arcane/Material/MaterialGraph.hpp`
- `ArcaneTests/src/GraphFitTest.cpp:26` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/GraphFitTest.cpp:27` → `Arcane/Material/MaterialGraph.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:32` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/Helpers/NodePageDocs.hpp:10` → `Arcane/Material/MaterialGraph.hpp`
- `ArcaneTests/src/Helpers/NodePageDocs.hpp:9` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/HostBootTest.cpp:12` → `Arcane/Assets/AssetId.hpp`
- `ArcaneTests/src/HostBootTest.cpp:15` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneTests/src/HostBootTest.cpp:23` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/HostBootTest.cpp:24` → `Arcane/Material/MaterialInstance.hpp`
- `ArcaneTests/src/HostBootTest.cpp:25` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneTests/src/HostBootTest.cpp:26` → `Arcane/Material/MaterialTemplate.hpp`
- `ArcaneTests/src/HostBootTest.cpp:5` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/ImageCompareCascadeTest.cpp:11` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneTests/src/ImageCompareChannelTest.cpp:8` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneTests/src/ImageCompareColorTest.cpp:10` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneTests/src/ImageCompareConformanceTest.cpp:14` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneTests/src/ImageCompareConformanceTest.cpp:15` → `Arcane/Assets/ImageIo.hpp`
- `ArcaneTests/src/ImageCompareStatsTest.cpp:12` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneTests/src/MaterialAssetTest.cpp:7` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/MaterialAssetTest.cpp:8` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneTests/src/MaterialGraphTest.cpp:10` → `Arcane/Material/MaterialGraph.hpp`
- `ArcaneTests/src/MaterialGraphTest.cpp:11` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneTests/src/MaterialGraphTest.cpp:9` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/MaterialSourceTest.cpp:7` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneTests/src/MaterialTest.cpp:10` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/MaterialTest.cpp:11` → `Arcane/Material/MaterialInstance.hpp`
- `ArcaneTests/src/MaterialTest.cpp:12` → `Arcane/Material/MaterialTemplate.hpp`
- `ArcaneTests/src/MaterialTest.cpp:13` → `Arcane/Material/MaterialTypes.hpp`
- `ArcaneTests/src/MaterialTest.cpp:9` → `Arcane/Material/GlobalParams.hpp`
- `ArcaneTests/src/MeshArtifactReaderTest.cpp:21` → `Arcane/Assets/ArtifactReader.hpp`
- `ArcaneTests/src/MeshAssetTest.cpp:4` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneTests/src/MeshAssetTest.cpp:5` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/MeshBuilderTest.cpp:27` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/MeshCullNodeTest.cpp:7` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:36` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:28` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:29` → `Arcane/Assets/ImageIo.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:32` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/MeshResolveTest.cpp:18` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneTests/src/MeshSubmissionTest.cpp:23` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/MeshSubmissionTest.cpp:24` → `Arcane/Material/MaterialTypes.hpp`
- `ArcaneTests/src/MeshSubmissionTest.cpp:25` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneTests/src/MeshSubmissionTest.cpp:26` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:60` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:66` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:67` → `Arcane/Assets/ImageCompare.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:68` → `Arcane/Assets/ImageIo.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:71` → `Arcane/Assets/AssetId.hpp`
- `ArcaneTests/src/NodePageFixtureTest.cpp:10` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneTests/src/NodePageFixtureTest.cpp:8` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/NodePageFixtureTest.cpp:9` → `Arcane/Material/MaterialGraph.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:105` → `Arcane/Assets/ArtifactReader.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:75` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/NriMeshBufferCacheTest.cpp:20` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:55` → `Arcane/Assets/ArtifactReader.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:57` → `Arcane/Assets/ImageIo.hpp`
- `ArcaneTests/src/NriTextureCacheTest.cpp:20` → `Arcane/Assets/ImageIo.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:43` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/PostChainCacheTest.cpp:15` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/PostChainCacheTest.cpp:16` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneTests/src/ReferenceImagesTest.cpp:6` → `Arcane/Assets/ImageIo.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:38` → `Arcane/Mesh/MeshBuilder.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:43` → `Arcane/Material/MaterialSource.hpp`
- `ArcaneTests/src/RuntimeProjectTest.cpp:8` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:8` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:28` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:31` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:32` → `Arcane/Material/MaterialTypes.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:33` → `Arcane/Mesh/MeshAsset.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:34` → `Arcane/Assets/AssetId.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:39` → `Arcane/Sprite/SpriteAsset.hpp`
- `ArcaneTests/src/SeveranceTest.cpp:30` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/SeveranceTest.cpp:31` → `Arcane/Material/MaterialInstance.hpp`
- `ArcaneTests/src/SeveranceTest.cpp:32` → `Arcane/Material/MaterialTemplate.hpp`
- `ArcaneTests/src/SeveranceTest.cpp:33` → `Arcane/Material/MaterialTypes.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:29` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:30` → `Arcane/Material/MaterialGraph.hpp`
- `ArcaneTests/src/SpriteAssetTest.cpp:3` → `Arcane/Sprite/SpriteAsset.hpp`
- `ArcaneTests/src/SpriteDocumentUndoTest.cpp:29` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/SpriteDocumentUndoTest.cpp:34` → `Arcane/Assets/AssetId.hpp`
- `ArcaneTests/src/SpriteDocumentUndoTest.cpp:35` → `Arcane/Sprite/SpriteAsset.hpp`
- `ArcaneTests/src/SpriteMaterialCacheTest.cpp:26` → `Arcane/Material/MaterialAsset.hpp`
- `ArcaneTests/src/SweepAssetsTest.cpp:11` → `Arcane/Assets/Assets.hpp`
- `ArcaneTests/src/SweepAssetsTest.cpp:12` → `Arcane/Assets/AssetsSettings.hpp`
- `ArcaneTests/src/SweepAssetsTest.cpp:15` → `Arcane/Sprite/SpriteAsset.hpp`
- `ArcaneTests/src/VisibilityTest.cpp:11` → `Arcane/Mesh/MeshBuilder.hpp`

#### ArcaneAudio (4 sites)

- `ArcaneTests/src/AudioDeviceTest.cpp:1` → `Arcane/Audio/AudioDevice.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:9` → `Arcane/Audio/AudioDevice.hpp`
- `ArcaneTests/src/SweepAudioTest.cpp:3` → `Arcane/Audio/AudioSettings.hpp`
- `ArcaneTests/src/SweepAudioTest.cpp:4` → `Arcane/Audio/AudioTypes.hpp`

#### ArcaneClient (61 sites)

- `ArcaneTests/src/BootSequenceTest.cpp:13` → `Arcane/Host/BootSequence.hpp`
- `ArcaneTests/src/BootSplashPresenterTest.cpp:20` → `Arcane/Host/BootSequence.hpp`
- `ArcaneTests/src/BootSplashPresenterTest.cpp:21` → `Arcane/Host/BootSplashWindow.hpp`
- `ArcaneTests/src/BootStageParityTest.cpp:22` → `Arcane/Host/BootSequence.hpp`
- `ArcaneTests/src/BootStageParityTest.cpp:23` → `Arcane/Host/BootSplashWindow.hpp`
- `ArcaneTests/src/BootStageParityTest.cpp:24` → `Arcane/Host/EarlyConfig.hpp`
- `ArcaneTests/src/BootStageParityTest.cpp:25` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/BootStageParityTest.cpp:26` → `Arcane/Host/ProjectBoot.hpp`
- `ArcaneTests/src/ClientRuntimeTest.cpp:12` → `Arcane/Host/ProjectBoot.hpp`
- `ArcaneTests/src/ClientRuntimeTest.cpp:9` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/ExclusionExpiryTest.cpp:8` → `Arcane/Host/ExclusionList.hpp`
- `ArcaneTests/src/ExclusionListTest.cpp:6` → `Arcane/Host/ExclusionList.hpp`
- `ArcaneTests/src/GameInputTest.cpp:8` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:9` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:44` → `Arcane/Host/GpuSceneHost.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:45` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:8` → `Arcane/Host/GpuSceneHost.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:31` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/HostBootTest.cpp:1` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/HostBootTest.cpp:2` → `Arcane/Host/ProjectBoot.hpp`
- `ArcaneTests/src/HostBootTest.cpp:3` → `Arcane/Host/SceneRenderResolver.hpp`
- `ArcaneTests/src/HostBootTest.cpp:8` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/HostConfigTest.cpp:9` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/HostPresentationTest.cpp:10` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/HostSliceTest.cpp:10` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:30` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:33` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:35` → `Arcane/Host/SceneRenderResolver.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:31` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:51` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:63` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/NriHostFlavorTest.cpp:25` → `Arcane/Host/GpuContext.hpp`
- `ArcaneTests/src/NriHostFlavorTest.cpp:26` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:58` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/PathsTest.cpp:13` → `Arcane/Host/EarlyConfig.hpp`
- `ArcaneTests/src/PathsTest.cpp:14` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp:20` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp:21` → `Arcane/Host/ProjectBoot.hpp`
- `ArcaneTests/src/ResourceSwapTest.cpp:11` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/RuntimeEngineSystemsTest.cpp:18` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/RuntimeLaunchTest.cpp:14` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:11` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/SceneMigrationTest.cpp:14` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/SceneMigrationTest.cpp:15` → `Arcane/Host/ProjectBoot.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:29` → `Arcane/Client/ClientRuntime.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:30` → `Arcane/Host/SceneRenderResolver.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:16` → `Arcane/Host/EarlyConfig.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:17` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/SettleBoundTest.cpp:7` → `Arcane/Host/SettleBound.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:27` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/SweepDiagnosticsTest.cpp:10` → `Arcane/Host/EarlyConfig.hpp`
- `ArcaneTests/src/SweepDiagnosticsTest.cpp:11` → `Arcane/Host/FramePerf.hpp`
- `ArcaneTests/src/SweepDiagnosticsTest.cpp:12` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/SweepHostTest.cpp:4` → `Arcane/Host/HostSettings.hpp`
- `ArcaneTests/src/SweepProjectStoresTest.cpp:8` → `Arcane/Host/EarlyConfig.hpp`
- `ArcaneTests/src/SweepProjectStoresTest.cpp:9` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/SweepRenderDeviceTest.cpp:6` → `Arcane/Host/EarlyConfig.hpp`
- `ArcaneTests/src/SweepRenderDeviceTest.cpp:7` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/SweepRuntimeHudTest.cpp:3` → `Arcane/Host/RuntimeSettings.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:16` → `Arcane/Host/HostConfig.hpp`
- `ArcaneTests/src/test_main.cpp:11` → `Arcane/Client/ClientRuntime.hpp`

#### ArcaneCore (328 sites)

- `ArcaneTests/src/AabbTest.cpp:1` → `Arcane/Math/Aabb.hpp`
- `ArcaneTests/src/ArtifactReaderTest.cpp:30` → `Arcane/Guid.hpp`
- `ArcaneTests/src/ArtifactSizeContractTest.cpp:14` → `Arcane/Guid.hpp`
- `ArcaneTests/src/AssertRoutingTest.cpp:18` → `Arcane/Base/Assert.hpp`
- `ArcaneTests/src/AssetBrowserPanelTest.cpp:18` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/AssetBrowserRevealTest.cpp:21` → `Arcane/Guid.hpp`
- `ArcaneTests/src/AssetCreateInstanceTest.cpp:22` → `Arcane/Guid.hpp`
- `ArcaneTests/src/AssetFileOpsTest.cpp:13` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/AssetIdTest.cpp:6` → `Arcane/Guid.hpp`
- `ArcaneTests/src/AssetPanelModelTest.cpp:13` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/AssetPipelineFormatTest.cpp:18` → `Arcane/Guid.hpp`
- `ArcaneTests/src/AssetPipelineImporterTest.cpp:26` → `Arcane/Guid.hpp`
- `ArcaneTests/src/AssetPipelineImporterTest.cpp:27` → `Arcane/Util/Logger.hpp`
- `ArcaneTests/src/AssetPipelineSessionTest.cpp:30` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/AssetPipelineSessionTest.cpp:31` → `Arcane/Guid.hpp`
- `ArcaneTests/src/AssetPipelineStoreTest.cpp:20` → `Arcane/Guid.hpp`
- `ArcaneTests/src/AssetReferenceFieldTest.cpp:18` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/AssetRegistryTest.cpp:9` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/AssetStatusPanelClickTest.cpp:86` → `Arcane/Guid.hpp`
- `ArcaneTests/src/AssetsGraphCanvasTest.cpp:41` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/AssetsGraphCanvasTest.cpp:42` → `Arcane/Guid.hpp`
- `ArcaneTests/src/Batcher2DTest.cpp:10` → `Arcane/Guid.hpp`
- `ArcaneTests/src/BindlessTableTest.cpp:33` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/BootStageParityTest.cpp:19` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/BootStageParityTest.cpp:21` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/BuildDriverTest.cpp:12` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/BuildDriverTest.cpp:1931` → `Arcane/Build/Toolchain.hpp`
- `ArcaneTests/src/CVarAccessTest.cpp:6` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/CVarAccessTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarAccessTest.cpp:8` → `Arcane/Config/ConsoleModel.hpp`
- `ArcaneTests/src/CVarAliasTest.cpp:5` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/CVarAliasTest.cpp:6` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneTests/src/CVarAliasTest.cpp:7` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/CVarCommandTest.cpp:6` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneTests/src/CVarCommandTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarConfigDiagnosticsTest.cpp:10` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarConfigDiagnosticsTest.cpp:7` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/CVarConfigDiagnosticsTest.cpp:9` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/CVarConsoleTypesTest.cpp:4` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/CVarConsoleTypesTest.cpp:5` → `Arcane/Config/CVarFormat.hpp`
- `ArcaneTests/src/CVarConsoleTypesTest.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarDeclExternTest.cpp:5` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneTests/src/CVarDeclTest.cpp:5` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneTests/src/CVarDeclTest.cpp:8` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/CVarFileTypesTest.cpp:6` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/CVarFileTypesTest.cpp:7` → `Arcane/Config/CVarFormat.hpp`
- `ArcaneTests/src/CVarFileTypesTest.cpp:8` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarFileTypesTest.cpp:9` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/CVarFormatTest.cpp:5` → `Arcane/Config/CVarFormat.hpp`
- `ArcaneTests/src/CVarHistoryTest.cpp:5` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarMetadataTest.cpp:5` → `Arcane/Config/CVarFormat.hpp`
- `ArcaneTests/src/CVarMetadataTest.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarModuleLifetimeTest.cpp:11` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/CVarModuleLifetimeTest.cpp:12` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarModuleLifetimeTest.cpp:8` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/CVarModuleLifetimeTest.cpp:9` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/CVarModuleTest.cpp:6` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneTests/src/CVarModuleTest.cpp:7` → `Arcane/Config/CVarModule.hpp`
- `ArcaneTests/src/CVarModuleTest.cpp:8` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarPublishTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarRefTest.cpp:6` → `Arcane/Config/CVarRef.hpp`
- `ArcaneTests/src/CVarRefTest.cpp:7` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/CVarRegistrationRefusalTest.cpp:5` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarRegistryTest.cpp:2` → `Arcane/Config/ConsoleModel.hpp`
- `ArcaneTests/src/CVarRegistryTest.cpp:3` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/CVarRegistryTest.cpp:5` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneTests/src/CVarRegistryTest.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarRegistryTest.cpp:7` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/CVarRowSurfaceTest.cpp:7` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/CVarRowSurfaceTest.cpp:8` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarSnapshotTest.cpp:6` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneTests/src/CVarSnapshotTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/CVarSnapshotTest.cpp:8` → `Arcane/Jobs/JobSystem.hpp`
- `ArcaneTests/src/CVarValueTypesTest.cpp:5` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneTests/src/CliTest.cpp:6` → `Arcane/Cli/Cli.hpp`
- `ArcaneTests/src/CommandStackTest.cpp:20` → `Arcane/Guid.hpp`
- `ArcaneTests/src/ConstantGuardTest.cpp:7` → `Arcane/Core/Constant.hpp`
- `ArcaneTests/src/ContentDiscoveryTest.cpp:40` → `Arcane/Guid.hpp`
- `ArcaneTests/src/CookQueueTest.cpp:18` → `Arcane/Guid.hpp`
- `ArcaneTests/src/CoreDllTest.cpp:12` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/CoreDllTest.cpp:13` → `Arcane/Base/Engine.hpp`
- `ArcaneTests/src/CoreDllTest.cpp:14` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/CoreDllTest.cpp:21` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/CrashArenaTest.cpp:8` → `Arcane/Base/CrashArena.hpp`
- `ArcaneTests/src/CrashPathTest.cpp:12` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/CrashPathTest.cpp:13` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/CrashPathTest.cpp:14` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/CrashPathTest.cpp:15` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/CrashReportDocumentTest.cpp:11` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/CrashReportDocumentTest.cpp:19` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/CrashReportDocumentTest.cpp:20` → `Arcane/Guid.hpp`
- `ArcaneTests/src/CrashWitnessTest.cpp:5` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/CrashWitnessTest.cpp:6` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/CryptoTest.cpp:1` → `Arcane/Platform/SecureRandom.hpp`
- `ArcaneTests/src/DiagEnvelopeTest.cpp:8` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/DiagEnvelopeTest.cpp:9` → `Arcane/Guid.hpp`
- `ArcaneTests/src/DiagnosticSeamTest.cpp:15` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/DiagnosticsPosixTest.cpp:12` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/DiagnosticsPosixTest.cpp:16` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/DiagnosticsTest.cpp:26` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/DiagnosticsTest.cpp:27` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/EcsFacadeTest.cpp:6` → `Arcane/Config/CVarTypes.hpp`
- `ArcaneTests/src/EditorActionTableTest.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/EditorActionsTest.cpp:5` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/EditorActionsTest.cpp:6` → `Arcane/Config/CVarModule.hpp`
- `ArcaneTests/src/EditorCameraTest.cpp:29` → `Arcane/Guid.hpp`
- `ArcaneTests/src/EditorFontsTest.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/EditorInspectorHostTest.cpp:13` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/EditorInspectorHostTest.cpp:14` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:35` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/EditorRestartTest.cpp:6` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/EditorThemeSettingsTest.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/EditorUserRungTest.cpp:8` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/EditorUserRungTest.cpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/EditorWitnessTest.cpp:9` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/EngineExportTest.cpp:10` → `Arcane/Base/Engine.hpp`
- `ArcaneTests/src/EngineExportTest.cpp:11` → `Arcane/Version.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:11` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:17` → `Arcane/Guid.hpp`
- `ArcaneTests/src/EntityIdentityTest.cpp:9` → `Arcane/Guid.hpp`
- `ArcaneTests/src/EntityOpsTest.cpp:10` → `Arcane/Guid.hpp`
- `ArcaneTests/src/FontsPageTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/FontsPageTest.cpp:8` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/ForeignModulesTest.cpp:14` → `Arcane/Base/ForeignModules.hpp`
- `ArcaneTests/src/ForeignModulesTest.cpp:15` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/FunctionRefTest.cpp:2` → `Arcane/Util/FunctionRef.hpp`
- `ArcaneTests/src/GameInputTest.cpp:7` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/GameUserDirTest.cpp:10` → `Arcane/Config/PlayerSettings.hpp`
- `ArcaneTests/src/GameUserDirTest.cpp:11` → `Arcane/Platform/Paths.hpp`
- `ArcaneTests/src/GameUserDirTest.cpp:8` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/GameUserDirTest.cpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/GpuCrashReportTest.cpp:15` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/GpuCrashReportTest.cpp:16` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/GraphFitTest.cpp:20` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/Helpers/CVarTestDesc.hpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/Helpers/GpuCapability.hpp:17` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/Helpers/HostWitness.cpp:31` → `Arcane/Platform/Process.hpp`
- `ArcaneTests/src/Helpers/ModuleNames.hpp:11` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/Helpers/NodePageDocs.hpp:8` → `Arcane/Guid.hpp`
- `ArcaneTests/src/Helpers/ProtocolLayer.hpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/Helpers/ReferenceProjectDir.hpp:28` → `Arcane/Base/Engine.hpp`
- `ArcaneTests/src/Helpers/SettingsFixtures.hpp:8` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/Helpers/SettingsProbe.hpp:8` → `Arcane/Reflection.hpp`
- `ArcaneTests/src/Helpers/SettingsSweep.hpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/Helpers/UserDataDirs.hpp:17` → `Arcane/Platform/Paths.hpp`
- `ArcaneTests/src/HostBootTest.cpp:10` → `Arcane/Guid.hpp`
- `ArcaneTests/src/HostBootTest.cpp:44` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/HostBootTest.cpp:6` → `Arcane/Base/Engine.hpp`
- `ArcaneTests/src/HostBootTest.cpp:9` → `Arcane/Config/Config.hpp`
- `ArcaneTests/src/HostConfigTest.cpp:5` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/HostWitnessTest.cpp:11` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/IdeLaunchTest.cpp:12` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/JobSchedulerTest.cpp:6` → `Arcane/Jobs/JobSystem.hpp`
- `ArcaneTests/src/JobSystemSubmitTest.cpp:11` → `Arcane/Jobs/JobSystem.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:16` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:17` → `Arcane/Base/Engine.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:18` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:20` → `Arcane/Build/BuildToolSettings.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:21` → `Arcane/Build/Toolchain.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:22` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:23` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:24` → `Arcane/Config/PreferenceScope.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:25` → `Arcane/Platform/LaunchPath.hpp`
- `ArcaneTests/src/LayoutPageTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/LogBacklogTest.cpp:5` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/LoggerTest.cpp:14` → `Arcane/Util/Logger.hpp`
- `ArcaneTests/src/LruCacheTest.cpp:8` → `Arcane/Util/LruCache.hpp`
- `ArcaneTests/src/MaterialGraphTest.cpp:8` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/MaterialTest.cpp:8` → `Arcane/Guid.hpp`
- `ArcaneTests/src/MenuShortcutTest.cpp:5` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/MeshArtifactFormatTest.cpp:22` → `Arcane/Guid.hpp`
- `ArcaneTests/src/MeshArtifactReaderTest.cpp:25` → `Arcane/Guid.hpp`
- `ArcaneTests/src/MeshCacheTest.cpp:17` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/MeshCacheTest.cpp:18` → `Arcane/Guid.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:32` → `Arcane/Guid.hpp`
- `ArcaneTests/src/MeshImporterGeometryTest.cpp:20` → `Arcane/Guid.hpp`
- `ArcaneTests/src/MeshImporterRefusalTest.cpp:13` → `Arcane/Guid.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:26` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:30` → `Arcane/Guid.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:45` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/MeshSubmissionTest.cpp:22` → `Arcane/Guid.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:59` → `Arcane/Guid.hpp`
- `ArcaneTests/src/MosaicDiagnosticsTest.cpp:12` → `Arcane/Base/Assert.hpp`
- `ArcaneTests/src/MosaicDiagnosticsTest.cpp:13` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/NativeWindowTest.cpp:6` → `Arcane/Platform/NativeWindow.hpp`
- `ArcaneTests/src/NriDiagnosticsTest.cpp:35` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/NriDiagnosticsTest.cpp:36` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/NriMeshBufferCacheTest.cpp:19` → `Arcane/Guid.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:56` → `Arcane/Guid.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:61` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/NriTextureCacheTest.cpp:21` → `Arcane/Guid.hpp`
- `ArcaneTests/src/PathsTest.cpp:11` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/PathsTest.cpp:12` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/PathsTest.cpp:15` → `Arcane/Platform/Paths.hpp`
- `ArcaneTests/src/PathsTest.cpp:9` → `Arcane/Base/Engine.hpp`
- `ArcaneTests/src/PlayerSettingsTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/PlayerSettingsTest.cpp:8` → `Arcane/Config/PlayerSettings.hpp`
- `ArcaneTests/src/PlayerSettingsWitnessTest.cpp:5` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/PluginHostTest.cpp:18` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/PluginLoadDiagnosticsTest.cpp:14` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/PluginLoadDiagnosticsTest.cpp:33` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/PluginLoadDiagnosticsTest.cpp:35` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/PortableStackTest.cpp:18` → `Arcane/Base/ModuleTable.hpp`
- `ArcaneTests/src/PortableStackTest.cpp:19` → `Arcane/Base/PortableStack.hpp`
- `ArcaneTests/src/PortableStackTest.cpp:20` → `Arcane/Base/ForeignModules.hpp`
- `ArcaneTests/src/PostChainCacheTest.cpp:14` → `Arcane/Guid.hpp`
- `ArcaneTests/src/PreferenceScopeTest.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/PreferenceScopeTest.cpp:7` → `Arcane/Config/PreferenceScope.hpp`
- `ArcaneTests/src/ProjectTest.cpp:8` → `Arcane/Base/DiagEnvelope.hpp`
- `ArcaneTests/src/PropertyGridTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/ReferencePolicyTest.cpp:8` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/ReflectionFacadeTest.cpp:8` → `Arcane/Reflection.hpp`
- `ArcaneTests/src/RegistryStateCommandTest.cpp:12` → `Arcane/Guid.hpp`
- `ArcaneTests/src/RemoteCVarServiceTest.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/RemoteCVarServiceTest.cpp:7` → `Arcane/Config/RemoteCVarService.hpp`
- `ArcaneTests/src/RuntimeModulePluginTest.cpp:4` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/RuntimeProjectTest.cpp:2` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/RuntimeProjectTest.cpp:3` → `Arcane/Config/PreferenceScope.hpp`
- `ArcaneTests/src/RuntimeProjectTest.cpp:4` → `Arcane/Config/Config.hpp`
- `ArcaneTests/src/RuntimeProjectTest.cpp:5` → `Arcane/Platform/Paths.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:12` → `Arcane/Jobs/TaskExecutor.hpp`
- `ArcaneTests/src/SceneRecentsTest.cpp:9` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/SerializationNegativeTest.cpp:11` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/ServerAdminConsoleTest.cpp:10` → `Arcane/Config/RemoteCVarService.hpp`
- `ArcaneTests/src/ServerAdminConsoleTest.cpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/ServerWitnessTest.cpp:5` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/ServiceThreadTest.cpp:12` → `Arcane/Base/ServiceThread.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:10` → `Arcane/Config/Bindings/JobsBinding.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:11` → `Arcane/Config/Bindings/LogBinding.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:12` → `Arcane/Config/Bindings/Physics2DBinding.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:13` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:14` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:18` → `Arcane/Jobs/JobSystem.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:7` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:8` → `Arcane/Base/DiagnosticsSettings.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:9` → `Arcane/Config/Bindings/AstraBinding.hpp`
- `ArcaneTests/src/SettingsEditTest.cpp:7` → `Arcane/Config/CVarFormat.hpp`
- `ArcaneTests/src/SettingsFieldTest.cpp:7` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/SettingsFieldTest.cpp:8` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SettingsFieldTest.cpp:9` → `Arcane/Reflection.hpp`
- `ArcaneTests/src/SettingsSnapshotTest.cpp:6` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SettingsSnapshotTest.cpp:7` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SettingsStructTest.cpp:7` → `Arcane/Config/CVarDecl.hpp`
- `ArcaneTests/src/SettingsStructTest.cpp:8` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SettingsStructTest.cpp:9` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SettingsSweepTest.cpp:8` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SeveranceTest.cpp:29` → `Arcane/Guid.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:24` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/ShaderGraphPinTypesTest.cpp:10` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/ShortcutRoutingTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/ShortcutsPageTest.cpp:12` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/ShortcutsPageTest.cpp:13` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SkylinePackerTest.cpp:10` → `Arcane/Util/SkylinePacker.hpp`
- `ArcaneTests/src/SpriteDocumentUndoTest.cpp:30` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SpriteDocumentUndoTest.cpp:31` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SpriteDocumentUndoTest.cpp:33` → `Arcane/Guid.hpp`
- `ArcaneTests/src/SpriteMaterialCacheTest.cpp:24` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/SpriteMaterialCacheTest.cpp:25` → `Arcane/Guid.hpp`
- `ArcaneTests/src/SpriteRotationTest.cpp:25` → `Arcane/Guid.hpp`
- `ArcaneTests/src/StartPageModelTest.cpp:4` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/SweepAssetBrowserTest.cpp:11` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepAssetGraphTest.cpp:10` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepAssetsTest.cpp:13` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepBuildToolsTest.cpp:3` → `Arcane/Build/BuildToolSettings.hpp`
- `ArcaneTests/src/SweepBuildToolsTest.cpp:4` → `Arcane/Build/Toolchain.hpp`
- `ArcaneTests/src/SweepBuildToolsTest.cpp:5` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/SweepConsoleTest.cpp:14` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepConsoleTest.cpp:15` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SweepConsoleTest.cpp:9` → `Arcane/Config/ConsoleModel.hpp`
- `ArcaneTests/src/SweepCoverageTest.cpp:4` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepDiagnosticsTest.cpp:6` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/SweepDiagnosticsTest.cpp:7` → `Arcane/Base/DiagnosticsSettings.hpp`
- `ArcaneTests/src/SweepDiagnosticsTest.cpp:8` → `Arcane/Config/CVarConfig.hpp`
- `ArcaneTests/src/SweepDiagnosticsTest.cpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepDocumentUiTest.cpp:10` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepDocumentsTest.cpp:14` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepEditorCameraTest.cpp:10` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SweepGizmoTest.cpp:12` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SweepGraphCanvasTest.cpp:20` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepGraphCanvasTest.cpp:21` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SweepGraphThemeTest.cpp:17` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepGraphThemeTest.cpp:18` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SweepHostTest.cpp:3` → `Arcane/Config/ConsoleSettings.hpp`
- `ArcaneTests/src/SweepInspectorTest.cpp:11` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepInspectorTest.cpp:12` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SweepLogTest.cpp:10` → `Arcane/Base/DiagnosticsSettings.hpp`
- `ArcaneTests/src/SweepLogTest.cpp:11` → `Arcane/Base/Log.hpp`
- `ArcaneTests/src/SweepLogTest.cpp:12` → `Arcane/Base/LogFileSettings.hpp`
- `ArcaneTests/src/SweepLogTest.cpp:13` → `Arcane/Config/Bindings/LogBinding.hpp`
- `ArcaneTests/src/SweepLogTest.cpp:14` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepLogTest.cpp:15` → `Arcane/Util/LogServerSettings.hpp`
- `ArcaneTests/src/SweepLogTest.cpp:7` → `Arcane/Util/Logger.hpp`
- `ArcaneTests/src/SweepLogTest.cpp:9` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/SweepPhysicsDebugTest.cpp:10` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepPhysicsQueryTest.cpp:10` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepPhysicsQueryTest.cpp:9` → `Arcane/Config/Bindings/Physics2DBinding.hpp`
- `ArcaneTests/src/SweepPlayLayoutTest.cpp:12` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/SweepProjectStoresTest.cpp:4` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/SweepProjectStoresTest.cpp:6` → `Arcane/Config/CVarFormat.hpp`
- `ArcaneTests/src/SweepProjectStoresTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepRenderDeviceTest.cpp:5` → `Arcane/Config/CVarFormat.hpp`
- `ArcaneTests/src/SweepReporterTest.cpp:10` → `Arcane/Base/DiagnosticsSettings.hpp`
- `ArcaneTests/src/SweepReporterTest.cpp:11` → `Arcane/Base/ReporterSettings.hpp`
- `ArcaneTests/src/SweepReporterTest.cpp:12` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepReporterTest.cpp:13` → `Arcane/Config/UiSettings.hpp`
- `ArcaneTests/src/SweepReporterTest.cpp:9` → `Arcane/Base/Diagnostics.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:14` → `Arcane/Config/Bindings/JobsBinding.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:15` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepTextureImportTest.cpp:10` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepThumbnailTest.cpp:10` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepUiStyleTest.cpp:15` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/SweepViewportStoreTest.cpp:9` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/TaskExecutorTest.cpp:2` → `Arcane/Jobs/TaskExecutor.hpp`
- `ArcaneTests/src/TaskExecutorTest.cpp:3` → `Arcane/Jobs/JobSystem.hpp`
- `ArcaneTests/src/ThemePageTest.cpp:12` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/ThemePageTest.cpp:13` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/ThemePageTest.cpp:14` → `Arcane/Platform/Paths.hpp`
- `ArcaneTests/src/ThemePresetsTest.cpp:7` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/ThemePresetsTest.cpp:8` → `Arcane/Config/Settings.hpp`
- `ArcaneTests/src/ToolchainTest.cpp:18` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/ToolchainTest.cpp:20` → `Arcane/Build/Toolchain.hpp`
- `ArcaneTests/src/UndoPayloadTest.cpp:6` → `Arcane/Guid.hpp`
- `ArcaneTests/src/VerifyReportTest.cpp:8` → `Arcane/Base/ForeignModules.hpp`
- `ArcaneTests/src/VerifyReportTest.cpp:9` → `Arcane/Config/CVarRegistry.hpp`
- `ArcaneTests/src/VersionTest.cpp:3` → `Arcane/Version.hpp`
- `ArcaneTests/src/WitnessScenariosTest.cpp:56` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/test_main.cpp:10` → `Arcane/Platform/Platform.hpp`
- `ArcaneTests/src/test_main.cpp:9` → `Arcane/Base/Assert.hpp`

#### ArcaneCrypto (4 sites)

- `ArcaneTests/src/CryptoTest.cpp:27` → `Arcane/Crypto/Crypto.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:13` → `Arcane/Crypto/Crypto.hpp`
- `ArcaneTests/src/SweepCryptoTest.cpp:7` → `Arcane/Crypto/Crypto.hpp`
- `ArcaneTests/src/SweepCryptoTest.cpp:8` → `Arcane/Crypto/CryptoSettings.hpp`

#### ArcaneEdit (47 sites)

- `ArcaneTests/src/AssetReferenceFieldTest.cpp:19` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/AxisColorsTest.cpp:13` → `Arcane/Edit/Gizmo.hpp`
- `ArcaneTests/src/ChangeDetectionMarksTest.cpp:8` → `Arcane/Edit/ComponentEditCommand.hpp`
- `ArcaneTests/src/CommandStackTest.cpp:16` → `Arcane/Edit/Command.hpp`
- `ArcaneTests/src/CommandStackTest.cpp:17` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/CommandStackTest.cpp:18` → `Arcane/Edit/ComponentEditCommand.hpp`
- `ArcaneTests/src/CommandStackTest.cpp:19` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneTests/src/EditorComponentCatalogTest.cpp:10` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneTests/src/EditorComponentCatalogTest.cpp:11` → `Arcane/Edit/RegistryStateCommand.hpp`
- `ArcaneTests/src/EditorComponentCatalogTest.cpp:9` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/EditorInspectorColorTest.cpp:10` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/EditorInspectorTest.cpp:11` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneTests/src/EditorInspectorVectorTest.cpp:14` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/EditorOutlinerTest.cpp:12` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:36` → `Arcane/Edit/Command.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:37` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/EditorSceneSessionTest.cpp:12` → `Arcane/Edit/Command.hpp`
- `ArcaneTests/src/EditorSceneSessionTest.cpp:13` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/EditorUndoGateTest.cpp:6` → `Arcane/Edit/Command.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:14` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:15` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:16` → `Arcane/Edit/RegistryStateCommand.hpp`
- `ArcaneTests/src/EntityOpsTest.cpp:8` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneTests/src/EntityOpsTest.cpp:9` → `Arcane/Edit/Gizmo.hpp`
- `ArcaneTests/src/GizmoTest.cpp:10` → `Arcane/Edit/Gizmo.hpp`
- `ArcaneTests/src/GraphCanvasHeadlessTest.cpp:23` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/Helpers/AssetFileOpsFakes.hpp:9` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/Helpers/AssetOpsFixture.hpp:18` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/InputActionsDocumentUiTest.cpp:16` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/InputActionsEditorModelTest.cpp:10` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/InputPendingAddTest.cpp:6` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:31` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/PropertyGridTest.cpp:10` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/RegistryStateCommandTest.cpp:10` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneTests/src/RegistryStateCommandTest.cpp:11` → `Arcane/Edit/RegistryStateCommand.hpp`
- `ArcaneTests/src/RegistryStateCommandTest.cpp:9` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/SelectionOpsTest.cpp:9` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneTests/src/SettingsHostTest.cpp:15` → `Arcane/Edit/Command.hpp`
- `ArcaneTests/src/SettingsHostTest.cpp:16` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:25` → `Arcane/Edit/Command.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:26` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/SpriteDocumentUndoTest.cpp:32` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/SweepGizmoTest.cpp:10` → `Arcane/Edit/Gizmo.hpp`
- `ArcaneTests/src/TransformPropagationTest.cpp:9` → `Arcane/Edit/EntityOps.hpp`
- `ArcaneTests/src/UndoHotReloadTest.cpp:5` → `Arcane/Edit/ComponentEditCommand.hpp`
- `ArcaneTests/src/UndoPayloadTest.cpp:4` → `Arcane/Edit/CommandStack.hpp`
- `ArcaneTests/src/UndoPayloadTest.cpp:5` → `Arcane/Edit/UndoPayload.hpp`

#### ArcaneEngine (154 sites)

- `ArcaneTests/src/AssetBrowserPanelTest.cpp:17` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/AssetBrowserRevealTest.cpp:22` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/AssetBrowserTest.cpp:15` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/AssetCreateInstanceTest.cpp:23` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/AssetFileOpsTest.cpp:14` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/AssetFileOpsTest.cpp:15` → `Arcane/Project/MountTable.hpp`
- `ArcaneTests/src/AssetInspectorSourceTest.cpp:22` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/AssetInspectorSourceTest.cpp:23` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/AssetPanelCommonTest.cpp:18` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/AssetPanelModelTest.cpp:14` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/AssetReferenceFieldTest.cpp:17` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/AssetReferenceFieldTest.cpp:20` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/AssetRegistryTest.cpp:10` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/AssetRegistryTest.cpp:13` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/AssetRegistryTest.cpp:14` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/AssetStatusPanelClickTest.cpp:87` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/AssetsGraphCanvasTest.cpp:43` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/AssetsTest.cpp:34` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/BootStageParityTest.cpp:20` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/BootStageParityTest.cpp:27` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/CVarConfigDiagnosticsTest.cpp:11` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/CVarConfigDiagnosticsTest.cpp:8` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/CVarModuleLifetimeTest.cpp:10` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/CVarModuleLifetimeTest.cpp:13` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/CVarModuleLifetimeTest.cpp:14` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/CVarModuleLifetimeTest.cpp:15` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/ClassTemplatesTest.cpp:12` → `Arcane/Base/ProcessContext.hpp`
- `ArcaneTests/src/ClassTemplatesTest.cpp:13` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/ClassTemplatesTest.cpp:14` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/ClassTemplatesTest.cpp:15` → `Arcane/Plugin/SystemFactory.hpp`
- `ArcaneTests/src/ClientRuntimeTest.cpp:13` → `Arcane/Plugin/ClientHooks.hpp`
- `ArcaneTests/src/ClientRuntimeTest.cpp:14` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/ClientRuntimeTest.cpp:8` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/CommandStackTest.cpp:15` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/ContentDiscoveryTest.cpp:42` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/CookQueueTest.cpp:19` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/CrashPathTest.cpp:16` → `Arcane/Project/ProjectPaths.hpp`
- `ArcaneTests/src/CreateAssetDialogTest.cpp:17` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/EditorComponentCatalogTest.cpp:8` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/EditorInspectorColorTest.cpp:9` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/EditorInspectorTest.cpp:10` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/EditorInspectorVectorTest.cpp:13` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/EditorOutlinerTest.cpp:11` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:33` → `Arcane/Base/ProcessContext.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:34` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:38` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:39` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/EditorSceneSessionTest.cpp:11` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:12` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/EntityOpsTest.cpp:7` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/GameComponentsTest.cpp:14` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/GameComponentsTest.cpp:15` → `Arcane/Plugin/GameComponents.hpp`
- `ArcaneTests/src/GameUserDirTest.cpp:12` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/GameUserDirTest.cpp:13` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/GameUserDirTest.cpp:14` → `Arcane/Project/ProjectManifest.hpp`
- `ArcaneTests/src/GameUserDirTest.cpp:15` → `Arcane/Project/ProjectPaths.hpp`
- `ArcaneTests/src/GameUserDirTest.cpp:7` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/Helpers/AssetFileOpsFakes.hpp:10` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/Helpers/AssetOpsFixture.hpp:19` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/Helpers/TestTypeContext.hpp:8` → `Arcane/Base/ProcessContext.hpp`
- `ArcaneTests/src/Helpers/UserDataDirs.hpp:18` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/Helpers/UserDataDirs.hpp:19` → `Arcane/Project/ProjectOpenOptions.hpp`
- `ArcaneTests/src/Helpers/UserDataDirs.hpp:20` → `Arcane/Project/ProjectPaths.hpp`
- `ArcaneTests/src/HostBootTest.cpp:11` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/HostBootTest.cpp:13` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/HostBootTest.cpp:7` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/InputActionsDocumentUiTest.cpp:15` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/InputActionsEditorModelTest.cpp:11` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/InputPendingAddTest.cpp:7` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:19` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/LaunchesProgramTest.cpp:26` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/MaterialAssetTest.cpp:9` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:29` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:37` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:69` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:72` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/MountTableTest.cpp:5` → `Arcane/Project/MountTable.hpp`
- `ArcaneTests/src/MultiRuntimeReloadTest.cpp:7` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/MultiRuntimeReloadTest.cpp:8` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/NamespaceFacadeTest.cpp:11` → `Arcane/Scene/EngineRoster.hpp`
- `ArcaneTests/src/ParamSystemRegistrationTest.cpp:7` → `Arcane/Plugin/GameSystems.hpp`
- `ArcaneTests/src/ParamSystemRegistrationTest.cpp:8` → `Arcane/Plugin/SystemFactory.hpp`
- `ArcaneTests/src/PathsTest.cpp:10` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/PathsTest.cpp:16` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/PathsTest.cpp:17` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/PathsTest.cpp:18` → `Arcane/Project/ProjectPaths.hpp`
- `ArcaneTests/src/PerspectiveCameraTest.cpp:20` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/Physics2DTest.cpp:7` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/PhysicsFrameEventsTest.cpp:6` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/PhysicsFrameEventsTest.cpp:7` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:42` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/PlayerSettingsTest.cpp:6` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/PlayerSettingsTest.cpp:9` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/PluginHostTest.cpp:10` → `Arcane/Plugin/SystemFactory.hpp`
- `ArcaneTests/src/PluginHostTest.cpp:16` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/PluginHostTest.cpp:8` → `Arcane/Base/ProcessContext.hpp`
- `ArcaneTests/src/PluginHostTest.cpp:9` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/PluginLoadDiagnosticsTest.cpp:34` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/PluginLoadDiagnosticsTest.cpp:36` → `Arcane/Plugin/Module.hpp`
- `ArcaneTests/src/PluginLoadDiagnosticsTest.cpp:37` → `Arcane/Plugin/Plugin.hpp`
- `ArcaneTests/src/PluginLoadDiagnosticsTest.cpp:38` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/PluginLoadDiagnosticsTest.cpp:39` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/ProcessContextTest.cpp:10` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/ProcessContextTest.cpp:9` → `Arcane/Base/ProcessContext.hpp`
- `ArcaneTests/src/ProjectManifestTest.cpp:6` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/ProjectManifestTest.cpp:7` → `Arcane/Project/ProjectManifest.hpp`
- `ArcaneTests/src/ProjectManifestTest.cpp:9` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/ProjectSettingsPageTest.cpp:9` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/ProjectTest.cpp:10` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/ProjectTest.cpp:11` → `Arcane/Project/ProjectPaths.hpp`
- `ArcaneTests/src/ProjectTest.cpp:6` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/ReferenceImagesTest.cpp:7` → `Arcane/Project/ProjectPaths.hpp`
- `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp:23` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp:24` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/ReferencePolicyTest.cpp:7` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/ReferencePolicyTest.cpp:9` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/RegistryStateCommandTest.cpp:8` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/ResourceSwapTest.cpp:13` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/RoleMaskTest.cpp:10` → `Arcane/Plugin/SystemFactory.hpp`
- `ArcaneTests/src/RoleMaskTest.cpp:7` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/RoleMaskTest.cpp:8` → `Arcane/Plugin/GameSystems.hpp`
- `ArcaneTests/src/RoleMaskTest.cpp:9` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/RuntimeEngineSystemsTest.cpp:17` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/RuntimeModulePluginTest.cpp:5` → `Arcane/Plugin/Module.hpp`
- `ArcaneTests/src/RuntimeModulePluginTest.cpp:6` → `Arcane/Plugin/Plugin.hpp`
- `ArcaneTests/src/RuntimeModulePluginTest.cpp:8` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/RuntimeProjectTest.cpp:1` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/RuntimeProjectTest.cpp:6` → `Arcane/Plugin/PluginABI.hpp`
- `ArcaneTests/src/RuntimeProjectTest.cpp:7` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:10` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/SceneCameraTest.cpp:10` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/SceneMigrationTest.cpp:16` → `Arcane/Plugin/PluginHost.hpp`
- `ArcaneTests/src/SceneMigrationTest.cpp:17` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:35` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/SelectionOpsTest.cpp:8` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/SerializationNegativeTest.cpp:12` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/ServerFixedRateTest.cpp:13` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:6` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/SettingsModelTest.cpp:10` → `Arcane/Project/ProjectManifest.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:23` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:31` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/ShortcutsPageTest.cpp:11` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/SpriteAssetTest.cpp:4` → `Arcane/Project/AssetRegistry.hpp`
- `ArcaneTests/src/SweepAssetBrowserTest.cpp:12` → `Arcane/Project/ProjectOpenOptions.hpp`
- `ArcaneTests/src/SweepDiagnosticsTest.cpp:13` → `Arcane/Project/ProjectPaths.hpp`
- `ArcaneTests/src/SweepHostTest.cpp:6` → `Arcane/Project/AppSplashSettings.hpp`
- `ArcaneTests/src/SweepPluginTest.cpp:3` → `Arcane/Plugin/PluginSettings.hpp`
- `ArcaneTests/src/SweepProjectStoresTest.cpp:10` → `Arcane/Project/AppSplashSettings.hpp`
- `ArcaneTests/src/SweepProjectStoresTest.cpp:11` → `Arcane/Project/Project.hpp`
- `ArcaneTests/src/SweepProjectStoresTest.cpp:12` → `Arcane/Project/ProjectManifest.hpp`
- `ArcaneTests/src/SweepProjectStoresTest.cpp:5` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:9` → `Arcane/Base/Runtime.hpp`
- `ArcaneTests/src/ThemePageTest.cpp:15` → `Arcane/Project/ProjectPaths.hpp`
- `ArcaneTests/src/TransformPropagationTest.cpp:8` → `Arcane/Base/Runtime.hpp`

#### ArcaneImGui (6 sites)

- `ArcaneTests/src/ConsoleInputLineTest.cpp:4` → `Arcane/ImGui/ConsoleInputLine.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:1` → `Arcane/ImGui/ImGuiNriNode.hpp`
- `ArcaneTests/src/NriHostFlavorTest.cpp:27` → `Arcane/ImGui/ImGuiLayer.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:24` → `Arcane/ImGui/ImGuiNri.hpp`
- `ArcaneTests/src/SweepRenderBudgetTest.cpp:12` → `Arcane/ImGui/ImGuiNri.hpp`
- `ArcaneTests/src/test_main.cpp:13` → `Arcane/ImGui/ImGuiHostHooks.hpp`

#### ArcaneInput (32 sites)

- `ArcaneTests/src/AssetRegistryTest.cpp:11` → `Arcane/Input/InputActionAsset.hpp`
- `ArcaneTests/src/ClientRuntimeTest.cpp:10` → `Arcane/Input/InputActionAsset.hpp`
- `ArcaneTests/src/ClientRuntimeTest.cpp:11` → `Arcane/Input/LocalInputUser.hpp`
- `ArcaneTests/src/CoreDllTest.cpp:15` → `Arcane/Input/InputActions.hpp`
- `ArcaneTests/src/EditorActionsTest.cpp:7` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneTests/src/EditorActionsTest.cpp:8` → `Arcane/Input/KeyLayout.hpp`
- `ArcaneTests/src/GameInputTest.cpp:10` → `Arcane/Input/InputActionAsset.hpp`
- `ArcaneTests/src/GameInputTest.cpp:11` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneTests/src/GameInputTest.cpp:12` → `Arcane/Input/LocalInputUser.hpp`
- `ArcaneTests/src/GameInputTest.cpp:9` → `Arcane/Input/GameInput.hpp`
- `ArcaneTests/src/HostBootTest.cpp:14` → `Arcane/Input/InputActions.hpp`
- `ArcaneTests/src/HostSliceTest.cpp:11` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneTests/src/InputActionAssetTest.cpp:3` → `Arcane/Input/InputActionAsset.hpp`
- `ArcaneTests/src/InputActionsDocumentUiTest.cpp:17` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneTests/src/InputActionsTest.cpp:10` → `Arcane/Input/InputActionAsset.hpp`
- `ArcaneTests/src/InputActionsTest.cpp:9` → `Arcane/Input/InputActions.hpp`
- `ArcaneTests/src/InputBindingProfileTest.cpp:3` → `Arcane/Input/InputActionAsset.hpp`
- `ArcaneTests/src/InputBindingProfileTest.cpp:4` → `Arcane/Input/InputBindingProfile.hpp`
- `ArcaneTests/src/InputDevicesTest.cpp:8` → `Arcane/Input/InputDevices.hpp`
- `ArcaneTests/src/InputFixedTransitionsTest.cpp:3` → `Arcane/Input/InputActionAsset.hpp`
- `ArcaneTests/src/InputFixedTransitionsTest.cpp:4` → `Arcane/Input/InputActions.hpp`
- `ArcaneTests/src/InputRebindOperationTest.cpp:4` → `Arcane/Input/InputActions.hpp`
- `ArcaneTests/src/InputRebindOperationTest.cpp:5` → `Arcane/Input/InputRebindOperation.hpp`
- `ArcaneTests/src/KeyChordTest.cpp:5` → `Arcane/Input/KeyLayout.hpp`
- `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp:22` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneTests/src/ResourceSwapTest.cpp:12` → `Arcane/Input/GameInput.hpp`
- `ArcaneTests/src/ShortcutRoutingTest.cpp:8` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneTests/src/ShortcutRoutingTest.cpp:9` → `Arcane/Input/KeyLayout.hpp`
- `ArcaneTests/src/ShortcutsPageTest.cpp:14` → `Arcane/Input/InputSnapshot.hpp`
- `ArcaneTests/src/ShortcutsPageTest.cpp:15` → `Arcane/Input/KeyLayout.hpp`
- `ArcaneTests/src/SweepInputTest.cpp:4` → `Arcane/Input/InputActions.hpp`
- `ArcaneTests/src/SweepInputTest.cpp:5` → `Arcane/Input/InputSettings.hpp`

#### ArcaneNet (13 sites)

- `ArcaneTests/src/FramingTest.cpp:13` → `Arcane/Net/TcpSocket.hpp`
- `ArcaneTests/src/FramingTest.cpp:14` → `Arcane/Net/Protocol.hpp`
- `ArcaneTests/src/ProtocolLoaderTest.cpp:11` → `Arcane/Net/Protocol.hpp`
- `ArcaneTests/src/ProtocolMessageTest.cpp:5` → `Arcane/Net/Protocol.hpp`
- `ArcaneTests/src/RateLimiterTest.cpp:19` → `Arcane/Net/RateLimiter.hpp`
- `ArcaneTests/src/SweepNetTest.cpp:10` → `Arcane/Net/Protocol.hpp`
- `ArcaneTests/src/SweepNetTest.cpp:11` → `Arcane/Net/RateLimiter.hpp`
- `ArcaneTests/src/SweepNetTest.cpp:12` → `Arcane/Net/TcpSocket.hpp`
- `ArcaneTests/src/SweepNetTest.cpp:9` → `Arcane/Net/NetSettings.hpp`
- `ArcaneTests/src/SweepProtocolTest.cpp:8` → `Arcane/Net/NetSettings.hpp`
- `ArcaneTests/src/SweepProtocolTest.cpp:9` → `Arcane/Net/Protocol.hpp`
- `ArcaneTests/src/WireRoundTripTest.cpp:10` → `Arcane/Net/Protocol.hpp`
- `ArcaneTests/src/WireRoundTripTest.cpp:9` → `Arcane/Net/TcpSocket.hpp`

#### ArcanePhysics2D (37 sites)

- `ArcaneTests/src/AuthoredTransformSyncTest.cpp:10` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/AuthoredTransformSyncTest.cpp:9` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/ClientRuntimeTest.cpp:16` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/EditorComponentCatalogTest.cpp:13` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/EditorInspectorTest.cpp:13` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/EditorInspectorVectorTest.cpp:16` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:41` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:42` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/NamespaceFacadeTest.cpp:10` → `Arcane/Physics2D.hpp`
- `ArcaneTests/src/Physics2DTest.cpp:10` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/Physics2DTest.cpp:9` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/PhysicsComponentsTest.cpp:19` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/PhysicsEvents2DTest.cpp:10` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/PhysicsEvents2DTest.cpp:8` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/PhysicsEvents2DTest.cpp:9` → `Arcane/Scene/PhysicsEvents2D.hpp`
- `ArcaneTests/src/PhysicsFrameEventsTest.cpp:10` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/PhysicsFrameEventsTest.cpp:9` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/PhysicsPauseTest.cpp:7` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/PhysicsPauseTest.cpp:9` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/PhysicsSystemTest.cpp:25` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/PhysicsSystemTest.cpp:26` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:49` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:50` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/ProcessContextTest.cpp:11` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp:26` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/RenderInterpolationTest.cpp:27` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/RenderInterpolationTest.cpp:28` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/RuntimeEngineSystemsTest.cpp:19` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:15` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:16` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/SceneJsonTest.cpp:9` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/SceneMigrationTest.cpp:19` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/SceneUnknownFieldTest.cpp:8` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:19` → `Arcane/Scene/PhysicsSystem.hpp`
- `ArcaneTests/src/SweepPhysicsQueryTest.cpp:11` → `Arcane/Scene/PhysicsQuerySettings.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:11` → `Arcane/Scene/PhysicsComponents.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:12` → `Arcane/Scene/PhysicsSystem.hpp`

#### ArcaneRender (217 sites)

- `ArcaneTests/src/AgilitySdkTest.cpp:4` → `Arcane/Render/AgilitySdk.hpp`
- `ArcaneTests/src/AxisColorsTest.cpp:14` → `Arcane/Render/Nri/nodes/GridNode.hpp`
- `ArcaneTests/src/Batcher2DTest.cpp:11` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/BindlessTableTest.cpp:27` → `Arcane/Render/Nri/BindlessTable.hpp`
- `ArcaneTests/src/BindlessTableTest.cpp:28` → `Arcane/Render/Nri/Graveyard.hpp`
- `ArcaneTests/src/BindlessTableTest.cpp:29` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/BindlessTableTest.cpp:30` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/CVarDeclTest.cpp:6` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneTests/src/CVarDeclTest.cpp:7` → `Arcane/Render/Nri/nodes/MeshCullNode.hpp`
- `ArcaneTests/src/CVarRegistryTest.cpp:4` → `Arcane/Render/Nri/nodes/MeshCullNode.hpp`
- `ArcaneTests/src/ClientRuntimeTest.cpp:15` → `Arcane/Render/RenderSystems.hpp`
- `ArcaneTests/src/CoreDllTest.cpp:16` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/CrashReportDocumentTest.cpp:21` → `Arcane/Render/IGpuCrashBackend.hpp`
- `ArcaneTests/src/DiagnosticsTest.cpp:28` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneTests/src/EntityIdentityTest.cpp:10` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/EntityIdentityTest.cpp:12` → `Arcane/Render/RenderSystems.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:10` → `Arcane/Render/GraphicsBackend.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:12` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:13` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:14` → `Arcane/Render/Nri/nodes/MeshNode.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:15` → `Arcane/Render/RenderDeviceDesc.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:16` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/GpuBreadcrumbsTest.cpp:17` → `Arcane/Render/GpuBreadcrumbs.hpp`
- `ArcaneTests/src/GpuCrashReportTest.cpp:17` → `Arcane/Render/GpuBreadcrumbs.hpp`
- `ArcaneTests/src/GpuCrashReportTest.cpp:18` → `Arcane/Render/GpuCrashReport.hpp`
- `ArcaneTests/src/GpuCrashReportTest.cpp:19` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneTests/src/GpuCrashReportTest.cpp:20` → `Arcane/Render/IGpuCrashBackend.hpp`
- `ArcaneTests/src/GpuDumpContainerTest.cpp:9` → `Arcane/Render/IGpuCrashBackend.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:47` → `Arcane/Render/FramePacing.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:48` → `Arcane/Render/GpuSceneSync.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:49` → `Arcane/Render/GpuSceneTypes.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:50` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:51` → `Arcane/Render/VisibilitySystem.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:52` → `Arcane/Render/Nri/GpuScene.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:53` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:54` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:55` → `Arcane/Render/Nri/NriMeshBufferCache.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:56` → `Arcane/Render/Nri/nodes/MeshNode.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:10` → `Arcane/Render/GpuSceneTypes.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:11` → `Arcane/Render/VisibilitySystem.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:9` → `Arcane/Render/GpuSceneSync.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:33` → `Arcane/Render/RenderDeviceDesc.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:34` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:35` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:36` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:37` → `Arcane/Render/Nri/NriMeshBufferCache.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:38` → `Arcane/Render/Nri/nodes/GridNode.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:39` → `Arcane/Render/Nri/nodes/MeshNode.hpp`
- `ArcaneTests/src/Helpers/GpuCapability.cpp:3` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/Helpers/GpuCapability.cpp:4` → `Arcane/Render/RenderDeviceDesc.hpp`
- `ArcaneTests/src/Helpers/GpuCapability.hpp:18` → `Arcane/Render/GraphicsBackend.hpp`
- `ArcaneTests/src/HostBootTest.cpp:27` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneTests/src/HostBootTest.cpp:28` → `Arcane/Render/ShaderConventions.hpp`
- `ArcaneTests/src/HostBootTest.cpp:29` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneTests/src/MaterialGraphTest.cpp:12` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneTests/src/MaterialGraphTest.cpp:13` → `Arcane/Render/ShaderConventions.hpp`
- `ArcaneTests/src/MaterialGraphTest.cpp:14` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneTests/src/MaterialSourceTest.cpp:10` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneTests/src/MaterialSourceTest.cpp:8` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneTests/src/MaterialSourceTest.cpp:9` → `Arcane/Render/ShaderConventions.hpp`
- `ArcaneTests/src/MaterialTest.cpp:14` → `Arcane/Render/MeshMaterialCache.hpp`
- `ArcaneTests/src/MeshCacheTest.cpp:19` → `Arcane/Render/MeshCache.hpp`
- `ArcaneTests/src/MeshCullNodeTest.cpp:4` → `Arcane/Render/GpuSceneSync.hpp`
- `ArcaneTests/src/MeshCullNodeTest.cpp:5` → `Arcane/Render/Nri/nodes/MeshCullNode.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:38` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:25` → `Arcane/Render/Nri/nodes/MeshNode.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:33` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:34` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:35` → `Arcane/Render/Nri/NriMeshBufferCache.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:36` → `Arcane/Render/RenderDeviceDesc.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:37` → `Arcane/Render/RenderDeviceSettings.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:38` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:39` → `Arcane/Render/ShaderPaths.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:42` → `Arcane/Render/Nri/GpuScene.hpp`
- `ArcaneTests/src/MeshResidencyBudgetTest.cpp:4` → `Arcane/Render/Nri/MeshResidencyBudget.hpp`
- `ArcaneTests/src/MeshResidencyBudgetTest.cpp:5` → `Arcane/Render/RenderBudgetSettings.hpp`
- `ArcaneTests/src/MeshSubmissionTest.cpp:27` → `Arcane/Render/MeshCache.hpp`
- `ArcaneTests/src/MeshSubmissionTest.cpp:28` → `Arcane/Render/MeshMaterialCache.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:53` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:61` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:73` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:74` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneTests/src/NodePageFixtureTest.cpp:11` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneTests/src/NodePageFixtureTest.cpp:12` → `Arcane/Render/ShaderConventions.hpp`
- `ArcaneTests/src/NodePageFixtureTest.cpp:13` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneTests/src/NriDeviceCapsTest.cpp:6` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/NriDiagnosticsTest.cpp:37` → `Arcane/Render/GpuBreadcrumbs.hpp`
- `ArcaneTests/src/NriDiagnosticsTest.cpp:38` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneTests/src/NriDiagnosticsTest.cpp:39` → `Arcane/Render/IGpuCrashBackend.hpp`
- `ArcaneTests/src/NriDiagnosticsTest.cpp:40` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/NriDiagnosticsTest.cpp:41` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/NriDiagnosticsTest.cpp:42` → `Arcane/Render/Nri/NriDiagnostics.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:64` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:65` → `Arcane/Render/FramePacing.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:66` → `Arcane/Render/GpuSceneSync.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:67` → `Arcane/Render/VisibilitySystem.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:73` → `Arcane/Render/PickEmit.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:74` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:76` → `Arcane/Render/Nri/BindlessTable.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:77` → `Arcane/Render/Nri/GpuScene.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:78` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:79` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:80` → `Arcane/Render/Nri/NriMeshBufferCache.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:81` → `Arcane/Render/Nri/NriTextureCache.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:82` → `Arcane/Render/Nri/nodes/MeshNode.hpp`
- `ArcaneTests/src/NriHostFlavorTest.cpp:29` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/NriMeshBufferCacheTest.cpp:12` → `Arcane/Render/Nri/Graveyard.hpp`
- `ArcaneTests/src/NriMeshBufferCacheTest.cpp:13` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/NriMeshBufferCacheTest.cpp:14` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/NriMeshBufferCacheTest.cpp:15` → `Arcane/Render/Nri/NriMeshBufferCache.hpp`
- `ArcaneTests/src/NriMeshBufferCacheTest.cpp:21` → `Arcane/Render/RenderDeviceDesc.hpp`
- `ArcaneTests/src/NriMeshBufferCacheTest.cpp:22` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/NriSubstrateTest.cpp:20` → `Arcane/Render/RenderDeviceDesc.hpp`
- `ArcaneTests/src/NriSubstrateTest.cpp:21` → `Arcane/Render/RenderDeviceSettings.hpp`
- `ArcaneTests/src/NriSubstrateTest.cpp:22` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/NriSubstrateTest.cpp:23` → `Arcane/Render/Nri/Graveyard.hpp`
- `ArcaneTests/src/NriSubstrateTest.cpp:24` → `Arcane/Render/Nri/NriCommon.hpp`
- `ArcaneTests/src/NriSubstrateTest.cpp:25` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:45` → `Arcane/Render/Nri/Graveyard.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:46` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:47` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:48` → `Arcane/Render/Nri/NriTextureCache.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:59` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:60` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/NriTextureCacheArtifactTest.cpp:62` → `Arcane/Render/Nri/AssetsCookSettings.hpp`
- `ArcaneTests/src/NriTextureCacheTest.cpp:22` → `Arcane/Render/Nri/Graveyard.hpp`
- `ArcaneTests/src/NriTextureCacheTest.cpp:23` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/NriTextureCacheTest.cpp:24` → `Arcane/Render/Nri/NriTextureCache.hpp`
- `ArcaneTests/src/NriTextureCacheTest.cpp:25` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/OffscreenVehicleTest.cpp:3` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneTests/src/OffscreenVehicleTest.cpp:4` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/OffscreenVehicleTest.cpp:5` → `Arcane/Render/Nri/NriDiagnostics.hpp`
- `ArcaneTests/src/PhysicsDebugCapsuleTest.cpp:15` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/PhysicsDebugCapsuleTest.cpp:16` → `Arcane/Render/PhysicsDebugDraw.hpp`
- `ArcaneTests/src/PhysicsDebugRichTest.cpp:26` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/PhysicsDebugRichTest.cpp:27` → `Arcane/Render/PhysicsDebugDraw.hpp`
- `ArcaneTests/src/PhysicsDebugTransformTest.cpp:22` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/PhysicsDebugTransformTest.cpp:23` → `Arcane/Render/PhysicsDebugDraw.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:44` → `Arcane/Render/PickEmit.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:45` → `Arcane/Render/SpriteGeometry.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:46` → `Arcane/Render/VisibilitySystem.hpp`
- `ArcaneTests/src/PluginHostTest.cpp:11` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/PostChainCacheTest.cpp:17` → `Arcane/Render/PostChainCache.hpp`
- `ArcaneTests/src/PostChainCacheTest.cpp:18` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneTests/src/PostChainCacheTest.cpp:19` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:25` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:26` → `Arcane/Render/GpuBreadcrumbs.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:27` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:28` → `Arcane/Render/IGpuCrashBackend.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:29` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:30` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:31` → `Arcane/Render/Nri/NriPipelineCache.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:32` → `Arcane/Render/Nri/nodes/Batch2DNode.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:33` → `Arcane/Render/Nri/nodes/FullscreenNodes.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:34` → `Arcane/Render/Nri/nodes/MeshNode.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:35` → `Arcane/Render/Nri/nodes/MeshCullNode.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:36` → `Arcane/Render/Nri/nodes/GridNode.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:37` → `Arcane/Render/Nri/nodes/PickOutlineNodes.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:39` → `Arcane/Render/Nri/NriUploadRing.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:40` → `Arcane/Render/Nri/RenderGraph.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:41` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:42` → `Arcane/Render/PostChainCache.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:44` → `Arcane/Render/FramePacing.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:45` → `Arcane/Render/RenderBudgetSettings.hpp`
- `ArcaneTests/src/RenderInterpolationTest.cpp:13` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/RenderInterpolationTest.cpp:14` → `Arcane/Render/PhysicsDebugDraw.hpp`
- `ArcaneTests/src/RenderInterpolationTest.cpp:15` → `Arcane/Render/RenderSystems.hpp`
- `ArcaneTests/src/RuntimeEngineSystemsTest.cpp:20` → `Arcane/Render/RenderSystems.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:17` → `Arcane/Render/RenderSystems.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:36` → `Arcane/Render/SpriteCache.hpp`
- `ArcaneTests/src/SelectionOutlineTest.cpp:18` → `Arcane/Render/Nri/nodes/PickOutlineNodes.hpp`
- `ArcaneTests/src/SelectionOutlineTest.cpp:19` → `Arcane/Render/PickEmit.hpp`
- `ArcaneTests/src/SelectionOutlineTest.cpp:20` → `Arcane/Render/RenderOutlineSettings.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:22` → `Arcane/Render/RenderDeviceSettings.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:23` → `Arcane/Render/GpuInstrumentation.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:24` → `Arcane/Render/Nri/nodes/MeshCullNode.hpp`
- `ArcaneTests/src/SeveranceTest.cpp:34` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/SeveranceTest.cpp:35` → `Arcane/Render/PostChainCache.hpp`
- `ArcaneTests/src/SeveranceTest.cpp:36` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneTests/src/SeveranceTest.cpp:37` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneTests/src/SeveranceTest.cpp:38` → `Arcane/Render/SpriteMaterialCache.hpp`
- `ArcaneTests/src/ShaderCompilerTest.cpp:10` → `Arcane/Render/ShaderConventions.hpp`
- `ArcaneTests/src/ShaderCompilerTest.cpp:9` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:32` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:33` → `Arcane/Render/RenderErrorLatch.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:34` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:35` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneTests/src/ShaderSourceProviderTest.cpp:7` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneTests/src/SpriteMaterialCacheTest.cpp:27` → `Arcane/Render/ShaderCompiler.hpp`
- `ArcaneTests/src/SpriteMaterialCacheTest.cpp:28` → `Arcane/Render/ShaderSourceProvider.hpp`
- `ArcaneTests/src/SpriteMaterialCacheTest.cpp:29` → `Arcane/Render/SpriteMaterialCache.hpp`
- `ArcaneTests/src/SpriteRotationTest.cpp:26` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/SpriteRotationTest.cpp:28` → `Arcane/Render/RenderSystems.hpp`
- `ArcaneTests/src/SpriteRotationTest.cpp:29` → `Arcane/Render/SpriteGeometry.hpp`
- `ArcaneTests/src/SweepAssetsTest.cpp:14` → `Arcane/Render/Nri/AssetsCookSettings.hpp`
- `ArcaneTests/src/SweepFramesInFlightTest.cpp:7` → `Arcane/Render/FramePacing.hpp`
- `ArcaneTests/src/SweepFramesInFlightTest.cpp:8` → `Arcane/Render/RenderDeviceSettings.hpp`
- `ArcaneTests/src/SweepOutlineGridTest.cpp:6` → `Arcane/Render/RenderOutlineSettings.hpp`
- `ArcaneTests/src/SweepPhysicsDebugTest.cpp:11` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/SweepPhysicsDebugTest.cpp:12` → `Arcane/Render/PhysicsDebugDraw.hpp`
- `ArcaneTests/src/SweepRenderBudgetTest.cpp:13` → `Arcane/Render/Nri/Graveyard.hpp`
- `ArcaneTests/src/SweepRenderBudgetTest.cpp:14` → `Arcane/Render/Nri/NriDevice.hpp`
- `ArcaneTests/src/SweepRenderBudgetTest.cpp:15` → `Arcane/Render/Nri/NriMeshBufferCache.hpp`
- `ArcaneTests/src/SweepRenderBudgetTest.cpp:16` → `Arcane/Render/Nri/NriPipelineCache.hpp`
- `ArcaneTests/src/SweepRenderBudgetTest.cpp:17` → `Arcane/Render/Nri/nodes/Batch2DNode.hpp`
- `ArcaneTests/src/SweepRenderBudgetTest.cpp:18` → `Arcane/Render/RenderBudgetSettings.hpp`
- `ArcaneTests/src/SweepRenderDeviceTest.cpp:3` → `Arcane/Render/RenderDeviceSettings.hpp`
- `ArcaneTests/src/SweepRenderDeviceTest.cpp:4` → `Arcane/Render/RenderDeviceDesc.hpp`
- `ArcaneTests/src/SweepRenderLookTest.cpp:13` → `Arcane/Render/Nri/NriGraphContext.hpp`
- `ArcaneTests/src/SweepRenderLookTest.cpp:14` → `Arcane/Render/Nri/nodes/MeshCullNode.hpp`
- `ArcaneTests/src/SweepRenderLookTest.cpp:15` → `Arcane/Render/Nri/nodes/MeshNode.hpp`
- `ArcaneTests/src/SweepRenderLookTest.cpp:16` → `Arcane/Render/RenderLookSettings.hpp`
- `ArcaneTests/src/SweepRenderLookTest.cpp:17` → `Arcane/Render/VisibilitySystem.hpp`
- `ArcaneTests/src/SweepShaderTest.cpp:3` → `Arcane/Render/RenderShaderSettings.hpp`
- `ArcaneTests/src/ViewportGridTest.cpp:21` → `Arcane/Render/Batcher2D.hpp`
- `ArcaneTests/src/VisibilityTest.cpp:4` → `Arcane/Render/VisibilitySystem.hpp`
- `ArcaneTests/src/test_main.cpp:14` → `Arcane/Render/AgilitySdk.hpp`

#### ArcaneScene (221 sites)

- `ArcaneTests/src/AssetBrowserTest.cpp:16` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/AssetFileOpsTest.cpp:19` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/AssetFileOpsTest.cpp:20` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/AssetReferenceFieldTest.cpp:21` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/AssetReferenceFieldTest.cpp:22` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/AssetReferencesTest.cpp:14` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/AssetReferencesTest.cpp:15` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/AssetReferencesTest.cpp:16` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/AuthoredTransformSyncTest.cpp:11` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/AuthoredTransformSyncTest.cpp:12` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/AuthoredTransformSyncTest.cpp:8` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/BoundsSystemTest.cpp:10` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/BoundsSystemTest.cpp:11` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/BoundsSystemTest.cpp:7` → `Arcane/Scene/BoundsSystem.hpp`
- `ArcaneTests/src/BoundsSystemTest.cpp:8` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/BoundsSystemTest.cpp:9` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/ChangeDetectionMarksTest.cpp:10` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/ChangeDetectionMarksTest.cpp:11` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/ChangeDetectionMarksTest.cpp:12` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/ChangeDetectionMarksTest.cpp:9` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/ClientRuntimeTest.cpp:17` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/CommandStackTest.cpp:21` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/CommandStackTest.cpp:22` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EcsFacadeTest.cpp:7` → `Arcane/Ecs.hpp`
- `ArcaneTests/src/EditModeScheduleTest.cpp:6` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EditModeScheduleTest.cpp:7` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EditModeScheduleTest.cpp:8` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/EditModeScheduleTest.cpp:9` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/EditorCameraTest.cpp:31` → `Arcane/Scene/BoundsSystem.hpp`
- `ArcaneTests/src/EditorCameraTest.cpp:32` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EditorCameraTest.cpp:33` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EditorCameraTest.cpp:34` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/EditorComponentCatalogTest.cpp:12` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EditorComponentCatalogTest.cpp:14` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EditorEntityListTest.cpp:12` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EditorInspectorColorTest.cpp:11` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EditorInspectorColorTest.cpp:12` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EditorInspectorTest.cpp:12` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EditorInspectorTest.cpp:14` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EditorInspectorVectorTest.cpp:15` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EditorInspectorVectorTest.cpp:17` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EditorOutlinerTest.cpp:13` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EditorOutlinerTest.cpp:14` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:40` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:43` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:44` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:47` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/EditorSceneSessionTest.cpp:14` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EditorSceneSessionTest.cpp:15` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:18` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:19` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:20` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:21` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/EntityClipboardTest.cpp:22` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/EntityIdentityTest.cpp:11` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EntityIdentityTest.cpp:13` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/EntityIdentityTest.cpp:14` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EntityIdentityTest.cpp:15` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/EntityIdentityTest.cpp:16` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/EntityOpsTest.cpp:11` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/EntityOpsTest.cpp:12` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/EntityOpsTest.cpp:13` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/EntityOpsTest.cpp:14` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/EntityOpsTest.cpp:15` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/FrustumTest.cpp:1` → `Arcane/Scene/Frustum.hpp`
- `ArcaneTests/src/FrustumTest.cpp:2` → `Arcane/Scene/ViewTransform.hpp`
- `ArcaneTests/src/FrustumTest.cpp:3` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/GizmoTest.cpp:11` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/GizmoTest.cpp:12` → `Arcane/Scene/ViewTransform.hpp`
- `ArcaneTests/src/GizmoTest.cpp:13` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/GoldenImageTest.cpp:17` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:57` → `Arcane/Scene/BoundsSystem.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:58` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:59` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:60` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:61` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:62` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/GpuSceneCullTest.cpp:63` → `Arcane/Scene/ViewTransform.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:12` → `Arcane/Scene/BoundsSystem.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:13` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:14` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:15` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:16` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/GpuSceneSyncTest.cpp:17` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:40` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneTests/src/GridNodeTest.cpp:41` → `Arcane/Scene/ViewTransform.hpp`
- `ArcaneTests/src/HostBootTest.cpp:16` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/HostBootTest.cpp:17` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/HostBootTest.cpp:18` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/MaterialTest.cpp:15` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/MeshAssetTest.cpp:295` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/MeshAssetTest.cpp:296` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/MeshCullNodeTest.cpp:6` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:39` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:40` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/MeshNodeTest.cpp:40` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneTests/src/MeshSubmissionTest.cpp:29` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/NamespaceFacadeTest.cpp:9` → `Arcane/Ecs.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:68` → `Arcane/Scene/BoundsSystem.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:69` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:70` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:71` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:72` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:83` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:84` → `Arcane/Scene/ViewTransform.hpp`
- `ArcaneTests/src/NriGraphPixelTest.cpp:85` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/PerspectiveCameraTest.cpp:21` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneTests/src/PerspectiveCameraTest.cpp:22` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/Physics2DTest.cpp:11` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/Physics2DTest.cpp:8` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/PhysicsComponentsTest.cpp:20` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/PhysicsDebugRichTest.cpp:28` → `Arcane/Scene/ViewTransform.hpp`
- `ArcaneTests/src/PhysicsDebugRichTest.cpp:29` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/PhysicsEvents2DTest.cpp:11` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/PhysicsEvents2DTest.cpp:12` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/PhysicsEvents2DTest.cpp:13` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/PhysicsEvents2DTest.cpp:7` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/PhysicsFrameEventsTest.cpp:11` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/PhysicsFrameEventsTest.cpp:8` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/PhysicsPauseTest.cpp:10` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/PhysicsPauseTest.cpp:8` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/PhysicsSystemTest.cpp:24` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/PhysicsSystemTest.cpp:27` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/PhysicsSystemTest.cpp:28` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/PhysicsSystemTest.cpp:29` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:47` → `Arcane/Scene/BoundsSystem.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:48` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:51` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:52` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:53` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/PickBufferTest.cpp:54` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/PluginHostTest.cpp:12` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/ProcessContextTest.cpp:12` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/ProcessContextTest.cpp:13` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp:25` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp:27` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/ReflectionJsonTest.cpp:8` → `Arcane/Serialization/ReflectionJson.hpp`
- `ArcaneTests/src/RegistryStateCommandTest.cpp:13` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/RegistryStateCommandTest.cpp:14` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/RegistryStateCommandTest.cpp:15` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/RenderInterpolationTest.cpp:16` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/RenderInterpolationTest.cpp:17` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/RenderInterpolationTest.cpp:26` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/RenderInterpolationTest.cpp:29` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/RenderInterpolationTest.cpp:30` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/ResourceSwapTest.cpp:14` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/RuntimeEngineSystemsTest.cpp:21` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:13` → `Arcane/Scene/BoundsSystem.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:14` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:18` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:19` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:20` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:21` → `Arcane/Serialization/RegistrySnapshot.hpp`
- `ArcaneTests/src/RuntimeTest.cpp:22` → `Arcane/Serialization/ResourceSerialization.hpp`
- `ArcaneTests/src/SceneAssetTest.cpp:10` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/SceneAssetTest.cpp:7` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/SceneAssetTest.cpp:8` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/SceneAssetTest.cpp:9` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/SceneCameraTest.cpp:11` → `Arcane/Scene/SceneCamera.hpp`
- `ArcaneTests/src/SceneCameraTest.cpp:12` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/SceneCameraTest.cpp:13` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/SceneComponentsTest.cpp:7` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/SceneComponentsTest.cpp:8` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/SceneJsonTest.cpp:10` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/SceneJsonTest.cpp:11` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/SceneJsonTest.cpp:12` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/SceneJsonTest.cpp:13` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/SceneJsonTest.cpp:8` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/SceneMigrationTest.cpp:18` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/SceneMigrationTest.cpp:20` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/SceneMigrationTest.cpp:21` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/SceneMigrationTest.cpp:22` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/SceneMigrationTest.cpp:23` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/SceneNumberRangeTest.cpp:10` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/SceneNumberRangeTest.cpp:11` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/SceneNumberRangeTest.cpp:12` → `Arcane/Serialization/ReflectionJson.hpp`
- `ArcaneTests/src/SceneNumberRangeTest.cpp:13` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:37` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/SceneRenderResolverTest.cpp:38` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/SceneUnknownFieldTest.cpp:10` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/SceneUnknownFieldTest.cpp:11` → `Arcane/Serialization/ReflectionJson.hpp`
- `ArcaneTests/src/SceneUnknownFieldTest.cpp:12` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/SceneUnknownFieldTest.cpp:9` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/SelectionOpsTest.cpp:10` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/SelectionOpsTest.cpp:11` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/SelectionOpsTest.cpp:12` → `Arcane/Serialization/SceneAsset.hpp`
- `ArcaneTests/src/SerializationNegativeTest.cpp:13` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/SerializationNegativeTest.cpp:14` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/SerializationNegativeTest.cpp:15` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/SerializationNegativeTest.cpp:16` → `Arcane/Serialization/ReflectionJson.hpp`
- `ArcaneTests/src/SerializationNegativeTest.cpp:17` → `Arcane/Serialization/RegistrySnapshot.hpp`
- `ArcaneTests/src/SerializationNegativeTest.cpp:18` → `Arcane/Serialization/ResourceSerialization.hpp`
- `ArcaneTests/src/SerializationNegativeTest.cpp:19` → `Arcane/Serialization/SceneSerializer.hpp`
- `ArcaneTests/src/SpriteRotationTest.cpp:27` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/SpriteRotationTest.cpp:30` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/SpriteRotationTest.cpp:31` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/SpriteRotationTest.cpp:32` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/SweepGizmoTest.cpp:13` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/SweepGizmoTest.cpp:14` → `Arcane/Scene/ViewTransform.hpp`
- `ArcaneTests/src/SweepPhysicsQueryTest.cpp:12` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:10` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:13` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/TransformOrderTest.cpp:32` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/TransformOrderTest.cpp:33` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/TransformOrderTest.cpp:34` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/TransformOrderTest.cpp:35` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/TransformPropagationTest.cpp:10` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/TransformPropagationTest.cpp:11` → `Arcane/Scene/SceneResources.hpp`
- `ArcaneTests/src/TransformPropagationTest.cpp:12` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/TransformPropagationTest.cpp:13` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/TransformSpineTest.cpp:17` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/ViewTransformTest.cpp:3` → `Arcane/Scene/ViewTransform.hpp`
- `ArcaneTests/src/ViewTransformTest.cpp:4` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/ViewportGridTest.cpp:22` → `Arcane/Scene/ViewTransform.hpp`
- `ArcaneTests/src/ViewportGridTest.cpp:23` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/VisibilityTest.cpp:10` → `Arcane/Scene/TransformSystems.hpp`
- `ArcaneTests/src/VisibilityTest.cpp:5` → `Arcane/Scene/BoundsSystem.hpp`
- `ArcaneTests/src/VisibilityTest.cpp:6` → `Arcane/Scene/Components.hpp`
- `ArcaneTests/src/VisibilityTest.cpp:7` → `Arcane/Scene/RenderViewSettings.hpp`
- `ArcaneTests/src/VisibilityTest.cpp:8` → `Arcane/Scene/SceneModule.hpp`
- `ArcaneTests/src/VisibilityTest.cpp:9` → `Arcane/Scene/SceneResources.hpp`

#### ArcaneSim (20 sites)

- `ArcaneTests/src/ClassTemplatesTest.cpp:16` → `Arcane/Sim/Time.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:45` → `Arcane/Sim/SimSettings.hpp`
- `ArcaneTests/src/EditorPlayModeTest.cpp:46` → `Arcane/Sim/Time.hpp`
- `ArcaneTests/src/MultiRuntimeReloadTest.cpp:9` → `Arcane/Sim/NetDriver.hpp`
- `ArcaneTests/src/ParamSystemRegistrationTest.cpp:10` → `Arcane/Sim/SystemSchedulers.hpp`
- `ArcaneTests/src/ParamSystemRegistrationTest.cpp:11` → `Arcane/Sim/Time.hpp`
- `ArcaneTests/src/ParamSystemRegistrationTest.cpp:9` → `Arcane/Sim/RunLoop.hpp`
- `ArcaneTests/src/PhysicsFrameEventsTest.cpp:12` → `Arcane/Sim/Time.hpp`
- `ArcaneTests/src/PhysicsSystemTest.cpp:31` → `Arcane/Sim/RunLoop.hpp`
- `ArcaneTests/src/PhysicsSystemTest.cpp:32` → `Arcane/Sim/SystemSchedulers.hpp`
- `ArcaneTests/src/ResourceSwapTest.cpp:15` → `Arcane/Sim/Time.hpp`
- `ArcaneTests/src/RoleMaskTest.cpp:11` → `Arcane/Sim/SystemSchedulers.hpp`
- `ArcaneTests/src/RunLoopTest.cpp:191` → `Arcane/Sim/Time.hpp`
- `ArcaneTests/src/RunLoopTest.cpp:6` → `Arcane/Sim/RunLoop.hpp`
- `ArcaneTests/src/RunLoopTest.cpp:7` → `Arcane/Sim/SystemSchedulers.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:20` → `Arcane/Sim/RunLoop.hpp`
- `ArcaneTests/src/SettingsBindingsTest.cpp:21` → `Arcane/Sim/SimSettings.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:17` → `Arcane/Sim/RunLoop.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:18` → `Arcane/Sim/SimSettings.hpp`
- `ArcaneTests/src/SweepSimTest.cpp:19` → `Arcane/Sim/SystemSchedulers.hpp`

#### ArcaneVerify (14 sites)

- `ArcaneTests/src/Helpers/HostWitness.hpp:23` → `Arcane/Host/Verdict.hpp`
- `ArcaneTests/src/HostWitnessTest.cpp:10` → `Arcane/Host/Verdict.hpp`
- `ArcaneTests/src/MeshDocumentTest.cpp:34` → `Arcane/Host/OffscreenVehicle.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:52` → `Arcane/Host/OffscreenVehicle.hpp`
- `ArcaneTests/src/MeshThumbnailHarvestTest.cpp:70` → `Arcane/Host/ReferenceImages.hpp`
- `ArcaneTests/src/NriMeshBufferCacheTest.cpp:11` → `Arcane/Host/OffscreenVehicle.hpp`
- `ArcaneTests/src/OffscreenVehicleTest.cpp:2` → `Arcane/Host/OffscreenVehicle.hpp`
- `ArcaneTests/src/ReferenceImagesTest.cpp:5` → `Arcane/Host/ReferenceImages.hpp`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:28` → `Arcane/Host/OffscreenVehicle.hpp`
- `ArcaneTests/src/VerdictTest.cpp:18` → `Arcane/Host/Verdict.hpp`
- `ArcaneTests/src/VerdictTest.cpp:19` → `Arcane/Host/VerifyReport.hpp`
- `ArcaneTests/src/VerifyReportTest.cpp:6` → `Arcane/Host/VerifyReport.hpp`
- `ArcaneTests/src/WitnessScenariosTest.cpp:60` → `Arcane/Host/Verdict.hpp`
- `ArcaneTests/src/test_main.cpp:12` → `Arcane/Host/VerifyHooks.hpp`

#### ArcaneWindow (4 sites)

- `ArcaneTests/src/InputDevicesTest.cpp:9` → `Arcane/Platform/Window.hpp`
- `ArcaneTests/src/NriHostFlavorTest.cpp:28` → `Arcane/Platform/Window.hpp`
- `ArcaneTests/src/SweepHostTest.cpp:5` → `Arcane/Platform/Window.hpp`
- `ArcaneTests/src/WindowTest.cpp:7` → `Arcane/Platform/Window.hpp`

#### ImGui (83 sites)

- `ArcaneTests/src/AppearanceApplierTest.cpp:10` → `imgui_internal.h`
- `ArcaneTests/src/AppearanceApplierTest.cpp:9` → `imgui.h`
- `ArcaneTests/src/AssetBrowserPanelTest.cpp:20` → `imgui.h`
- `ArcaneTests/src/AssetBrowserPanelTest.cpp:21` → `imgui_internal.h`
- `ArcaneTests/src/AssetBrowserPanelTest.cpp:28` → `ImGuiTestKeys.hpp`
- `ArcaneTests/src/AssetBrowserRevealTest.cpp:24` → `imgui.h`
- `ArcaneTests/src/AssetBrowserRevealTest.cpp:25` → `imgui_internal.h`
- `ArcaneTests/src/AssetCreateInstanceTest.cpp:25` → `imgui.h`
- `ArcaneTests/src/AssetCreateInstanceTest.cpp:26` → `imgui_internal.h`
- `ArcaneTests/src/AssetInspectorSourceTest.cpp:25` → `imgui.h`
- `ArcaneTests/src/AssetInspectorSourceTest.cpp:26` → `imgui_internal.h`
- `ArcaneTests/src/AssetReferenceFieldTest.cpp:27` → `imgui.h`
- `ArcaneTests/src/AssetReferenceFieldTest.cpp:28` → `imgui_internal.h`
- `ArcaneTests/src/AssetStatusPanelClickTest.cpp:89` → `imgui.h`
- `ArcaneTests/src/AssetStatusPanelClickTest.cpp:94` → `imgui_internal.h`
- `ArcaneTests/src/AssetsGraphCanvasTest.cpp:45` → `imgui.h`
- `ArcaneTests/src/AssetsGraphCanvasTest.cpp:56` → `imgui_internal.h`
- `ArcaneTests/src/AxisColorsTest.cpp:15` → `imgui.h`
- `ArcaneTests/src/ConsoleInputLineTest.cpp:5` → `imgui.h`
- `ArcaneTests/src/DockTabInsetTest.cpp:10` → `imgui.h`
- `ArcaneTests/src/DockTabInsetTest.cpp:11` → `imgui_internal.h`
- `ArcaneTests/src/DocumentPageSelectionTest.cpp:5` → `imgui.h`
- `ArcaneTests/src/EditorActionsTest.cpp:9` → `imgui.h`
- `ArcaneTests/src/EditorFontsTest.cpp:7` → `imgui.h`
- `ArcaneTests/src/EditorFontsTest.cpp:8` → `imgui_internal.h`
- `ArcaneTests/src/EditorInspectorColorTest.cpp:17` → `imgui.h`
- `ArcaneTests/src/EditorInspectorColorTest.cpp:18` → `imgui_internal.h`
- `ArcaneTests/src/EditorInspectorHostTest.cpp:16` → `imgui.h`
- `ArcaneTests/src/EditorInspectorHostTest.cpp:17` → `imgui_internal.h`
- `ArcaneTests/src/EditorInspectorHostTest.cpp:24` → `ImGuiTestKeys.hpp`
- `ArcaneTests/src/EditorInspectorVectorTest.cpp:23` → `imgui.h`
- `ArcaneTests/src/EditorSeverityToggleTest.cpp:7` → `imgui.h`
- `ArcaneTests/src/EditorSeverityToggleTest.cpp:8` → `imgui_internal.h`
- `ArcaneTests/src/EditorThemeContrastTest.cpp:10` → `imgui.h`
- `ArcaneTests/src/EditorThemeSettingsTest.cpp:7` → `imgui.h`
- `ArcaneTests/src/EditorWidgetsTest.cpp:12` → `imgui.h`
- `ArcaneTests/src/EditorWidgetsTest.cpp:13` → `imgui_internal.h`
- `ArcaneTests/src/FontsPageTest.cpp:10` → `imgui_internal.h`
- `ArcaneTests/src/FontsPageTest.cpp:9` → `imgui.h`
- `ArcaneTests/src/GraphCanvasHeadlessTest.cpp:36` → `imgui.h`
- `ArcaneTests/src/GraphCanvasHeadlessTest.cpp:37` → `imgui_internal.h`
- `ArcaneTests/src/GraphCanvasHeadlessTest.cpp:50` → `ImGuiTestKeys.hpp`
- `ArcaneTests/src/Helpers/NodePageDocs.hpp:12` → `imgui.h`
- `ArcaneTests/src/Helpers/SettingsFixtures.hpp:9` → `imgui.h`
- `ArcaneTests/src/Helpers/SoftRaster.hpp:18` → `imgui.h`
- `ArcaneTests/src/ImGuiTest.cpp:15` → `imgui.h`
- `ArcaneTests/src/ImGuiTestKeys.hpp:3` → `imgui.h`
- `ArcaneTests/src/InputActionsDocumentUiTest.cpp:18` → `imgui.h`
- `ArcaneTests/src/MeshDocumentTest.cpp:47` → `imgui.h`
- `ArcaneTests/src/MeshDocumentTest.cpp:48` → `imgui_internal.h`
- `ArcaneTests/src/NriGraphPixelTest.cpp:112` → `imgui.h`
- `ArcaneTests/src/NriHostFlavorTest.cpp:31` → `imgui.h`
- `ArcaneTests/src/ProjectSettingsPageTest.cpp:10` → `imgui.h`
- `ArcaneTests/src/PropertyGridLabelHookTest.cpp:8` → `imgui.h`
- `ArcaneTests/src/PropertyGridTest.cpp:13` → `imgui.h`
- `ArcaneTests/src/PropertyGridTest.cpp:14` → `imgui_internal.h`
- `ArcaneTests/src/PropertyGridTest.cpp:25` → `ImGuiTestKeys.hpp`
- `ArcaneTests/src/RenderGraphTest.cpp:51` → `imgui.h`
- `ArcaneTests/src/SettingsHostTest.cpp:18` → `imgui.h`
- `ArcaneTests/src/SettingsHostTest.cpp:19` → `imgui_internal.h`
- `ArcaneTests/src/SettingsRowsTest.cpp:12` → `imgui.h`
- `ArcaneTests/src/SettingsWindowTest.cpp:10` → `imgui_internal.h`
- `ArcaneTests/src/SettingsWindowTest.cpp:9` → `imgui.h`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:39` → `imgui.h`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:40` → `imgui_internal.h`
- `ArcaneTests/src/ShaderEditorDocumentTest.cpp:54` → `ImGuiTestKeys.hpp`
- `ArcaneTests/src/ShaderGraphPinTypesTest.cpp:14` → `imgui.h`
- `ArcaneTests/src/ShortcutRoutingDocumentsTest.cpp:5` → `imgui.h`
- `ArcaneTests/src/SpriteDocumentUndoTest.cpp:40` → `imgui.h`
- `ArcaneTests/src/SpriteDocumentUndoTest.cpp:41` → `imgui_internal.h`
- `ArcaneTests/src/SweepGraphCanvasTest.cpp:17` → `imgui.h`
- `ArcaneTests/src/SweepGraphThemeTest.cpp:19` → `imgui.h`
- `ArcaneTests/src/SweepRenderBudgetTest.cpp:20` → `imgui.h`
- `ArcaneTests/src/SweepThemePaletteTest.cpp:9` → `imgui.h`
- `ArcaneTests/src/SweepThemePaletteTest.cpp:91` → `imgui_internal.h`
- `ArcaneTests/src/SweepUiStyleTest.cpp:16` → `imgui.h`
- `ArcaneTests/src/SweepUiStyleTest.cpp:17` → `imgui_internal.h`
- `ArcaneTests/src/SweepViewportStoreTest.cpp:10` → `imgui.h`
- `ArcaneTests/src/ThemePageTest.cpp:16` → `imgui.h`
- `ArcaneTests/src/ThemePageTest.cpp:17` → `imgui_internal.h`
- `ArcaneTests/src/ThemePresetsTest.cpp:9` → `imgui.h`
- `ArcaneTests/src/UiMetricsTest.cpp:7` → `imgui.h`
- `ArcaneTests/src/ViewportSettingsTest.cpp:14` → `imgui.h`

#### ImGuiNodeEditor (7 sites)

- `ArcaneTests/src/AssetsGraphCanvasTest.cpp:52` → `imgui_node_editor.h`
- `ArcaneTests/src/GraphCanvasHeadlessTest.cpp:15` → `imgui_node_editor_internal.h`
- `ArcaneTests/src/GraphCanvasHeadlessTest.cpp:38` → `imgui_node_editor.h`
- `ArcaneTests/src/GraphFitTest.cpp:12` → `imgui_node_editor_internal.h`
- `ArcaneTests/src/ShortcutRoutingDocumentsTest.cpp:6` → `imgui_node_editor.h`
- `ArcaneTests/src/SweepGraphCanvasTest.cpp:18` → `imgui_node_editor.h`
- `ArcaneTests/src/SweepGraphThemeTest.cpp:16` → `imgui_node_editor.h`

### ReferenceProject/Source

#### ArcaneClient (1 sites)

- `ReferenceProject/Source/Game/ReferenceGame.cpp:2` → `Arcane/Client/ClientRuntime.hpp`

#### ArcaneCore (5 sites)

- `ReferenceProject/Source/Game/PlayerController2D.hpp:11` → `Arcane/Reflection.hpp`
- `ReferenceProject/Source/Game/ReferenceGame.cpp:3` → `Arcane/Base/Log.hpp`
- `ReferenceProject/Source/Game/ReferenceGame.cpp:4` → `Arcane/Config/CVarDecl.hpp`
- `ReferenceProject/Source/Game/ReferenceGame.cpp:5` → `Arcane/Config/CVarRegistry.hpp`
- `ReferenceProject/Source/Game/TintOnContact.hpp:7` → `Arcane/Reflection.hpp`

#### ArcaneEngine (5 sites)

- `ReferenceProject/Source/Game/PlayerController2D.cpp:3` → `Arcane/Plugin/GameComponents.hpp`
- `ReferenceProject/Source/Game/PlayerController2DSystem.cpp:3` → `Arcane/Plugin/GameSystems.hpp`
- `ReferenceProject/Source/Game/ReferenceGame.cpp:1` → `Arcane/Plugin/GameModule.hpp`
- `ReferenceProject/Source/Game/TintOnContact.cpp:3` → `Arcane/Plugin/GameComponents.hpp`
- `ReferenceProject/Source/Game/TintOnContactSystem.cpp:3` → `Arcane/Plugin/GameSystems.hpp`

#### ArcaneInput (1 sites)

- `ReferenceProject/Source/Game/PlayerController2DSystem.hpp:15` → `Arcane/Input/GameInput.hpp`

#### ArcanePhysics2D (2 sites)

- `ReferenceProject/Source/Game/PlayerController2DSystem.hpp:16` → `Arcane/Physics2D.hpp`
- `ReferenceProject/Source/Game/TintOnContactSystem.hpp:9` → `Arcane/Physics2D.hpp`

#### ArcaneScene (3 sites)

- `ReferenceProject/Source/Game/PlayerController2DSystem.hpp:14` → `Arcane/Ecs.hpp`
- `ReferenceProject/Source/Game/TintOnContactSystem.hpp:7` → `Arcane/Ecs.hpp`
- `ReferenceProject/Source/Game/TintOnContactSystem.hpp:8` → `Arcane/Scene/Components.hpp`

#### ArcaneSim (1 sites)

- `ReferenceProject/Source/Game/PlayerController2DSystem.hpp:17` → `Arcane/Sim/Time.hpp`

## P1 NOTES

`GameModule.hpp` still has a direct `imgui.h` include under `ARC_GAME_HAS_IMGUI`;
this textual sweep reports it under ArcaneEngine. Its conditional is enabled for
in-tree game modules in P1 and becomes closure driven in P4. The new
ArcaneImGui -> ArcaneClient dependency is the HostOverlay implementation edge.
The `Arcane/Base/Api.hpp` include suppression is retained for its P2 retirement.

## VERDICT

UNASSIGNED none; MISSING DEP none; TYPE VIOLATIONS none; CYCLES none.
