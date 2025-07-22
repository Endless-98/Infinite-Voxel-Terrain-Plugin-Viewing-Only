// Copyright(c) 2024 Endless98. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ChunkActor.h"
#include "VoxelTypesDatabase.h"
#include "Engine/NetDriver.h"
#include "TimerManager.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Net/UnrealNetwork.h"
#include "Engine/EngineTypes.h"
#include "ChunkManager.generated.h"

UENUM(BlueprintType)
enum class ECompassDirection : uint8
{
	North UMETA(DisplayName = "North"),
	Northeast UMETA(DisplayName = "Northeast"),
	East UMETA(DisplayName = "East"),
	Southeast UMETA(DisplayName = "Southeast"),
	South UMETA(DisplayName = "South"),
	Southwest UMETA(DisplayName = "Southwest"),
	West UMETA(DisplayName = "West"),
	Northwest UMETA(DisplayName = "Northwest"),
	Up UMETA(DisplayName = "Up"),
	Down UMETA(DisplayName = "Down"),
	NorthUp UMETA(DisplayName = "NorthUp"),
	NortheastUp UMETA(DisplayName = "NortheastUp"),
	EastUp UMETA(DisplayName = "EastUp"),
	SoutheastUp UMETA(DisplayName = "SoutheastUp"),
	SouthUp UMETA(DisplayName = "SouthUp"),
	SouthwestUp UMETA(DisplayName = "SouthwestUp"),
	WestUp UMETA(DisplayName = "WestUp"),
	NorthwestUp UMETA(DisplayName = "NorthwestUp"),
	NorthDown UMETA(DisplayName = "NorthDown"),
	NortheastDown UMETA(DisplayName = "NortheastDown"),
	EastDown UMETA(DisplayName = "EastDown"),
	SoutheastDown UMETA(DisplayName = "SoutheastDown"),
	SouthDown UMETA(DisplayName = "SouthDown"),
	SouthwestDown UMETA(DisplayName = "SouthwestDown"),
	WestDown UMETA(DisplayName = "WestDown"),
	NorthwestDown UMETA(DisplayName = "NorthwestDown"),
	None UMETA(DisplayName = "None")
};

struct FChunkConstructionData
{

public:
	FVector ChunkLocation{};
	FIntVector Cell{};
	bool bShouldGenerateCollision{};
	TArray<uint8> Voxels{};
	bool bAreVoxelsCompressed{};
	FChunkMeshData MeshData{};

	FChunkConstructionData() = default;

	FChunkConstructionData(const FVector& InChunkLocation, const FIntVector& InCell, bool InbShouldGenerateCollision)
		: ChunkLocation(InChunkLocation)
		, Cell(InCell)
		, bShouldGenerateCollision(InbShouldGenerateCollision)
	{}

	FChunkConstructionData(FChunkConstructionData&& Other) noexcept
		: ChunkLocation(MoveTemp(Other.ChunkLocation))
		, Cell(MoveTemp(Other.Cell))
		, bShouldGenerateCollision(MoveTemp(Other.bShouldGenerateCollision))
		, Voxels(MoveTemp(Other.Voxels))
		, bAreVoxelsCompressed(MoveTemp(Other.bAreVoxelsCompressed))
		, MeshData(MoveTemp(Other.MeshData))
	{
		// Reset or clear Other's members to release ownership of resources
		Other.Cell = FIntVector{};
		Other.bShouldGenerateCollision = false;
		Other.Voxels.Empty();
	}

	FChunkConstructionData& operator=(FChunkConstructionData&& Other) noexcept
	{
		if (this != &Other)
		{
			ChunkLocation = MoveTemp(Other.ChunkLocation);
			Cell = MoveTemp(Other.Cell);
			bShouldGenerateCollision = MoveTemp(Other.bShouldGenerateCollision);
			Voxels = MoveTemp(Other.Voxels);
			bAreVoxelsCompressed = MoveTemp(Other.bAreVoxelsCompressed);
			MeshData = MoveTemp(Other.MeshData);

			// Reset or clear Other's members to release ownership of resources
			Other.Cell = FIntVector{};
			Other.bShouldGenerateCollision = false;
			Other.Voxels.Empty();
		}
		return *this;
	}

