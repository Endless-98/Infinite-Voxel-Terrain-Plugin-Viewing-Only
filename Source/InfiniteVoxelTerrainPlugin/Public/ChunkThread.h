// Copyright(c) 2024 Endless98. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypesDatabase.h"
#include "Engine/World.h"
#include "FastNoise/FastNoise.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFilemanager.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Async/Async.h"
#include "DrawDebugHelpers.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"

struct FChunkConstructionData;
struct FChunkMeshData;
struct FVoxelSaveData
{
    FIntVector ChunkCell;
    TArray<uint8> CompressedVoxelData;

    FVoxelSaveData() {}

    FVoxelSaveData(const FIntVector& InChunkCell, const TArray<uint8>& InCompressedVoxelData)
        : ChunkCell(InChunkCell), CompressedVoxelData(InCompressedVoxelData) {}

    friend FArchive& operator<<(FArchive& Ar, FVoxelSaveData& StreamSet)
    {
        Ar << StreamSet.ChunkCell;
        Ar << StreamSet.CompressedVoxelData;
        return Ar;
    }

    friend bool operator==(const FVoxelSaveData& LHS, const FVoxelSaveData& RHS)
    {
        return LHS.ChunkCell == RHS.ChunkCell;
    }
    friend bool operator==(const FVoxelSaveData& LHS, const FIntVector& ChunkCell)
    {
        return LHS.ChunkCell == ChunkCell;
    }
    friend bool operator==(const FIntVector& ChunkCell, const FVoxelSaveData& RHS)
    {
        return ChunkCell == RHS.ChunkCell;
    }
};
class AChunkActor;
class AVoxelGameMode;
class AChunkManager;

class INFINITEVOXELTERRAINPLUGIN_API FChunkThread : public FRunnable
{
public:

    friend AChunkManager;

    FChunkThread(
        AVoxelGameMode* VoxelGameMode,
        TArray<FVoxelDefinition> VoxelDefinitions,
        UWorld* World,
        AChunkManager* ChunkManager,
        int32 ChunkGenRadius,
        int32 ChunkDeletionBuffer,
        int32 AdjacentVoxelBuffer,
        float ThreadWorkingSleepTime,
        float ThreadIdleSleepTime,
        int32 TotalChunkVoxels,
        float ChunkSize,
        int32 VoxelCount,
        float VoxelSize,
        int32 CollisionGenerationRadius,
        int32 RegionSizeInChunks,
        float TerrainHeightMultiplier,
        float TerrainNoiseScale,
        float BiomeNoiseScale,
        int32 Seed,
        FString WorldSaveName,
        int32 ThreadIndex)
        : VoxelGameModeRef(VoxelGameMode), VoxelDefinitions(VoxelDefinitions), WorldRef(World), ChunkManagerRef(ChunkManager), ChunkGenerationRadius(ChunkGenRadius), ChunkDeletionBuffer(ChunkDeletionBuffer),
        AdjacentChunkVoxelBuffer(AdjacentVoxelBuffer), ThreadWorkingSleepTime(ThreadWorkingSleepTime), ThreadIdleSleepTime(ThreadIdleSleepTime), TotalChunkVoxels(TotalChunkVoxels),
        ChunkSize(ChunkSize), VoxelCount(VoxelCount), VoxelSize(VoxelSize), CollisionGenerationRadius(CollisionGenerationRadius), RegionSizeInChunks(RegionSizeInChunks),
        TerrainHeightMultiplier(TerrainHeightMultiplier), TerrainNoiseScale(TerrainNoiseScale), BiomeNoiseScale(BiomeNoiseScale), Seed(Seed),
        WorldSaveName(WorldSaveName), ThreadIndex(ThreadIndex)
    { Thread = FRunnableThread::Create(this, TEXT("ChunkThread"), 0, EThreadPriority::TPri_Lowest); }
     
