// Copyright(c) 2024 Endless98. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameMode.h"
#include "TimerManager.h"
#include "ChunkManager.h"
#include "Net/UnrealNetwork.h"
#include "VoxelGameMode.generated.h"

class AChunkManager;

UCLASS()
class INFINITEVOXELTERRAINPLUGIN_API AVoxelGameMode : public AGameMode
{
    GENERATED_BODY()

protected:
    virtual void PostLogin(APlayerController* NewPlayer) override;
    virtual void Logout(AController* Exiting) override;

    virtual void BeginPlay() override;

private:
    friend class AChunkManager;

    bool SetupHostChunkManagerRef();
    void StartRetryTimer();
    void AttemptToAddTrackedActors();
    void AttemptToAddTrackedActor(APlayerController* NewPlayer);

    float CheckRetryDelay{ 0.1 };
    FTimerHandle CheckForPawnTimerHandle;
    FTimerHandle CheckForChunkManagerTimerHandle;

    TArray<APlayerController*> PendingPlayers;

    AChunkManager* HostChunkManager;
    int32 TotalTrackedClients{};
};