	bool operator==(const FChunkConstructionData& Other) const
	{	return Cell == Other.Cell; }

	bool operator!=(const FChunkConstructionData& Other) const
	{	return Cell != Other.Cell; }

	FChunkConstructionData(const FChunkConstructionData& Other) = delete;
	FChunkConstructionData& operator=(const FChunkConstructionData& Other) = delete;
};

USTRUCT(BlueprintType)
struct FEncodedVoxelData
{
	GENERATED_BODY()

	FEncodedVoxelData() : ChunkCell(FIntVector()), Voxels() {}

	FEncodedVoxelData(FIntVector InChunkCell, const TArray<uint8>& InVoxels)
		: ChunkCell(InChunkCell),
		Voxels(InVoxels)
	{}

	FEncodedVoxelData(FIntVector InChunkCell, TArray<uint8>&& InVoxels) noexcept
		: ChunkCell(InChunkCell),
		Voxels(MoveTemp(InVoxels))
	{}

	// Copy constructor
	FEncodedVoxelData(const FEncodedVoxelData& Other)
		: ChunkCell(Other.ChunkCell),
		Voxels(Other.Voxels)
	{}

	// Copy assignment operator
	FEncodedVoxelData& operator=(const FEncodedVoxelData& Other)
	{
		if (this != &Other)
		{
			ChunkCell = Other.ChunkCell;
			Voxels = Other.Voxels;
		}
		return *this;
	}

	// Move assignment operator
	FEncodedVoxelData& operator=(FEncodedVoxelData&& Other) noexcept
	{
		if (this != &Other)
		{
			ChunkCell = MoveTemp(Other.ChunkCell);
			Voxels = MoveTemp(Other.Voxels);
		}
		return *this;
	}

	const int32 GetSizeInBytes()
	{
		int32 SizeInBytes{ Voxels.Num() };
		SizeInBytes += 13; // For the FIntVector ChunkCell, and bool bIsPartialChunk
		return SizeInBytes;
	}

	UPROPERTY()
	FIntVector ChunkCell{};

	UPROPERTY()
	TArray<uint8> Voxels{};
};

USTRUCT(BlueprintType)
struct FRegionData
{
	GENERATED_BODY()

	FRegionData() : Region(FIntPoint()), EncodedVoxelsArrays() {}

	FRegionData(FIntPoint InRegion, const TArray<FEncodedVoxelData>& InEncodedVoxelsArrays)
		: Region(InRegion),
		EncodedVoxelsArrays(InEncodedVoxelsArrays)
	{}

	// Move constructor
	FRegionData(FIntPoint InRegion, TArray<FEncodedVoxelData>&& InEncodedVoxelsArrays) noexcept
		: Region(InRegion),
		EncodedVoxelsArrays(MoveTemp(InEncodedVoxelsArrays))
	{}

	// Copy constructor
	FRegionData(const FRegionData& Other)
		: Region(Other.Region),
		EncodedVoxelsArrays(Other.EncodedVoxelsArrays)
	{}

	// Copy assignment operator
	FRegionData& operator=(const FRegionData& Other)
	{
		if (this != &Other)
		{
			Region = Other.Region;
			EncodedVoxelsArrays = Other.EncodedVoxelsArrays;
		}
		return *this;
	}

	// Move assignment operator
	FRegionData& operator=(FRegionData&& Other) noexcept
	{
		if (this != &Other)
		{
			Region = MoveTemp(Other.Region);
			EncodedVoxelsArrays = MoveTemp(Other.EncodedVoxelsArrays);
		}
		return *this;
	}

	bool operator==(const FRegionData& Other) const
	{
		return Region == Other.Region;
	}

