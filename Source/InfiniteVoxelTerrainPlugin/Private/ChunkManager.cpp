// Copyright(c) 2024 Endless98. All Rights Reserved.

#include "ChunkManager.h"
#include "ChunkThreadChild.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "VoxelGameMode.h"
#include "ChunkModifierComponent.h"
#include "EngineUtils.h"

AChunkManager::AChunkManager()
{
	PrimaryActorTick.bCanEverTick = true;

	SetActorTickInterval(ChunkManagerTickInterval);

	bReplicates = true;
	SetNetAddressable();
	bNetLoadOnClient = true;
	bAlwaysRelevant = true;
}

void AChunkManager::BeginPlay()
{
	Super::BeginPlay();
}

// Called in Blueprints to initialize the terrain generator
void AChunkManager::InitializeTerrainGenerator()
{
	UWorld* World = GetWorld();
	if (!World)
		return;

	LoadTerrainSettings(World);
	SetUpAutosaveTimer();

	if (GetNetMode() != ENetMode::NM_DedicatedServer)
		FindLocalPlayerControllerAndPawn();

	SetUpVoxelDatabaseRef(World);

	if (GetNetMode() == ENetMode::NM_DedicatedServer || GetNetMode() == ENetMode::NM_ListenServer || GetNetMode() == ENetMode::NM_Standalone)
		InitializeThreads(); // We don't want to do this on the client because it does it separately when connecting to the server
}

void AChunkManager::Tick(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::Tick);

	Super::Tick(DeltaTime);
	if(UpdateTrackedLocations())
		UpateNearbyChunkCollisions();

	if (GetNetMode() == ENetMode::NM_DedicatedServer || GetNetMode() == ENetMode::NM_ListenServer)
		HandleClientNeededServerData(); // Could happen asyncronously on a background thread if we can't get the lock immediately

	if (!ChunksToDestroyQueue.IsEmpty())
		DequeueAndDestroyChunks();

	UpdateRegionsAsync();
}

void AChunkManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::EndPlay);

	bool bSaveAsync{ false };
	SaveUnsavedRegionsOnThread(bSaveAsync);

	for (FChunkThreadChild* ChunkThread : ChunkThreads)
		if (ChunkThread)
			ChunkThread->Stop();
	ChunkThreads.Empty();

	Super::EndPlay(EndPlayReason);
}

void AChunkManager::SetUpVoxelDatabaseRef(UWorld* World)
{
	for (TActorIterator<AVoxelTypesDatabase> ActorItr(World); ActorItr; ++ActorItr)
	{
		VoxelTypesDatabase = *ActorItr;
		break;
	}

	if (VoxelTypesDatabase == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("No VoxelDatabase!"));
		return;
	}
}

void AChunkManager::SetUpAutosaveTimer()
{
	// Set a timer that loops and fires every AutosaveInterval seconds:
	FTimerHandle AutosaveTimerHandle;
	if (GetNetMode() != NM_Client)
		GetWorld()->GetTimerManager().SetTimer(AutosaveTimerHandle, this, &AChunkManager::Autosave, AutosaveInterval, true);
}
void AChunkManager::LoadTerrainSettings(UWorld* World)
{
	if (GetNetMode() == ENetMode::NM_DedicatedServer || GetNetMode() == ENetMode::NM_ListenServer || GetNetMode() == ENetMode::NM_Standalone)
	{
		FTerrainSettings TerrainSettings{};
		LoadTerrainSettings(TerrainSettings);
	}
}

// Only happens locally or on the server, never on clients
void AChunkManager::LoadTerrainSettings(FTerrainSettings& OutTerrainSettings)
{
	const FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames/WorldSaves/"), SaveGameName, TEXT("TerrainSettings.dat"));
	if (FPaths::FileExists(SavePath))
	{
		TArray<uint8> BinaryData;
		if (FFileHelper::LoadFileToArray(BinaryData, *SavePath))
		{
			FMemoryReader MemoryReader(BinaryData, true);
			MemoryReader.Seek(0);

			MemoryReader << OutTerrainSettings.Seed;
			MemoryReader << OutTerrainSettings.TerrainHeightMultiplier;
			MemoryReader << OutTerrainSettings.BiomeNoiseScale;
			MemoryReader << OutTerrainSettings.TerrainNoiseScale;
			MemoryReader << OutTerrainSettings.FoliageNoiseScale;
			MemoryReader << OutTerrainSettings.ChunkDeletionBuffer;
			MemoryReader << OutTerrainSettings.CollisionGenerationRadius;
			MemoryReader << OutTerrainSettings.VoxelSize;
			MemoryReader << OutTerrainSettings.VoxelCount;
		}
		else
			UE_LOG(LogTemp, Error, TEXT("Failed to load TerrainSettings.dat from %s"), *SavePath);

		ImplementTerrainSettings(OutTerrainSettings);
	}
	else
	{
		ImplementTerrainSettings(OutTerrainSettings);
		SaveTerrainSettings(OutTerrainSettings, SaveGameName);
	}
}

// Only happens locally or on the server, never on clients
void AChunkManager::SaveTerrainSettings(FTerrainSettings TerrainSettings, const FString& WorldSaveName)
{
	const FString SaveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames/WorldSaves/"), WorldSaveName);
	const FString SavePath = FPaths::Combine(SaveDirectory, TEXT("TerrainSettings.dat"));

	// Ensure the directory exists
	if (!FPaths::DirectoryExists(SaveDirectory))
	{
		if (!IFileManager::Get().MakeDirectory(*SaveDirectory, true))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to create save directory: %s"), *SaveDirectory);
			return;
		}
	}

	TArray<uint8> BinaryData;
	BinaryData.Empty(); // Ensure the array is empty before use
	FMemoryWriter MemoryWriter(BinaryData, true);

	MemoryWriter << TerrainSettings.Seed;
	MemoryWriter << TerrainSettings.TerrainHeightMultiplier;
	MemoryWriter << TerrainSettings.BiomeNoiseScale;
	MemoryWriter << TerrainSettings.TerrainNoiseScale;
	MemoryWriter << TerrainSettings.FoliageNoiseScale;
	MemoryWriter << TerrainSettings.ChunkDeletionBuffer;
	MemoryWriter << TerrainSettings.CollisionGenerationRadius;
	MemoryWriter << TerrainSettings.VoxelSize;
	MemoryWriter << TerrainSettings.VoxelCount;

	FFileHelper::SaveArrayToFile(BinaryData, *SavePath);
}

int32 PlayerRetryCount{ 0 };
const int32 MaxRetries{ 1000 };
float RetryDelay{ 0.1f };
FTimerHandle PlayerRetryTimerHandle;

void AChunkManager::FindLocalPlayerControllerAndPawn()
{
	// if (GetNetMode() == ENetMode::NM_Standalone)
	LocalPlayerController = GetWorld()->GetFirstPlayerController();
	// else we are the client

	if (!LocalPlayerController)
	{
		UGameInstance* GameInstance = GetGameInstance();
		if (GameInstance)
		{
			// Iterate over all local players
			const TArray<ULocalPlayer*>& LocalPlayers = GameInstance->GetLocalPlayers();
			for (ULocalPlayer* LocalPlayer : LocalPlayers)
			{
				if (LocalPlayer && LocalPlayer->PlayerController)
				{
					APlayerController* PlayerController = LocalPlayer->PlayerController;
					if (PlayerController->GetLocalRole() == ROLE_AutonomousProxy || (GetNetMode() == NM_ListenServer && PlayerController->GetLocalRole() == ROLE_Authority))
						LocalPlayerController = PlayerController;
				}
			}
		}
	}

	if (LocalPlayerController && LocalPlayerController->GetPawn())
	{
		bool bShouldInsertToFront{ true };
		AddTrackedPlayer(LocalPlayerController, bShouldInsertToFront);

		UE_LOG(LogTemp, Log, TEXT("Local PlayerPawn found and added to TrackedActorManager!"));

		return;
	}

	PlayerRetryCount++;
	if (PlayerRetryCount < MaxRetries)
	{
		if (LocalPlayerController)
		{
			UE_LOG(LogTemp, Warning, TEXT("LocalPlayerController was found but did not have a pawn. Retrying... (%i/%i)"), PlayerRetryCount, MaxRetries);
		}
		else
			UE_LOG(LogTemp, Warning, TEXT("LocalPlayerController did not exist. Retrying... (%i/%i)"), PlayerRetryCount, MaxRetries);

		GetWorld()->GetTimerManager().SetTimer(PlayerRetryTimerHandle, this, &AChunkManager::FindLocalPlayerControllerAndPawn, RetryDelay, false);
	}
	else
		UE_LOG(LogTemp, Error, TEXT("No PlayerPawn found! Maximum retries reached."));
}

int32 VoxelRetryCount{ 0 };
FTimerHandle VoxelRetryTimerHandle;
FTimerHandle RetryTimerHandle;

