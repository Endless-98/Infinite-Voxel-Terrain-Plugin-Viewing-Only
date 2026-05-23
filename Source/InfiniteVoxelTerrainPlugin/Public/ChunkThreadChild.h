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
        int32 LodNearDistanceInChunks,
        int32 LodFarDistanceInChunks,
        float LodDistanceCurveExponent,
        int32 MaxLodStepPower,
        bool bUseGreedyMeshing,
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
            LodNearDistanceInChunks,
            LodFarDistanceInChunks,
            LodDistanceCurveExponent,
            MaxLodStepPower,
            bUseGreedyMeshing,
            Seed,
            WorldSaveName,
            ThreadIndex)
    {}

protected:
    // Intentionally empty: base thread now owns default generation and mesh behavior.
};