	UPROPERTY()
	FIntPoint Region;

	UPROPERTY()
	TArray<FEncodedVoxelData> EncodedVoxelsArrays;

	static void DivideRegionIntoBundles(TArray<FEncodedVoxelData>& EncodedVoxelArrays, int32 MaxBundleSize, TArray<FRegionData>& OutDividedRegionBundles)
	{
		int32 BundleIndex{};
		FRegionData* BundlePtr{};
		for (FEncodedVoxelData& ChunkEncodedVoxelData : EncodedVoxelArrays)
		{
			if (!OutDividedRegionBundles.IsValidIndex(BundleIndex))
				OutDividedRegionBundles.Emplace(); // Add a new bundle
			BundlePtr = &OutDividedRegionBundles[BundleIndex];

			if (BundlePtr->GetSizeInBytes() + ChunkEncodedVoxelData.GetSizeInBytes() >= MaxBundleSize) // If the bundle would be too big after adding this chunk
			{
				BundleIndex++;
				if (!OutDividedRegionBundles.IsValidIndex(BundleIndex))
					OutDividedRegionBundles.Emplace(); // Add a new bundle

				BundlePtr = &OutDividedRegionBundles[BundleIndex];
			}
			BundlePtr->EncodedVoxelsArrays.Add(MoveTemp(ChunkEncodedVoxelData));
		}
		BundlePtr = nullptr;
	}

	int32 GetSizeInBytes()
	{
		int32 SizeInBytes{};
		for(FEncodedVoxelData& EncodedVoxelData : EncodedVoxelsArrays)
			SizeInBytes += EncodedVoxelData.GetSizeInBytes();
		SizeInBytes += 8; // For the FIntPoint Region
		return SizeInBytes;
	}
};

USTRUCT(BlueprintType)
struct FTerrainSettings
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Terrain Settings")
	int32 Seed{};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Terrain Settings")
	float TerrainHeightMultiplier{ 0.3 };
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Terrain Settings")
	float BiomeNoiseScale{ 0.04 };
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Terrain Settings")
	float TerrainNoiseScale{ 0.0075 };
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Terrain Settings")
	float FoliageNoiseScale{ 0.002 };
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Terrain Settings")
	int32 ChunkDeletionBuffer{ 2 };
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Terrain Settings")
	int32 CollisionGenerationRadius{ 5 };
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Terrain Settings")
	float VoxelSize{ 100.0 };
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Terrain Settings")
	int32 VoxelCount{ 32 };

	FTerrainSettings() = default;

	FTerrainSettings(int32 InSeed, float InTerrainHeightMultiplier, float InBiomeNoiseScale, float InTerrainNoiseScale, float InFoliageNoiseScale, int32 InChunkDeletionBuffer, int32 InCollisionChunkRadius, float InVoxelSize, int32 InVoxelCount)
		: Seed(InSeed), TerrainHeightMultiplier(InTerrainHeightMultiplier), BiomeNoiseScale(InBiomeNoiseScale), TerrainNoiseScale(InTerrainNoiseScale),
		FoliageNoiseScale(InFoliageNoiseScale), ChunkDeletionBuffer(InChunkDeletionBuffer), CollisionGenerationRadius(InCollisionChunkRadius), VoxelSize(InVoxelSize), VoxelCount(InVoxelCount) {}

	// Implement the Serialize function
	friend FArchive& operator<<(FArchive& Ar, FTerrainSettings& Settings)
	{
		Ar << Settings.Seed;
		Ar << Settings.TerrainHeightMultiplier;
		Ar << Settings.BiomeNoiseScale;
		Ar << Settings.TerrainNoiseScale;
		Ar << Settings.FoliageNoiseScale;
		Ar << Settings.ChunkDeletionBuffer;
		Ar << Settings.CollisionGenerationRadius;
		Ar << Settings.VoxelSize;
		Ar << Settings.VoxelCount;
		return Ar;
	}
};

