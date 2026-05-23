// Copyright (c) 2015-2025 TriAxis Games, L.L.C. All Rights Reserved.

#include "RealtimeMeshComponentModule.h"
#include "Serialization/CustomVersion.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
#include "RealtimeMeshCore.h"


// Register the custom version with core
FCustomVersionRegistration GRegisterRealtimeMeshCustomVersion(RealtimeMesh::FRealtimeMeshVersion::GUID, RealtimeMesh::FRealtimeMeshVersion::LatestVersion, TEXT("RealtimeMesh"));


class FRealtimeMeshComponentPlugin : public IRealtimeMeshComponentPlugin
{
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

IMPLEMENT_MODULE(FRealtimeMeshComponentPlugin, RealtimeMeshComponent)


void FRealtimeMeshComponentPlugin::StartupModule()
{
	const TSharedPtr<IPlugin> HostPlugin = IPluginManager::Get().FindPlugin(TEXT("InfiniteVoxelTerrainPlugin"));
	if (!HostPlugin.IsValid())
	{
		UE_LOG(LogRealtimeMesh, Warning, TEXT("Unable to locate InfiniteVoxelTerrainPlugin for shader directory mapping."));
		return;
	}

	const FString PluginShaderDir = FPaths::Combine(HostPlugin->GetBaseDir(), TEXT("Shaders"));
	if (FPaths::DirectoryExists(PluginShaderDir))
	{
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/InfiniteVoxelTerrainPlugin"), PluginShaderDir);
	}
	else
	{
		UE_LOG(LogRealtimeMesh, Verbose, TEXT("Shader directory not found: %s"), *PluginShaderDir);
	}
}

void FRealtimeMeshComponentPlugin::ShutdownModule()
{
}

DEFINE_LOG_CATEGORY(LogRealtimeMesh);
