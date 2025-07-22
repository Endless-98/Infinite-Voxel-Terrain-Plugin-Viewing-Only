// Copyright(c) 2024 Endless98. All Rights Reserved.

#include "ChunkThreadChild.h"
#include "ChunkManager.h"
#include "Interface\Core\RealtimeMeshDataTypes.h"
#include "Engine/World.h"
#include "Containers/Array.h"
#include "Math/IntVector.h"
#include "DrawDebugHelpers.h"

// This class is an abstraction of the FChunkThread class. It provides high level access to the functions you are most likely to want to modify, without needing to understand the lower level details of the FChunkThread class.

void FChunkThreadChild::InitializeNoiseGenerators()
{
	BiomeNoiseGenerator = FastNoise::NewFromEncodedNodeTree("IgAAAEBAmpmZPhsAEABxPQo/GwAeABcAAAAAAAAAgD9cj8I+AACAPw0AAwAAAAAAQEAJAADsUbg+AOxRuD4AAAAAAAETAI/CdT7//wEAAOxROD4AAAAAQA==");
	PlainsNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EQACAAAAAAAgQBAAAAAAQCcAAQAAABsAIAAJAAAAAAAAAArXoz8BEwAK1yM/DQACAAAArkexQP//AAAAKVxPPwDNzEw+AM3MTD4AMzMzPwAAAAA/");
	ForestNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EQACAAAAAAAgQBAAAAAAQCcAAQAAABsAIAAJAAAAAAAAAArXoz8BEwAK1yM/DQACAAAArkexQP//AAAAKVxPPwDNzEw+AM3MTD4AMzMzPwAAAAA/");
	HillsNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EwBcj8I+EQADAAAAcT1qQBAAzcxMPg0AAwAAAB+FS0AnAAEAAAAJAAAfhes+AHE9Cj8ArkdhPwApXI8+AD0K1z4=");
	MountainsNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EwAzM7M+EADhehQ/DQADAAAAhevBQCcAAQAAAAYAAAAAAD8AAACAPwAK1yM+");
}

