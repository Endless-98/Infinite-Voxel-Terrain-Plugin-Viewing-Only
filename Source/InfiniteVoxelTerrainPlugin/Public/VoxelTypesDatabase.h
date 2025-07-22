// Copyright(c) 2024 Endless98. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "VoxelTypesDatabase.generated.h"

UENUM(BlueprintType)
enum class EFaceDirection : uint8
{
	Up UMETA(DisplayName = "Up"),
	Down UMETA(DisplayName = "Down"),
	East UMETA(DisplayName = "East"), // Right
	West UMETA(DisplayName = "West"), // Left
	North UMETA(DisplayName = "North"), // Front
	South UMETA(DisplayName = "South"), // Back
	None UMETA(DisplayName = "None")
};

USTRUCT(BlueprintType)
struct FVoxelDefinition
{
	GENERATED_USTRUCT_BODY()

public:
	FVoxelDefinition() {}
	FVoxelDefinition(UMaterial* InVoxelMaterial, UTexture2D* InIconTopTexture = nullptr, UTexture2D* InIconSideTexture = nullptr)
		:
		VoxelMaterial(InVoxelMaterial),
		IconTopTexture(InIconTopTexture),
		IconSideTexture(InIconSideTexture)
	{}

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Properties")
	UMaterial* VoxelMaterial{}; // Material for the voxel

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Properties")
	UTexture2D* IconTopTexture{}; // Will be used for sides if SidesTexture is unspecified
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Properties")
	UTexture2D* IconSideTexture{}; // Only use if you want a different texture for the sides

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Properties")
	TEnumAsByte<ECollisionResponse> CollisionResponse{ ECollisionResponse::ECR_Block };	// How the voxel responds to all collisions

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Properties")
	bool bIsTranslucent{}; // If true, adjacent voxels will create faces toward this voxel
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Properties")
	bool bIsAir{}; // If true, this voxel will ignore all other settings and be treated as air
};

UCLASS()
class INFINITEVOXELTERRAINPLUGIN_API AVoxelTypesDatabase : public AActor
{
	GENERATED_BODY()

public:
	AVoxelTypesDatabase();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Definitions")
	TArray<FVoxelDefinition> VoxelDefinitions{};

protected:

	TArray<ECollisionResponse> VoxelTypes{};
};