void AChunkManager::InitializeThreads()
{
	UWorld* World = GetWorld();
	if (!World)
		return;

	if (!VoxelTypesDatabase)
	{
		UE_LOG(LogTemp, Error, TEXT("No VoxelDatabase! Retrying in %f seconds..."), RetryDelay);
		GetWorld()->GetTimerManager().SetTimer(RetryTimerHandle, this, &AChunkManager::InitializeThreads, RetryDelay, false, RetryDelay);
		return;
	}

	GetWorld()->GetTimerManager().ClearTimer(RetryTimerHandle);
	int32 NumThreadsToSpawn{ TotalThreadsAvailable - NumThreadsToKeepFree };
	UKismetSystemLibrary::PrintString(World, FString::Printf(TEXT("Creating %i threads for chunk generation"), NumThreadsToSpawn), true, false, FLinearColor::Green, 2.0f);
	for (uint8 ThreadIndex{}; ThreadIndex < NumThreadsToSpawn; ThreadIndex++)
	{
		FChunkThreadChild* ChunkThread = new FChunkThreadChild(
			VoxelGameModeRef,
			VoxelTypesDatabase->VoxelDefinitions,
			World,
			this,
			FMath::Max(ChunkGenerationRadius, CollisionGenerationRadius),
			ChunkDeletionBuffer,
			AdjacentChunkVoxelBuffer,
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
			SaveGameName,
			ThreadIndex);

		ChunkThreads.Add(ChunkThread);
	}
}

bool AChunkManager::UpdateTrackedLocations()
{
	bool bWereLocationsChanged{};
	TArray< FIntPoint> ChangedLocations{};

	for (int32 TrackedIndex{}; TrackedIndex < TrackedPlayers.Num(); TrackedIndex++)
	{
		APlayerController* TrackedPlayerController{ TrackedPlayers[TrackedIndex] };
		if (!TrackedPlayerController || !TrackedPlayerController->IsValidLowLevel())
		{
			UE_LOG(LogTemp, Error, TEXT("TrackedPlayer was nullptr!"));
			continue;
		}
		APawn* TrackedPlayerPawn{ TrackedPlayerController->GetPawn() };
		if (!TrackedPlayerPawn)
		{
			UE_LOG(LogTemp, Error, TEXT("TrackedPlayer Pawn was nullptr!"));
			continue;
		}

		bool bDidThisActorMove{ false };
		FVector2D PlayerLocation{ GetChunkGridLocation(TrackedPlayerPawn->GetActorLocation(), ChunkSize) };
		FIntPoint TrackedLocation{ FMath::RoundToInt32(PlayerLocation.X), FMath::RoundToInt32(PlayerLocation.Y) };
		if (!PlayerLocations.IsValidIndex(TrackedIndex))
		{
			PlayerLocations.Add(TrackedLocation);
			ChangedLocations.Add(TrackedLocation);
			bDidThisActorMove = true;
			bWereLocationsChanged = true;
		}
		else if (PlayerLocations[TrackedIndex] != TrackedLocation)
		{
			PlayerLocations[TrackedIndex] = TrackedLocation;
			ChangedLocations.Add(TrackedLocation);
			bDidThisActorMove = true;
			bWereLocationsChanged = true;
		}

		if (!TrackedHasFoundChunkInSpawnLocation[TrackedIndex])
		{
			if (IsChunkGeneratedInThis2DLocation(PlayerLocation))
			{
				TrackedHasFoundChunkInSpawnLocation[TrackedIndex] = true;
				// Unfreeze player once we see we have generate the chunk we're in
				TrackedPlayerPawn->CustomTimeDilation = 1.f;
			}
			else
			{
				// Freeze player until we generate the chunk we're in
				TrackedPlayerPawn->CustomTimeDilation = 0.f;
			}
		}

		// Only replicate collision chunks if we are the server
		if (!(GetNetMode() == ENetMode::NM_DedicatedServer || (GetNetMode() == ENetMode::NM_ListenServer && TrackedIndex > 0))) // On listen server, tracked index 0 is the host's player
			continue;

		if (bDidThisActorMove)
			ReplicateChunkNamesAsync(PlayerLocation);
	} // Remove all nullptr TrackedPlayers:

	if (bWasGenRangeChanged || bWereLocationsChanged)
	{
		if (ThreadPlayerLocationsLock.TryWriteLock())
		{
			ThreadUseableLocations = PlayerLocations;
			ThreadPlayerLocationsLock.WriteUnlock();
		}
	}

	for (int32 PlayerIndex{}; PlayerIndex < TrackedPlayers.Num(); PlayerIndex++)
		if (!TrackedPlayers[PlayerIndex] || !TrackedPlayers[PlayerIndex]->IsValidLowLevel())
		{
			RemoveTrackedPlayer(TrackedPlayers[PlayerIndex]);
			PlayerIndex--;
		}

	if (bWereLocationsChanged && (GetNetMode() == ENetMode::NM_DedicatedServer || GetNetMode() == ENetMode::NM_ListenServer))
		ReplicatePlayerChunkLocations(PlayerLocations);

	return bWereLocationsChanged;
}

void AChunkManager::UpateNearbyChunkCollisions()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::UpateNearbyChunkCollisions);

	// Using the step by step spiral method we use in FindNextNeededHeightmap, we can check chunk cells for needed collision
	// We start by finding the heightmap location closest to the tracked actor and spiral outwards from there
	// We then generate collision for each chunk that is within the SearchRadius of the tracked actor (unless it is already generated)
	TArray<FIntVector> FoundChunkCells{};
	TArray<AChunkActor*> FoundChunks{};
	TArray<FIntPoint> Missing2DCells{};
	for (FVector2D PlayerLocation : PlayerLocations)
	{
		GetAllChunkCellsInRadius(CollisionGenerationRadius, PlayerLocation, FoundChunkCells, Missing2DCells);

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::AsyncGenerateCollisionForNearbyChunksAndDecompressVoxels);
			// Now it's safe to process OutFoundChunks
			for (FIntVector& ChunkCell : FoundChunkCells)
			{
				AChunkActor* Chunk{};
				if (ChunksByCell.Contains(ChunkCell))
					Chunk = ChunksByCell.FindRef(ChunkCell);

				if (!Chunk || !IsValid(Chunk))
					continue;

				FoundChunks.Add(Chunk);

				if (ChunkThreads.IsValidIndex(0) && ChunkThreads[0] != nullptr && !Chunk->GetIsReplicated() && (GetNetMode() == ENetMode::NM_DedicatedServer || GetNetMode() == ENetMode::NM_ListenServer))
					ChunkThreads[0]->EnableReplicationForChunk(Chunk);
			}
		}
	}
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::GenerateCollisionForNearbyChunksAndDecompressVoxels::GenerateCollision);
		for (AChunkActor* Chunk : FoundChunks)
		{
			if (!Chunk || !IsValid(Chunk))
				continue;

			if (!Chunk->bIsCollisionGenerated && Chunk->bHasFinishedGeneration)
				Chunk->GenerateChunkCollision();
		}

		AsyncTask(ENamedThreads::AnyHiPriThreadHiPriTask, [FoundChunks]()
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::GenerateCollisionForNearbyChunksAndDecompressVoxels::GenerateCollisionAsync);

				for (AChunkActor* Chunk : FoundChunks)
				{
					if (Chunk && IsValid(Chunk))
						Chunk->GenerateChunkCollision();

					if (Chunk->bAreVoxelsCompressed)
					{
						Chunk->bAreVoxelsCompressed = false;
						RunLengthDecode(Chunk->Voxels, Chunk->ChunkCell);
					}
				}
			});
	}
}

void AChunkManager::HandleClientNeededServerData()
{
	// We don't want to wait for a lock on the game thread, so if we can't aqcuire the lock immediately, we will do this on a background thread
	if ((IsInGameThread() && RegionMutex.TryLock()) || !IsInGameThread())
	{
		if (!IsInGameThread())
			RegionMutex.Lock();

		if (GetNetMode() == ENetMode::NM_DedicatedServer || GetNetMode() == ENetMode::NM_ListenServer)
		{
			for (APlayerController* PlayerController : TrackedPlayers)
			{
				if (!PlayerController || !PlayerController->IsValidLowLevel())
				{
					UE_LOG(LogTemp, Error, TEXT("PlayerController was nullptr!"));
					continue;
				}
				if (!PlayerController->GetPawn())
				{
					UE_LOG(LogTemp, Error, TEXT("PlayerController Pawn was nullptr!"));
					continue;
				}

				TArray<FIntPoint>* RegionsPendingData{ TrackedRegionsPendingServerData.Find(PlayerController) };
				if (!RegionsPendingData)
					continue;

				for (int32 RegionIndex{}; RegionIndex < RegionsPendingData->Num(); RegionIndex++)
				{
					FIntPoint Region{ (*RegionsPendingData)[RegionIndex] };
					if (RegionsAlreadyLoaded.Contains(Region))
						SendNeededRegionDataOnGameThread(Region);
					else if (!RegionsPendingLoad.Contains(Region))
					{   // This likely indicate some flaw in our logic, but isn't necessarily a problem
						//UE_LOG(LogTemp, Warning, TEXT("Region %s was not loaded yet. And not pending load. Adding to pending load"), *Region.ToString());
						RegionsPendingLoad.Add(Region);
					} //else 
						//UE_LOG(LogTemp, Warning, TEXT("Region %s is pending load when the client needs it. If this message persists, we may not be loading when we should."), *Region.ToString());
				}
			}
			// Remove all nullptr TrackedPlayers:
			for (int32 PlayerIndex{}; PlayerIndex < TrackedPlayers.Num(); PlayerIndex++)
				if (!TrackedPlayers[PlayerIndex] || !TrackedPlayers[PlayerIndex]->IsValidLowLevel())
				{
					TrackedPlayers.RemoveAt(PlayerIndex);
					PlayerIndex--;
				}
		}

		RegionMutex.Unlock();
	}
	else if (IsInGameThread()) // If we are in the game thread and couldn't get an immediate lock
	{
		AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this]()
			{ HandleClientNeededServerData(); });
	}
}