void FChunkThreadChild::GenerateHeightmap(TArray<int16>& OutGeneratedHeightmap, const FVector2D& NeededHeightmapLocation, TArray<int32>& OutNeededChunksVerticalIndices)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateHeightmap);

	int32 const HeightmapVoxels1D{ VoxelCount + 2 };
	int32 const TotalHeightmapVoxels{ HeightmapVoxels1D * HeightmapVoxels1D };

	OutGeneratedHeightmap.Empty();
	std::vector<float> BiomeHeightmap{};
	OutGeneratedHeightmap.Reserve(TotalHeightmapVoxels);
	BiomeHeightmap.reserve(TotalHeightmapVoxels);
	const FVector2D NoiseStartPoint((NeededHeightmapLocation / FVector2D(VoxelSize)) - 1);
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateHeightmap::FastNoise2NoiseGen);
		BiomeNoiseGenerator->GenUniformGrid2D(BiomeHeightmap.data(), NoiseStartPoint.X, NoiseStartPoint.Y, HeightmapVoxels1D, HeightmapVoxels1D, TerrainNoiseScale * BiomeNoiseScale, Seed);
	}

	float HighestVoxel{ FLT_MIN };
	float LowestVoxel{ FLT_MAX };

	int32 LowerIndex{};
	int32 UpperIndex{};

	TArray<TPair<int32, float>> BiomeIndexPercentPair;
	BiomeIndexPercentPair.Reserve(TotalHeightmapVoxels);
	// === First we calculate which biomes are present at each NoiseIndex ===
	const TArray<float> BiomeValues{ -0.666666667, -0.333333333, 0.0, 0.333333333, 0.666666667 };
	for (int32 NoiseIndex{}; NoiseIndex < TotalHeightmapVoxels; NoiseIndex++)
	{
		LowerIndex = 1;
		UpperIndex = 0;

		const float& BiomeNoisePoint = BiomeHeightmap[NoiseIndex];
		for (int32 BiomeIndex{}; BiomeIndex < BiomeValues.Num(); ++BiomeIndex)
		{
			float BiomeValue = BiomeValues[BiomeIndex];
			if (BiomeNoisePoint == BiomeValue)
			{
				LowerIndex = BiomeIndex;
				UpperIndex = BiomeIndex;
				BiomeIndexPercentPair.Add(TPair<int32, float>(BiomeIndex, 1.0f));
				break; // No need to continue if BiomeNoisePoint matches exactly
			}
			else if (BiomeNoisePoint > BiomeValue)
			{
				LowerIndex = BiomeIndex;
			}
			else if (BiomeNoisePoint < BiomeValue)
			{
				UpperIndex = BiomeIndex;
				break; // No need to continue further once UpperIndex is found
			}
		}

		if (LowerIndex != UpperIndex)
		{
			float LowerPercentage = (BiomeNoisePoint - BiomeValues[UpperIndex]) / (BiomeValues[LowerIndex] - BiomeValues[UpperIndex]);
			float UpperPercentage = 1.0f - LowerPercentage;

			BiomeIndexPercentPair.Add(TPair<int32, float>(LowerIndex, LowerPercentage));
			BiomeIndexPercentPair.Add(TPair<int32, float>(UpperIndex, UpperPercentage));
		}
	}

	/// === Next we use that BiomeIndexPercentPair to determine what Noise should be generated for this Point
	int32 PositionIndex{};
	FVector2D NoiseLocation{};
	bool bIsFirstPoint{ true };
	bool bHasAnotherPoint{ false };

	// BiomeIndexPercentPair Contains either 1 or 2 elements for each noise Point in this chunk
	for (const TPair<int32, float>& BiomePoint : BiomeIndexPercentPair)
	{
		bool bPointBelongsToAdjacentCell{ false };

		if (bIsFirstPoint) // Only calculate the postion once per PositionIndex change
		{
			int32 LocationX = PositionIndex % HeightmapVoxels1D;
			int32 LocationY = PositionIndex / HeightmapVoxels1D;
			NoiseLocation = (NoiseStartPoint + FVector2D(LocationX, LocationY)) * TerrainNoiseScale;

			// Check if the location is on the border
			if (LocationX <= 0 || LocationY <= 0 || LocationX >= HeightmapVoxels1D - 1 || LocationY >= HeightmapVoxels1D - 1)
				bPointBelongsToAdjacentCell = true;

			// If the Point isn't 100% one PositionIndex then we know there is another Point
			bHasAnotherPoint = (BiomePoint.Value != 1.0);
		}
		else
			bHasAnotherPoint = false;

		float NoisePoint{};

		if (!bIsRunning)
			return;

		switch (BiomePoint.Key)
		{
		default:
			NoisePoint = 0.0;
			break;
		case 0: // Flat
			NoisePoint = 0.0;
			break;
		case 1: // Forest
			NoisePoint = ForestNoiseGenerator->GenSingle2D(NoiseLocation.X, NoiseLocation.Y, Seed) * 0.4;
			break;
		case 2: // Grassy Plains
			NoisePoint = PlainsNoiseGenerator->GenSingle2D(NoiseLocation.X, NoiseLocation.Y, Seed) * 0.7;
			break;
		case 3: // Rough Hills
			NoisePoint = HillsNoiseGenerator->GenSingle2D(NoiseLocation.X, NoiseLocation.Y, Seed) * 1.4;
			break;
		case 4: // Mountains
			NoisePoint = MountainsNoiseGenerator->GenSingle2D(NoiseLocation.X, NoiseLocation.Y, Seed) * 6.3;
			break;
		}

		// Scale the noise by the percentage of it's biome found at this location
		NoisePoint *= BiomePoint.Value;
		int32 VoxelHeight = (NoisePoint * VoxelSize) * TerrainHeightMultiplier;
		if (bIsFirstPoint)
		{
			VoxelHeight -= VoxelCount / 2.0;
			OutGeneratedHeightmap.Add(VoxelHeight);
		}
		else // Add the second Point to the first
			VoxelHeight = OutGeneratedHeightmap[PositionIndex] += VoxelHeight;

		if (!bHasAnotherPoint)
		{
			VoxelHeight *= VoxelSize;
			VoxelHeight -= VoxelSize;
			VoxelHeight -= FMath::GridSnap(ChunkSize / 2, VoxelSize);

			// Calculate the lowest and highest voxels so we know which vertical chunks to spawn
			if (VoxelHeight > HighestVoxel)
				HighestVoxel = VoxelHeight;
			if (VoxelHeight < LowestVoxel)
				LowestVoxel = VoxelHeight;
			PositionIndex++; // Move on to next position
			bIsFirstPoint = true;
		}
		else // Check this Point again
			bIsFirstPoint = false;
	}

	int32 HighestChunkIndex = FMath::GridSnap(HighestVoxel, ChunkSize) / ChunkSize;
	int32 LowestChunkIndex = FMath::GridSnap(LowestVoxel, ChunkSize) / ChunkSize;
	for (int32 ChunkIndex{ LowestChunkIndex }; ChunkIndex <= HighestChunkIndex; ChunkIndex++)
		OutNeededChunksVerticalIndices.Add(ChunkIndex);
}