class FChunkThread;
class FChunkThreadChild;
struct FChunkNameData;

UCLASS()
class INFINITEVOXELTERRAINPLUGIN_API AChunkManager : public AActor
{
	GENERATED_BODY()

public:
	AChunkManager();

	UFUNCTION(BlueprintCallable, Category = "Ininitalization")
	void InitializeTerrainGenerator();

	friend class FChunkThread;
	friend class FChunkThreadChild;
	friend class UChunkModifierComponent;
	friend class AVoxelGameMode;
	friend class AVoxelMenuGameMode;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaTime) override; 

private: 

	// === BeginPlay Functions === 
	void LoadTerrainSettings(UWorld* World);
	void SetUpAutosaveTimer();
	void Autosave(); // Called by a timer setup at begin play
	void FindLocalPlayerControllerAndPawn();
	void SetUpVoxelDatabaseRef(UWorld* World);
	void InitializeThreads();

	// === Tick Functions ===
	bool UpdateTrackedLocations();
	void UpateNearbyChunkCollisions();
	void HandleClientNeededServerData();
	void DequeueAndDestroyChunks();
	void UpdateRegionsAsync(bool bForUpdate = false);

	// == EndPlay Functions ===
	void SaveUnsavedRegionsOnThread(bool bSaveAsync = true);

	// === Player Tracking Functions ===
	bool AddTrackedPlayer(APlayerController* TrackedPlayer, bool bShouldInsertAtFront = false);
	void RemoveTrackedPlayer(APlayerController* TrackedPlayer);

	// === TerrainSettings Functions ===
	void LoadTerrainSettings(FTerrainSettings& OutTerrainSettings);
	static void SaveTerrainSettings(FTerrainSettings TerrainSettings, const FString& WorldSaveName);
	void ImplementTerrainSettingsAndInitializeThreads(FTerrainSettings& TerrainSettings);
	void ImplementTerrainSettings(FTerrainSettings& NewTerrainSettings);

	// === Region Functions ===
	bool UpdateRegionVariables();
	void RemoveRegionAndAddPendingSave(APlayerController* PlayerController, FIntPoint& OldRegion);
	void CalculateNeededRegions(FIntPoint CenterRegion, TArray<FIntPoint>& TrackedRegionsByPlayer);
	void AddRegionPendingDataIfNeeded(APlayerController* PlayerController, FIntPoint& Region);
	void SendNeededRegionDataOnGameThread(FIntPoint Region);
	void SendNeededRegionData(const FIntPoint& Region);
	void ImplementRegionData(FRegionData RegionData);
	void AddToRegionsThatHaveData(FIntPoint Region);

	// === Chunk Replication Functions ===
	void ReplicateChunkNamesAsync(const FVector2D& PlayerLocation);
	void ReplicateChunkNames(FIntVector CenterCell, bool bEnsureNoneMissing = false); // Do not call from game thread
	void GetAllChunkCellsInRadius(int32 SearchRadius, const FVector2D& TrackedLocation, TArray<FIntVector>& OutFoundChunkCells, TArray<FIntPoint>& OutMissing2DCells);
	void SetChunkName(AChunkActor* Chunk, const FIntVector& ChunkRepCell, const int32& ChunkRepCount);
	void UnreplicateChunk(FIntVector ChunkCell);
	void SendChunkNameDataToClients(FChunkNameData& ChunkNameData);
	void ClientSetChunkNames(const FChunkNameData& ChunkNameData);
	void ClientReadyForReplication(APlayerController* PlayerController); // Called by the ChunkReplicationComponent

	// === Functions used by SetVoxel ===
	void SetBorderVoxels(FIntVector& VoxelIntPosition, const FVector& VoxelWorldLocation, int32 VoxelValue, const FIntVector& ChunkCell);
	void GetMaterialsForChunkData(TArray<uint8> VoxelSections, TArray<UMaterial*>& VoxelMaterials);
	void UpdateChunkMesh(AChunkActor* Chunk);
	void UpdateModifiedVoxels(const FIntVector& ChunkCell, int32 VoxelIndex, int32 VoxelValue);
	void CheckForNeededNeighborChunks(FVector VoxelLocation, TArray<FIntVector>& OutNeededChunkCells);
	int32 GetVoxelIndex(FVector ChunkLocation, const FVector& VoxelWorldLocation, FIntVector& OutVoxelIntPosition);
	void SpawnAdditionalVerticalChunk(FVector VoxelWorldLocation, int32 VoxelValue, const FIntVector ChunkCell);

	// === Chunk Hiding and Destroying ===
	void DestroyChunksAtHeightmapLocation(const FVector2D& HeightmapLocation, const TArray<int32> ChunkZIndices);
	void DestroyOrHideChunk(FIntVector ChunkCell, bool& OutbWasHidden);
	void DestroyOrHideChunk(AChunkActor* Chunk, bool& OutbWasHidden);
	void DestroyChunk(FIntVector& ChunkCell);
	bool HideChunk(FIntVector ChunkCell);
	bool HideChunk(AChunkActor* Chunk);
	bool UnhideChunk(AChunkActor* Chunk);
	bool UnhideChunk(FIntVector ChunkCell);
	void UnhideChunksInHeightmapLocations(TArray<FVector2D>* HeightmapLocations);

