// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealAgentLink : ModuleRules
{
	public UnrealAgentLink(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Json",
				"JsonUtilities",
				"WebSockets"
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"InputCore",
				"EditorFramework",
				"UnrealEd",
				"Kismet",
				"KismetCompiler",
				"AssetTools",
				"AssetRegistry",
				"ToolMenus",
				"CoreUObject",
				"Engine",
				"PythonScriptPlugin",
				"ContentBrowser",
				"Slate",
				"SlateCore",
			}
			);

		// UE 5.1+ 可能需要额外模块，这里预留动态依赖
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 1)
		{
			// 示例：可在此添加 5.1+ 特有模块
		}
	}
}