void AChunkManager::DequeueAndDestroyChunks()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::DequeueAndDestroyChunks);

	// If you want chunk deletion to be almost instant, increase ChunksToDestroyPerFrame. This may cause a performance hit though, which is why we pace it.
	for (int32 ChunkIndex{}; ChunkIndex < ChunksToDestroyPerFrame; ChunkIndex++)
	{
		if (ChunksToDestroyQueue.IsEmpty())
			break;

		FIntVector ChunkCell;
		ChunkCell = ChunksToDestroyQueue[0];
		ChunksToDestroyQueue.RemoveAt(0);

		DestroyChunk(ChunkCell);
	}
}

void AChunkManager::UpdateRegionsAsync(bool bForceUpdate)
{
	AsyncTask(ENamedThreads::AnyNormalThreadHiPriTask, [this]()
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::UpdateRegionsAsync::TaskCompletionEvent);
			UpdateRegionVariables();
		});
}

// This function currently assumes chunks stay on their grid and don't rotate, if you want to have mobile chunks, you will need to modify this function
void AChunkManager::SetVoxel(FVector VoxelWorldLocation, int32 VoxelValue, const FIntVector ChunkCell, bool bSetVoxelInAdjacentChunk, bool bCheckForMissingAdjacentChunks)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::SetVoxel);

	AChunkActor* Chunk{ ChunksByCell.FindRef(ChunkCell) };
	if (!Chunk || !IsValid(Chunk))
	{
		UE_LOG(LogTemp, Error, TEXT("Chunk at cell '%s' not found."), *ChunkCell.ToString());
		return;
	}
	if (Chunk->bAreVoxelsCompressed) // Ideally this will happen on the ChunkThread, but if it doesn't, we need to decompress the voxels here
	{
		UE_LOG(LogTemp, Warning, TEXT("SetVoxel() Voxels for Cell were compressed when we needed them, so we are decompressing them on the game thread"));
		RunLengthDecode(Chunk->Voxels, ChunkCell);
		Chunk->bAreVoxelsCompressed = false;
	}

	FIntVector VoxelIntPosition;
	int32 VoxelIndex{ GetVoxelIndex(GetLocationFromChunkCell(ChunkCell, ChunkSize), VoxelWorldLocation, VoxelIntPosition) };
	if (!Chunk->Voxels.IsValidIndex(VoxelIndex))
	{
		UE_LOG(LogTemp, Error, TEXT("VoxelIndex %i was OOB of Voxels.Num() %i"), VoxelIndex, Chunk->Voxels.Num());
		return;
	}
	Chunk->Voxels[VoxelIndex] = VoxelValue;

	if (bSetVoxelInAdjacentChunk)
		SetBorderVoxels(VoxelIntPosition, VoxelWorldLocation, VoxelValue, ChunkCell);

	UpdateChunkMesh(Chunk);
	UpdateModifiedVoxels(ChunkCell, VoxelIndex, VoxelValue);

	if (bCheckForMissingAdjacentChunks)
	{
		TArray<FIntVector> NeededChunkCells{};
		CheckForNeededNeighborChunks(VoxelWorldLocation, NeededChunkCells);
		for (FIntVector NeededChunkCell : NeededChunkCells)
		{
			AsyncTask(ENamedThreads::AnyHiPriThreadHiPriTask, [VoxelWorldLocation, VoxelValue, NeededChunkCell, this]()
				{ SpawnAdditionalVerticalChunk(VoxelWorldLocation, VoxelValue, NeededChunkCell); });
		}
	}
}

void AChunkManager::SetBorderVoxels(FIntVector& VoxelIntPosition, const FVector& VoxelWorldLocation, int32 VoxelValue, const FIntVector& ChunkCell)
{
	TArray<int32> AdjacentChunks{};
	if (GetVoxelOnBorder(static_cast<FIntVector>(VoxelIntPosition), VoxelCount, AdjacentChunks))
	{
		bool bSetAdjacent{ false };
		bool bTempCheckForMissingAdjacentChunks{ false };
		for (int32 Index : AdjacentChunks)
			SetVoxel(VoxelWorldLocation, VoxelValue, ChunkCell + FIntVector(FaceDirections[Index]), bSetAdjacent, bTempCheckForMissingAdjacentChunks);
	}
}

void AChunkManager::UpdateChunkMesh(AChunkActor* Chunk)
{
	bool bShouldGenerateCollision{ true };
	FChunkMeshData ChunkMeshData;
	if (ChunkThreads.IsValidIndex(0) && ChunkThreads[0])
		ChunkThreads[0]->GenerateChunkMeshData(ChunkMeshData, Chunk->Voxels, Chunk->ChunkCell, bShouldGenerateCollision);
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ChunkThreads[0] was nullptr!")); 
		return;
	}
	TArray<UMaterial*> VoxelMaterials{};
	GetMaterialsForChunkData(ChunkMeshData.VoxelSections, VoxelMaterials);

	Chunk->GenerateChunkMesh(ChunkMeshData, VoxelMaterials);
}

void AChunkManager::UpdateModifiedVoxels(const FIntVector& ChunkCell, int32 VoxelIndex, int32 VoxelValue)
{
	FIntPoint Region{};
	{
		Region = GetRegionByLocation(FVector2D(FVector(ChunkCell) * ChunkSize), ChunkSize, RegionSizeInChunks);

		FScopeLock Lock(&RegionMutex);
		if (!RegionsChangedSinceLastSave.Contains(Region))
			RegionsChangedSinceLastSave.Add(Region);
	}
	{
		FScopeLock Lock(&ModifiedVoxelsMutex);
		TMap<FIntVector, TArray<uint8>>* ModifiedVoxelsByCell{};
		ModifiedVoxelsByCell = ModifiedVoxelsByCellByRegion.Find(Region);
		if (ModifiedVoxelsByCell == nullptr)
		{
			ModifiedVoxelsByCell = new TMap<FIntVector, TArray<uint8>>;
			ModifiedVoxelsByCellByRegion.Add(Region, *ModifiedVoxelsByCell);
		}

		if (ModifiedVoxelsByCell->Contains(ChunkCell))
		{
			TArray<uint8>* ModifiedVoxels = ModifiedVoxelsByCell->Find(ChunkCell);
			// Update the voxels map for saving later	
			if (ModifiedVoxels && ModifiedVoxels->IsValidIndex(VoxelIndex))
				(*ModifiedVoxels)[VoxelIndex] = VoxelValue;
		}
		else // Should only need to happen once for a given chunk:
		{
			TArray<uint8> NewVoxelsArray{};
			// Use the max value to represent an unmodified voxel (Can't be 0 because that represents a deleted voxel)
			NewVoxelsArray.Init(UINT8_MAX, TotalChunkVoxels);
			if (NewVoxelsArray.IsValidIndex(VoxelIndex))
				NewVoxelsArray[VoxelIndex] = VoxelValue;

			ModifiedVoxelsByCell->Emplace(ChunkCell, NewVoxelsArray);
		}
	}
}

void AChunkManager::CheckForNeededNeighborChunks(FVector VoxelLocation, TArray<FIntVector>& OutNeededChunkCells)
{
	VoxelLocation = VoxelLocation.GridSnap(VoxelSize);
	// Loop AdjacentChunkVoxelBuffer number of times in each direction, and snap the resulting VoxelLocation to the nearest ChunkCell
	// If it lands on a ChunkCell that doesn't have a Chunk, add it to OutNeededChunkCells
	for (int32 XIndex = -AdjacentChunkVoxelBuffer; XIndex <= AdjacentChunkVoxelBuffer; XIndex++)
	{
		for (int32 YIndex = -AdjacentChunkVoxelBuffer; YIndex <= AdjacentChunkVoxelBuffer; YIndex++)
		{
			for (int32 ZIndex = -AdjacentChunkVoxelBuffer; ZIndex <= AdjacentChunkVoxelBuffer; ZIndex++)
			{
				FVector SnappedLocation{ VoxelLocation };
				SnappedLocation.X += XIndex * VoxelSize;
				SnappedLocation.Y += YIndex * VoxelSize;
				SnappedLocation.Z += ZIndex * VoxelSize;

				FIntVector ChunkCell{ GetCellFromChunkLocation(SnappedLocation, ChunkSize) };

				if (OutNeededChunkCells.Contains(ChunkCell))
					continue;

				bool bChunkFound{ ChunksByCell.Contains(ChunkCell) };
				if (!bChunkFound)
				{
					OutNeededChunkCells.Add(ChunkCell);
				}
			}
		}
	}
}