public:

	UFUNCTION(BlueprintCallable, Category = "Generation Settings")
	void SetChunkGenerationRadius(int32 GenDistance);
	UFUNCTION(BlueprintCallable, Category = "Generation Settings")
	int32 GetChunkGenerationRadius() { return ChunkGenerationRadius; }
	UFUNCTION(BlueprintCallable, Category = "Generation Settings")
	int32 GetCollisionChunkRadius() { return CollisionGenerationRadius; }

	UFUNCTION(BlueprintCallable, Category = "Set Voxel")
	virtual void SetVoxel(FVector VoxelLocation, int32 VoxelValue, const FIntVector ChunkCell, bool bSetVoxelInAdjacentChunk = true, bool bCheckForMissingAdjacentChunks = true);
	UFUNCTION(BlueprintCallable, Category = "Set Voxel")
	const int32 GetVoxel(FVector VoxelLocation, FIntVector ChunkCell);
	UFUNCTION(BlueprintCallable, Category = "World Save")
	void SetSaveGameName(const FString& NewWorldSaveName);
protected:

	UFUNCTION(NetMulticast, Reliable, Category = "Set Voxel")
	void SetVoxelMulticast(FVector VoxelLocation, int32 VoxelValue, const FIntVector ChunkCell); // Called from ChunkModifierComponent's server function
	
	UFUNCTION(NetMulticast, Reliable, Category = "Replication")
	void ReplicatePlayerChunkLocations(const TArray<FVector2D>& PlayerHeightmapCells); // Used to let the clients know which chunks are safe to destroy

