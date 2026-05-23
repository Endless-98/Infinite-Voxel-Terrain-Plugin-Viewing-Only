// Copyright(c) 2024 Endless98. All Rights Reserved.

#include "ChunkThread.h"
#include "ChunkManager.h"
#include "RegionDataService.h"
#include "Engine/World.h"
#include "Containers/Array.h"
#include "Math/IntVector.h"
#include "DrawDebugHelpers.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

FCriticalSection FChunkThread::ChunkZMutex;
TMap<FIntPoint, TArray<int32>> FChunkThread::ChunkZIndicesBy2DCell{};
TMap<FIntPoint, TArray<int32>> FChunkThread::ModifiedAdditionalChunkZIndicesBy2DCell{};

namespace
{
	struct FGreedyQuad
	{
		int32 FaceIndex{ 0 };
		uint8 VoxelValue{ 0 };
		int32 X{ 0 };
		int32 Y{ 0 };
		int32 Z{ 0 };
		int32 Width{ 1 };
		int32 Height{ 1 };
	};

	struct FRleReadCursor
	{
		int32 PairIndex{ 0 };
		int32 RunStart{ 0 };
		int32 RunEnd{ 0 };
		uint8 Value{ 0 };
		bool bHasRun{ false };
	};

	static bool IsValidRleData(const TArray<uint8>& EncodedData)
	{
		return !EncodedData.IsEmpty() && (EncodedData.Num() % 2 == 0);
	}

	static int32 GetDenseVoxelIndex(const int32 VoxelCount, const int32 X, const int32 Y, const int32 Z)
	{
		const int32 Side = VoxelCount + 2;
		return (X + 1) * Side * Side + (Y + 1) * Side + (Z + 1);
	}

	static uint8 GetDenseVoxelAt(const TArray<uint8>& Voxels, const int32 VoxelCount, const int32 X, const int32 Y, const int32 Z)
	{
		if (X < 0 || Y < 0 || Z < 0 || X >= VoxelCount || Y >= VoxelCount || Z >= VoxelCount)
		{
			return 0;
		}

		const int32 Index = GetDenseVoxelIndex(VoxelCount, X, Y, Z);
		return Voxels.IsValidIndex(Index) ? Voxels[Index] : 0;
	}

	static int32 ComputeLodStepByDistanceChunks(const int32 DistanceInChunks, const int32 NearDistanceInChunks, const int32 FarDistanceInChunks, const float DistanceCurveExponent, const int32 MaxLodStepPower)
	{
		const int32 SafeNear = FMath::Max(0, NearDistanceInChunks);
		const int32 SafeFar = FMath::Max(SafeNear + 1, FarDistanceInChunks);
		const int32 SafeMaxPower = FMath::Clamp(MaxLodStepPower, 0, 6);

		const float Denominator = static_cast<float>(SafeFar - SafeNear);
		const float NormalizedDistance = FMath::Clamp((static_cast<float>(DistanceInChunks - SafeNear) / Denominator), 0.0f, 1.0f);
		const float CurvedDistance = FMath::Pow(NormalizedDistance, FMath::Max(DistanceCurveExponent, 0.1f));
		const int32 StepPower = FMath::Clamp(FMath::RoundToInt(CurvedDistance * SafeMaxPower), 0, SafeMaxPower);

		return 1 << StepPower;
	}

	static bool DecodeRleToDense(const TArray<uint8>& EncodedVoxels, const int32 ExpectedVoxelCount, TArray<uint8>& OutDenseVoxels)
	{
		if (!IsValidRleData(EncodedVoxels) || ExpectedVoxelCount <= 0)
		{
			return false;
		}

		OutDenseVoxels.Reset(ExpectedVoxelCount);
		for (int32 PairIndex = 0; PairIndex < EncodedVoxels.Num(); PairIndex += 2)
		{
			const int32 RunCount = EncodedVoxels[PairIndex];
			const uint8 RunValue = EncodedVoxels[PairIndex + 1];
			for (int32 RunIndex = 0; RunIndex < RunCount; ++RunIndex)
			{
				OutDenseVoxels.Add(RunValue);
				if (OutDenseVoxels.Num() == ExpectedVoxelCount)
				{
					return true;
				}
			}
		}

		return OutDenseVoxels.Num() == ExpectedVoxelCount;
	}

	template<typename TQuadFunc>
	static void ForEachGreedyQuad(const TArray<uint8>& Voxels, const int32 VoxelCount, TQuadFunc&& QuadFunc)
	{
		TArray<int32> Mask;
		Mask.Init(0, VoxelCount * VoxelCount);

		// X axis planes (faces 4 / 5)
		for (int32 X = -1; X < VoxelCount; ++X)
		{
			for (int32 Z = 0; Z < VoxelCount; ++Z)
			{
				for (int32 Y = 0; Y < VoxelCount; ++Y)
				{
					const uint8 A = GetDenseVoxelAt(Voxels, VoxelCount, X, Y, Z);
					const uint8 B = GetDenseVoxelAt(Voxels, VoxelCount, X + 1, Y, Z);
					int32 EncodedMask = 0;
					if ((A > 0) != (B > 0))
					{
						EncodedMask = (A > 0) ? static_cast<int32>(A) : -static_cast<int32>(B);
					}
					Mask[Y + Z * VoxelCount] = EncodedMask;
				}
			}

			for (int32 Z = 0; Z < VoxelCount; ++Z)
			{
				for (int32 Y = 0; Y < VoxelCount;)
				{
					const int32 Cell = Mask[Y + Z * VoxelCount];
					if (Cell == 0)
					{
						++Y;
						continue;
					}

					int32 Width = 1;
					while ((Y + Width) < VoxelCount && Mask[(Y + Width) + Z * VoxelCount] == Cell)
					{
						++Width;
					}

					int32 Height = 1;
					bool bCanGrow = true;
					while ((Z + Height) < VoxelCount && bCanGrow)
					{
						for (int32 WidthIndex = 0; WidthIndex < Width; ++WidthIndex)
						{
							if (Mask[(Y + WidthIndex) + (Z + Height) * VoxelCount] != Cell)
							{
								bCanGrow = false;
								break;
							}
						}
						if (bCanGrow)
						{
							++Height;
						}
					}

					const uint8 VoxelValue = static_cast<uint8>(FMath::Abs(Cell));
					const int32 FaceIndex = (Cell > 0) ? 4 : 5;
					const int32 FaceX = (Cell > 0) ? X : X + 1;
					QuadFunc(FGreedyQuad{ FaceIndex, VoxelValue, FaceX, Y, Z, Width, Height });

					for (int32 HeightIndex = 0; HeightIndex < Height; ++HeightIndex)
					{
						for (int32 WidthIndex = 0; WidthIndex < Width; ++WidthIndex)
						{
							Mask[(Y + WidthIndex) + (Z + HeightIndex) * VoxelCount] = 0;
						}
					}

					Y += Width;
				}
			}
		}

		// Y axis planes (faces 2 / 3)
		for (int32 Y = -1; Y < VoxelCount; ++Y)
		{
			for (int32 Z = 0; Z < VoxelCount; ++Z)
			{
				for (int32 X = 0; X < VoxelCount; ++X)
				{
					const uint8 A = GetDenseVoxelAt(Voxels, VoxelCount, X, Y, Z);
					const uint8 B = GetDenseVoxelAt(Voxels, VoxelCount, X, Y + 1, Z);
					int32 EncodedMask = 0;
					if ((A > 0) != (B > 0))
					{
						EncodedMask = (A > 0) ? static_cast<int32>(A) : -static_cast<int32>(B);
					}
					Mask[X + Z * VoxelCount] = EncodedMask;
				}
			}

			for (int32 Z = 0; Z < VoxelCount; ++Z)
			{
				for (int32 X = 0; X < VoxelCount;)
				{
					const int32 Cell = Mask[X + Z * VoxelCount];
					if (Cell == 0)
					{
						++X;
						continue;
					}

					int32 Width = 1;
					while ((X + Width) < VoxelCount && Mask[(X + Width) + Z * VoxelCount] == Cell)
					{
						++Width;
					}

					int32 Height = 1;
					bool bCanGrow = true;
					while ((Z + Height) < VoxelCount && bCanGrow)
					{
						for (int32 WidthIndex = 0; WidthIndex < Width; ++WidthIndex)
						{
							if (Mask[(X + WidthIndex) + (Z + Height) * VoxelCount] != Cell)
							{
								bCanGrow = false;
								break;
							}
						}
						if (bCanGrow)
						{
							++Height;
						}
					}

					const uint8 VoxelValue = static_cast<uint8>(FMath::Abs(Cell));
					const int32 FaceIndex = (Cell > 0) ? 2 : 3;
					const int32 FaceY = (Cell > 0) ? Y : Y + 1;
					QuadFunc(FGreedyQuad{ FaceIndex, VoxelValue, X, FaceY, Z, Width, Height });

					for (int32 HeightIndex = 0; HeightIndex < Height; ++HeightIndex)
					{
						for (int32 WidthIndex = 0; WidthIndex < Width; ++WidthIndex)
						{
							Mask[(X + WidthIndex) + (Z + HeightIndex) * VoxelCount] = 0;
						}
					}

					X += Width;
				}
			}
		}

		// Z axis planes (faces 0 / 1)
		for (int32 Z = -1; Z < VoxelCount; ++Z)
		{
			for (int32 Y = 0; Y < VoxelCount; ++Y)
			{
				for (int32 X = 0; X < VoxelCount; ++X)
				{
					const uint8 A = GetDenseVoxelAt(Voxels, VoxelCount, X, Y, Z);
					const uint8 B = GetDenseVoxelAt(Voxels, VoxelCount, X, Y, Z + 1);
					int32 EncodedMask = 0;
					if ((A > 0) != (B > 0))
					{
						EncodedMask = (A > 0) ? static_cast<int32>(A) : -static_cast<int32>(B);
					}
					Mask[X + Y * VoxelCount] = EncodedMask;
				}
			}

			for (int32 Y = 0; Y < VoxelCount; ++Y)
			{
				for (int32 X = 0; X < VoxelCount;)
				{
					const int32 Cell = Mask[X + Y * VoxelCount];
					if (Cell == 0)
					{
						++X;
						continue;
					}

					int32 Width = 1;
					while ((X + Width) < VoxelCount && Mask[(X + Width) + Y * VoxelCount] == Cell)
					{
						++Width;
					}

					int32 Height = 1;
					bool bCanGrow = true;
					while ((Y + Height) < VoxelCount && bCanGrow)
					{
						for (int32 WidthIndex = 0; WidthIndex < Width; ++WidthIndex)
						{
							if (Mask[(X + WidthIndex) + (Y + Height) * VoxelCount] != Cell)
							{
								bCanGrow = false;
								break;
							}
						}
						if (bCanGrow)
						{
							++Height;
						}
					}

					const uint8 VoxelValue = static_cast<uint8>(FMath::Abs(Cell));
					const int32 FaceIndex = (Cell > 0) ? 0 : 1;
					const int32 FaceZ = (Cell > 0) ? Z : Z + 1;
					QuadFunc(FGreedyQuad{ FaceIndex, VoxelValue, X, Y, FaceZ, Width, Height });

					for (int32 HeightIndex = 0; HeightIndex < Height; ++HeightIndex)
					{
						for (int32 WidthIndex = 0; WidthIndex < Width; ++WidthIndex)
						{
							Mask[(X + WidthIndex) + (Y + HeightIndex) * VoxelCount] = 0;
						}
					}

					X += Width;
				}
			}
		}
	}