    virtual bool Init() override; // Do not call manually
    virtual uint32 Run() override; // Do not call manually
    virtual void Stop() override; // Call to signal thread to stop

    virtual void InitializeNoiseGenerators();
    bool UpdateTrackingVariables();
    void UpdateTempVariables();
    void UpdateChunks(); // Only the first ChunkThread runs this. This helps reduce the complexity of memory management for these operations
    bool IsNeededHeightmapLocation(FVector2D NeededHeightmapLocation, const TArray<FVector2D>& PlayerLocations, int32 ChunkGenRadius, int32 CollisionGenRadius);
    bool PrepareRegionForGeneration();
    bool FindNextNeededHeightmap(FVector2D& OutHeightmapLocation, TArray<FVector2D>*& OutLocationsNeedingUnhide); // Returns true if ready to move on
    static int32 CalculateCircumferenceInChunks(const int32 RadiusInChunks, float ChunkSize);
    bool GenerateChunkData(FVector2D& HeightmapLocation, TArray<int32>& TerrainZIndices, TArray<TSharedPtr<FChunkConstructionData>>& ChunkConstructionDataArray);
    FVector CalculateTangent(const FVector& Normal);
    virtual void GenerateHeightmap(TArray<int16>& OutGeneratedHeightmap, const FVector2D& NeededHeightmapLocation, TArray<int32>& OutNeededChunksVerticalIndices);
    void CombineChunkZIndices(const FVector2D& HeightmapLocation, TArray<int32>& TerrainZIndices);
    bool AddConstructionData(TArray<TSharedPtr<FChunkConstructionData>>& OutNeededChunks, const FVector2D& ChunkLocation2D, const TArray<int32>& NeededChunksVerticalIndices);
    void GenerateVoxelsForChunks(TArray<TSharedPtr<FChunkConstructionData>>& OutConstructionChunks, const TArray<int16>& Heightmap);
    virtual bool GenerateChunkVoxels(TArray<uint8>& Voxels, const TArray<int16>& Heightmap, const FVector& ChunkLocation);
    void ApplyModifiedVoxelsToChunk(TArray<uint8>& Voxels, FIntVector ChunkCell);
    void GenerateMeshDataForChunks(TArray<TSharedPtr<FChunkConstructionData>>& OutConstructionChunks); // Returns false if construction data failed to generated
    virtual void GenerateChunkMeshData(FChunkMeshData& OutChunkMeshData, TArray<uint8>& Voxels, const FIntVector ChunkCell, const bool bShouldGenerateCollisionAtChunkSpawn);
    bool DoesLocationNeedCollision(FVector2D Location2D, const TArray<FVector2D>& PlayerLocations, int32 ChunkGenRadius);
    void CompressVoxelData(TArray<TSharedPtr<FChunkConstructionData>>& ChunkConstructionDataArray);
    void AsyncSpawnChunks(TArray<TSharedPtr<FChunkConstructionData>>& ChunkConstructionDataArray, const FVector2D& HeightmapLocation, const TArray<int32>& TerrainZIndices);

    bool ShouldSpawnHidden(FVector2D ChunkLocation, int32 ChunkGenRadius);
    void SpawnChunkFromConstructionData(TSharedPtr<FChunkConstructionData> OutNeededChunkPtr, int32 ChunkGenRadius, int32 CollisionGenRadius, bool bShouldGenerateMesh = true);
    void SaveUnsavedRegions(bool bSaveAsync = true);
    void AsyncSaveVoxelsForRegion(FIntPoint Region, FString SaveName, bool bRemoveDataWhenDone = false, bool bRunAsync = true);
    void SaveVoxelsForRegion(const FString& SaveName, const FIntPoint& Region, bool bRemoveDataWhenDone);
    void LoadVoxelsForRegion(FIntPoint Region, FString SaveName);

    void GetRegionsToSave(TArray<FIntPoint>& RegionsToSave);
    void GetRegionsToLoad(TArray<FIntPoint>& RegionsToLoad);

