// Copyright(c) 2024 Endless98. All Rights Reserved.

#include "ChunkModifierComponent.h"
#include "ChunkManager.h"
#include "ChunkThread.h" // Only needed for the FaceDirections array
#include "VoxelGameMode.h"
#include "Engine/EngineTypes.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

UChunkModifierComponent::UChunkModifierComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	UWorld* World = GetWorld();
	if (!World)
		return;

	// Use world to get a reference to the first AChunkManager
	for (TActorIterator<AChunkManager> ActorItr(World); ActorItr; ++ActorItr)
	{
		ChunkManager = *ActorItr;
		break;
	}

	if (!ChunkManager)
		UE_LOG(LogTemp, Error, TEXT("VoxelReplicationComponent did not find an AChunkManager in the world!"));
} 

void UChunkModifierComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetNetMode() == NM_Client)
		ServerReadyForReplication();

	// Create the BoxComponent
	CollisionCheckerBox = NewObject<UBoxComponent>(this);

	if (CollisionCheckerBox)
	{
		CollisionCheckerBox->SetupAttachment(GetOwner()->GetRootComponent());
		CollisionCheckerBox->RegisterComponent();

		CollisionCheckerBox->SetCollisionProfileName(TEXT("BlockAll"));
		CollisionCheckerBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		CollisionCheckerBox->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
		CollisionCheckerBox->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
		CollisionCheckerBox->SetBoxExtent(FVector(ChunkManager->VoxelSize * 0.5f));
	}
}

bool UChunkModifierComponent::VoxelLineTrace(FVector StartPoint, FRotator FacingDirection, FVector &OutHitVoxelLocation, FVector& OutHitNormal, AChunkActor*& OutHitChunk)
{
	if (!ChunkManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("VoxelLineTrace failed because ChunkManager was nullptr"));
		return false;
	}
	FVector EndPoint = StartPoint + FacingDirection.Vector() * ReachDistance;
	FHitResult HitResult;
	FCollisionQueryParams TraceParams;
	TraceParams.AddIgnoredActor(GetOwner());
	TraceParams.TraceTag = "VoxelTrace";
	OutHitVoxelLocation = EndPoint;
	GetWorld()->LineTraceSingleByChannel(HitResult, StartPoint, EndPoint, ECC_Destructible, TraceParams);

	if (HitResult.bBlockingHit)
		OutHitVoxelLocation = HitResult.ImpactPoint;
	else
	{
		//DrawDebugLine(GetWorld(), StartPoint, OutHitVoxelLocation, FColor::Red, false, 5, 0, 1);
		return false;
	}
	//DrawDebugPoint(GetWorld(), OutHitVoxelLocation, 15, FColor::Green, false, 5, 0);
	//DrawDebugLine(GetWorld(), OutHitVoxelLocation, EndPoint, FColor::Green, false, 5, 0, 1);
	//DrawDebugLine(GetWorld(), StartPoint, OutHitVoxelLocation, Flet it rain mat kearneyColor::Red, false, 5, 0, 1);

	OutHitNormal = HitResult.Normal;
	OutHitChunk = HitResult.GetActor() ? Cast<AChunkActor>(HitResult.GetActor()) : nullptr;
	if (!OutHitChunk)
	{
		UE_LOG(LogTemp, Warning, TEXT("VoxelLineTrace failed because OutHitChunk was nullptr"));
		return false;
	}
	return true;
}

bool UChunkModifierComponent::AttemptSetVoxel(FVector StartPoint, FRotator FacingDirection, int32 VoxelValue, int32& OutPreviousVoxelValue, FVector& OutModifiedVoxelLocation)
{
	if (!ChunkManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("AttemptSetVoxel failed because ChunkManager was nullptr"));

		return false;
	}
	FVector HitVoxelLocation;
	FVector HitNormal;
	AChunkActor* HitChunk{};

	if (!VoxelLineTrace(StartPoint, FacingDirection, HitVoxelLocation, HitNormal, HitChunk))
		return false;

	if (!HitChunk)
	{
		UE_LOG(LogTemp, Warning, TEXT("AttemptSetVoxel failed because HitChunk was nullptr"));
		return false;
	}

	bool bIsEmptyVoxel{ VoxelValue == 0 };
	GetVoxelLocationFromHitLocation(HitNormal, HitVoxelLocation, bIsEmptyVoxel, HitChunk, OutModifiedVoxelLocation);

	OutPreviousVoxelValue = ChunkManager->GetVoxel(OutModifiedVoxelLocation, HitChunk->ChunkCell);

	if (GetNetMode() == NM_DedicatedServer)
		return SetVoxelIfWeHaveRoom(bIsEmptyVoxel, OutModifiedVoxelLocation, VoxelValue, HitChunk);

	if (GetNetMode() == NM_Client || GetNetMode() == NM_Standalone)
		if (!SetVoxelIfWeHaveRoom(bIsEmptyVoxel, OutModifiedVoxelLocation, VoxelValue, HitChunk))
		{
			ChunkManager->SetVoxel(OutModifiedVoxelLocation, OutPreviousVoxelValue, HitChunk->ChunkCell);
			return false;
		}

	if(GetNetMode() == NM_Client || GetNetMode() == NM_ListenServer)
		ServerSetVoxel(OutModifiedVoxelLocation, HitChunk->ChunkCell, VoxelValue, this);

	return true;
}