	static int32 CalculateNaiveVisibleFaceCount(const TArray<uint8>& Voxels, const int32 VoxelCount)
	{
		int32 FaceCount = 0;
		for (int32 X = 0; X < VoxelCount; ++X)
		{
			for (int32 Y = 0; Y < VoxelCount; ++Y)
			{
				for (int32 Z = 0; Z < VoxelCount; ++Z)
				{
					const uint8 VoxelValue = GetDenseVoxelAt(Voxels, VoxelCount, X, Y, Z);
					if (VoxelValue == 0)
					{
						continue;
					}
					FaceCount += (GetDenseVoxelAt(Voxels, VoxelCount, X, Y, Z + 1) == 0) ? 1 : 0;
					FaceCount += (GetDenseVoxelAt(Voxels, VoxelCount, X, Y, Z - 1) == 0) ? 1 : 0;
					FaceCount += (GetDenseVoxelAt(Voxels, VoxelCount, X, Y + 1, Z) == 0) ? 1 : 0;
					FaceCount += (GetDenseVoxelAt(Voxels, VoxelCount, X, Y - 1, Z) == 0) ? 1 : 0;
					FaceCount += (GetDenseVoxelAt(Voxels, VoxelCount, X + 1, Y, Z) == 0) ? 1 : 0;
					FaceCount += (GetDenseVoxelAt(Voxels, VoxelCount, X - 1, Y, Z) == 0) ? 1 : 0;
				}
			}
		}

		return FaceCount;
	}

	static int32 CalculateGreedyQuadCount(const TArray<uint8>& Voxels, const int32 VoxelCount)
	{
		int32 QuadCount = 0;
		ForEachGreedyQuad(Voxels, VoxelCount, [&QuadCount](const FGreedyQuad&)
			{
				++QuadCount;
			});
		return QuadCount;
	}

	static bool ReadRleVoxelAt(const TArray<uint8>& EncodedData, int32 TargetIndex, FRleReadCursor& Cursor, uint8& OutVoxelValue)
	{
		if (TargetIndex < 0 || !IsValidRleData(EncodedData))
		{
			return false;
		}

		if (!Cursor.bHasRun || TargetIndex < Cursor.RunStart)
		{
			Cursor = FRleReadCursor{};
		}

		while (true)
		{
			if (Cursor.bHasRun && TargetIndex < Cursor.RunEnd)
			{
				OutVoxelValue = Cursor.Value;
				return true;
			}

			if (Cursor.PairIndex + 1 >= EncodedData.Num())
			{
				return false;
			}

			const int32 Count = EncodedData[Cursor.PairIndex];
			const uint8 Value = EncodedData[Cursor.PairIndex + 1];
			Cursor.PairIndex += 2;

			if (Count <= 0)
			{
				continue;
			}

			Cursor.RunStart = Cursor.RunEnd;
			Cursor.RunEnd += Count;
			Cursor.Value = Value;
			Cursor.bHasRun = true;
		}
	}
}

namespace
{
	static void BuildGreedyMeshData(FChunkThread& Thread, FChunkMeshData& OutChunkMeshData, const TArray<uint8>& Voxels, const FIntVector& ChunkCell, const bool bShouldGenerateCollisionAtChunkSpawn, const int32 VoxelCount, const float VoxelSize, const float ChunkSize, const TArray<FVoxelDefinition>& VoxelDefinitions)
	{
		OutChunkMeshData.CollisionType = ECR_Block;
		OutChunkMeshData.ChunkCell = ChunkCell;
		OutChunkMeshData.bShouldGenCollision = bShouldGenerateCollisionAtChunkSpawn;

		if (Voxels.IsEmpty())
		{
			OutChunkMeshData.bIsMeshEmpty = true;
			return;
		}

		RealtimeMesh::TRealtimeMeshStreamBuilder<FVector3f> PositionBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Position, RealtimeMesh::GetRealtimeMeshBufferLayout<FVector3f>()));
		RealtimeMesh::TRealtimeMeshStreamBuilder<RealtimeMesh::FRealtimeMeshTangentsHighPrecision, RealtimeMesh::FRealtimeMeshTangentsNormalPrecision> TangentBuilder(
			OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Tangents, RealtimeMesh::GetRealtimeMeshBufferLayout<RealtimeMesh::FRealtimeMeshTangentsNormalPrecision>()));
		RealtimeMesh::TRealtimeMeshStreamBuilder<FVector2f, FVector2DHalf> TexCoordsBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::TexCoords, RealtimeMesh::GetRealtimeMeshBufferLayout<FVector2DHalf>()));
		RealtimeMesh::TRealtimeMeshStreamBuilder<FColor> ColorBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Color, RealtimeMesh::GetRealtimeMeshBufferLayout<FColor>()));
		RealtimeMesh::TRealtimeMeshStreamBuilder<uint32, uint16> PolygroupsBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::PolyGroups, RealtimeMesh::GetRealtimeMeshBufferLayout<uint16>()));

		TMap<uint8, int32> GroupByVoxel;
		TArray<uint8> VoxelSectionsInOrder;
		TArray<TArray<FIntVector>> TrianglesByGroup;
		int32 NumberOfTris = 0;
		const FVector3f ChunkMeshOffset{ -ChunkSize / 2.0f };

		auto GetOrAddGroup = [&](const uint8 VoxelValue)
			{
				if (const int32* ExistingGroup = GroupByVoxel.Find(VoxelValue))
				{
					return *ExistingGroup;
				}

				const int32 NewGroup = TrianglesByGroup.Num();
				GroupByVoxel.Add(VoxelValue, NewGroup);
				TrianglesByGroup.AddDefaulted();
				VoxelSectionsInOrder.Add(VoxelValue);
				return NewGroup;
			};

		auto ToWorldPosition = [&](const float GridX, const float GridY, const float GridZ)
			{
				return ChunkMeshOffset + FVector3f(GridX * VoxelSize, GridY * VoxelSize, GridZ * VoxelSize);
			};

		ForEachGreedyQuad(Voxels, VoxelCount, [&](const FGreedyQuad& Quad)
			{
				if (!VoxelDefinitions.IsValidIndex(Quad.VoxelValue) || VoxelDefinitions[Quad.VoxelValue].bIsAir)
				{
					return;
				}

				const int32 GroupIndex = GetOrAddGroup(Quad.VoxelValue);
				FVector3f VertPositions[4]{};

				switch (Quad.FaceIndex)
				{
				case 0: // Up (+Z)
					VertPositions[0] = ToWorldPosition(Quad.X - 0.5f, Quad.Y + Quad.Height - 0.5f, Quad.Z + 0.5f);
					VertPositions[1] = ToWorldPosition(Quad.X - 0.5f, Quad.Y - 0.5f, Quad.Z + 0.5f);
					VertPositions[2] = ToWorldPosition(Quad.X + Quad.Width - 0.5f, Quad.Y - 0.5f, Quad.Z + 0.5f);
					VertPositions[3] = ToWorldPosition(Quad.X + Quad.Width - 0.5f, Quad.Y + Quad.Height - 0.5f, Quad.Z + 0.5f);
					break;
				case 1: // Down (-Z)
					VertPositions[0] = ToWorldPosition(Quad.X + Quad.Width - 0.5f, Quad.Y - 0.5f, Quad.Z - 0.5f);
					VertPositions[1] = ToWorldPosition(Quad.X - 0.5f, Quad.Y - 0.5f, Quad.Z - 0.5f);
					VertPositions[2] = ToWorldPosition(Quad.X - 0.5f, Quad.Y + Quad.Height - 0.5f, Quad.Z - 0.5f);
					VertPositions[3] = ToWorldPosition(Quad.X + Quad.Width - 0.5f, Quad.Y + Quad.Height - 0.5f, Quad.Z - 0.5f);
					break;
				case 2: // Right (+Y)
					VertPositions[0] = ToWorldPosition(Quad.X + Quad.Width - 0.5f, Quad.Y + 0.5f, Quad.Z + Quad.Height - 0.5f);
					VertPositions[1] = ToWorldPosition(Quad.X + Quad.Width - 0.5f, Quad.Y + 0.5f, Quad.Z - 0.5f);
					VertPositions[2] = ToWorldPosition(Quad.X - 0.5f, Quad.Y + 0.5f, Quad.Z - 0.5f);
					VertPositions[3] = ToWorldPosition(Quad.X - 0.5f, Quad.Y + 0.5f, Quad.Z + Quad.Height - 0.5f);
					break;
				case 3: // Left (-Y)
					VertPositions[0] = ToWorldPosition(Quad.X - 0.5f, Quad.Y - 0.5f, Quad.Z + Quad.Height - 0.5f);
					VertPositions[1] = ToWorldPosition(Quad.X - 0.5f, Quad.Y - 0.5f, Quad.Z - 0.5f);
					VertPositions[2] = ToWorldPosition(Quad.X + Quad.Width - 0.5f, Quad.Y - 0.5f, Quad.Z - 0.5f);
					VertPositions[3] = ToWorldPosition(Quad.X + Quad.Width - 0.5f, Quad.Y - 0.5f, Quad.Z + Quad.Height - 0.5f);
					break;
				case 4: // Front (+X)
					VertPositions[0] = ToWorldPosition(Quad.X + 0.5f, Quad.Y - 0.5f, Quad.Z + Quad.Height - 0.5f);
					VertPositions[1] = ToWorldPosition(Quad.X + 0.5f, Quad.Y - 0.5f, Quad.Z - 0.5f);
					VertPositions[2] = ToWorldPosition(Quad.X + 0.5f, Quad.Y + Quad.Width - 0.5f, Quad.Z - 0.5f);
					VertPositions[3] = ToWorldPosition(Quad.X + 0.5f, Quad.Y + Quad.Width - 0.5f, Quad.Z + Quad.Height - 0.5f);
					break;
				default: // Back (-X)
					VertPositions[0] = ToWorldPosition(Quad.X - 0.5f, Quad.Y + Quad.Width - 0.5f, Quad.Z + Quad.Height - 0.5f);
					VertPositions[1] = ToWorldPosition(Quad.X - 0.5f, Quad.Y + Quad.Width - 0.5f, Quad.Z - 0.5f);
					VertPositions[2] = ToWorldPosition(Quad.X - 0.5f, Quad.Y - 0.5f, Quad.Z - 0.5f);
					VertPositions[3] = ToWorldPosition(Quad.X - 0.5f, Quad.Y - 0.5f, Quad.Z + Quad.Height - 0.5f);
					break;
				}

				const FVector Normal = FaceDirections[Quad.FaceIndex];
				const FVector Tangent = Thread.CalculateTangent(Normal);
				const FVector2f UVs[4] = {
					FVector2f(0.0f, static_cast<float>(Quad.Height)),
					FVector2f(0.0f, 0.0f),
					FVector2f(static_cast<float>(Quad.Width), 0.0f),
					FVector2f(static_cast<float>(Quad.Width), static_cast<float>(Quad.Height))
				};

				int32 VertIndices[4]{};
				for (int32 VertIndex = 0; VertIndex < 4; ++VertIndex)
				{
					VertIndices[VertIndex] = PositionBuilder.Add(VertPositions[VertIndex]);
					TangentBuilder.Add(RealtimeMesh::FRealtimeMeshTangentsHighPrecision(FVector3f(Normal), FVector3f(Tangent)));
					ColorBuilder.Add(FColor(Quad.FaceIndex, 0, 0, 0));
					TexCoordsBuilder.Add(UVs[VertIndex]);
				}

				TrianglesByGroup[GroupIndex].Add(FIntVector(VertIndices[0], VertIndices[3], VertIndices[2]));
				TrianglesByGroup[GroupIndex].Add(FIntVector(VertIndices[2], VertIndices[1], VertIndices[0]));
				NumberOfTris += 2;
			});

		RealtimeMesh::TRealtimeMeshStreamBuilder<RealtimeMesh::TIndex3<uint32>, RealtimeMesh::TIndex3<uint16>> TrianglesBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Triangles, RealtimeMesh::GetRealtimeMeshBufferLayout<RealtimeMesh::TIndex3<uint16>>()));
		TrianglesBuilder.Reserve(NumberOfTris);
		for (int32 GroupIndex = 0; GroupIndex < TrianglesByGroup.Num(); ++GroupIndex)
		{
			for (const FIntVector& Triangle : TrianglesByGroup[GroupIndex])
			{
				PolygroupsBuilder.Add(GroupIndex);
				TrianglesBuilder.Add(RealtimeMesh::TIndex3<uint32>(Triangle.X, Triangle.Y, Triangle.Z));
			}
		}

		OutChunkMeshData.VoxelSections = MoveTemp(VoxelSectionsInOrder);
		OutChunkMeshData.bIsMeshEmpty = OutChunkMeshData.VoxelSections.IsEmpty();
	}
}

