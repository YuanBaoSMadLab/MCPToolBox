## 已发现MCP工具 (55个)

直接 `call_tool(toolset_name="...", tool_name="...", arguments={})` 调用。

- **EditorToolset.EditorAppToolset**: WorldPosToScreenCoords, StopPIE, StartPIE, SetContentBrowserPath, SetCameraTransform, SelectAssets, SelectActors, SearchCVars, ScreenCoordsToWorld, OpenEditorForAsset, IsPIERunning, GetVisibleActors, GetSelectedAssets, GetSelectedActors, GetOpenAssets, GetContentBrowserPath, GetCameraTransform, FocusOnActors, CaptureViewport, CaptureEditorImage, CaptureAssetImage
- **EditorToolset.LogsToolset**: SetVerbosity, GetVerbosity, GetLogEntries, GetLogCategories
- **GameFeaturesToolset.GameFeaturesToolset**: RequestDeactivateGameFeature, RequestActivateGameFeature, ListEnabledGameFeaturePlugins, ListDiscoveredGameFeaturePlugins, IsGameFeaturePlugin, IsGameFeatureActive, GetGameFeatureState
- **PhysicsToolsets.PhysicsAssetToolset**: SetSphere, SetConstraintLimits, SetCapsule, SetBox, SetBodyPhysicsMode, SetBodyMassScale, RemoveShape, RemoveConstraint, RemoveBody, GetConstraints, GetBodyShapes, GetBodyPhysicsMode, GetBodyNames, GetBodyMassScale, CreateFromMesh, AddConstraint, AddBody
- **ToolsetRegistry.AgentSkillToolset**: UpdateSkill, ListSkills, GetSkills, CreateSkill
- **editor_toolset.toolsets.programmatic.ProgrammaticToolset**: execute_tool_script, get_execution_environment