void AChunkManager::SpawnAdditionalVerticalChunk(FVector VoxelWorldLocation, int32 VoxelValue, const FIntVector ChunkCell)
{
	FScopeLock Lock(&FChunkThread::ChunkZMutex);

	TArray<int32>* ChunkZIndicesPtr{ FChunkThread::ChunkZIndicesBy2DCell.Find(FIntPoint(ChunkCell.X, ChunkCell.Y)) };
	if (!ChunkZIndicesPtr || ChunkZIndicesPtr->Contains(ChunkCell.Z))
	{
		ChunkZIndicesPtr = nullptr;
		return;
	}
	ChunkZIndicesPtr->Add(ChunkCell.Z);
	Lock.Unlock();

	FVector ChunkLocation{ GetLocationFromChunkCell(ChunkCell, ChunkSize) };
	bool bChunkNeedsCollision{ true };
	TSharedPtr<FChunkConstructionData> ChunkConstructionData{ MakeShared<FChunkConstructionData>(ChunkLocation, ChunkCell, bChunkNeedsCollision) };
	if (ChunkThreads.IsValidIndex(0) && ChunkThreads[0])
	{
		FChunkThread* Thread = ChunkThreads[0];
		TArray<int16> Heightmap{};
		TArray<int32> UnneededVerticalIndices{};
		// We could keep the heightmap for this 2D around if we want to avoid recalculating it, but it would take up memory. Since we have a buffer for this, we will probably be fine.
		Thread->GenerateHeightmap(Heightmap, FVector2D(ChunkLocation), UnneededVerticalIndices);
		Thread->GenerateChunkVoxels(ChunkConstructionData->Voxels, Heightmap, ChunkLocation);
		Thread->ApplyModifiedVoxelsToChunk(ChunkConstructionData->Voxels, ChunkCell);
	}
	AsyncTask(ENamedThreads::GameThread, [this, ChunkConstructionData, VoxelWorldLocation, VoxelValue, ChunkCell]() mutable
		{
			bool bShouldGenerateMesh{ false };
			if (ChunkThreads.IsValidIndex(0) && ChunkThreads[0])
				ChunkThreads[0]->SpawnChunkFromConstructionData(MoveTemp(ChunkConstructionData), ChunkGenerationRadius, CollisionGenerationRadius, bShouldGenerateMesh);

			if (GetNetMode() == NM_DedicatedServer || GetNetMode() == NM_ListenServer)
				ReplicateChunkNamesAsync(FVector2D(GetLocationFromChunkCell(ChunkCell, ChunkSize)));
		});
}

// Called from ChunkModifierComponent's server function
void AChunkManager::SetVoxelMulticast_Implementation(FVector VoxelLocation, int32 VoxelValue, const FIntVector ChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::ClientRequestSetVoxel_Implementation);
	SetVoxel(VoxelLocation, VoxelValue, ChunkCell, true);
}

int32 AChunkManager::GetVoxelIndex(FVector ChunkLocation, const FVector& VoxelWorldLocation, FIntVector& OutVoxelIntPosition)
{
	FVector LocalChunkCorner{ ChunkSize / 2.0f };
	FVector LocalPosition{ (VoxelWorldLocation.GridSnap(VoxelSize) - ChunkLocation + LocalChunkCorner) };
	OutVoxelIntPosition = FIntVector(FVector(LocalPosition / VoxelSize).GridSnap(1));

	return (OutVoxelIntPosition.X + 1) * (VoxelCount + 2) * (VoxelCount + 2) + (OutVoxelIntPosition.Y + 1) * (VoxelCount + 2) + (OutVoxelIntPosition.Z + 1);
}

const int32 AChunkManager::GetVoxel(FVector VoxelWorldLocation, FIntVector ChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::GetVoxel);

	AChunkActor* Chunk{};
	if (ChunksByCell.Contains(ChunkCell))
		Chunk = *ChunksByCell.Find(ChunkCell);
	if (!Chunk)
	{
		// Log a warning if the chunk is not found
		UE_LOG(LogTemp, Warning, TEXT("GetVoxel: Chunk at cell '%s' not found."), *ChunkCell.ToString());
		return -1;
	}

	if (Chunk->Voxels.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("GetVoxel: Voxels array is empty."));
		return -1;
	}

	FIntVector OutVoxelIntPosition{};
	int32 VoxelIndex = GetVoxelIndex(Chunk->GetActorLocation(), VoxelWorldLocation, OutVoxelIntPosition);

	if (VoxelIndex < 0 || VoxelIndex >= Chunk->Voxels.Num())
	{
		UE_LOG(LogTemp, Error, TEXT("GetVoxel: Voxel index %i out of bounds of array with num %i."), VoxelIndex, Chunk->Voxels.Num());
		return -1;
	}

	return Chunk->Voxels[VoxelIndex];
}

void AChunkManager::SetSaveGameName(const FString& NewWorldSaveName)
{
	SaveGameName = NewWorldSaveName;
}

void AChunkManager::GetMaterialsForChunkData(TArray<uint8> VoxelSections, TArray<UMaterial*>& VoxelMaterials)
{
	// Set the actor reference in the FChunkConstructionData struct
	for (uint8 VoxelSectionValue : VoxelSections)
	{
		if (!VoxelTypesDatabase->VoxelDefinitions.IsValidIndex(VoxelSectionValue))
		{
			UE_LOG(LogTemp, Error, TEXT("VoxelSectionValue %i was OOB of, VoxelDefinitions.Num() %i"), static_cast<int32>(VoxelSectionValue), VoxelTypesDatabase->VoxelDefinitions.Num());
			return;
		}

		if (VoxelTypesDatabase->VoxelDefinitions[VoxelSectionValue].VoxelMaterial != nullptr)
			VoxelMaterials.Add(VoxelTypesDatabase->VoxelDefinitions[VoxelSectionValue].VoxelMaterial);
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Material for VoxelSectionValue %i was nullptr!"), static_cast<int32>(VoxelSectionValue));
			return;
		}
	}
}

void AChunkManager::SetChunkGenerationRadius(int32 GenDistance)
{
	GenDistance = FMath::Max(GenDistance, CollisionGenerationRadius);
	ChunkGenerationRadius = GenDistance;

	for (FChunkThread* ChunkThread : ChunkThreads)
		if(ChunkThread)
			ChunkThread->SetChunkGenRadius(GenDistance);

	bWasGenRangeChanged = true;
}

bool AChunkManager::AddTrackedPlayer(APlayerController* TrackedPlayer, bool bShouldInsertAtFront)
{
	if (!TrackedPlayer || !TrackedPlayer->IsValidLowLevel())
	{
		UE_LOG(LogTemp, Error, TEXT("TrackedPlayer was nullptr!"));
		return false;
	}
	if (!TrackedPlayer->GetPawn() || !TrackedPlayer->GetPawn()->IsValidLowLevel())
	{
		UE_LOG(LogTemp, Error, TEXT("TrackedPlayer Pawn was nullptr!"));
		return false;
	} 

	if(TrackedPlayers.Contains(TrackedPlayer))
		return false;

	//UKismetSystemLibrary::PrintString(GetWorld(), FString::Printf(TEXT("Adding Player %s to TrackedActorManager"), *TrackedPlayer->GetName()), true, false, FLinearColor::Green, 200.0f);

	TrackedRegionsByPlayer.Add(TPair<APlayerController*, TArray<FIntPoint>>(TrackedPlayer, TArray<FIntPoint>()));
	TrackedChunkNamesUpToDate.Add(TrackedPlayer, TArray<FIntVector>());
	TrackedPlayers.Add(TrackedPlayer);
	TrackedHasFoundChunkInSpawnLocation.Add(IsChunkGeneratedInThis2DLocation(FVector2D(TrackedPlayer->GetPawn()->GetActorLocation())));

	if (GetNetMode() == ENetMode::NM_DedicatedServer || GetNetMode() == ENetMode::NM_ListenServer)
	{
		// Send the player the terrain settings
		FTerrainSettings TerrainSettings( Seed, TerrainHeightMultiplier, BiomeNoiseScale, TerrainNoiseScale,
			FoliageNoiseScale, ChunkDeletionBuffer, CollisionGenerationRadius, VoxelSize, VoxelCount);

		UChunkModifierComponent* ChunkModifierComponent = TrackedPlayer->FindComponentByClass<UChunkModifierComponent>();
		if (ChunkModifierComponent)
			ChunkModifierComponent->ClientReceiveTerrainSettings(TerrainSettings);
	}

	UpdateTrackedLocations();
	UpdateRegionsAsync();

	return true;
}

// On clients this is Received from the server when we join the game, otherwise it is loaded from a save file
void AChunkManager::ImplementTerrainSettingsAndInitializeThreads(FTerrainSettings& NewTerrainSettings)
{
	ImplementTerrainSettings(NewTerrainSettings);
	InitializeThreads();
}

void AChunkManager::ImplementTerrainSettings(FTerrainSettings& NewTerrainSettings)
{
	Seed = NewTerrainSettings.Seed;
	TerrainHeightMultiplier = NewTerrainSettings.TerrainHeightMultiplier;
	BiomeNoiseScale = NewTerrainSettings.BiomeNoiseScale;
	TerrainNoiseScale = NewTerrainSettings.TerrainNoiseScale;
	FoliageNoiseScale = NewTerrainSettings.FoliageNoiseScale;
	ChunkDeletionBuffer = NewTerrainSettings.ChunkDeletionBuffer;
	CollisionGenerationRadius = NewTerrainSettings.CollisionGenerationRadius;
	VoxelSize = NewTerrainSettings.VoxelSize;
	VoxelCount = NewTerrainSettings.VoxelCount;
	ChunkSize = VoxelCount * VoxelSize;
	TotalChunkVoxels = FMath::Pow((VoxelCount + 2.0f), 3.0f);
}