    void GetVoxelIndex(int32& VoxelIndex, int32& X, int32& Y, int32& Z);
    void GetVoxelIndex(int32& VoxelIndex, const FIntVector XYZ);

    void SetChunkGenRadius(int32 Radius);
    // Only call from server
    bool EnableReplicationForChunk(AChunkActor* Chunk, bool bShouldDirectlySetbReplicates = false);
    FString GetDeterministicNameByLocationAndRepCount(const FIntVector& ChunkCell, int32 ReplicationCount);
    void DeleteSaveGame(FString WorldSaveName);
    TArray<FString> GetSaveFoldersNames();

    static inline FVector2D GetLocationSnappedToChunkGrid2D(const FVector2D& CurrentLocation, float ChunkSize) { return FVector2D(FMath::GridSnap(CurrentLocation.X, ChunkSize), FMath::GridSnap(CurrentLocation.Y, ChunkSize)); }
    inline FIntPoint GetRegionByLocation(const FVector2D& CurrentLocation) { return FIntPoint(FMath::GridSnap(CurrentLocation.X, ChunkSize * RegionSizeInChunks) / (ChunkSize * RegionSizeInChunks), FMath::GridSnap(CurrentLocation.Y, ChunkSize * RegionSizeInChunks) / (ChunkSize * RegionSizeInChunks)); } // Make sure this function matches the one in ChunkManager.h  
    inline FVector GetLocationSnappedToChunkGrid(const FVector& CurrentLocation) { return (CurrentLocation / ChunkSize).GridSnap(ChunkSize); }
    inline bool GetGenDistanceShouldBeCollision(int32 TrackedPlayerIndex) { return ((TrackedPlayerIndex > 0 && WorldRef->GetNetMode() == NM_ListenServer) || WorldRef->GetNetMode() == NM_DedicatedServer); }
    inline bool IsHeightmapInRange(const FVector2D& ChunkLocation2D, const FVector2D& TargetLocation2D, const int32& ChunkRadius) { return GetDistanceInChunks(ChunkLocation2D, TargetLocation2D) <= ChunkRadius; }
    int32 GetDistanceInChunks(const FVector2D& ChunkLocation2D, const FVector2D& TargetLocation2D) { return FMath::CeilToInt32(FMath::Abs(FVector2D::Distance(ChunkLocation2D, TargetLocation2D)) / ChunkSize); }
    FVector2f CalculateUV(const int32& FaceIndex, const int32& VertIndex);

    const FString SaveFolderName{ "SaveGames/WorldSaves/" };

protected:

    bool bIsRunning{ true };
    bool bDidTrackedActorMove{ false };

    // Noise Generators
    FastNoise::SmartNode<> BiomeNoiseGenerator;
    FastNoise::SmartNode<> LakeNoiseGenerator;
    FastNoise::SmartNode<> ForestNoiseGenerator;
    FastNoise::SmartNode<> PlainsNoiseGenerator;
    FastNoise::SmartNode<> HillsNoiseGenerator;
    FastNoise::SmartNode<> MountainsNoiseGenerator;

    FCriticalSection ChunkGenMutex{};   

    // === Settings (Set in Constructor) ===
    AVoxelGameMode* VoxelGameModeRef{};
    TArray<FVoxelDefinition> VoxelDefinitions;
    UWorld* WorldRef;
    AChunkManager* ChunkManagerRef{};
    int32 ChunkGenerationRadius;
    int32 ChunkDeletionBuffer;
    int32 AdjacentChunkVoxelBuffer;
    float ThreadWorkingSleepTime;
    float ThreadIdleSleepTime;
    int32 TotalChunkVoxels;
    const float ChunkSize;
    const int32 VoxelCount;
    const float VoxelSize;
    int32 CollisionGenerationRadius;
    const int32 RegionSizeInChunks;
    float TerrainHeightMultiplier;
    float TerrainNoiseScale;
    float BiomeNoiseScale;
    int32 Seed;