bool FChunkThread::Init()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::Init);
	return true;
}

uint32 FChunkThread::Run()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::Run);

	InitializeNoiseGenerators();
	if (!WorldRef) return 1;

	while (bIsRunning) // Generate chunks until we are told to stop
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::RunLoop);

		UpdateTrackingVariables();
		UpdateTempVariables();

		if (TrackerState.PlayerLocations.IsEmpty() || !TrackerState.PlayerLocations.IsValidIndex(TrackerState.TrackedIndex))
		{
			FPlatformProcess::Sleep(ThreadIdleSleepTime);
			continue;
		}

		if (!bIsRunning || !WorldRef) return 0;

		UpdateChunks();

		if (bIsFirstTime)
			LastHeightmapLocation = TrackerState.PlayerLocations[TrackerState.TrackedIndex];

		if (!PrepareRegionForGeneration())
		{
			FPlatformProcess::Sleep(ThreadIdleSleepTime);
			continue;
		}

		FVector2D HeightmapLocation{};
		TArray<FVector2D> LocationsNeedingUnhide; // If we are the client, we may need to unhide chunks that were hidden
		bool bWasHeightmapNeeded{ FindNextNeededHeightmap(HeightmapLocation, LocationsNeedingUnhide) };
		ChunkManagerRef->UnhideChunksInHeightmapLocations(LocationsNeedingUnhide);
		LastHeightmapLocation = HeightmapLocation;

		if (!bWasHeightmapNeeded)
		{
			FPlatformProcess::Sleep(ThreadIdleSleepTime);
			continue;
		}

		TArray<TSharedPtr<FChunkConstructionData>> ChunkConstructionDataArray{};
		TArray<int32> TerrainZIndices{};
		
		if (!GenerateChunkData(HeightmapLocation, TerrainZIndices, ChunkConstructionDataArray))
			continue;
		
		AsyncSpawnChunks(ChunkConstructionDataArray, HeightmapLocation, TerrainZIndices);

		FPlatformProcess::Sleep(ThreadWorkingSleepTime);
	}

	return 0;
}

void FChunkThread::Stop()
{
	if (ThreadIndex > 0)
	{
		bIsRunning = false;

		return; // Only the first thread should save the world
	}

	if (WorldRef->GetNetMode() == ENetMode::NM_DedicatedServer || WorldRef->GetNetMode() == ENetMode::NM_ListenServer || WorldRef->GetNetMode() == ENetMode::NM_Standalone)
		SaveUnsavedRegions(false);

	bIsRunning = false;
}

void FChunkThread::InitializeNoiseGenerators()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::InitializeNoiseGenerators);

	BiomeNoiseGenerator = FastNoise::NewFromEncodedNodeTree("IgAAAEBAmpmZPhsAEABxPQo/GwAeABcAAAAAAAAAgD9cj8I+AACAPw0AAwAAAAAAQEAJAADsUbg+AOxRuD4AAAAAAAETAI/CdT7//wEAAOxROD4AAAAAQA==");
	PlainsNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EQACAAAAAAAgQBAAAAAAQCcAAQAAABsAIAAJAAAAAAAAAArXoz8BEwAK1yM/DQACAAAArkexQP//AAAAKVxPPwDNzEw+AM3MTD4AMzMzPwAAAAA/");
	ForestNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EQACAAAAAAAgQBAAAAAAQCcAAQAAABsAIAAJAAAAAAAAAArXoz8BEwAK1yM/DQACAAAArkexQP//AAAAKVxPPwDNzEw+AM3MTD4AMzMzPwAAAAA/");
	HillsNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EwBcj8I+EQADAAAAcT1qQBAAzcxMPg0AAwAAAB+FS0AnAAEAAAAJAAAfhes+AHE9Cj8ArkdhPwApXI8+AD0K1z4=");
	MountainsNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EwAzM7M+EADhehQ/DQADAAAAhevBQCcAAQAAAAYAAAAAAD8AAACAPwAK1yM+");
}

