// Copyright(c) 2024 Endless98. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ChunkActor.h"
#include "Components/ActorComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "Containers/UnrealString.h"
#include "GameFramework/PlayerController.h"
#include "ChunkModifierComponent.generated.h"

class AChunkManager;
struct FRegionData;
struct FTerrainSettings;

USTRUCT()
struct FChunkNameData
{
	GENERATED_BODY()

public:

	FChunkNameData() = default;

	FChunkNameData(FIntVector CenterCell) 
		: CenterCell(CenterCell)
	{}

	// Copy constructor
	FChunkNameData(const FChunkNameData& Other)
		: CenterCell(Other.CenterCell)
		, ChunkRepCells(Other.ChunkRepCells)
		, ChunkRepCounts(Other.ChunkRepCounts)
	{}

	// Assignment operator overload
	FChunkNameData& operator=(const FChunkNameData& Other)
	{
		if (this != &Other)
		{
			CenterCell = Other.CenterCell;
			ChunkRepCells = Other.ChunkRepCells;
			ChunkRepCounts = Other.ChunkRepCounts;
		}
		return *this;
	}

	UPROPERTY()
	FIntVector CenterCell{};

	UPROPERTY()
	TArray<FIntVector> ChunkRepCells{};
	UPROPERTY()
	TArray<int32> ChunkRepCounts;
};

UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class INFINITEVOXELTERRAINPLUGIN_API UChunkModifierComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	UChunkModifierComponent();

	void BeginPlay() override;

	UFUNCTION(BlueprintCallable, Category = "Set Voxel")
	bool AttemptSetVoxel(FVector StartPoint, FRotator FacingDirection, int32 VoxelValue, int32& OutPreviousVoxelValue, FVector& OutModifiedVoxelLocation);

	bool VoxelLineTrace(FVector StartPoint, FRotator FacingDirection, FVector& OutHitVoxelLocation, FVector& OutHitNormal, AChunkActor*& OutHitChunk);

	bool SetVoxelIfWeHaveRoom(bool bIsEmptyVoxel, const FVector& VoxelLocation, int32 VoxelValue, AChunkActor* HitChunk);

	UFUNCTION(NetMulticast, Reliable, Category = "Set Voxel")
	void MulticastSetVoxel(bool bIsEmptyVoxel, const FVector& VoxelLocation, int32 VoxelValue, AChunkActor* HitChunk);

	UFUNCTION(Server, Reliable, Category = "Set Voxel")
	void ServerSetVoxel(FVector DesiredVoxelLocation, FIntVector ChunkCell, int32 VoxelValue, UChunkModifierComponent* CallingComponent);

	UFUNCTION(Client, Reliable, Category = "Set Voxel")
	void ClientSetVoxel(FVector VoxelLocation, int32 VoxelValue, FIntVector ChunkCell);

	UFUNCTION(Client, Reliable, Category = "Set Voxel")
	void FailedSetVoxel(FVector VoxelLocation, int32 PreviousVoxelValue);

	UFUNCTION(Server, Reliable, Category = "Set Voxel")
	void ServerReadyForReplication();

	bool GetIsReadyForReplication() const { return bIsReadyForReplication; }

	UFUNCTION(Client, Reliable, Category = "Region Data")
	void ClientReceiveRegionData(FRegionData RegionData, bool bIsLastBundle);

	UFUNCTION(Client, Reliable, Category = "Region Data")
	void ClientReceiveChunkNameData(const FChunkNameData& ChunkNameData);

	UFUNCTION(Client, Reliable, Category = "Terrain Settins")
	void ClientReceiveTerrainSettings(FTerrainSettings TerrainSettings);
private:

	UBoxComponent* CollisionCheckerBox;

	bool AreThereAnyOverlappingPawns(const FVector& VoxelLocation, float VoxelSize);

	void GetVoxelLocationFromHitLocation(FVector Normal, FVector HitLocation, bool bIsEmptyVoxel, AChunkActor* HitChunk, FVector& OutVoxelLocation);
	bool bIsReadyForReplication{ false };

	float ReachDistance{ 800 };

	// The higher this value the easier it will be to place voxels close to the player, but the more jarring it could feel when we bump the player
	const float MaxBumpDistance{ 60.0f };
	const float MinBumpDistance{ 5.0f };
	AChunkManager* ChunkManager{};

	void AddOrCombineTempRegionData(FRegionData& RegionData);
	TArray<FRegionData> TempRegionDataBundles{};
};