    FString WorldSaveName{ "DefaultWorld" };

    FVector2D LastHeightmapLocation{};
    bool bIsFirstTime{ true };
    int32 TempGenrationRadius{ ChunkGenerationRadius };
    int32 TempCollisionGenRadius{ CollisionGenerationRadius };
    int32 TempChunkGenRadius{ ChunkGenerationRadius };

	bool bFoundHeightmapOnLastCheck{ false };
	bool bWasRangeChanged{ false };

    // Used by threads to determine which spot to generate next
    TArray<FVector2D> PlayerLocations{};
    int32 TrackedIndex{};
    TArray<int32> TrackedChunkRingDistance{};
    TArray<int32> TrackedChunkRingCount{};

    static FCriticalSection ChunkZMutex;
    static TMap<FIntPoint, TArray<int32>> ChunkZIndicesBy2DCell;
    static TMap<FIntPoint, TArray<int32>> ModifiedAdditionalChunkZIndicesBy2DCell;

    int32 ThreadIndex{ -1 };
    int32 ChunkAngleIndex{};
    int32 LastRingCount{ -1 };
    int32 CircumferenceInChunks{};
    FRunnableThread* Thread{};
    
    const int32 CubeFaceOffsets[6] {
    1,                                // Positive X
    -1,                               // Negative X
    -VoxelCount,                      // Negative Y
    VoxelCount,                       // Positive Y
    VoxelCount * VoxelCount,          // Positive Z
    -VoxelCount * VoxelCount          // Negative Z
    };
};

void RunLengthEncode(TArray<uint8>& InputData, FIntVector OwningChunkCell);
void RunLengthDecode(TArray<uint8>& EncodedData, FIntVector OwningChunkCell);

const TArray<FVector> FaceDirections{ FVector::UpVector, FVector::DownVector, FVector::RightVector, FVector::LeftVector, FVector::ForwardVector, FVector::BackwardVector };
const TArray<FIntVector> FaceIntDirections{ FIntVector(0,0,1), FIntVector(0,0,-1), FIntVector(0,1,0), FIntVector(0,-1,0), FIntVector(1,0,0), FIntVector(-1,0,0) };
const TArray<TArray<FVector3f>> CubeVertLocations{
    { // Up 0
    FVector3f(-0.5f, 0.5f, 0.5f), FVector3f(-0.5f, -0.5f, 0.5f), FVector3f(0.5f, -0.5f, 0.5f), FVector3f(0.5f, 0.5f, 0.5f) },
    { // Down 1
    FVector3f(0.5f, -0.5f, -0.5f), FVector3f(-0.5f, -0.5f, -0.5f), FVector3f(-0.5f, 0.5f, -0.5f), FVector3f(0.5f, 0.5f, -0.5f) },
    { // Right 2 
    FVector3f(0.5f, 0.5f, 0.5f), FVector3f(0.5f, 0.5f, -0.5f), FVector3f(-0.5f, 0.5f, -0.5f), FVector3f(-0.5f, 0.5f, 0.5f) },
    { // Left 3 
    FVector3f(-0.5f, -0.5f, 0.5f), FVector3f(-0.5f, -0.5f, -0.5f), FVector3f(0.5f, -0.5f, -0.5f), FVector3f(0.5f, -0.5f, 0.5f) },
    { // Front 4
    FVector3f(0.5f, -0.5f, 0.5f), FVector3f(0.5f, -0.5f, -0.5f), FVector3f(0.5f, 0.5f, -0.5f), FVector3f(0.5f, 0.5f, 0.5f) },
    { // Back 5
    FVector3f(-0.5f, 0.5f, 0.5f), FVector3f(-0.5f, 0.5f, -0.5f), FVector3f(-0.5f, -0.5f, -0.5f), FVector3f(-0.5f, -0.5f, 0.5f) }
};
