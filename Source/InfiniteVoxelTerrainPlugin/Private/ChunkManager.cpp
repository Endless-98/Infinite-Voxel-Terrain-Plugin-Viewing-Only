// Copyright(c) 2024 Endless98. All Rights Reserved.

#include "ChunkManager.h"
#include "ChunkThreadChild.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "VoxelGameMode.h"
#include "ChunkModifierComponent.h"
#include "ChunkReplicationService.h"
#include "ChunkManagerServices.h"
#include "EngineUtils.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace IVTChunkVoxelHelpers
{
	static bool IsValidRleData(const TArray<uint8>& EncodedData)
	{
		return !EncodedData.IsEmpty() && EncodedData.Num() % 2 == 0;
	}

	static void AppendRun(TArray<uint8>& OutEncodedData, int32 Count, uint8 Value)
	{
		while (Count > 0)
		{
			int32 ChunkCount = FMath::Min(Count, static_cast<int32>(MAX_uint8));
			if (OutEncodedData.Num() >= 2 && OutEncodedData[OutEncodedData.Num() - 1] == Value)
			{
				uint8& LastCountRef = OutEncodedData[OutEncodedData.Num() - 2];
				int32 Mergeable = FMath::Min(ChunkCount, static_cast<int32>(MAX_uint8) - static_cast<int32>(LastCountRef));
				LastCountRef = static_cast<uint8>(static_cast<int32>(LastCountRef) + Mergeable);
				ChunkCount -= Mergeable;
				Count -= Mergeable;

				if (ChunkCount <= 0)
				{
					continue;
				}
			}

			OutEncodedData.Add(static_cast<uint8>(ChunkCount));
			OutEncodedData.Add(Value);
			Count -= ChunkCount;
		}
	}

	static bool TryGetVoxelFromRle(const TArray<uint8>& EncodedData, int32 TargetVoxelIndex, int32 TotalChunkVoxels, uint8& OutVoxelValue)
	{
		if (!IsValidRleData(EncodedData) || TargetVoxelIndex < 0 || TargetVoxelIndex >= TotalChunkVoxels)
		{
			return false;
		}

		int32 DecodedIndex = 0;
		for (int32 PairIndex = 0; PairIndex < EncodedData.Num(); PairIndex += 2)
		{
			const int32 RunCount = EncodedData[PairIndex];
			const uint8 RunValue = EncodedData[PairIndex + 1];
			const int32 NextDecodedIndex = DecodedIndex + RunCount;

			if (TargetVoxelIndex < NextDecodedIndex)
			{
				OutVoxelValue = RunValue;
				return true;
			}

			DecodedIndex = NextDecodedIndex;
		}

		return false;
	}

	static bool TrySetVoxelInRle(TArray<uint8>& EncodedData, int32 TargetVoxelIndex, uint8 NewVoxelValue, int32 TotalChunkVoxels)
	{
		if (!IsValidRleData(EncodedData) || TargetVoxelIndex < 0 || TargetVoxelIndex >= TotalChunkVoxels)
		{
			return false;
		}

		TArray<uint8> RebuiltData{};

		bool bDidWriteTarget{ false };
		int32 DecodedIndex = 0;

		for (int32 PairIndex = 0; PairIndex < EncodedData.Num(); PairIndex += 2)
		{
			const int32 RunCount = EncodedData[PairIndex];
			const uint8 RunValue = EncodedData[PairIndex + 1];
			const int32 RunStart = DecodedIndex;
			const int32 RunEnd = DecodedIndex + RunCount;

			if (!bDidWriteTarget && TargetVoxelIndex >= RunStart && TargetVoxelIndex < RunEnd)
			{
				if (RunValue == NewVoxelValue)
				{
					return true;
				}
				else
				{
					if (RebuiltData.IsEmpty())
					{
						RebuiltData.Reserve(EncodedData.Num() + 6);
						if (PairIndex > 0)
						{
							RebuiltData.Append(EncodedData.GetData(), PairIndex);
						}
					}

					const int32 LeftCount = TargetVoxelIndex - RunStart;
					const int32 RightCount = RunEnd - TargetVoxelIndex - 1;

					AppendRun(RebuiltData, LeftCount, RunValue);
					AppendRun(RebuiltData, 1, NewVoxelValue);
					AppendRun(RebuiltData, RightCount, RunValue);
					bDidWriteTarget = true;
				}
			}
		else if (!RebuiltData.IsEmpty())
			{
				AppendRun(RebuiltData, RunCount, RunValue);
			}

			DecodedIndex = RunEnd;
		}

		if (!bDidWriteTarget)
		{
			return false;
		}

		EncodedData = MoveTemp(RebuiltData);
		return true;
	}

	static void BuildDensePatchFromSparse(const TMap<int32, uint8>& SparsePatch, int32 TotalChunkVoxels, TArray<uint8>& OutDensePatch)
	{
		OutDensePatch.Init(UINT8_MAX, TotalChunkVoxels);
		for (const TPair<int32, uint8>& Entry : SparsePatch)
		{
			if (OutDensePatch.IsValidIndex(Entry.Key))
			{
				OutDensePatch[Entry.Key] = Entry.Value;
			}
		}
	}

	static void BuildSparsePatchFromDense(const TArray<uint8>& DensePatch, TMap<int32, uint8>& OutSparsePatch)
	{
		OutSparsePatch.Empty();
		for (int32 Index = 0; Index < DensePatch.Num(); ++Index)
		{
			if (DensePatch[Index] != UINT8_MAX)
			{
				OutSparsePatch.Add(Index, DensePatch[Index]);
			}
		}
	}
}

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
	ProcessWorldCommands();
	if(UpdateTrackedLocations())
		UpateNearbyChunkCollisions();

	if (GetNetMode() == ENetMode::NM_DedicatedServer || GetNetMode() == ENetMode::NM_ListenServer)
		HandleClientNeededServerData(); // Could happen asyncronously on a background thread if we can't get the lock immediately

	if (!ChunksToDestroyQueue.IsEmpty())
		DequeueAndDestroyChunks();

	UpdateRegionsAsync();
}