// Returns false if no TrackedPlayers have moved
bool FChunkThread::UpdateTrackingVariables()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::UpdateTrackingVariables);

	TArray<FVector2D> NewTrackedLocations{  };

	{
		FReadScopeLock Lock(ChunkManagerRef->ThreadPlayerLocationsLock);
		NewTrackedLocations = ChunkManagerRef->ThreadUseableLocations;
	}

	if (NewTrackedLocations.IsEmpty() || !bIsRunning)
	{
		bDidTrackedActorMove = false;
		return bDidTrackedActorMove;
	}

	for (FVector2D& TrackedLocation : NewTrackedLocations)
		TrackedLocation = GetLocationSnappedToChunkGrid2D(TrackedLocation, ChunkSize);

	if (TrackerState.PlayerLocations.Num() > 0)
		TrackerState.TrackedIndex = (TrackerState.TrackedIndex + 1) % TrackerState.PlayerLocations.Num();

	// If nothing has changed we don't need to continue
	if (NewTrackedLocations == TrackerState.PlayerLocations)
	{
		bDidTrackedActorMove = false;
		return bDidTrackedActorMove;
	}

	for (int32 PlayerIndex{}; PlayerIndex < NewTrackedLocations.Num(); PlayerIndex++)
	{
		if (!TrackerState.TrackedChunkRingCount.IsValidIndex(PlayerIndex))
		{
			TrackerState.TrackedChunkRingCount.Add(0);
			TrackerState.TrackedChunkRingDistance.Add(0);
		}
	}

	for (int32 TrackedActorIndex{}; TrackedActorIndex < TrackerState.PlayerLocations.Num(); TrackedActorIndex++)
	{
		if (!TrackerState.TrackedChunkRingCount.IsValidIndex(TrackedActorIndex))
		{
			TrackerState.TrackedChunkRingCount.Add(0);
			TrackerState.TrackedChunkRingDistance.Add(0);
		}

		if (!NewTrackedLocations.IsValidIndex(TrackedActorIndex) || !TrackerState.PlayerLocations.IsValidIndex(TrackedActorIndex) || !TrackerState.TrackedChunkRingCount.IsValidIndex(TrackedActorIndex) || !TrackerState.TrackedChunkRingDistance.IsValidIndex(TrackedActorIndex))
			continue;

		FVector2D CurrentActorLocation = NewTrackedLocations[TrackedActorIndex];
		FVector2D OldActorLocation = TrackerState.PlayerLocations[TrackedActorIndex];
		int32 ChunksMoved = FMath::Max(FMath::CeilToInt32(FVector2D::Distance(CurrentActorLocation, OldActorLocation)) / ChunkSize, 2) + 1;
		TrackerState.TrackedChunkRingCount[TrackedActorIndex] = FMath::Max(TrackerState.TrackedChunkRingCount[TrackedActorIndex] - ChunksMoved, 0);
		TrackerState.TrackedChunkRingDistance[TrackedActorIndex] = FMath::Max(TrackerState.TrackedChunkRingDistance[TrackedActorIndex] - ChunksMoved, 0);

		TrackerState.ChunkAngleIndex = 0;
	}

	TrackerState.PlayerLocations = NewTrackedLocations;

	bDidTrackedActorMove = true;
	return bDidTrackedActorMove;
}

void FChunkThread::UpdateTempVariables()
{
	if (bWasRangeChanged)
		bDidTrackedActorMove = true;

	{
		// These values might be changed by the game thread while we loop, so we copy them to local variables
		FScopeLock Lock(&ChunkGenMutex);

		TempGenerationRadius = ChunkGenerationRadius;
		TempCollisionGenRadius = CollisionGenerationRadius;
		TempChunkGenRadius = ChunkGenerationRadius;
	}

	if (GetGenDistanceShouldBeCollision(TrackerState.TrackedIndex))
		TempGenerationRadius = TempCollisionGenRadius;
	else
		TempGenerationRadius = TempChunkGenRadius;
}

void FChunkThread::UpdateChunks()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::UpdateChunks);

	if (!(bDidTrackedActorMove && ThreadIndex == 0))
		return;


	if (!WorldRef)
		return;
	TArray<FIntVector> CellsToRemove;
	TArray<FIntVector> CellsToUnhide;
	TArray<FIntVector> CellsToHide;

	TArray TempPlayerLocations{ TrackerState.PlayerLocations };
	{
		FScopeLock HeightmapLock(&ChunkManagerRef->HeightmapMutex);
		FScopeLock ChunkZLock(&ChunkZMutex);
		bool bIsDedicatedServer{ WorldRef->GetNetMode() == ENetMode::NM_DedicatedServer };
		bool bIsListenServer{ WorldRef->GetNetMode() == ENetMode::NM_ListenServer };
		for (const FVector2D& ExistingHeightmapLocation : ChunkManagerRef->ExistingHeightmapLocations)
		{
			FIntPoint ChunkCell2D{ AChunkManager::Get2DCellFromChunkLocation2D(ExistingHeightmapLocation, ChunkSize) };
			TArray<int32>* ChunkZIndices{ ChunkZIndicesBy2DCell.Find(ChunkCell2D) };
			if (!ChunkZIndices)
				continue;

			bool bIsNeeded{ IsNeededHeightmapLocation(ExistingHeightmapLocation, TempPlayerLocations, TempChunkGenRadius + ChunkDeletionBuffer, TempCollisionGenRadius) };
			if (!bIsNeeded) // If we don't need the chunk at all, no other checks are performed
			{
				for (int32& ChunkZ : *ChunkZIndices)
				{
					FIntVector ChunkCell{ ChunkCell2D.X, ChunkCell2D.Y, ChunkZ };
					CellsToRemove.Add(ChunkCell);
				}
				continue;
			}

			if (!bIsListenServer) // Only the listen server does chunk hiding here
				continue;

			bool bServerNeedsChunk{ IsHeightmapInRange(ExistingHeightmapLocation, TempPlayerLocations[0], TempChunkGenRadius + ChunkDeletionBuffer) };
			if (bServerNeedsChunk)
			{
				for (int32& ChunkZ : *ChunkZIndices)
				{
					FIntVector ChunkCell{ ChunkCell2D.X, ChunkCell2D.Y, ChunkZ };
					CellsToUnhide.Add(ChunkCell);
				}
			}
			else // Listen Server does not need to see the chunk
			{
				for (int32& ChunkZ : *ChunkZIndices)
				{
					FIntVector ChunkCell{ ChunkCell2D.X, ChunkCell2D.Y, ChunkZ };
					CellsToHide.Add(ChunkCell);
				}
			}
			ChunkZIndices = nullptr;
		}

		for (const FIntVector& CellToRemove : CellsToRemove)
		{
			FIntPoint ChunkCell2D{ CellToRemove.X, CellToRemove.Y };
			FVector2D HeightmapLocation{ AChunkManager::GetLocationFromChunkCell(CellToRemove, ChunkSize) };
			ChunkManagerRef->ExistingHeightmapLocations.Remove(HeightmapLocation);
			ChunkZIndicesBy2DCell.Remove(ChunkCell2D);
		}
	}

	for (const FIntVector& CellToRemove : CellsToRemove)
		ChunkManagerRef->EnqueueWorldCommand(FChunkWorldCommand::MakeCellCommand(EChunkWorldCommandType::DestroyOrHide, CellToRemove));

	for (const FIntVector& CellToUnhide : CellsToUnhide)
		ChunkManagerRef->EnqueueWorldCommand(FChunkWorldCommand::MakeCellCommand(EChunkWorldCommandType::Unhide, CellToUnhide));

	for (const FIntVector& CellToHide : CellsToHide)
		ChunkManagerRef->EnqueueWorldCommand(FChunkWorldCommand::MakeCellCommand(EChunkWorldCommandType::Hide, CellToHide));
}

bool FChunkThread::IsNeededHeightmapLocation(FVector2D ChunkLocation2D, const TArray<FVector2D>& TrackedLocationsRef, int32 ChunkGenRadius, int32 CollisionGenRadius)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::IsNeededHeightmapLocation);

	for (int32 LocationIndex{}; LocationIndex < TrackedLocationsRef.Num(); LocationIndex++)
	{
		FVector2D TrackedLocation{ TrackedLocationsRef[LocationIndex] };
		int32 GenRadius{ (GetGenDistanceShouldBeCollision(LocationIndex) ? CollisionGenRadius + ChunkDeletionBuffer : ChunkGenRadius) };
		if (IsHeightmapInRange(ChunkLocation2D, TrackedLocation, GenRadius))
			return true;
	}

	return false;
}

bool FChunkThread::PrepareRegionForGeneration()
{
	FIntPoint Region{ GetRegionByLocation(FVector2D(LastHeightmapLocation)) };
	bool bIsReadyForGeneration{ false };

	bool bShouldCheckForRegionData{ (bDidTrackedActorMove || bWasRangeChanged || bIsFirstTime) && ThreadIndex == 0 };
	if (bShouldCheckForRegionData)
	{
		bIsFirstTime = false;
		bWasRangeChanged = false;

		TArray<FIntPoint> RegionsToLoad{};
		TArray<FIntPoint> RegionsToSave{};
		GetRegionsToLoad(RegionsToLoad);
		GetRegionsToSave(RegionsToSave);
		if (WorldRef->GetNetMode() != NM_Client) // We don't need to load voxels on the client, we will get them from the server
		{
			for (FIntPoint RegionToLoad : RegionsToLoad)
			{
				LoadVoxelsForRegion(RegionToLoad, WorldSaveName);
				ChunkManagerRef->SendNeededRegionDataOnGameThread(RegionToLoad);
			}
		}

		bool bRemoveVoxelWhenSaved{ true };
		for (FIntPoint RegionToSave : RegionsToSave)
		{
			if (WorldRef->GetNetMode() != NM_Client) // When regions are getting saved this way, it's because they are no longer relevant, so we can remove the ModifiedVoxels stored in memory
				AsyncSaveVoxelsForRegion(RegionToSave, WorldSaveName, bRemoveVoxelWhenSaved);
			else // If we are on the client, we don't save data, but we use the RegionsPendingSave tracking system to know which regions we are safe to remove from memory. They will be sent again when needed
			{
				{
					FScopeLock Lock(&ChunkManagerRef->ModifiedVoxelsMutex);
					ChunkManagerRef->ModifiedVoxelsByCellByRegion.Remove(Region);
				}
				FScopeLock Lock(&ChunkManagerRef->RegionMutex);
				ChunkManagerRef->RegionsPendingSave.Remove(Region);
			}
		}
	}

	if (WorldRef->GetNetMode() != NM_Client)
	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		if (!ChunkManagerRef->RegionsAlreadyLoaded.Contains(Region) && ChunkManagerRef->RegionsPendingLoad.Contains(Region))
		{
			Lock.Unlock();

			if (!bIsRunning || !WorldRef)
				return false;

			if (WorldRef->GetNetMode() != NM_Client && ThreadIndex == 0)
				LoadVoxelsForRegion(Region, WorldSaveName);

			return false;
		}
		else if (!ChunkManagerRef->RegionsAlreadyLoaded.Contains(Region) && !ChunkManagerRef->RegionsPendingLoad.Contains(Region))
		{
			ChunkManagerRef->RegionsPendingLoad.Add(Region);
			return false;
		}
	}
	else // Client
	{
		if (!ChunkManagerRef)
			return false;
		// If this region is not tracked as having data
		bool bDoesClientHaveRegionData{};
		{
			FScopeLock Lock(&ChunkManagerRef->RegionMutex);
			bDoesClientHaveRegionData = ChunkManagerRef->GetDoesClientHaveRegionData(nullptr, Region);
		}

		if (!bDoesClientHaveRegionData)
		{
			FPlatformProcess::Sleep(ThreadIdleSleepTime);
			return false;
		}
	}

	return true;
}