private:

	// === FastNoise Module ===
	//FUnrealFastNoise2Module* FastNoiseModule{};

	// === Terrain Settings ===
	int32 Seed{};
	float TerrainHeightMultiplier{ 0.3 };
	int32 AdjacentChunkVoxelBuffer{ 5 };
	float BiomeNoiseScale{ 0.04 };
	float TerrainNoiseScale{ 0.0075 };
	float FoliageNoiseScale{ 0.002 };
	int32 ChunkGenerationRadius{ 10 };
	int32 ChunkDeletionBuffer{ 2 };
	int32 CollisionGenerationRadius{ 6 };
	float ChunkSize; // The size in units along an axis of a chunk // initilaized in BeginPlay based on VoxelSize and VoxelCount
	float VoxelSize{ 100 };
	int32 VoxelCount{ 32 };

	// === Other terrain variables ===
	FString SaveGameName{};
	int32 TotalChunkVoxels;
	bool bWasGenRangeChanged{ false };

	// === Additional Settings ===
	float ChunkManagerTickInterval{ 0.05 };
	float AutosaveInterval{60.f};
	const float RegionBundleSendInterval{ 2.f };
	const int32 MaxRegionDataSendSizeInBytes{ 60000 };
	const int32 RegionSizeInChunks{ 50 };
	const int32 RegionBufferSize{ 1 };

	// === ChunkThreads ===
	TArray<FChunkThreadChild*> ChunkThreads{};
	int32 TotalThreadsAvailable{ FPlatformMisc::NumberOfCoresIncludingHyperthreads() };
	int32 NumThreadsToKeepFree{ 4 }; // Subtract this from TotalThreadsAvailable to get the number of threads we can use
	float ThreadWorkingSleepTime{ 0.014 }; // Recommended Minimum Sleep Time: 0.01
	float ThreadIdleSleepTime{ 0.03 }; // Recommended Minimum Sleep Time: 0.01

	// === Player Tracking ===
	APlayerController* LocalPlayerController{};
	TArray<APlayerController*> TrackedPlayers;
	TArray<FVector2D> PlayerLocations{};
	TArray<bool> TrackedHasFoundChunkInSpawnLocation{};
	TMap<APlayerController*, TArray<FIntVector>> TrackedChunkNamesUpToDate{};
	FRWLock ThreadPlayerLocationsLock{};
	TArray<FVector2D> ThreadUseableLocations{}; // Lock the ThreadPlayerLocationsLock before accessing this

	// === Chunk Tracking ===
	TMap<FIntVector, AChunkActor*> ChunksByCell; // This one is important. It stores our references to the chunk actors. // Only access this from the Game Thread
	TMap<FIntPoint, TArray<int32>> ChunkZIndicesBy2DCell; // Only access this from the Game Thread
	FCriticalSection HeightmapMutex; // Lock this before accessing the HeightmapLocations
	TSet<FVector2D> ExistingHeightmapLocations; // Only the ChunkThreads actually need this. It's how we know which chunks are already generated // Lock the HeightmapMutex before accessing this
	TMap<FIntVector, int32> ChunkSpawnCountByCell{}; // Used to generate a unique deterministic name for each chunk every time we replicate it
	TArray<FString> NamesAlreadyUsed{};

	// === Modified Voxels === 
	FCriticalSection ModifiedVoxelsMutex{};
	TMap<FIntPoint, TMap<FIntVector, TArray<uint8>>> ModifiedVoxelsByCellByRegion; 	// Lock the mutex before accessing

	// === Region Tracking ===
	TMap<APlayerController*, TArray<FIntPoint>> TrackedRegionsByPlayer{};
	FCriticalSection RegionMutex{};
	TArray<FIntPoint> RegionsPendingLoad;          // Lock the RegionMutex before accessing
	TArray<FIntPoint> RegionsAlreadyLoaded;        // Lock the RegionMutex before accessing
	TArray<FIntPoint> RegionsPendingSave;		   // Lock the RegionMutex before accessing
	TArray<FIntPoint> RegionsChangedSinceLastSave; // Lock the RegionMutex before accessing
	TMap<APlayerController*, TArray<FIntPoint>> TrackedRegionsPendingServerData; // Server uses these to track which clients need or have data. Client uses them to track locally. Nullptr if viewing on client
	TMap<APlayerController*, TArray<FIntPoint>> TrackedRegionsThatHaveServerData; // Server uses these to track which clients need or have data. Client uses them to track locally. Nullptr if viewing on client

	// === References ===
	AVoxelGameMode* VoxelGameModeRef;
	AVoxelTypesDatabase *VoxelTypesDatabase{};

	// === Chunk Destroying === 
	TArray<FIntVector> ChunksToDestroyQueue{}; // Destroying AActors can get expensive, so we spread them out over multiple frames
	const int32 ChunksToDestroyPerFrame{ 150 };

	// === Utility Functions ===
	bool GetDoesClientNeedRegionData(APlayerController* PlayerController, FIntPoint Region) { return !GetDoesClientHaveRegionData(PlayerController, Region) && GetIsClientPendingRegionData(PlayerController, Region); }
	bool GetDoesClientHaveRegionData(APlayerController* PlayerController, FIntPoint Region) { return TrackedRegionsThatHaveServerData.Find(PlayerController) && TrackedRegionsThatHaveServerData.Find(PlayerController)->Contains(Region); }
	bool GetIsClientPendingRegionData(APlayerController* PlayerController, FIntPoint Region) { return TrackedRegionsPendingServerData.Find(PlayerController) && TrackedRegionsPendingServerData.Find(PlayerController)->Contains(Region); }
	bool IsChunkGeneratedInThis2DLocation(const FVector2D& PlayerLocation) { return ChunkZIndicesBy2DCell.Contains(Get2DCellFromChunkLocation2D(PlayerLocation, ChunkSize)); }
	static FORCEINLINE FVector GetChunkGridLocation(FVector Location, float ChunkSize) { return FVector(FMath::GridSnap(Location.X, ChunkSize), FMath::GridSnap(Location.Y, ChunkSize), FMath::GridSnap(Location.Z, ChunkSize)); };
	static FORCEINLINE FIntVector GetCellFromChunkLocation(FVector ChunkLocation, float ChunkSize) { return FIntVector(ChunkLocation.GridSnap(ChunkSize) / ChunkSize); }
	static FORCEINLINE FIntPoint Get2DCellFromChunkLocation2D(FVector2D ChunkLocation, float ChunkSize) { return FIntPoint((FMath::GridSnap(ChunkLocation.X, ChunkSize) / ChunkSize), (FMath::GridSnap(ChunkLocation.Y, ChunkSize) / ChunkSize)); }
	static FORCEINLINE FVector GetLocationFromChunkCell(FIntVector ChunkCell, float ChunkSize) { return FVector(ChunkCell) * ChunkSize; }
	static FORCEINLINE FVector2D Get2DLocationFromChunkCell2D(FIntPoint ChunkCell2D, float ChunkSize) { return FVector2D(ChunkCell2D) * ChunkSize; }
	static FORCEINLINE FIntPoint GetRegionByLocation(const FVector2D& CurrentLocation, float ChunkSize, int32 RegionSizeInChunks) { return FIntPoint(FMath::GridSnap(CurrentLocation.X, ChunkSize * RegionSizeInChunks) / (ChunkSize * RegionSizeInChunks), FMath::GridSnap(CurrentLocation.Y, ChunkSize * RegionSizeInChunks) / (ChunkSize * RegionSizeInChunks)); }
};

inline bool GetVoxelOnBorder(FIntVector VoxelIntPosition, int32 VoxelCount, TArray<int32>& OutFaceDirectionIndices);

// 0 = Up, 1 = Down, 2 = East, 3 = West, 4 = North, 5 = South
const TArray<TArray<int32>> AdjacentChunkIndices = {
{4},                    // North
{4, 2},                 // Northeast
{2},                    // East
{5, 2},                 // Southeast
{5},                    // South
{5, 3},                 // Southwest
{3},                    // West
{4, 3},                 // Northwest
{0},                    // Up
{1},                    // Down
{4, 0},                 // NorthUp
{4, 2, 0},              // NortheastUp
{2, 0},                 // EastUp
{5, 2, 0},              // SoutheastUp
{5, 0},                 // SouthUp
{5, 3, 0},              // SouthwestUp
{3, 0},                 // WestUp
{4, 3, 0},              // NorthwestUp
{4, 1},                 // NorthDown
{4, 3, 1},              // NortheastDown
{2, 1},                 // EastDown
{5, 2, 1},              // SoutheastDown
{5, 1},                 // SouthDown
{5, 3, 1},              // SouthwestDown
{3, 1},                 // WestDown
{4, 3, 1},              // NorthwestDown
};