void AChunkManager::EnqueueWorldCommand(FChunkWorldCommand&& Command)
{
	FScopeLock Lock(&WorldCommandMutex);
	PendingWorldCommands.Add(MoveTemp(Command));
}

void AChunkManager::ProcessWorldCommands()
{
	TArray<FChunkWorldCommand> LocalCommands;
	{
		FScopeLock Lock(&WorldCommandMutex);
		if (PendingWorldCommands.IsEmpty())
		{
			return;
		}
		LocalCommands = MoveTemp(PendingWorldCommands);
		PendingWorldCommands.Empty();
	}

	for (FChunkWorldCommand& Command : LocalCommands)
	{
		switch (Command.Type)
		{
		case EChunkWorldCommandType::Spawn:
			if (ChunkThreads.IsValidIndex(0) && ChunkThreads[0] && Command.ConstructionData.IsValid())
			{
				ChunkThreads[0]->SpawnChunkFromConstructionData(Command.ConstructionData, Command.ChunkGenRadius, Command.CollisionGenRadius, Command.bShouldGenerateMesh);
			}
			break;
		case EChunkWorldCommandType::DestroyOrHide:
		{
			bool bWasHidden = false;
			DestroyOrHideChunk(Command.ChunkCell, bWasHidden);
			break;
		}
		case EChunkWorldCommandType::Unreplicate:
			UnreplicateChunk(Command.ChunkCell);
			break;
		case EChunkWorldCommandType::Hide:
			HideChunk(Command.ChunkCell);
			break;
		case EChunkWorldCommandType::Unhide:
			UnhideChunk(Command.ChunkCell);
			break;
		default:
			break;
		}
	}
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
			if (!MemoryReader.AtEnd())
				MemoryReader << OutTerrainSettings.LodNearDistanceInChunks;
			if (!MemoryReader.AtEnd())
				MemoryReader << OutTerrainSettings.LodFarDistanceInChunks;
			if (!MemoryReader.AtEnd())
				MemoryReader << OutTerrainSettings.LodDistanceCurveExponent;
			if (!MemoryReader.AtEnd())
				MemoryReader << OutTerrainSettings.MaxLodStepPower;
			if (!MemoryReader.AtEnd())
				MemoryReader << OutTerrainSettings.bUseGreedyMeshing;
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
	MemoryWriter << TerrainSettings.LodNearDistanceInChunks;
	MemoryWriter << TerrainSettings.LodFarDistanceInChunks;
	MemoryWriter << TerrainSettings.LodDistanceCurveExponent;
	MemoryWriter << TerrainSettings.MaxLodStepPower;
	MemoryWriter << TerrainSettings.bUseGreedyMeshing;

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

		return;
	}

	PlayerRetryCount++;
	if (PlayerRetryCount < MaxRetries)
	{
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
			LodNearDistanceInChunks,
			LodFarDistanceInChunks,
			LodDistanceCurveExponent,
			MaxLodStepPower,
			bUseGreedyMeshing,
			Seed,
			SaveGameName,
			ThreadIndex);

		ChunkThreads.Add(ChunkThread);
	}
}

