// Copyright(c) 2024 Endless98. All Rights Reserved.

#include "ChunkActor.h"
AChunkActor::AChunkActor()
{
	if (!GetWorld() || GetWorld()->bIsTearingDown)
		return;

	if (RealtimeMeshComponent)
		RealtimeMeshComponent->SetCollisionResponseToChannel(ECC_Destructible, ECR_Block); // Set it to block the VoxelModification channel to allow traces

	CollsionConfig.bShouldFastCookMeshes = false;
	CollsionConfig.bUseAsyncCook = true;
	bFrozen = true;

}

void AChunkActor::BeginPlay()
{
	if (!GetWorld() || GetWorld()->bIsTearingDown)
		return;

	// Disable tick:
	SetActorTickEnabled(false);

	bGeneratedMeshRebuildPending = false;
	if (RealtimeMeshComponent)
		RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
	
	if (GetNetMode() == NM_DedicatedServer || GetNetMode() == NM_ListenServer)
		bIsSafeToDestroy = false;

	Super::BeginPlay();
}

void AChunkActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AChunkActor::EndPlay::UnregisterWithGenerationManager);
		UnregisterWithGenerationManager();
	}
	Super::EndPlay(EndPlayReason);
}

void AChunkActor::SetCollisionType(ECollisionEnabled::Type CollisionType)
{
	if (!GetWorld() || GetWorld()->bIsTearingDown)
		return;

	RealtimeMeshComponent->SetCollisionEnabled(CollisionType);
}

// This may run on any thread. Be sure it stays thread-safe! (No DebugDraws, etc.)
void AChunkActor::GenerateChunkCollision()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkActor::GenerateChunkCollision);

	if (!GetWorld() || GetWorld()->bIsTearingDown)
		return;

	if (!bHasFinishedGeneration) // Since this function is async, we may run this function before mesh data was generated,
		return;
	if (!RealtimeMesh || !IsValid(RealtimeMesh) || bIsCollisionGenerated )
		return;

	bShouldGenerateCollisionOverride = true;

	bIsCollisionGenerated = true;
	for (int32 SectionIndex{}; SectionIndex < MeshSectionKeys.Num(); SectionIndex++)
	{
		FRealtimeMeshSectionConfig SectionConfig(SectionIndex);
		RealtimeMesh->UpdateSectionConfig(MeshSectionKeys[SectionIndex], SectionConfig, bShouldGenerateCollisionOverride);
	}
}

void AChunkActor::GenerateChunkMesh(FChunkMeshData& ChunkMeshData, TArray<UMaterial*>& VoxelMaterials)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkActor::GenerateChunkMesh);

	if (!GetWorld() || GetWorld()->bIsTearingDown)
		return;

	if (!RealtimeMesh || !IsValid(RealtimeMesh) || !RealtimeMeshComponent || !IsValid(RealtimeMeshComponent))
	{
		UE_LOG(LogTemp, Error, TEXT("RealtimeMesh or RealtimeMeshComponent was nullptr!"));
		return;
	}

	if (ChunkMeshData.bIsMeshEmpty || ChunkMeshData.ChunkStreamSet.IsEmpty() || ChunkMeshData.VoxelSections.IsEmpty())
	{
		SetActorEnableCollision(false);
		bHasFinishedGeneration = true;
		for (FRealtimeMeshSectionKey &SectionKey : MeshSectionKeys)
			RealtimeMesh->RemoveSection(SectionKey);
		MeshSectionKeys.Empty();
		return;
	}
	SetActorEnableCollision(true);

	MeshSectionKeys.Empty();
	RealtimeMesh->SetCollisionConfig(CollsionConfig);

	for (int32 VoxelSectionIndex{}; VoxelSectionIndex < ChunkMeshData.VoxelSections.Num(); VoxelSectionIndex++)
	{
		if (!VoxelMaterials.IsValidIndex(VoxelSectionIndex) || VoxelMaterials[VoxelSectionIndex] == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("VoxelMaterial[%i] was nullptr!"), VoxelSectionIndex);
			bHasFinishedGeneration = true;

			continue;
		}

		RealtimeMesh->SetupMaterialSlot(VoxelSectionIndex, VoxelMaterials[VoxelSectionIndex]->GetFName(), VoxelMaterials[VoxelSectionIndex]);
	}

	const FRealtimeMeshLODKey LOD{0};
	const FRealtimeMeshSectionGroupKey GroupKey{ FRealtimeMeshSectionGroupKey::Create(LOD, FName("ChunkGroundMesh")) };
	for (int32 GroupIndex{}; GroupIndex < ChunkMeshData.VoxelSections.Num(); GroupIndex++)
		MeshSectionKeys.Add(FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, GroupIndex));

	RealtimeMesh->CreateSectionGroup(GroupKey, ChunkMeshData.ChunkStreamSet);

	bShouldGenerateCollisionOverride = ChunkMeshData.bShouldGenCollision && LOD.Index() == 0;
	bIsCollisionGenerated = bShouldGenerateCollisionOverride;

	for (int32 SectionIndex{}; SectionIndex < MeshSectionKeys.Num(); SectionIndex++)
		RealtimeMesh->UpdateSectionConfig(MeshSectionKeys[SectionIndex], FRealtimeMeshSectionConfig(SectionIndex), bShouldGenerateCollisionOverride);

	bHasFinishedGeneration = true;
}