bool FChunkThread::FindNextNeededHeightmap(FVector2D& OutHeightmapLocation, TArray<FVector2D>& OutLocationsNeedingUnhide)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::FindNextNeededHeightmap);
	const FVector2D& TrackedLocation{ TrackerState.PlayerLocations[TrackerState.TrackedIndex] };
	int32& RingChunkDistance{ TrackerState.TrackedChunkRingDistance[TrackerState.TrackedIndex] };
	int32& RingCount{ TrackerState.TrackedChunkRingCount[TrackerState.TrackedIndex] };
	const int32& ChunkGenRadius{ TempGenerationRadius };
	const int32& CollisionGenRadius{ TempCollisionGenRadius };

	OutLocationsNeedingUnhide.Reset();
	if (!WorldRef)
	{
		UE_LOG(LogTemp, Error, TEXT("WorldRef was nullptr!"));
		return false;
	}

	bool bFoundNeededHeightmap{ false };
	while (RingChunkDistance <= ChunkGenRadius && bIsRunning) // Loop until we find a needed heightmap or we reach the edge of the generation radius
	{
		if (TrackerState.LastRingCount != RingCount) // If the radius has changed
		{
			TrackerState.CircumferenceInChunks = FMath::Max(CalculateCircumferenceInChunks(RingCount, ChunkSize), 1);
			TrackerState.ChunkAngleIndex = 0;
		}
		TrackerState.LastRingCount = RingCount;

		while (TrackerState.ChunkAngleIndex < TrackerState.CircumferenceInChunks && bIsRunning)
		{
			float ChunkYawAngle = (360.f / TrackerState.CircumferenceInChunks) * TrackerState.ChunkAngleIndex;
			FVector2D HeightmapLocation = FVector2D(GetLocationSnappedToChunkGrid2D(TrackedLocation + FVector2D(FRotator(0, ChunkYawAngle, 0).Vector()) * (FVector2D(ChunkSize) * RingCount / 2.0), ChunkSize));

			if (TrackerState.ChunkAngleIndex == 0)
				RingChunkDistance = FMath::RoundToInt32(FMath::Abs(FVector2D::Distance(HeightmapLocation, TrackedLocation)) / ChunkSize);

			bool bHeightmapNeedsCollision{ DoesLocationNeedCollision(HeightmapLocation, TrackerState.PlayerLocations, CollisionGenRadius) };
			{
				FScopeLock Lock(&ChunkManagerRef->HeightmapMutex);
				if (ChunkManagerRef && !ChunkManagerRef->ExistingHeightmapLocations.Contains(HeightmapLocation))
				{
					ChunkManagerRef->ExistingHeightmapLocations.Emplace(HeightmapLocation);
					OutHeightmapLocation = HeightmapLocation;
					TrackerState.ChunkAngleIndex++;
					bFoundNeededHeightmap = true;

					return bFoundNeededHeightmap;
				}
				else if (WorldRef->GetNetMode() == NM_Client || WorldRef->GetNetMode() == NM_ListenServer) // Location did have a chunk
				{
					OutLocationsNeedingUnhide.Add(HeightmapLocation);
				}
			}
			TrackerState.ChunkAngleIndex++;
		}
        // We've completed a circle
		if (TrackerState.ChunkAngleIndex == TrackerState.CircumferenceInChunks)
			RingCount++;
	}

	return bFoundNeededHeightmap;
}

int32 FChunkThread::CalculateCircumferenceInChunks(const int32 RadiusInChunks, float ChunkSize)
{
	// Calculate the circumference of the circle in units
	float CircumferenceInUnits{ 2.0f * PI * (RadiusInChunks * ChunkSize) };
	// Convert the circumference to chunks based on the chunk scale along the X and Y axes
	int32 TempCircumferenceInChunks{ FMath::CeilToInt32(CircumferenceInUnits / ChunkSize) };

	return TempCircumferenceInChunks;
}

bool FChunkThread::GenerateChunkData(FVector2D& HeightmapLocation, TArray<int32>& TerrainZIndices, TArray<TSharedPtr<FChunkConstructionData>>& ChunkConstructionDataArray)
{
	TArray<int16> Heightmap{};
	GenerateHeightmap(Heightmap, HeightmapLocation, TerrainZIndices);
	CombineChunkZIndices(HeightmapLocation, TerrainZIndices);

	if (!AddConstructionData(ChunkConstructionDataArray, HeightmapLocation, TerrainZIndices))
		return false;
	
	GenerateVoxelsForChunks(ChunkConstructionDataArray, Heightmap);
	GenerateMeshDataForChunks(ChunkConstructionDataArray);
	// Keep canonical chunk storage compressed at all times.
	CompressVoxelData(ChunkConstructionDataArray);

	return true;
}

void FChunkThread::GenerateHeightmap(TArray<int16>& OutGeneratedHeightmap, const FVector2D& NeededHeightmapLocation, TArray<int32>& OutNeededChunksVerticalIndices)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateHeightmap);

	if (Policy && Policy->GenerateHeightmap)
	{
		Policy->GenerateHeightmap(*this, OutGeneratedHeightmap, NeededHeightmapLocation, OutNeededChunksVerticalIndices);
		return;
	}

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
			NoisePoint = PlainsNoiseGenerator->GenSingle2D(NoiseLocation.X, NoiseLocation.Y, Seed);
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
			//if (!bPointBelongsToAdjacentCell)
			//{
			VoxelHeight *= VoxelSize;
			VoxelHeight -= VoxelSize;
			VoxelHeight -= FMath::GridSnap(ChunkSize / 2, VoxelSize);

			// Calculate the lowest and highest voxels so we know which vertical chunks to spawn
			if (VoxelHeight > HighestVoxel)
				HighestVoxel = VoxelHeight;
			if (VoxelHeight < LowestVoxel)
				LowestVoxel = VoxelHeight;
			//}

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

void FChunkThread::CombineChunkZIndices(const FVector2D& HeightmapLocation, TArray<int32>& TerrainZIndices)
{
	FScopeLock Lock(&ChunkZMutex);
	FIntPoint Cell2D{ AChunkManager::Get2DCellFromChunkLocation2D(HeightmapLocation, ChunkSize) };
	TArray<int32>& CombinedIndices{ ChunkZIndicesBy2DCell.FindOrAdd(Cell2D) };
	for (int32 TerrainZIndex : TerrainZIndices)
		if (!CombinedIndices.Contains(TerrainZIndex))
			CombinedIndices.Add(TerrainZIndex);

	TArray<int32>* ModifiedChunkAdditionalIndices{ ModifiedAdditionalChunkZIndicesBy2DCell.Find(Cell2D) };
	if (ModifiedChunkAdditionalIndices) // Add every Z Index we don't already have
		for (int32 AdditionalIndex : *ModifiedChunkAdditionalIndices)
			if (!CombinedIndices.Contains(AdditionalIndex))
				CombinedIndices.Add(AdditionalIndex);

	TerrainZIndices = CombinedIndices;
}

bool FChunkThread::AddConstructionData(TArray<TSharedPtr<FChunkConstructionData>>& OutChunkConstructionDataArray, const FVector2D& ChunkLocation2D, const TArray<int32>& VerticalChunkIndices)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::AddConstructionData);

	bool bNeedsCollision = DoesLocationNeedCollision(ChunkLocation2D, TrackerState.PlayerLocations, CollisionGenerationRadius);

	for (int32 ChunkIndex : VerticalChunkIndices)
	{
		float ChunkHeight = ChunkIndex * ChunkSize;
		FVector ChunkLocation{ ChunkLocation2D.X, ChunkLocation2D.Y, ChunkHeight };
		FIntVector ChunkCell{ AChunkManager::GetCellFromChunkLocation(ChunkLocation, ChunkSize) };
		OutChunkConstructionDataArray.Emplace(MakeShared<FChunkConstructionData>(ChunkLocation, ChunkCell, bNeedsCollision));
	}

	return !OutChunkConstructionDataArray.IsEmpty();
}

void FChunkThread::GenerateVoxelsForChunks(TArray<TSharedPtr<FChunkConstructionData>>& OutChunksConstructionData, const TArray<int16>& Heightmap)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateVoxelsForChunks);

	for (int32 Index{}; Index < OutChunksConstructionData.Num(); ++Index)
	{
		TSharedPtr<FChunkConstructionData>& ConstructionData = OutChunksConstructionData[Index];
		ConstructionData->LodStep = DetermineLodStepForChunk(ConstructionData->ChunkLocation);
		ActiveChunkLodStep = ConstructionData->LodStep;

		GenerateChunkVoxels(ConstructionData->Voxels, Heightmap, ConstructionData->ChunkLocation);
		ActiveChunkLodStep = 1;

		// ModifiedVoxelsByCell could exist from changes we made this session, changes from a loaded save, or they could be Received from the server if other players have modified this chunk
		ApplyModifiedVoxelsToChunk(ConstructionData->Voxels, ConstructionData->Cell);
	}

}