bool UChunkModifierComponent::SetVoxelIfWeHaveRoom(bool bIsEmptyVoxel, const FVector& VoxelLocation, int32 VoxelValue, AChunkActor* HitChunk)
{
	if (!ChunkManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("SetVoxelIfWeHaveRoom failed because ChunkManager was nullptr"));
		return false;
	}

	if (bIsEmptyVoxel)
		ChunkManager->SetVoxel(VoxelLocation, VoxelValue, HitChunk->ChunkCell);
	else
	{
		if (AreThereAnyOverlappingPawns(VoxelLocation, HitChunk->VoxelSize))
			return false; // We don't want to place a voxel where there are pawns. Later we might implement way to bump them instead
		ChunkManager->SetVoxel(VoxelLocation, VoxelValue, HitChunk->ChunkCell);
	}

	return true;
}

void UChunkModifierComponent::MulticastSetVoxel_Implementation(bool bIsEmptyVoxel, const FVector& VoxelLocation, int32 VoxelValue, AChunkActor* HitChunk)
{
	SetVoxelIfWeHaveRoom(bIsEmptyVoxel, VoxelLocation, VoxelValue, HitChunk);
}

bool UChunkModifierComponent::AreThereAnyOverlappingPawns(const FVector& VoxelLocation, float VoxelSize)
{
	TArray<AActor*> OverlappingActors;
	CollisionCheckerBox->SetWorldLocation(VoxelLocation);
	CollisionCheckerBox->GetOverlappingActors(OverlappingActors, APawn::StaticClass());

	return !OverlappingActors.IsEmpty();
}

// Will (probably?) not work if the chunk is rotated/not aligned with the grid
void UChunkModifierComponent::GetVoxelLocationFromHitLocation(FVector Normal, FVector HitLocation, bool bIsEmptyVoxel, AChunkActor* HitChunk, FVector& OutVoxelLocation)
{
	if (!HitChunk)
		return;
	OutVoxelLocation = HitLocation + -Normal + (bIsEmptyVoxel ? -Normal : Normal) * (HitChunk->VoxelSize / 2.f);
	OutVoxelLocation = OutVoxelLocation.GridSnap(ChunkManager->VoxelSize);
}

// Runs on server. Called by client or server
void UChunkModifierComponent::ServerSetVoxel_Implementation(FVector DesiredVoxelLocation, FIntVector ChunkCell, int32 VoxelValue, UChunkModifierComponent* CallingComponent)
{
	if (!ChunkManager)
		return;

	int32 PreviousVoxelValue{};
	bool bIsEmptyVoxel{ VoxelValue == 0 };
	AChunkActor ** Chunk{ ChunkManager->ChunksByCell.Find(ChunkCell) };
	bool bWasVoxelSet{};
	if (Chunk)
		bWasVoxelSet = SetVoxelIfWeHaveRoom(bIsEmptyVoxel, DesiredVoxelLocation, VoxelValue, *Chunk);

	if (bWasVoxelSet)
	{
		// Loop through every UChunkModifierComponent except the one that called this, and call ClientSetVoxel 
		AVoxelGameMode* VoxelGameMode{ Cast<AVoxelGameMode>(GetWorld()->GetAuthGameMode()) };
		if (!VoxelGameMode)
			return;

		TArray<APlayerController*> PlayerControllers{ ChunkManager->TrackedPlayers };
		TArray<UChunkModifierComponent*> ChunkModifierComponents;
		for (APlayerController* PlayerController : PlayerControllers)
		{
			UChunkModifierComponent* Component{ PlayerController->FindComponentByClass<UChunkModifierComponent>() };
			if (Component && Component != CallingComponent)
				ChunkModifierComponents.Add(Component);
		}
		ChunkModifierComponents.Remove(this);
		for (UChunkModifierComponent* ChunkModifier : ChunkModifierComponents)
			ChunkModifier->ClientSetVoxel(DesiredVoxelLocation, VoxelValue, ChunkManager->GetCellFromChunkLocation(DesiredVoxelLocation, ChunkManager->ChunkSize));
	}
	else // The server and client disagreed on circumstances for setting the voxel, so we need to revert the voxel
	{
		// We tell it what voxel value the server's version of this chunk has at this location, because the client will have already set it locally to the desired value which was unsuccessful
		int32 ActualVoxelValue{ ChunkManager->GetVoxel(DesiredVoxelLocation, ChunkCell) };
		FailedSetVoxel(DesiredVoxelLocation, ActualVoxelValue);
	}
}

