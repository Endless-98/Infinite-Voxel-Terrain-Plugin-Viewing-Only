// Copyright(c) 2024 Endless98. All Rights Reserved.

#include "VoxelGameMode.h"
#include "ChunkManager.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

void AVoxelGameMode::BeginPlay()
{
    Super::BeginPlay();
}

void AVoxelGameMode::PostLogin(APlayerController* NewPlayer)
{
    Super::PostLogin(NewPlayer);

    SetupHostChunkManagerRef();

    PendingPlayers.Add(NewPlayer);
    StartRetryTimer();
}

void AVoxelGameMode::Logout(AController* Exiting)
{
	Super::Logout(Exiting);

	if (HostChunkManager)
		HostChunkManager->RemoveTrackedPlayer(static_cast<APlayerController*>(Exiting));
}

bool AVoxelGameMode::SetupHostChunkManagerRef()
{
    if (HostChunkManager != nullptr)
        return true;

    TArray<AActor*> ChunkManagers;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AChunkManager::StaticClass(), ChunkManagers);

    if (ChunkManagers.Num() > 0)
    {
        HostChunkManager = Cast<AChunkManager>(ChunkManagers[0]);
        return true;
    }

    return false;
}

void AVoxelGameMode::StartRetryTimer()
{
    if (!GetWorldTimerManager().IsTimerActive(CheckForPawnTimerHandle))
    {
        GetWorldTimerManager().SetTimer(CheckForPawnTimerHandle, this, &AVoxelGameMode::AttemptToAddTrackedActors, CheckRetryDelay, true);
    }
}

void AVoxelGameMode::AttemptToAddTrackedActors()
{
    for (int32 Index{ PendingPlayers.Num() - 1 }; Index >= 0; --Index)
    {
        APlayerController* NewPlayer = PendingPlayers[Index];
        APawn* PlayerPawn = NewPlayer->GetPawn();

        if (NewPlayer && PlayerPawn && IsValid(PlayerPawn) && HostChunkManager)
        {
            PendingPlayers.RemoveAt(Index);
            if(!HostChunkManager->AddTrackedPlayer(NewPlayer))
				continue;
        } 
    }

    // Stop the timer if there are no pending players
    if (PendingPlayers.Num() == 0)
    {
        GetWorldTimerManager().ClearTimer(CheckForPawnTimerHandle);
    }
}