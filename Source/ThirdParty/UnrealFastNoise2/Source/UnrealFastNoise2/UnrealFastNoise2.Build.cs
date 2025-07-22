// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class UnrealFastNoise2 : ModuleRules
{
	public UnrealFastNoise2(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Engine",
				"Projects",
				"FastNoise2",
			}
		);


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject"
			}
		);

        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));

   	    PublicIncludePaths.Add(Path.Combine(PluginDirectory, "Source", "ThirdParty", "UnrealFastNoise2", "Source", "UnrealFastNoise2"));
        PublicIncludePaths.Add(Path.Combine(PluginDirectory, "Source", "ThirdParty", "UnrealFastNoise2", "Source", "UnrealFastNoise2", "Public"));
        PublicIncludePaths.Add(Path.Combine(PluginDirectory, "Source", "ThirdParty", "UnrealFastNoise2", "Source", "UnrealFastNoise2", "Public", "FastNoise2"));
        PublicIncludePaths.Add(Path.Combine(PluginDirectory, "Source", "ThirdParty", "UnrealFastNoise2", "Source", "ThirdParty", "FastNoise2", "include", "FastNoise"));
    }
}