void AChunkManager::RemoveTrackedPlayer(APlayerController* TrackedPlayer)
{
	int32 RemovalIndex{ TrackedPlayers.Find(TrackedPlayer) };
	TrackedPlayers.Remove(TrackedPlayer);
	TrackedHasFoundChunkInSpawnLocation.RemoveAt(RemovalIndex);
	PlayerLocations.RemoveAt(RemovalIndex);
	TrackedChunkNamesUpToDate.Remove(TrackedPlayer);
	TrackedRegionsByPlayer.Remove(TrackedPlayer);
	TrackedRegionsPendingServerData.Remove(TrackedPlayer);
	TrackedRegionsThatHaveServerData.Remove(TrackedPlayer);
}

void AChunkManager::ReplicateChunkNamesAsync(const FVector2D& PlayerLocation)
{
	if (!IsInGameThread())
		ReplicateChunkNames(GetCellFromChunkLocation(FVector(PlayerLocation, 0), ChunkSize));
	else
		AsyncTask(ENamedThreads::AnyNormalThreadHiPriTask, [PlayerLocation, this]()
			{ ReplicateChunkNames(GetCellFromChunkLocation(FVector(PlayerLocation, 0), ChunkSize)); });
}

// Runs on a thread, Do not call manually
bool AChunkManager::UpdateRegionVariables()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::UpdateRegionVariables);
	bool bWereRegionsChanged{};

	TArray<APlayerController*> PlayerControllers{};
	TrackedRegionsByPlayer.GetKeys(PlayerControllers);
	for (APlayerController* PlayerController : PlayerControllers)
	{
		if (!PlayerController || !PlayerController->IsValidLowLevel() || !PlayerController->GetPawn())
		{
			UE_LOG(LogTemp, Error, TEXT("PlayerController or Pawn was invalid when updating Regions!"));
			continue;
		}
		FVector2D CurrentActorLocation{ FVector2D(GetChunkGridLocation(PlayerController->GetPawn()->GetActorLocation(), ChunkSize)) };
		FIntPoint CenterRegion{ GetRegionByLocation(CurrentActorLocation, ChunkSize, RegionSizeInChunks) };

		bWereRegionsChanged = TrackedRegionsByPlayer.Contains(PlayerController);
		TArray<FIntPoint> TrackedRegions{ TrackedRegionsByPlayer.FindOrAdd(PlayerController) };
		TArray<FIntPoint> OldRegions{ TrackedRegions };

		CalculateNeededRegions(CenterRegion, TrackedRegions);
		if (OldRegions != TrackedRegions)
			bWereRegionsChanged = true;

		FScopeLock Lock(&RegionMutex);

		if(!TrackedRegions.IsEmpty())
			for (FIntPoint& OldRegion : OldRegions)
				if (!TrackedRegions.Contains(OldRegion)) // The region is no longer needed
					RemoveRegionAndAddPendingSave(PlayerController, OldRegion);

		for (FIntPoint& Region : TrackedRegions)
			if (!OldRegions.Contains(Region)) // The region is newly needed
				AddRegionPendingDataIfNeeded(PlayerController, Region);

		TrackedRegionsByPlayer.FindRef(PlayerController) = TrackedRegions;
	}

	return bWereRegionsChanged;
}

void AChunkManager::CalculateNeededRegions(FIntPoint CenterRegion, TArray<FIntPoint>& NeededRegions)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::CalculateNeededRegions);
	NeededRegions.Empty();

	for (int32 XIndex{ -RegionBufferSize }; XIndex <= RegionBufferSize; XIndex++)
		for (int32 YIndex{ -RegionBufferSize }; YIndex <= RegionBufferSize; YIndex++)
			NeededRegions.Add(FIntPoint(CenterRegion.X + XIndex, CenterRegion.Y + YIndex));
}

void AChunkManager::RemoveRegionAndAddPendingSave(APlayerController* PlayerController, FIntPoint& OldRegion)
{
	RegionsPendingSave.Add(OldRegion);
	RegionsAlreadyLoaded.Remove(OldRegion);
	RegionsPendingLoad.Remove(OldRegion);

	//DebugHighlightRegion(OldRegion, FColor::Red, 10.0f);

	if (GetNetMode() == ENetMode::NM_Client)
	{
		if (TrackedRegionsPendingServerData.Contains(nullptr))
			TrackedRegionsPendingServerData.Find(nullptr)->Remove(OldRegion);
		if (TrackedRegionsThatHaveServerData.Contains(nullptr))
			TrackedRegionsThatHaveServerData.Find(nullptr)->Remove(OldRegion);
	}
	else
	{
		if (TrackedRegionsPendingServerData.Contains(PlayerController))
			TrackedRegionsPendingServerData.Find(PlayerController)->Remove(OldRegion);
		if (TrackedRegionsThatHaveServerData.Contains(PlayerController))
			TrackedRegionsThatHaveServerData.Find(PlayerController)->Remove(OldRegion);
	}
}

void AChunkManager::AddRegionPendingDataIfNeeded(APlayerController* PlayerController, FIntPoint& Region)
{
	if (GetNetMode() == NM_Client)
		return;
	
	if (GetNetMode() != NM_Standalone) // We are on the server:
		if (!GetDoesClientHaveRegionData(PlayerController, Region) && !GetIsClientPendingRegionData(PlayerController, Region))
			TrackedRegionsPendingServerData.FindOrAdd(PlayerController).Add(Region);
	
	if (!RegionsPendingLoad.Contains(Region) && !RegionsAlreadyLoaded.Contains(Region))
	{
		RegionsPendingLoad.Add(Region);

		//DebugHighlightRegion(Region, FColor::Blue, 10.f);
		return;
	}
}

void AChunkManager::ClientReadyForReplication(APlayerController* PlayerController)
{
	if (!PlayerController)
	{
		UE_LOG(LogTemp, Error, TEXT("PlayerController was nullptr when reporting ready for replication!"));
		return;
	}
	TArray<FIntVector>* UpToDateCells{ TrackedChunkNamesUpToDate.Find(PlayerController) };
	if (UpToDateCells)
		UpToDateCells->Empty();

	AsyncTask(ENamedThreads::AnyNormalThreadHiPriTask, [this, PlayerController]()
		{ ReplicateChunkNames(GetCellFromChunkLocation(PlayerController->GetPawn()->GetActorLocation(), ChunkSize), true); });
}

void AChunkManager::SendNeededRegionDataOnGameThread(FIntPoint Region)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::SendNeededRegionDataOnGameThread);

	if (GetNetMode() == ENetMode::NM_Client || GetNetMode() == ENetMode::NM_Standalone)
		return;

	if(IsInGameThread())
		SendNeededRegionData(Region);
	else
		AsyncTask(ENamedThreads::GameThread, [this, Region]()
			{ SendNeededRegionData(Region); });
}

TArray<FTimerHandle> SendTimerHandles{};
TArray<FTimerDelegate> SendTimerDelegates{};
void AChunkManager::SendNeededRegionData(const FIntPoint& Region)
{
	TArray<APlayerController*> PlayerControllers{};
	TrackedRegionsByPlayer.GetKeys(PlayerControllers);
	for (APlayerController* PlayerController: PlayerControllers)
	{
		if (!PlayerController || !PlayerController->IsValidLowLevel())
		{
			UE_LOG(LogTemp, Error, TEXT("PlayerController was invalid when trying to add a tracked actor!"));
			continue;
		}

		if (GetNetMode() == NM_ListenServer && PlayerController == LocalPlayerController)
		{
			TrackedRegionsThatHaveServerData.FindOrAdd(PlayerController).Add(Region);
			TArray<FIntPoint>* RegionsPendingData{ TrackedRegionsPendingServerData.Find(PlayerController) };
			if (RegionsPendingData)
				RegionsPendingData->Remove(Region);

			continue;
		}
		FScopeLock RegionLock(&RegionMutex);
		if (GetDoesClientHaveRegionData(PlayerController, Region))
			continue;

		UChunkModifierComponent* ChunkModifierComponent = PlayerController->FindComponentByClass<UChunkModifierComponent>();
		if (!ChunkModifierComponent)
		{
			UE_LOG(LogTemp, Error, TEXT("ChunkModifierComponent was nullptr, so we can't send region data!"));
			continue;
		}

		TrackedRegionsThatHaveServerData.FindOrAdd(PlayerController).Add(Region);
		if (TrackedRegionsPendingServerData.Contains(PlayerController))
			TrackedRegionsPendingServerData.Find(PlayerController)->Remove(Region);

		FRegionData RegionData{};
		RegionData.Region = Region;

		FScopeLock Lock(&ModifiedVoxelsMutex);
		if (!ModifiedVoxelsByCellByRegion.Contains(Region)) // This is fine. It just means there were no modified voxels here. We still want to send the empty region data to the client so it knows it's up to date
		{
			bool bIsLastBundle{ true };
			ChunkModifierComponent->ClientReceiveRegionData(RegionData, bIsLastBundle);
			continue;
		}

		TMap<FIntVector, TArray<uint8>>& ModifiedVoxelsByCell{ *ModifiedVoxelsByCellByRegion.Find(Region) };
		for (TPair<FIntVector, TArray<uint8>>& CellVoxelPair : ModifiedVoxelsByCell)
		{
			FIntVector Cell{ CellVoxelPair.Key };
			TArray<uint8> CompressedVoxels{ CellVoxelPair.Value }; // We make a copy so we don't modify the original data
			RunLengthEncode(CompressedVoxels, Cell);
			RegionData.EncodedVoxelsArrays.Add(FEncodedVoxelData{ Cell, MoveTemp(CompressedVoxels) });
		}

		if (RegionData.EncodedVoxelsArrays.IsEmpty()) // This probably won't happen, but it's fine. We still want to send the empty region data to the client so it knows it's up to date
		{
			bool bIsLastBundle{ true };
			ChunkModifierComponent->ClientReceiveRegionData(RegionData, bIsLastBundle);
			continue;
		}

		int32 RegionDataSizeInBytes{ static_cast<int32>(RegionData.GetSizeInBytes()) };
		// If the region data is too large, we need to split it into multiple bundles
		if(RegionDataSizeInBytes > MaxRegionDataSendSizeInBytes)
		{
			// Calculate how many bundles we need to split it into
			TArray<FRegionData> RegionDataBundles;
			FRegionData::DivideRegionIntoBundles(RegionData.EncodedVoxelsArrays, MaxRegionDataSendSizeInBytes, RegionDataBundles);
			int32 BundleIndex{};
			for (FRegionData& DividedRegionData : RegionDataBundles)
			{
				DividedRegionData.Region = Region;
				bool bIsLastBundle{ BundleIndex == RegionDataBundles.Num() - 1 };
				float SendDelay{ FMath::Max(RegionBundleSendInterval * BundleIndex++, 0.1f) };

				// Create a lambda to capture the parameters and call ClientReceiveRegionData
				SendTimerDelegates.Emplace();
				SendTimerDelegates.Last().BindLambda([ChunkModifierComponent, RegionDataBundle = MoveTemp(DividedRegionData), bIsLastBundle, BundleIndex]() mutable
					{
						ChunkModifierComponent->ClientReceiveRegionData(MoveTemp(RegionDataBundle), bIsLastBundle); 
					});

				// Set the timer to execute the lambda after SendDelay seconds
				SendTimerHandles.Emplace();
				ChunkModifierComponent->GetWorld()->GetTimerManager().SetTimer(SendTimerHandles.Last(), SendTimerDelegates.Last(), SendDelay, false);
			}
		}
		else // The region data is small enough to send in one bundle
		{
			bool bIsLastBundle{ true };
			ChunkModifierComponent->ClientReceiveRegionData(RegionData, bIsLastBundle);
		}
	}
}