int32 FChunkThread::DetermineLodStepForChunk(const FVector& ChunkLocation) const
{
	if (TrackerState.PlayerLocations.IsEmpty())
	{
		return 1;
	}

	const FVector2D ChunkLocation2D(ChunkLocation.X, ChunkLocation.Y);
	int32 MinDistanceInChunks = MAX_int32;
	for (const FVector2D& PlayerLocation : TrackerState.PlayerLocations)
	{
		const int32 DistanceInChunks = FMath::CeilToInt32(FVector2D::Distance(ChunkLocation2D, PlayerLocation) / ChunkSize);
		MinDistanceInChunks = FMath::Min(MinDistanceInChunks, DistanceInChunks);
	}

	const int32 RawStep = ComputeLodStepByDistanceChunks(MinDistanceInChunks, LodNearDistanceInChunks, LodFarDistanceInChunks, LodDistanceCurveExponent, MaxLodStepPower);
	return FMath::Clamp(RawStep, 1, FMath::Max(1, VoxelCount));
}

bool FChunkThread::GenerateChunkVoxels(TArray<uint8>& Voxels, const TArray<int16>& Heightmap, const FVector& ChunkLocation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateChunkVoxels);

	if (Policy && Policy->GenerateChunkVoxels)
	{
		return Policy->GenerateChunkVoxels(*this, Voxels, Heightmap, ChunkLocation);
	}

	Voxels.Empty(TotalChunkVoxels);

	bool bIsBuried{ false }; // Used to determine if the ChunkMesh would be empty
	bool bIsAllAir{ false }; // Used to determine if the ChunkMesh would be empty

	const uint8 GrassBlockIndex = 1;
	const uint8 DirtBlockIndex = 2;
	const uint8 StoneBlockIndex = 4;
	const uint8 DirtDepth = 2;
	const int32 HeightmapVoxels1D = VoxelCount + 2;
	const int32 LODStep = FMath::Clamp(ActiveChunkLodStep, 1, FMath::Max(1, VoxelCount));

	auto SampleHeight = [&](const int32 X, const int32 Y)
		{
			const int32 CoarseX = FMath::Clamp((((X + 1) / LODStep) * LODStep) - 1, -1, VoxelCount);
			const int32 CoarseY = FMath::Clamp((((Y + 1) / LODStep) * LODStep) - 1, -1, VoxelCount);
			const int32 SampleIndex = (CoarseX + 1) * HeightmapVoxels1D + (CoarseY + 1);
			return Heightmap.IsValidIndex(SampleIndex) ? Heightmap[SampleIndex] : 25;
		};

	for (int32 Y{ -1 }; Y < VoxelCount + 1; Y++)
	{
		for (int32 X{ -1 }; X < VoxelCount + 1; X++)
		{
			const int32 TerrainNoiseSample = SampleHeight(X, Y);

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

void FChunkThread::ApplyModifiedVoxelsToChunk(TArray<uint8>& Voxels, FIntVector ChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::ApplyModifiedVoxelsToChunk);

	FScopeLock Lock(&ChunkManagerRef->ModifiedVoxelsMutex);
	FIntPoint Region{ GetRegionByLocation(FVector2D(FVector(ChunkCell) * ChunkSize)) };
	TMap<FIntVector, TMap<int32, uint8>>* ModifiedVoxelsByCell{ ChunkManagerRef->ModifiedVoxelsByCellByRegion.Find(Region) };

	if (!ModifiedVoxelsByCell)
		return;
	TMap<int32, uint8>* ModifiedVoxels{ ModifiedVoxelsByCell->Find(ChunkCell) };
	if (!ModifiedVoxels || ModifiedVoxels->IsEmpty())
		return;

	for (const TPair<int32, uint8>& VoxelPatch : *ModifiedVoxels)
	{
		const int32 VoxelIndex = VoxelPatch.Key;
		if (!Voxels.IsValidIndex(VoxelIndex))
			continue;

		Voxels[VoxelIndex] = VoxelPatch.Value;
	}
}

void FChunkThread::GenerateMeshDataForChunks(TArray<TSharedPtr<FChunkConstructionData>>& OutConstructionChunks)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateMeshDataForChunks);

	for (TSharedPtr<FChunkConstructionData>& NeededChunk : OutConstructionChunks)
	{
		GenerateChunkMeshData(
			NeededChunk->MeshData,
			NeededChunk->Voxels,
			NeededChunk->Cell,
			NeededChunk->bShouldGenerateCollision);
	}
}

// Can be called from any thread
void FChunkThread::GenerateChunkMeshData(FChunkMeshData& OutChunkMeshData, TArray<uint8>& Voxels, const FIntVector ChunkCell, const bool bShouldGenerateCollisionAtChunkSpawn)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTerrainChunkThread::GenerateChunkMeshData);

	if (Policy && Policy->GenerateChunkMeshData)
	{
		Policy->GenerateChunkMeshData(*this, OutChunkMeshData, Voxels, ChunkCell, bShouldGenerateCollisionAtChunkSpawn);
		return;
	}

	if (bUseGreedyMeshing)
	{
		BuildGreedyMeshData(*this, OutChunkMeshData, Voxels, ChunkCell, bShouldGenerateCollisionAtChunkSpawn, VoxelCount, VoxelSize, ChunkSize, VoxelDefinitions);
		return;
	}

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
					if (AdjacentVoxelValue > 0) // If this voxel is solid, we assume this face is buried
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

void FChunkThread::GenerateChunkMeshDataFromEncoded(FChunkMeshData& OutChunkMeshData, const TArray<uint8>& EncodedVoxels, const FIntVector ChunkCell, const bool bShouldGenerateCollisionAtChunkSpawn)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTerrainChunkThread::GenerateChunkMeshDataFromEncoded);

	if (!IsValidRleData(EncodedVoxels))
	{
		UE_LOG(LogTemp, Error, TEXT("Tried to generate a chunk from invalid encoded voxel data!"));
		return;
	}

	if (bUseGreedyMeshing)
	{
		TArray<uint8> DenseVoxels{};
		if (!DecodeRleToDense(EncodedVoxels, TotalChunkVoxels, DenseVoxels))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to decode RLE voxel data for greedy mesh generation."));
			return;
		}

		BuildGreedyMeshData(*this, OutChunkMeshData, DenseVoxels, ChunkCell, bShouldGenerateCollisionAtChunkSpawn, VoxelCount, VoxelSize, ChunkSize, VoxelDefinitions);
		return;
	}

	OutChunkMeshData.CollisionType = ECR_Block;
	OutChunkMeshData.ChunkCell = ChunkCell;
	OutChunkMeshData.bShouldGenCollision = bShouldGenerateCollisionAtChunkSpawn;

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

	FRleReadCursor MainCursor{};
	FRleReadCursor AdjacentCursors[6]{};

	int32 VoxelIndex{};
	int32 AdjacentVoxelIndex{};
	FVector3f VoxelLocation{ ChunkMeshOffset };
	FIntVector XYZ{};

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

				uint8 VoxelValue{};
				if (!ReadRleVoxelAt(EncodedVoxels, VoxelIndex, MainCursor, VoxelValue))
					continue;

				if (VoxelDefinitions[VoxelValue].bIsAir)
					continue;

				FSetElementId PolyGroupID = VoxelValuesInThisChunk.FindId(VoxelValue);

				for (int32 FaceIndex{}; FaceIndex < 6; FaceIndex++)
				{
					GetVoxelIndex(AdjacentVoxelIndex, XYZ + FaceIntDirections[FaceIndex]);

					uint8 AdjacentVoxelValue{};
					if (!ReadRleVoxelAt(EncodedVoxels, AdjacentVoxelIndex, AdjacentCursors[FaceIndex], AdjacentVoxelValue))
						continue;

					if (AdjacentVoxelValue > 0)
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
		TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateChunkMeshDataFromEncoded::CombineStreams);
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

FVector2f FChunkThread::CalculateUV(const int32& FaceIndex, const int32& VertIndex)
{
	return IVTChunkUtilities::CalculateUV(FaceIndex, VertIndex, CubeVertLocations);
}

bool FChunkThread::DoesLocationNeedCollision(FVector2D ChunkLocation2D, const TArray<FVector2D>& TrackedLocationsRef, int32 ChunkGenRadius)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::DoesLocationNeedCollision);

	for (int32 LocationIndex{}; LocationIndex < TrackedLocationsRef.Num(); LocationIndex++)
	{
		FVector2D TrackedLocation{ TrackedLocationsRef[LocationIndex] };
		if (IsHeightmapInRange(ChunkLocation2D, TrackedLocation, ChunkGenRadius))
			return true;
	}

	return false;
}

void FChunkThread::CompressVoxelData(TArray<TSharedPtr<FChunkConstructionData>>& ChunkConstructionDataArray)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::CompressVoxelData);

	for (TSharedPtr<FChunkConstructionData>& ConstrctionChunk : ChunkConstructionDataArray)
	{
		ConstrctionChunk->bAreVoxelsCompressed = true;
		IVTChunkUtilities::RunLengthEncode(ConstrctionChunk->Voxels);
	}
}

void FChunkThread::AsyncSpawnChunks(TArray<TSharedPtr<FChunkConstructionData>>& ChunkConstructionDataArray, const FVector2D& HeightmapLocation, const TArray<int32>& TerrainZIndices)
{
	int32 ChunkCount{};
	for (TSharedPtr<FChunkConstructionData>& ChunkConstructionData : ChunkConstructionDataArray)
	{
		// Throttle this so we don't have a massive number of chunks needed to be generated on the game thread at once (especially useful when we have large stacks of vertical chunks)
		FPlatformProcess::Sleep(FMath::Min(ThreadWorkingSleepTime * ChunkCount++, 0.05));
		if (!bIsRunning || !WorldRef) return;

		int32 TempChunkRadius{ TempChunkGenRadius };
		int32 TempCollisionRadius{ TempCollisionGenRadius };
		ChunkManagerRef->EnqueueWorldCommand(FChunkWorldCommand::MakeSpawn(ChunkConstructionData, TempChunkRadius, TempCollisionRadius, true));
	}
	ChunkConstructionDataArray.Empty();
}

