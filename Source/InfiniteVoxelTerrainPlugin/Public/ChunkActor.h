// Copyright(c) 2024 Endless98. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "RealtimeMeshActor.h"
#include "RealtimeMeshSimple.h"
#include "Interface/Core/RealtimeMeshDataStream.h"
#include "Materials/Material.h"
#include "ChunkActor.generated.h"

struct FChunkMeshData
{

public:
    RealtimeMesh::FRealtimeMeshStreamSet ChunkStreamSet;
    ECollisionResponse CollisionType{};
    FIntVector ChunkCell{};
    TArray<uint8> VoxelSections{};
    bool bShouldGenCollision{};
    bool bIsMeshEmpty{};

    FChunkMeshData()
        : CollisionType(ECollisionResponse::ECR_Block)
        , bShouldGenCollision(false)
        , bIsMeshEmpty(false)
    {}

    FChunkMeshData(ECollisionResponse InCollisionType, const FIntVector& InChunkCell, bool InbShouldGenCollision)
        : CollisionType(InCollisionType)
        , ChunkCell(InChunkCell)
        , bShouldGenCollision(InbShouldGenCollision)
        , bIsMeshEmpty(false)
    { }

    FChunkMeshData(FChunkMeshData&& Other) noexcept
        : ChunkStreamSet(MoveTemp(Other.ChunkStreamSet)),
        CollisionType(MoveTemp(Other.CollisionType)),
        ChunkCell(MoveTemp(Other.ChunkCell)),
        VoxelSections(MoveTemp(Other.VoxelSections)),
        bShouldGenCollision(MoveTemp(Other.bShouldGenCollision)),
        bIsMeshEmpty(MoveTemp(Other.bIsMeshEmpty))
    { }

    // Move assignment operator
    FChunkMeshData& operator=(FChunkMeshData&& Other) noexcept
    {
        if (this != &Other) // Check for self-assignment
        {
            ChunkStreamSet = MoveTemp(Other.ChunkStreamSet);
            CollisionType = MoveTemp(Other.CollisionType);
            ChunkCell = MoveTemp(Other.ChunkCell);
            VoxelSections = MoveTemp(Other.VoxelSections);
            bShouldGenCollision = MoveTemp(Other.bShouldGenCollision);
            bIsMeshEmpty = MoveTemp(Other.bIsMeshEmpty);
        }
        return *this;
    }

    // Delete copy constructor and copy assignment operator
    FChunkMeshData(const FChunkMeshData& Other) = delete;
    FChunkMeshData& operator=(const FChunkMeshData& Other) = delete;
};

UCLASS()
class INFINITEVOXELTERRAINPLUGIN_API AChunkActor : public ARealtimeMeshActor
{
    GENERATED_BODY()

public:
    AChunkActor();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// We override this function to prevent the chunk from being destroyed on the client when the server is destroying a chunk. We will handle the destruction of the chunk on the client separately
	// Better named ShouldPreventDestruction() but Unreal Engine uses this name
    virtual bool DestroyNetworkActorHandled() override 
    { 
        bool bShouldPreventDestruction{ GetNetMode() == NM_Client && !bIsClientAttemptingToDestroyChunk };
        return bShouldPreventDestruction;
    };

    friend class AChunkManager;
    friend class FChunkThread;
    friend class UChunkModifierComponent;

protected:

    FIntVector ChunkCell{};
    // Voxels are sometimes passed to the Chunk actor in a compressed state but will be decompressed when the chunk needs to be modified
    TArray<uint8> Voxels;
    bool bAreVoxelsCompressed{};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
    float VoxelSize;
    float ChunkSize{};
    int32 VoxelCount{};

private:

    FRealtimeMeshCollisionConfiguration CollsionConfig{};
	bool bShouldGenerateCollisionOverride{ false };
    bool bHasFinishedGeneration{ false };
    bool bCollisionAllowed{ true };
	bool bIsCollisionGenerated{ false };

    bool bIsSafeToDestroy{true};
    // if this is false it means destroy was called from the server (or Destroy was called somewhere outside of DestroyOrHideChunk)
	bool bIsClientAttemptingToDestroyChunk{ false };
	bool bShouldDestroyWhenUnneeded{ false };

    URealtimeMeshSimple* RealtimeMesh;
    TArray<FRealtimeMeshSectionKey> MeshSectionKeys{};
    
    void GenerateChunkCollision();
    void GenerateChunkMesh(FChunkMeshData& ChunkMeshData, TArray<UMaterial*>& VoxelMaterials);
    void SetCollisionType(ECollisionEnabled::Type CollisionType);
};