void UChunkModifierComponent::ClientSetVoxel_Implementation(FVector VoxelLocation, int32 VoxelValue, FIntVector ChunkCell)
{
	if (GetNetMode() != NM_Client)
		return;

	if (!ChunkManager)
		return;
	ChunkManager->SetVoxel(VoxelLocation, VoxelValue, ChunkCell);
}

void UChunkModifierComponent::FailedSetVoxel_Implementation(FVector VoxelLocation, int32 PreviousVoxelValue)
{
	if (!ChunkManager)
		return;

	bool bSetVoxelInAdjacentChunk{ true };
	FIntVector ChunkCell{ ChunkManager->GetCellFromChunkLocation(VoxelLocation, ChunkManager->ChunkSize) };
	// Sets the voxel back to its previous value
	ChunkManager->SetVoxel(VoxelLocation, PreviousVoxelValue, ChunkCell, bSetVoxelInAdjacentChunk);
}

void UChunkModifierComponent::ClientReceiveRegionData_Implementation(FRegionData RegionData, bool bIsLastBundle)
{
	if (!ChunkManager)
		return;

	FIntPoint Region{ RegionData.Region };
	if (bIsLastBundle)
	{
		AddOrCombineTempRegionData(RegionData);
		ChunkManager->ImplementRegionData(MoveTemp(RegionData));
		TempRegionDataBundles.Remove(RegionData);
	}
	else
		AddOrCombineTempRegionData(RegionData);
}

void UChunkModifierComponent::AddOrCombineTempRegionData(FRegionData& RegionData)
{
	FIntPoint Region{ RegionData.Region };

	if (TempRegionDataBundles.Contains(RegionData))
	{
		FRegionData *ExistingRegionData{ TempRegionDataBundles.FindByKey(RegionData) };
		if (!ExistingRegionData)
		{
			UE_LOG(LogTemp, Error, TEXT("ExistingRegionData was nullptr!"));
			return;
		}
		ExistingRegionData->EncodedVoxelsArrays.Append(RegionData.EncodedVoxelsArrays);
		RegionData = *ExistingRegionData;
		ExistingRegionData = nullptr;
	}
	else
		TempRegionDataBundles.Add(RegionData);
}

void UChunkModifierComponent::ClientReceiveTerrainSettings_Implementation(FTerrainSettings TerrainSettings)
{
	if(GetNetMode() != NM_Client)
		return;

	if (!ChunkManager)
		return;

	ChunkManager->ImplementTerrainSettingsAndInitializeThreads(TerrainSettings);
}

void UChunkModifierComponent::ClientReceiveChunkNameData_Implementation(const FChunkNameData& ChunkNameData)
{
	if (!ChunkManager)
	{
		UE_LOG(LogTemp, Error, TEXT("ChunkManager was nullptr!"));
		return;
	}
	const FChunkNameData& NameData{ ChunkNameData };
	ChunkManager->ClientSetChunkNames(NameData);
}

void UChunkModifierComponent::ServerReadyForReplication_Implementation()
{
	if (!ChunkManager)
		return;

	bIsReadyForReplication = true;
	AActor* OwnerActor = GetOwner();
	APlayerController* OwningController = Cast<APlayerController>(OwnerActor);
	if (!OwningController)
	{
		UE_LOG(LogTemp, Warning, TEXT("Owner of VoxelReplicationComponent is not a player controller!"));
		return;
	}

	ChunkManager->ClientReadyForReplication(OwningController);
}