bool FChunkThread::ShouldSpawnHidden(FVector2D ChunkLocation, int32 ChunkGenRadius)
{
	return ChunkManagerRef->GetNetMode() == ENetMode::NM_ListenServer && !IsHeightmapInRange(ChunkLocation, TrackerState.PlayerLocations[0], ChunkGenRadius);
}

// This function runs on the game thread. Called by AsyncTaskGraph in ChunkThread's Run()
void FChunkThread::SpawnChunkFromConstructionData(TSharedPtr<FChunkConstructionData> OutNeededChunk, int32 ChunkGenRadius, int32 CollisionGenRadius, bool bShouldGenerateMesh)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::SpawnChunkFromConstructionData);

	if (!WorldRef || WorldRef->bIsTearingDown)
		return;

	if (!ChunkManagerRef || !IsValid(ChunkManagerRef))
		return;

	if (!OutNeededChunk || !OutNeededChunk.IsValid())
		return;

	FIntPoint Cell2D{ OutNeededChunk->Cell.X, OutNeededChunk->Cell.Y };
	FIntVector ChunkCell{ OutNeededChunk->Cell };
	AChunkActor* Chunk{};
	
	if(ChunkManagerRef->ChunksToDestroyQueue.Contains(ChunkCell))
		ChunkManagerRef->ChunksToDestroyQueue.Remove(ChunkCell);
	
	if (ChunkManagerRef->ChunksByCell.Contains(ChunkCell))
		Chunk = *ChunkManagerRef->ChunksByCell.Find(ChunkCell);

	bool bClientHadChunkName{ false };
	bool bIsNewChunk{ Chunk == nullptr };
	if (bIsNewChunk)
	{
		// Set actor parameters
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParameters.bDeferConstruction = true;
		SpawnParameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		SpawnParameters.Owner = ChunkManagerRef;
		FString ChunkName{};

		Chunk = WorldRef->SpawnActor<AChunkActor>(OutNeededChunk->ChunkLocation, FRotator::ZeroRotator, SpawnParameters);
	}
	if (!Chunk)
	{
		DrawDebugPoint(WorldRef, OutNeededChunk->ChunkLocation, 15, FColor(200, 25, 55), false, 5.f);
		DrawDebugString(WorldRef, OutNeededChunk->ChunkLocation + FVector(0, 0, 50), TEXT("Failed to find or spawn Chunk Actor"), nullptr, FColor(200, 25, 55), 5.f);

		return;
	}

	// Set up the chunk
	if (bIsNewChunk)
	{
		Chunk->bReplicates = false;
		Chunk->bAlwaysRelevant = true;
		Chunk->bNetLoadOnClient = false;
		FString ChunkCellString{ ChunkCell.ToString() };
		Chunk->Tags.Add(*ChunkCellString);
		Chunk->ChunkCell = ChunkCell;
		Chunk->VoxelCount = VoxelCount;
		Chunk->VoxelSize = VoxelSize;
		Chunk->ChunkSize = ChunkSize;
		Chunk->Voxels = MoveTemp(OutNeededChunk->Voxels);
		Chunk->bAreVoxelsCompressed = OutNeededChunk->bAreVoxelsCompressed;
	}
	if (ChunkManagerRef->GetNetMode() == ENetMode::NM_Client)
	{
		if (ChunkManagerRef->GetIsReplicated() == false)
			UE_LOG(LogTemp, Error, TEXT("ChunkManagerRef was not replicated!"));

		if (ChunkManagerRef->ChunkSpawnCountByCell.Contains(ChunkCell))
		{
			bClientHadChunkName = true;
			int32 ChunkSpawnCount{ ChunkManagerRef->ChunkSpawnCountByCell.FindRef(ChunkCell) };

			ChunkManagerRef->SetChunkName(Chunk, ChunkCell, ChunkSpawnCount);
		}
	}

	ChunkManagerRef->ChunkZIndicesBy2DCell.FindOrAdd(FIntPoint(ChunkCell.X, ChunkCell.Y)).Add(ChunkCell.Z);

	if (bClientHadChunkName)
		Chunk->bIsSafeToDestroy = false;
	else
		Chunk->bIsSafeToDestroy = true;

	if (bIsNewChunk) // Finish spawning the actor
		Chunk->FinishSpawning(FTransform(OutNeededChunk->ChunkLocation));

	if (!Chunk)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to spawn Chunk Actor at %s"), *OutNeededChunk->ChunkLocation.ToString());
		return;
	}

	if ((ChunkManagerRef->GetNetMode() == ENetMode::NM_DedicatedServer || ChunkManagerRef->GetNetMode() == NM_ListenServer) && IsNeededHeightmapLocation(FVector2D(OutNeededChunk->ChunkLocation), TrackerState.PlayerLocations, CollisionGenRadius, CollisionGenRadius)) // We don't want to modify this data if we are on a client, as the client populates this data from the server
		EnableReplicationForChunk(Chunk);

	// This indicates we generated this chunk for a player other than the host player, so we can hide it
	if (ShouldSpawnHidden(FVector2D(Chunk->GetActorLocation()), ChunkGenRadius + ChunkDeletionBuffer))
		ChunkManagerRef->HideChunk(Chunk);

	if (!ChunkManagerRef->VoxelTypesDatabase)
	{
		DrawDebugPoint(WorldRef, OutNeededChunk->ChunkLocation, 15, FColor(255, 25, 75), false, 5.f);
		DrawDebugString(WorldRef, OutNeededChunk->ChunkLocation + FVector(0, 0, 50), TEXT("VoxelTypesDatabase was nullptr!"), nullptr, FColor(255, 25, 75), 5.f);

		return;
	}

	if (bIsNewChunk)
		ChunkManagerRef->ChunksByCell.Add(OutNeededChunk->Cell, Chunk);
	if (!bShouldGenerateMesh)
		return;

	TArray<UMaterial*> VoxelMaterials{};
	ChunkManagerRef->GetMaterialsForChunkData(OutNeededChunk->MeshData.VoxelSections, VoxelMaterials);
	Chunk->GenerateChunkMesh(OutNeededChunk->MeshData, VoxelMaterials);
}

void FChunkThread::SaveUnsavedRegions(bool bSaveAsync)
{
	TArray<FIntPoint> RegionsToSave;
	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		RegionsToSave = ChunkManagerRef->RegionsChangedSinceLastSave;
	}
	for (FIntPoint Region : RegionsToSave) // We only run this async if we aren't closing out the thread. If we are we need to make sure we save the data before we close, so we can't do it Async
		AsyncSaveVoxelsForRegion(Region, WorldSaveName, false, bSaveAsync);
}

void FChunkThread::AsyncSaveVoxelsForRegion(FIntPoint Region, FString SaveName, bool bRemoveDataWhenDone, bool bRunAsync)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::AsyncSaveVoxelsForRegion);

	if (IsInGameThread() && bRunAsync)
	{
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, Region, SaveName, bRemoveDataWhenDone]()
			{
				SaveVoxelsForRegion(SaveName, Region, bRemoveDataWhenDone);
			});

	}
	else
		SaveVoxelsForRegion(SaveName, Region, bRemoveDataWhenDone);
}

void FChunkThread::SaveVoxelsForRegion(const FString& SaveName, const FIntPoint& Region, bool bRemoveDataWhenDone)
{
	if (!ChunkManagerRef)
	{
		return;
	}

	FRegionDataService::SaveVoxelsForRegion(ChunkManagerRef, SaveFolderName, SaveName, Region, bRemoveDataWhenDone);
}

// Do not call from game thread
void FChunkThread::LoadVoxelsForRegion(FIntPoint Region, FString SaveName)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::LoadVoxelsForRegion);
	FRegionDataService::LoadVoxelsForRegion(ChunkManagerRef, SaveFolderName, SaveName, Region, ChunkZMutex, ModifiedAdditionalChunkZIndicesBy2DCell);
}

void FChunkThread::GetRegionsToSave(TArray<FIntPoint>& RegionsToSave)
{
	FRegionDataService::GetRegionsToSave(ChunkManagerRef, RegionsToSave, bIsRunning);
}

void FChunkThread::GetRegionsToLoad(TArray<FIntPoint>& RegionsToLoad)
{
	FRegionDataService::GetRegionsToLoad(ChunkManagerRef, RegionsToLoad, bIsRunning, WorldRef->GetNetMode());
}

void FChunkThread::SetChunkGenRadius(int32 Radius)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::SetChunkGenRadius);
	FScopeLock Lock(&ChunkGenMutex);

	if (Radius < CollisionGenerationRadius)
		Radius = CollisionGenerationRadius;


	bWasRangeChanged = true;
	ChunkGenerationRadius = Radius;     
	TrackerState.LastRingCount = 0;
	for (int32 TrackedActorIndex{}; TrackedActorIndex < TrackerState.PlayerLocations.Num(); TrackedActorIndex++)
	{
		if (!TrackerState.TrackedChunkRingDistance.IsValidIndex(TrackedActorIndex))
			continue;
		TrackerState.TrackedChunkRingDistance[TrackedActorIndex] = FMath::Min(TrackerState.TrackedChunkRingDistance[TrackedActorIndex], Radius);

		if (!TrackerState.TrackedChunkRingCount.IsValidIndex(TrackedActorIndex))
			continue;

		TrackerState.TrackedChunkRingCount[TrackedActorIndex] = FMath::Min(TrackerState.TrackedChunkRingCount[TrackedActorIndex], ChunkGenerationRadius * 1.4);
	}
}

void FChunkThread::GetVoxelIndex(int32& VoxelIndex, int32& X, int32& Y, int32& Z)
{
	VoxelIndex = IVTChunkUtilities::GetVoxelIndex(VoxelCount, X, Y, Z);
}