bool AChunkManager::UpdateTrackedLocations()
{
	return FChunkManagerTrackingService::UpdateTrackedLocations(this);
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
	if (!Chunk->bAreVoxelsCompressed)
	{
		IVTChunkUtilities::RunLengthEncode(Chunk->Voxels);
		Chunk->bAreVoxelsCompressed = true;
	}

	FIntVector VoxelIntPosition;
	int32 VoxelIndex{ GetVoxelIndex(GetLocationFromChunkCell(ChunkCell, ChunkSize), VoxelWorldLocation, VoxelIntPosition) };
	if (!IVTChunkVoxelHelpers::TrySetVoxelInRle(Chunk->Voxels, VoxelIndex, static_cast<uint8>(VoxelValue), TotalChunkVoxels))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to update encoded voxel data for chunk '%s' at voxel index %i"), *ChunkCell.ToString(), VoxelIndex);
		return;
	}

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
	if (!Chunk || !IsValid(Chunk))
		return;
	if (!Chunk->bAreVoxelsCompressed)
	{
		IVTChunkUtilities::RunLengthEncode(Chunk->Voxels);
		Chunk->bAreVoxelsCompressed = true;
	}

	bool bShouldGenerateCollision{ true };
	FChunkMeshData ChunkMeshData;
	if (ChunkThreads.IsValidIndex(0) && ChunkThreads[0])
		ChunkThreads[0]->GenerateChunkMeshDataFromEncoded(ChunkMeshData, Chunk->Voxels, Chunk->ChunkCell, bShouldGenerateCollision);
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
		TMap<FIntVector, TMap<int32, uint8>>* ModifiedVoxelsByCell{};
		ModifiedVoxelsByCell = ModifiedVoxelsByCellByRegion.Find(Region);
		if (ModifiedVoxelsByCell == nullptr)
		{
			ModifiedVoxelsByCell = new TMap<FIntVector, TMap<int32, uint8>>;
			ModifiedVoxelsByCellByRegion.Add(Region, *ModifiedVoxelsByCell);
		}

		if (ModifiedVoxelsByCell->Contains(ChunkCell))
		{
			TMap<int32, uint8>* ModifiedVoxels = ModifiedVoxelsByCell->Find(ChunkCell);
			if (ModifiedVoxels)
				ModifiedVoxels->Add(VoxelIndex, static_cast<uint8>(VoxelValue));
		}
		else // Should only need to happen once for a given chunk:
		{
			TMap<int32, uint8> NewSparsePatch{};
			NewSparsePatch.Add(VoxelIndex, static_cast<uint8>(VoxelValue));
			ModifiedVoxelsByCell->Emplace(ChunkCell, MoveTemp(NewSparsePatch));
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
		ChunkConstructionData->bAreVoxelsCompressed = true;
		IVTChunkUtilities::RunLengthEncode(ChunkConstructionData->Voxels);
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
		return -1;
	}
	if (!Chunk->bAreVoxelsCompressed)
	{
		IVTChunkUtilities::RunLengthEncode(Chunk->Voxels);
		Chunk->bAreVoxelsCompressed = true;
	}

	FIntVector OutVoxelIntPosition{};
	int32 VoxelIndex = GetVoxelIndex(Chunk->GetActorLocation(), VoxelWorldLocation, OutVoxelIntPosition);
	uint8 VoxelValue{};
	if (!IVTChunkVoxelHelpers::TryGetVoxelFromRle(Chunk->Voxels, VoxelIndex, TotalChunkVoxels, VoxelValue))
	{
		return -1;
	}

	return VoxelValue;
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
			FoliageNoiseScale, ChunkDeletionBuffer, CollisionGenerationRadius, VoxelSize, VoxelCount, LodNearDistanceInChunks, LodFarDistanceInChunks, LodDistanceCurveExponent, MaxLodStepPower, bUseGreedyMeshing);

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
	LodNearDistanceInChunks = NewTerrainSettings.LodNearDistanceInChunks;
	LodFarDistanceInChunks = NewTerrainSettings.LodFarDistanceInChunks;
	LodDistanceCurveExponent = NewTerrainSettings.LodDistanceCurveExponent;
	MaxLodStepPower = NewTerrainSettings.MaxLodStepPower;
	bUseGreedyMeshing = NewTerrainSettings.bUseGreedyMeshing;
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
	return FChunkManagerRegionService::UpdateRegionVariables(this);
}

