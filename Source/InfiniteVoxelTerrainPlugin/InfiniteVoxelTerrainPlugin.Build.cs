// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System;
using System.IO;
using System.Runtime.InteropServices;

public class InfiniteVoxelTerrainPlugin : ModuleRules
{
	public InfiniteVoxelTerrainPlugin(ReadOnlyTargetRules Target) : base(Target)
	{
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"RealtimeMeshComponent",
                "UnrealFastNoise2",
                "FastNoise2"
            });

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore"
            });
    }
}