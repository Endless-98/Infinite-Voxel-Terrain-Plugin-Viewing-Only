// Copyright(c) 2024 Endless98. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameMode.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFilemanager.h"
#include "VoxelMenuGameMode.generated.h"

/**
 * 
 */
UCLASS()
class INFINITEVOXELTERRAINPLUGIN_API AVoxelMenuGameMode : public AGameMode
{
	GENERATED_BODY()

	UFUNCTION(BlueprintCallable, Category = "World Save")
	TArray<FString> GetAllWorldSaveNames();
	UFUNCTION(BlueprintCallable, Category = "World Save")
	bool CreateWorldSave(const FString& NewWorldSaveName, const FTerrainSettings& TerrainSettings);
};