void AChunkManager::CalculateNeededRegions(FIntPoint CenterRegion, TArray<FIntPoint>& NeededRegions)
{
	FChunkManagerRegionService::CalculateNeededRegions(this, CenterRegion, NeededRegions);
}

void AChunkManager::RemoveRegionAndAddPendingSave(APlayerController* PlayerController, FIntPoint& OldRegion)
{
	FChunkManagerRegionService::RemoveRegionAndAddPendingSave(this, PlayerController, OldRegion);
}

void AChunkManager::AddRegionPendingDataIfNeeded(APlayerController* PlayerController, FIntPoint& Region)
{
	FChunkManagerRegionService::AddRegionPendingDataIfNeeded(this, PlayerController, Region);
}

void AChunkManager::ClientReadyForReplication(APlayerController* PlayerController)
{
	FChunkReplicationService::ClientReadyForReplication(this, PlayerController);
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

		TMap<FIntVector, TMap<int32, uint8>>& ModifiedVoxelsByCell{ *ModifiedVoxelsByCellByRegion.Find(Region) };
		for (TPair<FIntVector, TMap<int32, uint8>>& CellVoxelPair : ModifiedVoxelsByCell)
		{
			FIntVector Cell{ CellVoxelPair.Key };
			TArray<uint8> CompressedVoxels{};
			IVTChunkVoxelHelpers::BuildDensePatchFromSparse(CellVoxelPair.Value, TotalChunkVoxels, CompressedVoxels);
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
				TMap<FIntVector, TMap<int32, uint8>> ModifiedVoxelsByCell{};
				for (FEncodedVoxelData& EncodedVoxelData : RegionData.EncodedVoxelsArrays)
				{
					RunLengthDecode(EncodedVoxelData.Voxels, EncodedVoxelData.ChunkCell);
					TMap<int32, uint8> SparsePatch{};
					IVTChunkVoxelHelpers::BuildSparsePatchFromDense(EncodedVoxelData.Voxels, SparsePatch);
					ModifiedVoxelsByCell.Add(EncodedVoxelData.ChunkCell, MoveTemp(SparsePatch));
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
				TMap<FIntVector, TMap<int32, uint8>>* ModifiedVoxelsByCell{ ModifiedVoxelsByCellByRegion.Find(RegionData.Region) };
				if (ModifiedVoxelsByCell == nullptr)
				{
					UE_LOG(LogTemp, Error, TEXT("ModifiedVoxelsByCell was nullptr!"));
					AddToRegionsThatHaveData(RegionData.Region);

					return;
				}
				for (FEncodedVoxelData& EncodedVoxelData : RegionData.EncodedVoxelsArrays)
				{
					RunLengthDecode(EncodedVoxelData.Voxels, EncodedVoxelData.ChunkCell);
					TMap<int32, uint8> SparsePatch{};
					IVTChunkVoxelHelpers::BuildSparsePatchFromDense(EncodedVoxelData.Voxels, SparsePatch);
					ModifiedVoxelsByCell->Add(EncodedVoxelData.ChunkCell, MoveTemp(SparsePatch));
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

// Do not call from game thread. Server calls this function when it sees a client has moved
void AChunkManager::ReplicateChunkNames(FIntVector CenterCell, bool bEnsureNoneMissing)
{
	FChunkReplicationService::ReplicateChunkNames(this, CenterCell, bEnsureNoneMissing);
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
	FChunkReplicationService::SendChunkNameDataToClients(this, ChunkNameData);
}

// Runs on client, called by ChunkModifierComponent when we Receive the needed data to name our chunks the same as the server
void AChunkManager::ClientSetChunkNames(const FChunkNameData& ChunkNameData)
{
	FChunkReplicationService::ClientSetChunkNames(this, ChunkNameData);
}

void AChunkManager::SetChunkName(AChunkActor* Chunk, const FIntVector& ChunkRepCell, const int32& ChunkRepCount)
{
	FChunkReplicationService::SetChunkName(this, Chunk, ChunkRepCell, ChunkRepCount);
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
void AChunkManager::UnhideChunksInHeightmapLocations(const TArray<FVector2D>& HeightmapLocations)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AChunkManager::UnhideChunksInHeightmapLocations);
	
	if (GetNetMode() != NM_Client || HeightmapLocations.IsEmpty())
		return;

	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this, HeightmapLocations]()
			{ UnhideChunksInHeightmapLocations(HeightmapLocations); });
		return;
	}
	for (const FVector2D& HeightmapLocation : HeightmapLocations)
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

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIVTRleMutationCorrectnessTest, "IVT.ChunkMath.RLEMutationCorrectness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIVTRleMutationCorrectnessTest::RunTest(const FString& Parameters)
{
	TArray<uint8> DenseVoxels{};
	DenseVoxels.Init(0, 32);
	DenseVoxels[10] = 4;
	DenseVoxels[11] = 4;
	DenseVoxels[12] = 7;

	TArray<uint8> Encoded = DenseVoxels;
	RunLengthEncode(Encoded, FIntVector::ZeroValue);

	TestTrue(TEXT("Encoded voxels should be valid run-length format"), IVTChunkVoxelHelpers::IsValidRleData(Encoded));

	uint8 ValueAt12{};
	TestTrue(TEXT("TryGetVoxelFromRle should read valid voxel"), IVTChunkVoxelHelpers::TryGetVoxelFromRle(Encoded, 12, DenseVoxels.Num(), ValueAt12));
	TestEqual(TEXT("Voxel index 12 should match source data"), static_cast<int32>(ValueAt12), 7);

	TestTrue(TEXT("TrySetVoxelInRle should update an interior voxel"), IVTChunkVoxelHelpers::TrySetVoxelInRle(Encoded, 11, 9, DenseVoxels.Num()));
	uint8 UpdatedValue{};
	TestTrue(TEXT("TryGetVoxelFromRle should read updated voxel"), IVTChunkVoxelHelpers::TryGetVoxelFromRle(Encoded, 11, DenseVoxels.Num(), UpdatedValue));
	TestEqual(TEXT("Updated voxel value should be applied"), static_cast<int32>(UpdatedValue), 9);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIVTRleNoOpMutationTest, "IVT.ChunkMath.RLENoopMutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIVTRleNoOpMutationTest::RunTest(const FString& Parameters)
{
	TArray<uint8> DenseVoxels{};
	DenseVoxels.Init(3, 64);
	DenseVoxels[32] = 5;

	TArray<uint8> Encoded = DenseVoxels;
	RunLengthEncode(Encoded, FIntVector::ZeroValue);
	const TArray<uint8> BeforeMutation = Encoded;

	TestTrue(TEXT("No-op mutation should return true"), IVTChunkVoxelHelpers::TrySetVoxelInRle(Encoded, 32, 5, DenseVoxels.Num()));
	TestEqual(TEXT("No-op mutation should preserve encoded length"), Encoded.Num(), BeforeMutation.Num());
	TestTrue(TEXT("No-op mutation should preserve encoded payload"), Encoded == BeforeMutation);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIVTRleBoundaryMutationTest, "IVT.ChunkMath.RLEBoundaryMutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIVTRleBoundaryMutationTest::RunTest(const FString& Parameters)
{
	TArray<uint8> DenseVoxels{};
	DenseVoxels.Init(1, 16);

	TArray<uint8> Encoded = DenseVoxels;
	RunLengthEncode(Encoded, FIntVector::ZeroValue);

	TestTrue(TEXT("Boundary mutation at index 0 should succeed"), IVTChunkVoxelHelpers::TrySetVoxelInRle(Encoded, 0, 2, DenseVoxels.Num()));
	TestTrue(TEXT("Boundary mutation at last index should succeed"), IVTChunkVoxelHelpers::TrySetVoxelInRle(Encoded, 15, 4, DenseVoxels.Num()));

	uint8 FirstValue{};
	uint8 LastValue{};
	TestTrue(TEXT("Read first voxel"), IVTChunkVoxelHelpers::TryGetVoxelFromRle(Encoded, 0, DenseVoxels.Num(), FirstValue));
	TestTrue(TEXT("Read last voxel"), IVTChunkVoxelHelpers::TryGetVoxelFromRle(Encoded, 15, DenseVoxels.Num(), LastValue));
	TestEqual(TEXT("First voxel should reflect update"), static_cast<int32>(FirstValue), 2);
	TestEqual(TEXT("Last voxel should reflect update"), static_cast<int32>(LastValue), 4);

	return true;
}
#endif