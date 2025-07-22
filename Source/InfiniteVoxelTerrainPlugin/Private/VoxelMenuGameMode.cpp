// Copyright(c) 2024 Endless98. All Rights Reserved.

#include "VoxelMenuGameMode.h"
#include "ChunkManager.h"

TArray<FString> AVoxelMenuGameMode::GetAllWorldSaveNames()
{
	TArray<FString> SaveFolderNames{};

	FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), "SaveGames/WorldSaves");

	if (FPaths::DirectoryExists(SavePath))
	{
		TArray<FString> SubDirectoryNames;
		IFileManager::Get().FindFilesRecursive(SubDirectoryNames, *SavePath, TEXT("*"), false, true);

		for (const FString& SubDirectoryName : SubDirectoryNames)
		{
			if (!IFileManager::Get().DirectoryExists(*SubDirectoryName))
				continue;

			FString FolderName = FPaths::GetPathLeaf(SubDirectoryName);
			SaveFolderNames.Add(FolderName);
		}
	}
	else
		UE_LOG(LogTemp, Warning, TEXT("Save directory does not exist: %s"), *SavePath);

	return SaveFolderNames;
}

// Called in UI Blueprints on main menu
bool AVoxelMenuGameMode::CreateWorldSave(const FString& NewWorldSaveName, const FTerrainSettings& TerrainSettings)
{
	const FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames/WorldSaves/"), NewWorldSaveName, TEXT("TerrainSettings.json"));
	// Check if save path exists already:
	if (FPaths::FileExists(SavePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("Save already exists with name: %s"), *NewWorldSaveName);
		return false;
	}

	AChunkManager::SaveTerrainSettings(TerrainSettings, NewWorldSaveName);
	return true;
}