// Called locally on the client from the ChunkModifierComponent when we Receive DividedRegionData from the server
void AChunkManager::ImplementRegionData(FRegionData RegionData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::ClientReceiveRegionEncodedVoxelDataFromServer);

	if(GetNetMode() != ENetMode::NM_Client)
	{
		UE_LOG(LogTemp, Error, TEXT("ImplementRegionData was called on the server!"));
		return;
	}

	AsyncTask(ENamedThreads::AnyNormalThreadHiPriTask, [this, RegionData = MoveTemp(RegionData)]() mutable
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::ClientReceiveRegionEncodedVoxelDataFromServer::TaskCompletionEvent);

			bool bModifiedVoxelsDoesNotContainRegion{};

			{
				FScopeLock Lock(&ModifiedVoxelsMutex);
				bModifiedVoxelsDoesNotContainRegion = !ModifiedVoxelsByCellByRegion.Contains(RegionData.Region);
			}
			if (bModifiedVoxelsDoesNotContainRegion)
			{
				TMap<FIntVector, TArray<uint8>> ModifiedVoxelsByCell{};
				for (FEncodedVoxelData& EncodedVoxelData : RegionData.EncodedVoxelsArrays)
				{
					RunLengthDecode(EncodedVoxelData.Voxels, EncodedVoxelData.ChunkCell);
					ModifiedVoxelsByCell.Add(EncodedVoxelData.ChunkCell, MoveTemp(EncodedVoxelData.Voxels));
					FScopeLock ZMutexLock(&FChunkThread::ChunkZMutex);
					FChunkThread::ModifiedAdditionalChunkZIndicesBy2DCell.FindOrAdd(FIntPoint(EncodedVoxelData.ChunkCell.X, EncodedVoxelData.ChunkCell.Y)).Add(EncodedVoxelData.ChunkCell.Z);
				}

				{
					FScopeLock Lock(&ModifiedVoxelsMutex);
					ModifiedVoxelsByCellByRegion.Add(RegionData.Region, ModifiedVoxelsByCell);
				}
				AddToRegionsThatHaveData(RegionData.Region);

				return;
			}

			{
				FScopeLock Lock(&ModifiedVoxelsMutex);
				TMap<FIntVector, TArray<uint8>>* ModifiedVoxelsByCell{ ModifiedVoxelsByCellByRegion.Find(RegionData.Region) };
				if (ModifiedVoxelsByCell == nullptr)
				{
					UE_LOG(LogTemp, Error, TEXT("ModifiedVoxelsByCell was nullptr!"));
					AddToRegionsThatHaveData(RegionData.Region);

					return;
				}
				for (FEncodedVoxelData& EncodedVoxelData : RegionData.EncodedVoxelsArrays)
				{
					RunLengthDecode(EncodedVoxelData.Voxels, EncodedVoxelData.ChunkCell);
					ModifiedVoxelsByCell->Add(EncodedVoxelData.ChunkCell, MoveTemp(EncodedVoxelData.Voxels));
					{
						FScopeLock ZMutexLock(&FChunkThread::ChunkZMutex);
						FChunkThread::ModifiedAdditionalChunkZIndicesBy2DCell.FindOrAdd(FIntPoint(EncodedVoxelData.ChunkCell.X, EncodedVoxelData.ChunkCell.Y)).Add(EncodedVoxelData.ChunkCell.Z);
					}
				}
			}
			AddToRegionsThatHaveData(RegionData.Region);
		});
}

void AChunkManager::AddToRegionsThatHaveData(FIntPoint Region)
{
	FScopeLock Lock(&RegionMutex);
	TArray<FIntPoint>* RegionsPendingData{ TrackedRegionsPendingServerData.Find(nullptr) };
	if (RegionsPendingData)
		RegionsPendingData->Remove(Region);
	TrackedRegionsThatHaveServerData.FindOrAdd(nullptr).Add(Region);
}

// This multicast event is called on the server when a client moves a chunk
void AChunkManager::ReplicatePlayerChunkLocations_Implementation(const TArray<FVector2D>& Player2DCells)
{
	TArray<FVector2D> TrackedPlayerLocations{};
	for (const FVector2D& PlayerChunkCell2D : Player2DCells)
		TrackedPlayerLocations.Emplace(GetLocationFromChunkCell(FIntVector(PlayerChunkCell2D.X, PlayerChunkCell2D.Y, 0), ChunkSize));

	TArray<FIntPoint> AllHeightmapCells{};
	AllHeightmapCells.Reserve(ChunksByCell.Num());
	for (const TPair<FIntVector, AChunkActor*>& CellChunkPair : ChunksByCell)
	{
		if (ChunkThreads.IsValidIndex(0) && ChunkThreads[0])
		{
			if (!ChunkThreads[0]->DoesLocationNeedCollision(FVector2D(GetLocationFromChunkCell(FIntVector(CellChunkPair.Key.X, CellChunkPair.Key.Y, 0), ChunkSize)), TrackedPlayerLocations, CollisionGenerationRadius + ChunkDeletionBuffer))
				CellChunkPair.Value->bIsSafeToDestroy = true;
			else
				CellChunkPair.Value->bIsSafeToDestroy = false;
		}
	}
}

void AChunkManager::Autosave()
{
	if(!GetWorld() || GetWorld()->bIsTearingDown)
		return;

	SaveUnsavedRegionsOnThread();
}

// Only allowed on the owner of this save. Fires every AutosaveInterval seconds
void AChunkManager::SaveUnsavedRegionsOnThread(bool bSaveAsync)
{
	if(GetNetMode() == NM_Client)
		return;

	if (ChunkThreads.IsValidIndex(0) && ChunkThreads[0])
		ChunkThreads[0]->SaveUnsavedRegions(bSaveAsync);
}

void AChunkManager::DestroyChunk(FIntVector& ChunkCell)
{
	AChunkActor* Chunk{ ChunksByCell.FindRef(ChunkCell) };
	ChunksByCell.Remove(ChunkCell);

	if (ChunkZIndicesBy2DCell.Contains(FIntPoint(ChunkCell.X, ChunkCell.Y)))
	{
		TArray<int32>* ZIndices{ ChunkZIndicesBy2DCell.Find(FIntPoint(ChunkCell.X, ChunkCell.Y)) };
		if (!ZIndices)
			return;

		ZIndices->Remove(ChunkCell.Z);
		if (ZIndices->IsEmpty())
			ChunkZIndicesBy2DCell.Remove(FIntPoint(ChunkCell.X, ChunkCell.Y));
	}

	if (!Chunk || !Chunk->IsValidLowLevel())
	{
		UE_LOG(LogTemp, Error, TEXT("Chunk %s was nullptr!"), *ChunkCell.ToString());
		return;
	}

	Chunk->Destroy();
}

