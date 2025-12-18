// Copyright Epic Games, Inc. All Rights Reserved.
// Rebuild triggered
// Rebuild triggered

using UnrealBuildTool;

public class UnrealAgentLink : ModuleRules
{
	public UnrealAgentLink(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		// 添加子目录到 include 路径
		PublicIncludePaths.AddRange(
			new string[]
			{
				System.IO.Path.Combine(ModuleDirectory, "Public/Core"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Commands"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Utils"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Network"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Extensions"),
			}
		);
		
		PublicIncludePathModuleNames.AddRange(
			new string[] {
				"BlueprintGraph",
				"GraphEditor",
				"KismetCompiler",
				"UnrealEd"
			}
		);
		
		PrivateIncludePaths.AddRange(
			new string[]
			{
				System.IO.Path.Combine(ModuleDirectory, "Private/Core"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Commands"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Utils"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Network"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Extensions"),
			}
		);
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Json",
				"JsonUtilities",
				"WebSockets",
				"BlueprintGraph",
				"UnrealEd",
				"KismetCompiler",
				"GraphEditor" // Ensure editor graph headers are available
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"InputCore",
				"EditorFramework",
				"EditorFramework",
				// "UnrealEd", // Moved to Public
				"Kismet",
				// "KismetCompiler", // Moved to Public
				// "BlueprintGraph", // Moved to Public
				"AssetTools",
				"AssetRegistry",
				"ToolMenus",
				"CoreUObject",
				"Engine",
				"PhysicsCore",
				"PythonScriptPlugin",
				"ContentBrowser",
				"Slate",
				"SlateCore",
				"RenderCore",
				"RHI",
				"GameProjectGeneration",
				"PropertyEditor", // Added for FPropertyEditorModule
				"MaterialEditor", // Added for FMaterialEditorUtilities
				"MediaAssets", // Added for FileMediaSource support
				"ImageWrapper",  // Added for screenshot support
				"UMG" // Added for FWidgetRenderer
			}
			);

		// UE 5.1+ 可能需要额外模块，这里预留动态依赖
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 1)
		{
			// 示例：可在此添加 5.1+ 特有模块
		}
	}
}