void FChunkThread::GetVoxelIndex(int32& VoxelIndex, const FIntVector XYZ)
{
	VoxelIndex = IVTChunkUtilities::GetVoxelIndex(VoxelCount, XYZ);
}

FVector FChunkThread::CalculateTangent(const FVector& Normal)
{
	return IVTChunkUtilities::CalculateTangent(Normal);
}

// Only call from the game thread, Only call from server
bool FChunkThread::EnableReplicationForChunk(AChunkActor* Chunk, bool bShouldDirectlySetbReplicates)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::EnableReplicationForChunk);

	if (!WorldRef)
		return false;

	if (!IsInGameThread())
	{
		UE_LOG(LogTemp, Error, TEXT("EnableChunkReplication was called from a non-game thread!"));
		return false;
	}

	if (ChunkManagerRef->GetNetMode() == NM_Client)
	{
		UE_LOG(LogTemp, Error, TEXT("EnableChunkReplication was called on a client!"));
		return false;
	}

	if (!Chunk || !IsValid(Chunk))
	{
		UE_LOG(LogTemp, Error, TEXT("Chunk was nullptr!"));
		return false;
	}

	if (!ChunkManagerRef)
	{
		UE_LOG(LogTemp, Error, TEXT("ChunkManagerRef was nullptr!"));
		return false;
	}

	FIntVector& ChunkCell{ Chunk->ChunkCell };
	bool bDidChunkSpawnCountExist{ ChunkManagerRef->ChunkSpawnCountByCell.Contains(ChunkCell) };
	int32* ChunkSpawnCount{ &ChunkManagerRef->ChunkSpawnCountByCell.FindOrAdd(ChunkCell, 0) };

	if (Chunk->bReplicates)
	{
		if (!bDidChunkSpawnCountExist)
			UE_LOG(LogTemp, Error, TEXT("Chunk %s was replicated, but no spawn count was found!"), *Chunk->GetName());
		return true;
	}

	if (bDidChunkSpawnCountExist)
		(*ChunkSpawnCount)++;
	FString NewName{ FString(GetDeterministicNameByLocationAndRepCount(ChunkCell, *ChunkSpawnCount)) };

	if (NewName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to generate a new name for Chunk %s!"), *Chunk->GetName());
		return false;
	}

	if (!Chunk->GetName().Equals(NewName))
	{
		if (!Chunk->Rename(*NewName, ChunkManagerRef, REN_ForceNoResetLoaders))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to rename Chunk %s to %s!"), *Chunk->GetName(), *NewName);
			return false;
		}

		if (bShouldDirectlySetbReplicates)
			Chunk->bReplicates = true;
		else
			Chunk->SetReplicates(true);

		Chunk->bAlwaysRelevant = true;
		Chunk->bOnlyRelevantToOwner = false;

		Chunk->bIsSafeToDestroy = false;

		// for every key in TrackedChunkNamesUpToDate, remove the ChunkKey from the value at that key
		for (TPair<APlayerController*, TArray<FIntVector>>& TrackedCellArray : ChunkManagerRef->TrackedChunkNamesUpToDate)
		{
			TArray<FIntVector>& ChunkCells{ TrackedCellArray.Value };
			ChunkCells.Remove(ChunkCell); // We know it's not up to date, because we just modified the count, and haven't sent it to the client yet
		}
	}
	else
	{
		return false;
	}

	if (!WorldRef)
	{
		UE_LOG(LogTemp, Error, TEXT("WorldRef is nullptr! Cannot draw debug string for Chunk %s"), *Chunk->GetName());
		return false;
	}

	return true;
}

FString FChunkThread::GetDeterministicNameByLocationAndRepCount(const FIntVector& ChunkCell, int32 ReplicationCount)
{
	return FString::Printf(TEXT("X%i_Y%i_Z%i_N%i"), ChunkCell.X, ChunkCell.Y, ChunkCell.Z, ReplicationCount);
}

void FChunkThread::DeleteSaveGame(FString SaveName)
{
	// Validate SaveName
	if (SaveName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid WorldSaveName: %s"), *SaveName);
		return;
	}

	FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), SaveFolderName, SaveName);

	// Check if the file exists
	if (FPaths::DirectoryExists(SavePath))
	{
		// Try to delete the save file
		if (!FPlatformFileManager::Get().GetPlatformFile().DeleteDirectoryRecursively(*SavePath))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to delete SavePath %s."), *SavePath);
		}
	}
}

TArray<FString> FChunkThread::GetSaveFoldersNames()
{
	TArray<FString> SaveFolderNames{};

	FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), SaveFolderName);

	if (FPaths::DirectoryExists(SavePath))
	{
		TArray<FString> SubDirectoryNames;
		IFileManager::Get().FindFilesRecursive(SubDirectoryNames, *SavePath, TEXT("*"), false, true);

		for (const FString& SubDirectoryName : SubDirectoryNames)
		{
			if (!IFileManager::Get().DirectoryExists(*SubDirectoryName))
				continue;

			FString FolderName = FPaths::GetPathLeaf(SubDirectoryName);
			SaveFolderNames.Add(FolderName);
		}
	}


	return SaveFolderNames;
}

void RunLengthEncode(TArray<uint8>& VoxelData, FIntVector OwningChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RunLengthEncode);

	if (VoxelData.IsEmpty())
		return;


	IVTChunkUtilities::RunLengthEncode(VoxelData);
}

void RunLengthDecode(TArray<uint8>& EncodedData, FIntVector OwningChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RunLengthDecode);

	if (EncodedData.IsEmpty())
		return;


	IVTChunkUtilities::RunLengthDecode(EncodedData);
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIVTRleReadCursorSequentialTest, "IVT.ChunkMath.RLEReadCursorSequential", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIVTRleReadCursorSequentialTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> EncodedData{ 3, 5, 2, 1, 4, 9 };
	FRleReadCursor Cursor{};

	const TArray<int32> Expected{ 5, 5, 5, 1, 1, 9, 9, 9, 9 };
	for (int32 Index = 0; Index < Expected.Num(); ++Index)
	{
		uint8 Value{};
		TestTrue(FString::Printf(TEXT("Cursor should decode index %d"), Index), ReadRleVoxelAt(EncodedData, Index, Cursor, Value));
		TestEqual(FString::Printf(TEXT("Decoded value mismatch at index %d"), Index), static_cast<int32>(Value), Expected[Index]);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIVTRleReadCursorRandomAccessResetTest, "IVT.ChunkMath.RLEReadCursorRandomAccessReset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIVTRleReadCursorRandomAccessResetTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> EncodedData{ 1, 8, 3, 2, 1, 7 };
	FRleReadCursor Cursor{};

	uint8 Value{};
	TestTrue(TEXT("Read high index first"), ReadRleVoxelAt(EncodedData, 4, Cursor, Value));
	TestEqual(TEXT("Index 4 should decode to 7"), static_cast<int32>(Value), 7);

	TestTrue(TEXT("Read lower index should force cursor reset and still succeed"), ReadRleVoxelAt(EncodedData, 0, Cursor, Value));
	TestEqual(TEXT("Index 0 should decode to 8"), static_cast<int32>(Value), 8);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIVTCalculateCircumferenceInChunksTest, "IVT.ChunkMath.CalculateCircumferenceInChunks", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIVTCalculateCircumferenceInChunksTest::RunTest(const FString& Parameters)
{
	const int32 RadiusOne = FChunkThread::CalculateCircumferenceInChunks(1, 100.0f);
	const int32 RadiusThree = FChunkThread::CalculateCircumferenceInChunks(3, 100.0f);

	TestEqual(TEXT("Radius 1 should ceil to 7 chunks"), RadiusOne, 7);
	TestEqual(TEXT("Radius 3 should ceil to 19 chunks"), RadiusThree, 19);
	TestTrue(TEXT("Circumference should be monotonic with radius"), RadiusThree > RadiusOne);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIVTLodStepCurveTest, "IVT.ChunkMath.LODStepCurve", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIVTLodStepCurveTest::RunTest(const FString& Parameters)
{
	const int32 NearStep = ComputeLodStepByDistanceChunks(0, 4, 20, 1.6f, 3);
	const int32 MidStep = ComputeLodStepByDistanceChunks(12, 4, 20, 1.6f, 3);
	const int32 FarStep = ComputeLodStepByDistanceChunks(30, 4, 20, 1.6f, 3);

	TestEqual(TEXT("Near chunks should stay full detail"), NearStep, 1);
	TestTrue(TEXT("LOD step should increase with distance"), MidStep >= NearStep);
	TestEqual(TEXT("Far chunks should clamp to max configured step"), FarStep, 8);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIVTGreedyMeshingReductionTest, "IVT.ChunkMath.GreedyMeshingReduction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIVTGreedyMeshingReductionTest::RunTest(const FString& Parameters)
{
	const int32 TestVoxelCount = 8;
	const int32 Side = TestVoxelCount + 2;
	TArray<uint8> DenseVoxels;
	DenseVoxels.Init(0, Side * Side * Side);

	for (int32 X = 0; X < TestVoxelCount; ++X)
	{
		for (int32 Y = 0; Y < TestVoxelCount; ++Y)
		{
			for (int32 Z = 0; Z < TestVoxelCount; ++Z)
			{
				DenseVoxels[GetDenseVoxelIndex(TestVoxelCount, X, Y, Z)] = 1;
			}
		}
	}

	const int32 NaiveFaceCount = CalculateNaiveVisibleFaceCount(DenseVoxels, TestVoxelCount);
	const int32 GreedyQuadCount = CalculateGreedyQuadCount(DenseVoxels, TestVoxelCount);

	TestEqual(TEXT("Solid cube naive visible faces should be 6*N^2"), NaiveFaceCount, 6 * TestVoxelCount * TestVoxelCount);
	TestEqual(TEXT("Greedy meshing should collapse solid cube to 6 quads"), GreedyQuadCount, 6);

	return true;
}
#endif