int32 MaxChunkRetryCount{50};
int32 TotalChunkRetries{0};
// Do not call from game thread. Server calls this function when it sees a client has moved
void AChunkManager::ReplicateChunkNames(FIntVector CenterCell, bool bEnsureNoneMissing)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::ReplicateChunkNames);

	if (GetNetMode() != ENetMode::NM_DedicatedServer && GetNetMode() != ENetMode::NM_ListenServer)
	{
		UE_LOG(LogTemp, Error, TEXT("ReplicateCollisionChunks was called on a client!"));
		return;
	}
	bool bWereAnyChunksMissing{false};
	TArray<FIntVector> FoundChunkCells{};
	TArray<FIntPoint> MissingCells2D{};
	if(!ChunkThreads.IsValidIndex(0) || !ChunkThreads[0])
	{
		UE_LOG(LogTemp, Error, TEXT("ChunkThreads[0] was nullptr!"));
		return;
	}
	GetAllChunkCellsInRadius(CollisionGenerationRadius, FVector2D(GetLocationFromChunkCell(CenterCell, ChunkSize)), FoundChunkCells, MissingCells2D);
	bWereAnyChunksMissing = !MissingCells2D.IsEmpty();

	// if any chunks were missing, set a timer to try again:
	if (bWereAnyChunksMissing && bEnsureNoneMissing)
	{
		float ReplicationRetryDelay{ 0.1f };
		if (TotalChunkRetries >= MaxChunkRetryCount)
		{
			TotalChunkRetries = 0;
			UE_LOG(LogTemp, Error, TEXT("Max retries reached for replicating collision chunks!"));
			return;
		}
		TotalChunkRetries++;
		FPlatformProcess::Sleep(ReplicationRetryDelay);
		AsyncTask(ENamedThreads::AnyNormalThreadHiPriTask, [this, CenterCell]()
			{ ReplicateChunkNames(CenterCell); });
	
		return;
	}

	TotalChunkRetries = 0;
	AsyncTask(ENamedThreads::GameThread, [this, ChunkCells = MoveTemp(FoundChunkCells), CenterCell]() //, MissingCells2D]()
		{
			FChunkNameData ChunkNameData{ CenterCell };

			// Now it's safe to process OutFoundChunks
			for (FIntVector ChunkCell : ChunkCells)
			{
				AChunkActor* Chunk{};
				if (ChunksByCell.Contains(ChunkCell))
					Chunk = ChunksByCell.FindRef(ChunkCell);

				if (!Chunk || !IsValid(Chunk))
					continue;

				if (!ChunkThreads[0]->EnableReplicationForChunk(Chunk))
				{
					UE_LOG(LogTemp, Error, TEXT("Failed to enable replication for Chunk %s"), *Chunk->GetName());
					continue;
				}

				int32* SpawnCountPtr = ChunkSpawnCountByCell.Find(Chunk->ChunkCell);
				if (!SpawnCountPtr)
				{
					UE_LOG(LogTemp, Error, TEXT("Server didn't have a RepCount for this ChunkCell %s"), *Chunk->ChunkCell.ToString());
					continue;
				}
				ChunkNameData.ChunkRepCells.Add(Chunk->ChunkCell);
				ChunkNameData.ChunkRepCounts.Add(*SpawnCountPtr);
			}
			SendChunkNameDataToClients(ChunkNameData);
		});

	return;
}

void AChunkManager::GetAllChunkCellsInRadius(int32 SearchRadius, const FVector2D& TrackedLocation, TArray<FIntVector>& OutFoundChunkCells, TArray<FIntPoint>& OutMissing2DCells)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::GetAllChunkCellsInRadius);

	int32 SearchRingChunkDistance{};
	int32 SearchChunkAngleIndex{};
	int32 SearchRingCount{};
	int32 SearchLastRingCount{};
	int32 SearchCirumferenceInChunks{};
	OutFoundChunkCells.Empty();
	OutMissing2DCells.Empty();
	FVector2D TrackedGridLocation{ FChunkThread::GetLocationSnappedToChunkGrid2D(TrackedLocation, ChunkSize) };
	while (SearchRingChunkDistance < SearchRadius) // Loop until we find a needed heightmap or we reach the edge of the generation radius
	{
		if (SearchLastRingCount != SearchRingCount) // If the radius has changed
		{
			SearchCirumferenceInChunks = FMath::Max(FChunkThread::CalculateCircumferenceInChunks(SearchRingCount, ChunkSize), 1);
			SearchChunkAngleIndex = 0;
		}
		SearchLastRingCount = SearchRingCount;

		while (SearchChunkAngleIndex < SearchCirumferenceInChunks)
		{
			float ChunkYawAngle = (360.f / SearchCirumferenceInChunks) * SearchChunkAngleIndex;
			FVector2D HeightmapLocation = FVector2D(FChunkThread::GetLocationSnappedToChunkGrid2D(TrackedGridLocation + FVector2D(FRotator(0, ChunkYawAngle, 0).Vector()) * (FVector2D(ChunkSize) * SearchRingCount / 2.0), ChunkSize));

			if (SearchChunkAngleIndex <= 0)
			{
				int32 ManhattanDistance = FMath::Abs(TrackedGridLocation.X - HeightmapLocation.X) + FMath::Abs(TrackedGridLocation.Y - HeightmapLocation.Y);
				SearchRingChunkDistance = FMath::Abs(FMath::RoundToInt32(ManhattanDistance / ChunkSize));
			}

			FIntPoint ChunkCell2D{ AChunkManager::Get2DCellFromChunkLocation2D(HeightmapLocation, ChunkSize) };

			if (!ChunkZIndicesBy2DCell.Contains(ChunkCell2D))
			{
				OutMissing2DCells.Add(ChunkCell2D);
				SearchChunkAngleIndex++;
				continue;
			}
			TArray<int32>& TerrainZIndices{ *ChunkZIndicesBy2DCell.Find(ChunkCell2D) };
			for (int32 ZIndex : TerrainZIndices)
			{
				FIntVector ChunkCell{ ChunkCell2D.X, ChunkCell2D.Y, ZIndex };
				if (!OutFoundChunkCells.Contains(ChunkCell))
					OutFoundChunkCells.Emplace(ChunkCell);
			}

			SearchChunkAngleIndex++;
		}

		if (SearchChunkAngleIndex >= SearchCirumferenceInChunks)
			SearchRingCount++;
	}
}

void AChunkManager::SendChunkNameDataToClients(FChunkNameData& ChunkNameData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::SendChunkNameDataToClients);

	int32 ClientsSentDataCount{};
	for (APlayerController* PlayerController : TrackedPlayers)
	{
		if (!PlayerController || !PlayerController->IsValidLowLevel())
		{
			UE_LOG(LogTemp, Error, TEXT("PlayerController was nullptr!"));
			continue;
		}

		if (PlayerController->IsLocalPlayerController() || (PlayerController == LocalPlayerController && GetNetMode() == NM_ListenServer))
			continue;

		UChunkModifierComponent* ChunkModifierComponent = PlayerController->FindComponentByClass<UChunkModifierComponent>();
		if (!ChunkModifierComponent)
		{
			UE_LOG(LogTemp, Error, TEXT("ChunkModifierComponent was nullptr!"));
			continue;
		}
		if (!ChunkModifierComponent->GetIsReadyForReplication())
			continue;

		FChunkNameData TempChunkName( ChunkNameData );

		TArray<FIntVector> *UpToDateCellsPtr = TrackedChunkNamesUpToDate.Find(PlayerController);
		if (UpToDateCellsPtr)
		{
			for (int32 ChunkRepIndex{ TempChunkName.ChunkRepCells.Num() - 1 }; ChunkRepIndex >= 0; --ChunkRepIndex)
			{
				FIntVector ChunkCell{ TempChunkName.ChunkRepCells[ChunkRepIndex] };

				if (UpToDateCellsPtr->Contains(ChunkCell))
				{
					TempChunkName.ChunkRepCells.RemoveAt(ChunkRepIndex);
					TempChunkName.ChunkRepCounts.RemoveAt(ChunkRepIndex);
				}
				else
					UpToDateCellsPtr->Add(ChunkCell);
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("UpToDateCellsPtr was nullptr"));
			continue;
		}

		ClientsSentDataCount++;
		ChunkModifierComponent->ClientReceiveChunkNameData(TempChunkName);
	}
}

// Runs on client, called by ChunkModifierComponent when we Receive the needed data to name our chunks the same as the server
void AChunkManager::ClientSetChunkNames(const FChunkNameData& ChunkNameData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::ClientSetChunkNames);

	//UKismetSystemLibrary::PrintString(GetWorld(), FString::Printf(TEXT("Client Receive name bundle")), true, false, FLinearColor::White, 15.f);

	if (GetNetMode() != ENetMode::NM_Client)
	{
		UE_LOG(LogTemp, Error, TEXT("ClientSetChunkNames was called on the server!"));
		return;
	}

	TArray<AChunkActor*> ChunksToName{};
	bool bWereAnyChunksMissing{};

	// Loop through ChunkRepCells and see if we have any chunks that match the cell. If we do, rename them
	for (int32 ChunkRepIndex{}; ChunkRepIndex < ChunkNameData.ChunkRepCells.Num(); ChunkRepIndex++)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::ChunkNameDataLoop);
		AChunkActor* Chunk{};
		const FIntVector& ChunkRepCell = ChunkNameData.ChunkRepCells[ChunkRepIndex];
		const int32& ChunkRepCount{ ChunkNameData.ChunkRepCounts[ChunkRepIndex] };

		Chunk = ChunksByCell.FindRef(ChunkRepCell);
		SetChunkName(Chunk, ChunkRepCell, ChunkRepCount);
	}
}