bool FChunkThreadChild::GenerateChunkVoxels(TArray<uint8>& Voxels, const TArray<int16>& Heightmap, const FVector& ChunkLocation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateChunkVoxels);

	Voxels.Empty(TotalChunkVoxels);

	bool bIsBuried{ false }; // Used to determine if the ChunkMesh would be empty
	bool bIsAllAir{ false }; // Used to determine if the ChunkMesh would be empty

	const uint8 GrassBlockIndex = 1;
	const uint8 DirtBlockIndex = 2;
	const uint8 StoneBlockIndex = 4;
	const uint8 DirtDepth = 2;

	for (int32 Y{ -1 }; Y < VoxelCount + 1; Y++)
	{
		for (int32 X{ -1 }; X < VoxelCount + 1; X++)
		{
			int32 TerrainNoiseSample{ 25 };
			int32 SampleIndex{ (X + 1) * (VoxelCount + 2) + (Y + 1) };

			if (Heightmap.IsValidIndex(SampleIndex))
				TerrainNoiseSample = Heightmap[SampleIndex];

			for (int32 Z{ -1 }; Z < VoxelCount + 1; Z++)
			{
				int32 VoxelZ{ Z + FMath::RoundToInt32((ChunkLocation.Z / VoxelSize)) };

				if (VoxelZ == TerrainNoiseSample - 1)
				{
					bIsAllAir = false;

					Voxels.Add(GrassBlockIndex);
				}
				else if (VoxelZ < TerrainNoiseSample - 1)
				{
					if (VoxelZ < TerrainNoiseSample - 1 - DirtDepth)
					{
						bIsAllAir = false;

						Voxels.Add(StoneBlockIndex);
					}
					else
					{
						bIsAllAir = false;

						Voxels.Add(DirtBlockIndex);
					}
				}
				else if (VoxelZ >= TerrainNoiseSample)
				{
					bIsBuried = false;

					Voxels.Add(0);
				}
			}
		}
	}

	if (Voxels.IsEmpty() || bIsBuried || bIsAllAir)
		return false;

	return true;
}

