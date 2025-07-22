// Copyright(c) 2024 Endless98. All Rights Reserved.

#pragma once

#include "ChunkThread.h"
// This class is an abstraction of the FChunkThread class. It provides high level access to the functions you are most likely to want to modify, without needing to understand the lower level details of the FChunkThread class.

class INFINITEVOXELTERRAINPLUGIN_API FChunkThreadChild : public FChunkThread
{
public:
    friend AChunkManager;

    FChunkThreadChild(
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
        : FChunkThread(
            VoxelGameMode,
			VoxelDefinitions,
            World,
            ChunkManager,
            ChunkGenRadius,
            ChunkDeletionBuffer,
            AdjacentVoxelBuffer,
            ThreadWorkingSleepTime,
            ThreadIdleSleepTime,
            TotalChunkVoxels,
            ChunkSize,
            VoxelCount,
            VoxelSize,
            CollisionGenerationRadius,
            RegionSizeInChunks,
            TerrainHeightMultiplier,
            TerrainNoiseScale,
            BiomeNoiseScale,
            Seed,
            WorldSaveName,
            ThreadIndex)
    {}

protected:
	void InitializeNoiseGenerators() override;
	void GenerateHeightmap(TArray<int16>& OutGeneratedHeightmap, const FVector2D& NeededHeightmapLocation, TArray<int32>& OutNeededChunksVerticalIndices) override;
	bool GenerateChunkVoxels(TArray<uint8>& Voxels, const TArray<int16>& Heightmap, const FVector& ChunkLocation) override;
	void GenerateChunkMeshData(FChunkMeshData& OutChunkMeshData, TArray<uint8>& Voxels, const FIntVector ChunkCell, const bool bShouldGenerateCollisionAtChunkSpawn) override;
};