void AChunkManager::SetChunkName(AChunkActor* Chunk, const FIntVector& ChunkRepCell, const int32& ChunkRepCount)
{
	if (!IsValid(Chunk))
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::ChunkNotFound);

		ChunkSpawnCountByCell.Add(ChunkRepCell, ChunkRepCount);
		return;
	}

	Chunk->bShouldDestroyWhenUnneeded = false;
	Chunk->bIsSafeToDestroy = false;

	if (!ChunkThreads.IsValidIndex(0) || !ChunkThreads[0])
	{
		UE_LOG(LogTemp, Error, TEXT("ChunkThreads[0] was nullptr!"));
		return;
	}
	FString OldName{ FString(Chunk->GetName()) };
	FString NewName{ FString(ChunkThreads[0]->GetDeterministicNameByLocationAndRepCount(Chunk->ChunkCell, ChunkRepCount)) };

	if (NamesAlreadyUsed.Contains(NewName))
	{
		UE_LOG(LogTemp, Error, TEXT("NewName %s was already used!"), *NewName);
		{return; };
	}

	if (OldName == NewName)
	{
		UE_LOG(LogTemp, Warning, TEXT("Chunk %s already had the correct name"), *Chunk->GetName());
		return;
	}

	if (NewName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("NewName was empty!"));
		return;
	}
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::RenameChunk);
		Chunk->Rename(*NewName, this, REN_ForceNoResetLoaders);
	}
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::AddNewNameToUsedNames);
		NamesAlreadyUsed.Add(NewName);
	}
}

void AChunkManager::DestroyChunksAtHeightmapLocation(const FVector2D& HeightmapLocation, const TArray<int32> ChunkZIndices)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::DestroyChunksAtHeightmapLocation);

	FIntPoint ChunkCell2D = Get2DCellFromChunkLocation2D(HeightmapLocation, ChunkSize);
	for (int32 ZIndex : ChunkZIndices)
	{
		FIntVector ChunkCell = GetCellFromChunkLocation(FVector(HeightmapLocation.X, HeightmapLocation.Y, ZIndex * ChunkSize), ChunkSize);
		AChunkActor* Chunk = ChunksByCell.FindRef(ChunkCell);

		bool bWasHidden{};
		DestroyOrHideChunk(Chunk, bWasHidden);
	}
}

void AChunkManager::DestroyOrHideChunk(FIntVector ChunkCell, bool& OutbWasHidden)
{
	AChunkActor** Chunk = ChunksByCell.Find(ChunkCell);
	if(!Chunk || !IsValid(*Chunk))
		return;

	DestroyOrHideChunk(*Chunk, OutbWasHidden);
}

void AChunkManager::DestroyOrHideChunk(AChunkActor* Chunk, bool &OutbWasHidden)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::DestroyOrHideChunk);

	if (!Chunk || !IsValid(Chunk) || !GetWorld())
		return;
	
	FVector ChunkLocation{ Chunk->GetActorLocation() };
	FIntVector& ChunkCell{ Chunk->ChunkCell };

	if (!Chunk->bHasFinishedGeneration)
	{
		UE_LOG(LogTemp, Error, TEXT("Chunk was not finished generating!"));
		
		if (GetNetMode() == ENetMode::NM_DedicatedServer || GetNetMode() == ENetMode::NM_ListenServer)
			Chunk->TearOff();

		ChunksToDestroyQueue.Add(ChunkCell);
		return;
	}

	if (GetNetMode() == ENetMode::NM_DedicatedServer || GetNetMode() == ENetMode::NM_ListenServer)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::DestroyChunkOnServer);

		Chunk->TearOff();
		ChunksToDestroyQueue.Add(ChunkCell);
	}
	else if (GetNetMode() == ENetMode::NM_Client || GetNetMode() == ENetMode::NM_Standalone)
	{
		if (!Chunk->bIsSafeToDestroy) // If we can't destroy the chunk, it's because it is still relevant on the server. In this case, we hide the chunk, but keep it around, because if we need it again, we'll need it to be the same chunk with the same name, and we can't reuse names locally, so this is our best option 		{
		{
			HideChunk(Chunk);
			OutbWasHidden = true;
		}
		else // Chunk is allowed to be destroyed
		{
			Chunk->bIsClientAttemptingToDestroyChunk = true;
			ChunkSpawnCountByCell.Remove(ChunkCell); // As the client, we don't want to track this, because we will receive the new name from the server.
			ChunksToDestroyQueue.Add(ChunkCell);
			Chunk = nullptr;
		}
	}
}

// Returns wether chunk was hidden
// Only call from game thread
bool AChunkManager::HideChunk(FIntVector ChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::HideChunk);

	AChunkActor** Chunk = ChunksByCell.Find(ChunkCell);
	if (!Chunk || !IsValid(*Chunk))
	{
		UE_LOG(LogTemp, Error, TEXT("Chunk was nullptr or invalid!"));
		return false;
	}
	return HideChunk(*Chunk);
}

// Returns wether chunk was hidden
// Only call from game thread
bool AChunkManager::HideChunk(AChunkActor* Chunk)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::HideChunk);

	if(!IsInGameThread())
	{
		UE_LOG(LogTemp, Error, TEXT("HideChunk was called outside the game thread!"));
		return false;
	}

	if (!Chunk || !IsValid(Chunk))
	{
		UE_LOG(LogTemp, Error, TEXT("Chunk was nullptr or invalid!"));
		return false;
	}

	Chunk->bShouldDestroyWhenUnneeded = true;

	Chunk->SetActorHiddenInGame(true);

	return true;
}

// Returns wether chunk was unhidden
// Only call from game thread
bool AChunkManager::UnhideChunk(AChunkActor* Chunk)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::UnhideChunk);

	if (!Chunk || !IsValid(Chunk))
		return false;

	if (!Chunk)
		return false;

	Chunk->bShouldDestroyWhenUnneeded = false;

	Chunk->SetActorHiddenInGame(false);

	return true;
}

// Returns wether chunk was unhidden
// Only call from game thread
bool AChunkManager::UnhideChunk(FIntVector ChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::UnhideChunk);

	AChunkActor* Chunk{};

	if (ChunksByCell.Contains(ChunkCell))
		Chunk = ChunksByCell.FindRef(ChunkCell);

	return UnhideChunk(Chunk);
}

// Only call from game thread
void AChunkManager::UnhideChunksInHeightmapLocations(TArray<FVector2D>* HeightmapLocations)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::UnhideChunksInHeightmapLocations);
	
	if (!HeightmapLocations || (GetNetMode() != NM_Client))
		return;

	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this, HeightmapLocations]()
			{ UnhideChunksInHeightmapLocations(HeightmapLocations); });
		return;
	}
	for (FVector2D& HeightmapLocation : *HeightmapLocations)
	{
		FIntPoint ChunkCell2D = Get2DCellFromChunkLocation2D(HeightmapLocation, ChunkSize);
		TArray<int32>* ChunkZIndicesPtr = ChunkZIndicesBy2DCell.Find(ChunkCell2D);
		if (!ChunkZIndicesPtr)
			continue;

		for (const int32& ZIndex : *ChunkZIndicesPtr)
		{
			FIntVector ChunkCell = GetCellFromChunkLocation(FVector(HeightmapLocation.X, HeightmapLocation.Y, ZIndex * ChunkSize), ChunkSize);
			if (!UnhideChunk(ChunkCell))
				return; // If we didn't need to unhide one, the rest are already unhidden
		}
	}
}

void AChunkManager::UnreplicateChunk(FIntVector ChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::UnreplicateChunk);

	if (GetNetMode() == ENetMode::NM_Client || GetNetMode() == ENetMode::NM_Standalone)
	{
		UE_LOG(LogTemp, Error, TEXT("UnreplicateChunkCell was called on a client!"));
		return;
	}

	AChunkActor* Chunk = ChunksByCell.FindRef(ChunkCell);
	if (!Chunk || !IsValid(Chunk))
		return;

	Chunk->SetReplicates(false);
}

// 0 = Up, 1 = Down, 2 = East, 3 = West, 4 = North, 5 = South
inline bool GetVoxelOnBorder(FIntVector VoxelIntPosition, int32 VoxelCount, TArray<int32>& OutFaceDirectionIndices)
{
	OutFaceDirectionIndices.Empty();

	if (VoxelIntPosition.X <= 0)
		OutFaceDirectionIndices.Add(5); // South
	else if (VoxelIntPosition.X >= VoxelCount - 1)
		OutFaceDirectionIndices.Add(4); // North

	if (VoxelIntPosition.Y <= 0)
		OutFaceDirectionIndices.Add(3); // West	
	else if (VoxelIntPosition.Y >= VoxelCount - 1)
		OutFaceDirectionIndices.Add(2); // East

	if (VoxelIntPosition.Z <= 0)
		OutFaceDirectionIndices.Add(1);  // Down
	else if (VoxelIntPosition.Z >= VoxelCount - 1)
		OutFaceDirectionIndices.Add(0);  // Up

	return OutFaceDirectionIndices.Num() > 0;
}