void FChunkThreadChild::GenerateChunkMeshData(FChunkMeshData& OutChunkMeshData, TArray<uint8>& Voxels, const FIntVector ChunkCell, const bool bShouldGenerateCollisionAtChunkSpawn)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTerrainChunkThread::GenerateChunkMeshData);

	OutChunkMeshData.CollisionType = ECR_Block;
	OutChunkMeshData.ChunkCell = ChunkCell;
	OutChunkMeshData.bShouldGenCollision = bShouldGenerateCollisionAtChunkSpawn;

	if (Voxels.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Tried to generate a chunk with no voxels!"));
		return;
	}

	RealtimeMesh::TRealtimeMeshStreamBuilder<FVector3f> PositionBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Position, RealtimeMesh::GetRealtimeMeshBufferLayout<FVector3f>()));
	RealtimeMesh::TRealtimeMeshStreamBuilder<RealtimeMesh::FRealtimeMeshTangentsHighPrecision, RealtimeMesh::FRealtimeMeshTangentsNormalPrecision> TangentBuilder(
		OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Tangents, RealtimeMesh::GetRealtimeMeshBufferLayout<RealtimeMesh::FRealtimeMeshTangentsNormalPrecision>()));
	RealtimeMesh::TRealtimeMeshStreamBuilder<FVector2f, FVector2DHalf> TexCoordsBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::TexCoords, RealtimeMesh::GetRealtimeMeshBufferLayout<FVector2DHalf>()));
	RealtimeMesh::TRealtimeMeshStreamBuilder<FColor> ColorBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Color, RealtimeMesh::GetRealtimeMeshBufferLayout<FColor>()));
	RealtimeMesh::TRealtimeMeshStreamBuilder<uint32, uint16> PolygroupsBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::PolyGroups, RealtimeMesh::GetRealtimeMeshBufferLayout<uint16>()));
	TArray<TArray<FVector>> TrianglesByVoxelValue{};

	int32 NumberOfTris{};
	FVector3f ChunkMeshOffset{ -ChunkSize / 2 };
	TSet<uint8> VoxelValuesInThisChunk{};

	int32 VoxelIndex{};
	int32 AdjacentVoxelIndex{};
	FVector3f VoxelLocation{ ChunkMeshOffset };
	FIntVector XYZ{};
	// Loop through all voxels in the chunk except the border voxels technically belonging to adjacent chunks
	// The chunk is the same size in each direction. This knowledge can help us take some shortcuts
	for (int32 X{}; X < VoxelCount; X++)
	{
		XYZ.X = X;
		VoxelLocation.X = ChunkMeshOffset.X + (X * VoxelSize);
		for (int32 Y{}; Y < VoxelCount; Y++)
		{
			XYZ.Y = Y;
			VoxelLocation.Y = ChunkMeshOffset.Y + (Y * VoxelSize);
			for (int32 Z{}; Z < VoxelCount; Z++)
			{
				XYZ.Z = Z;
				GetVoxelIndex(VoxelIndex, X, Y, Z);
				VoxelLocation.Z = ChunkMeshOffset.Z + (Z * VoxelSize);

				if (!Voxels.IsValidIndex(VoxelIndex))
					continue;
				const uint8& VoxelValue{ Voxels[VoxelIndex] };
				if (VoxelDefinitions[VoxelValue].bIsAir) // Skip the voxel if it is air
					continue;

				FSetElementId PolyGroupID = VoxelValuesInThisChunk.FindId(VoxelValue);

				for (int32 FaceIndex{}; FaceIndex < 6; FaceIndex++)
				{
					GetVoxelIndex(AdjacentVoxelIndex, XYZ + FaceIntDirections[FaceIndex]);
					if (!(Voxels.IsValidIndex(AdjacentVoxelIndex)))
						continue;
					const uint8& AdjacentVoxelValue = Voxels[AdjacentVoxelIndex];
					if (!VoxelDefinitions[AdjacentVoxelValue].bIsTranslucent && !VoxelDefinitions[AdjacentVoxelValue].bIsAir) // If this voxel is solid, we don't need to render this face
						continue;

					if (!PolyGroupID.IsValidId())
					{
						VoxelValuesInThisChunk.Add(VoxelValue);
						PolyGroupID = VoxelValuesInThisChunk.FindId(VoxelValue);
						TrianglesByVoxelValue.Add(TArray<FVector>());
					}

					TArray<int32> Verts{};
					Verts.Reserve(4);
					for (int32 VertIndex{}; VertIndex < 4; VertIndex++)
					{
						FVector3f Tangent{};
						FVector Normal{ FaceDirections[FaceIndex] };
						CalculateTangent(Normal);
						Verts.Add(PositionBuilder.Add(VoxelLocation + (CubeVertLocations[FaceIndex][VertIndex] * FVector3f(VoxelSize))));
						TangentBuilder.Add(RealtimeMesh::FRealtimeMeshTangentsHighPrecision(FVector3f(Normal), Tangent));
						ColorBuilder.Add(FColor(FaceIndex, 0, 0, 0));
						TexCoordsBuilder.Add(CalculateUV(FaceIndex, VertIndex));
					}

					TrianglesByVoxelValue[PolyGroupID.AsInteger()].Add(FVector(Verts[0], Verts[3], Verts[2]));
					TrianglesByVoxelValue[PolyGroupID.AsInteger()].Add(FVector(Verts[2], Verts[1], Verts[0]));
					NumberOfTris += 2;
				}
			}
		}
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateChunkMeshData::CombineStreams);
		RealtimeMesh::TRealtimeMeshStreamBuilder<RealtimeMesh::TIndex3<uint32>, RealtimeMesh::TIndex3<uint16>> TrianglesBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Triangles, RealtimeMesh::GetRealtimeMeshBufferLayout<RealtimeMesh::TIndex3<uint16>>()));
		TrianglesBuilder.Reserve(NumberOfTris);
		for (int32 GroupIndex{}; GroupIndex < TrianglesByVoxelValue.Num(); GroupIndex++)
		{
			int32 TrisInThisSection{ TrianglesByVoxelValue[GroupIndex].Num() };
			for (int32 TriangleIndex{}; TriangleIndex < TrisInThisSection; TriangleIndex++)
			{
				PolygroupsBuilder.Add(GroupIndex);
				TrianglesBuilder.Add(RealtimeMesh::TIndex3<uint32>(TrianglesByVoxelValue[GroupIndex][TriangleIndex].X, TrianglesByVoxelValue[GroupIndex][TriangleIndex].Y, TrianglesByVoxelValue[GroupIndex][TriangleIndex].Z));
			}
		}

		for (uint8 VoxelValue : VoxelValuesInThisChunk)
			OutChunkMeshData.VoxelSections.Add(VoxelValue);
	}
	OutChunkMeshData.bIsMeshEmpty = VoxelValuesInThisChunk.IsEmpty();

}
