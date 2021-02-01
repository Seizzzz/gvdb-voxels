//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2020 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
// 
// Version 1.1.1: Neil Bickford, 8/12/2020
//----------------------------------------------------------------------------------

#include <algorithm> // For min/max

#include "gvdb_export_nanovdb.h"

namespace nvdb {
	using namespace nanovdb;

	static CUmodule exportModule = (CUmodule)(-1);
	static CUfunction cuFuncRender = (CUfunction)(-1);
	static CUfunction cuFuncProcessLeaves = (CUfunction)(-1);
	static CUfunction cuFuncProcessInternalNodes = (CUfunction)(-1);

	static const int TREE_DEPTH = 3; // NanoVDB trees are always 3 levels deep

	// Equivalent to VolumeGVDB::LoadFunction, will be removed when this is incorporated into the
	// main GVDB library.
	void LoadFunction(CUfunction& function, char* functionName) {
		if (exportModule == (CUmodule)(-1)) {
			cudaCheck(cuModuleLoad(&exportModule, "cuda_export_nanovdb.ptx"), "nvdb", "LoadFunction", "cuModuleLoad", "cuda_export_nanovdb.ptx", false);
		}

		if (function == (CUfunction)(-1)) {
			cudaCheck(cuModuleGetFunction(&function, exportModule, functionName), "nvdb", "LoadFunction", "cuModuleGetFunction", functionName, false);
		}
	}

	// Stores the size of each of the NanoVDB types for a particular GVDB configuration.
	struct NanoVDBTypeSizes {
		size_t grid;
		size_t tree;
		size_t root;
		size_t rootTile;
		size_t node2;
		size_t node1;
		size_t leaf;
	};

	// Converts a supported gvdbType into an index that can be used to look up a function in a
	// function table.
	int TypeTableIndex(uchar gvdbType) {
		switch (gvdbType) {
		case T_FLOAT:
			return 0;
		case T_FLOAT3:
			return 1;
		case T_INT:
			return 2;
		default:
			assert(!"TypeTableIndex: Unrecognized type!");
			return 0;
		}
	}

	size_t DataTypeSizeLookup(const size_t sizes[3][6], uchar gvdbType, int log2Dim) {
		if (log2Dim < 2 || log2Dim > 7) {
			assert(!"DataTypeSizeLookup: log2Dim was out of range!");
			return 0;
		}

		return sizes[TypeTableIndex(gvdbType)][log2Dim - 2];
	}

	// Autogenerated list of sizes of NanoVDB types. You can generate this list using the following Python code:
	/*
print('static const size_t nodeSizes[3][6] = {')
for type in ['float', 'Vec3f', 'int']:
  print('{', end='')
  for ld in range(2, 8):
	print(f'sizeof(InternalNode<LeafNode<{type}>, {ld}>)', end='')
	if ld != 7:
	  print(', ', end='')
  if(type == 'int'):
	print('}')
  else:
	print('},')
print('};')
	*/
	static const size_t nodeSizes[3][6] = {
	{sizeof(InternalNode<LeafNode<float>, 2>), sizeof(InternalNode<LeafNode<float>, 3>), sizeof(InternalNode<LeafNode<float>, 4>), sizeof(InternalNode<LeafNode<float>, 5>), sizeof(InternalNode<LeafNode<float>, 6>), sizeof(InternalNode<LeafNode<float>, 7>)},
	{sizeof(InternalNode<LeafNode<Vec3f>, 2>), sizeof(InternalNode<LeafNode<Vec3f>, 3>), sizeof(InternalNode<LeafNode<Vec3f>, 4>), sizeof(InternalNode<LeafNode<Vec3f>, 5>), sizeof(InternalNode<LeafNode<Vec3f>, 6>), sizeof(InternalNode<LeafNode<Vec3f>, 7>)},
	{sizeof(InternalNode<LeafNode<int>, 2>), sizeof(InternalNode<LeafNode<int>, 3>), sizeof(InternalNode<LeafNode<int>, 4>), sizeof(InternalNode<LeafNode<int>, 5>), sizeof(InternalNode<LeafNode<int>, 6>), sizeof(InternalNode<LeafNode<int>, 7>)}
	};

	// Autogenerated list of sizes of leaf nodes. You can generate this list using the following Python code:
	/*
print('static const size_t leafSizes[3][6] = {')
for type in ['float', 'Vec3f', 'int']:
  print('{', end='')
  for ld in range(2, 8):
	print(f'sizeof(LeafNode<{type}, Coord, Mask, {ld}>)', end='')
	if ld != 7:
	  print(', ', end='')
  if(type == 'int'):
	print('}')
  else:
	print('},')
print('};')
	*/
	static const size_t leafSizes[3][6] = {
	{sizeof(LeafNode<float, Coord, Mask, 2>), sizeof(LeafNode<float, Coord, Mask, 3>), sizeof(LeafNode<float, Coord, Mask, 4>), sizeof(LeafNode<float, Coord, Mask, 5>), sizeof(LeafNode<float, Coord, Mask, 6>), sizeof(LeafNode<float, Coord, Mask, 7>)},
	{sizeof(LeafNode<Vec3f, Coord, Mask, 2>), sizeof(LeafNode<Vec3f, Coord, Mask, 3>), sizeof(LeafNode<Vec3f, Coord, Mask, 4>), sizeof(LeafNode<Vec3f, Coord, Mask, 5>), sizeof(LeafNode<Vec3f, Coord, Mask, 6>), sizeof(LeafNode<Vec3f, Coord, Mask, 7>)},
	{sizeof(LeafNode<int, Coord, Mask, 2>), sizeof(LeafNode<int, Coord, Mask, 3>), sizeof(LeafNode<int, Coord, Mask, 4>), sizeof(LeafNode<int, Coord, Mask, 5>), sizeof(LeafNode<int, Coord, Mask, 6>), sizeof(LeafNode<int, Coord, Mask, 7>)}
	};

	template<class ValueT>
	using RootDataPrototype = RootData<InternalNode<InternalNode<LeafNode<ValueT>>>>;

	NanoVDBTypeSizes ComputeTypeSizes(uchar gvdbType, int brickLog2Dim, int node1Log2Dim, int node2Log2Dim) {
		// We could numerically compute the sizes of the types by manually looking at the source
		// code and counting, but it's hopefully more robust in the long run to get the sizes from
		// the types themselbes.
		// Unfortunately, this winds up being not very elegant in a different way, since we have to
		// instantiate all of the different types we might need, and then switch between the correct
		// cases. The PTX code has this issue as well; there, we'll use tables of functions, but here
		// (since we can afford the cost) we'll use variadic templates.
		// If you haven't seen variadic templates before, don't worry - here, we just use them as
		// a list of integers!

		NanoVDBTypeSizes result{};
		result.grid = sizeof(GridData); // Non-templated type
		result.tree = sizeof(TreeData<TREE_DEPTH>);
		// Relatively simple for the root to only switch over cases
		switch (gvdbType) {
		case T_FLOAT:
			result.root = sizeof(RootDataPrototype<float>);
			result.rootTile = sizeof(RootDataPrototype<float>::Tile);
			break;
		case T_INT:
			result.root = sizeof(RootDataPrototype<int>);
			result.rootTile = sizeof(RootDataPrototype<int>::Tile);
			break;
		case T_FLOAT3:
			result.root = sizeof(RootDataPrototype<Vec3f>);
			result.rootTile = sizeof(RootDataPrototype<Vec3f>::Tile);
			break;
		}
		result.node2 = DataTypeSizeLookup(nodeSizes, gvdbType, node2Log2Dim);
		result.node1 = DataTypeSizeLookup(nodeSizes, gvdbType, node1Log2Dim);
		result.leaf = DataTypeSizeLookup(leafSizes, gvdbType, brickLog2Dim);

		return result;
	}

	// Overloaded function to get the maximum and minimum value of a ValueT, since nanovdb::Maximum
	// doesn't handle Vec3f at the moment
	template<class T> T ExportToNanoVDB_MaximumValue() {
		return std::numeric_limits<T>::max();
	}
	template<> Vec3f ExportToNanoVDB_MaximumValue<Vec3f>() {
		return { FLT_MAX, FLT_MAX, FLT_MAX };
	}
	template<class T> T ExportToNanoVDB_MinimumValue() {
		return std::numeric_limits<T>::min();
	}
	template<> Vec3f ExportToNanoVDB_MinimumValue<Vec3f>() {
		return { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	}

	// A union type for the possible values of a ValueT.
	union ValueUnion {
		float f;
		nanovdb::Vec3f f3;
		int i;
	};

	template<class T>
	__device__ T* getValueUnion(ValueUnion& value) { return reinterpret_cast<T*>(&value.f3); }

	// Since level-2 nodes are templated, we use a table of functions in order to retrieve
	// data from them.

	struct NodeRangeData {
		ValueUnion valueMin;
		ValueUnion valueMax;
		CoordBBox aabb;
	};

	// Gets information about the range of the given level-2 node in a C-like format.
	template<class ValueT, int LOG2DIM> NodeRangeData GetNode2Range(uint8_t* node2Start, int nodeIdx) {
		// As usual, this isn't the actual type of the node, but it's enough to make this work:
		using Node2T = InternalNode<LeafNode<ValueT>, LOG2DIM>;

		Node2T* node = reinterpret_cast<Node2T*>(node2Start) + nodeIdx;

		NodeRangeData result;

		*getValueUnion<ValueT>(result.valueMin) = node->valueMin();
		*getValueUnion<ValueT>(result.valueMax) = node->valueMax();
		result.aabb = node->bbox();

		return result;
	}

	// A Node2RangeFunc is a function that takes a uint8_t* and an int and returns a NodeRangeData
	using Node2RangeFunc = NodeRangeData(*)(uint8_t*, int);

	// Autogenerated list of instantiations of GetNode2Range. You can generate this using the following Python code:
	/*
print('static const Node2RangeFunc rangeFunctions[3][6] = {')
for type in ['float', 'Vec3f', 'int']:
  print('{', end='')
  for ld in range(2, 8):
	print(f'GetNode2Range<{type}, {ld}>', end='')
	if ld != 7:
	  print(', ', end='')
  if(type == 'int'):
	print('}')
  else:
	print('},')
print('};')
	*/
	static const Node2RangeFunc rangeFunctions[3][6] = {
{GetNode2Range<float, 2>, GetNode2Range<float, 3>, GetNode2Range<float, 4>, GetNode2Range<float, 5>, GetNode2Range<float, 6>, GetNode2Range<float, 7>},
{GetNode2Range<Vec3f, 2>, GetNode2Range<Vec3f, 3>, GetNode2Range<Vec3f, 4>, GetNode2Range<Vec3f, 5>, GetNode2Range<Vec3f, 6>, GetNode2Range<Vec3f, 7>},
{GetNode2Range<int, 2>, GetNode2Range<int, 3>, GetNode2Range<int, 4>, GetNode2Range<int, 5>, GetNode2Range<int, 6>, GetNode2Range<int, 7>}
	};

	// Computes the bounding box and min/max values for the grid and root nodes from the level-2
	// node data.
	template<class ValueT>
	void ProcessGridExtents(GridData* gridData, uint8_t* rootDataPtr, uint8_t* node2Start,
		uint64_t activeVoxelCount, void* background, int numNode2s, int node2Log2Dim, int totalLog2Dim)
	{
		// This suffices to pass the right types to RootData, and pass the CoordToKey static_assert
		// (see below), even though the branching factors and TOTAL won't match.
		using RootDataT = RootData<InternalNode<InternalNode<LeafNode<ValueT>>>>;

		if (rootDataPtr == nullptr) {
			assert(!"Internal error in ProcessRoot: rootDataPtr was nullptr!");
			return;
		}

		if (background == nullptr) {
			assert(!"Internal error in ProcessRoot: background was nullptr!");
			return;
		}

		nanovdb::CoordBBox indexAABB; // Index-space bounding box
		// Initial bounds for values
		ValueT valueMin = ExportToNanoVDB_MaximumValue<ValueT>();
		ValueT valueMax = ExportToNanoVDB_MinimumValue<ValueT>();

		// Get the root data:
		RootDataT* rootData = reinterpret_cast<RootDataT*>(rootDataPtr);

		// Set the voxel count that was passed in:
		rootData->mActiveVoxelCount = activeVoxelCount;

		// Reinterpret and set the background:
		rootData->mBackground = *reinterpret_cast<ValueT*>(background);

		// One tile per level-2 node:
		rootData->mTileCount = numNode2s;

		// Normally, to get the child node, we would call rootData->child(tile). Unfortunately,
		// since node2Log2Dim is a parameter, not a type, we need to get a pointer to the
		// correct instantiation.
		Node2RangeFunc rangeFunc;
		if (std::is_same<ValueT, float>::value) {
			rangeFunc = rangeFunctions[0][node2Log2Dim - 2];
		}
		else if (std::is_same<ValueT, Vec3f>::value) {
			rangeFunc = rangeFunctions[1][node2Log2Dim - 2];
		}
		else if (std::is_same<ValueT, int>::value) {
			rangeFunc = rangeFunctions[2][node2Log2Dim - 2];
		}
		else {
			assert(!"Internal error in ProcessRoot: ValueT was unsupported!");
			return;
		}

		assert(32 - totalLog2Dim <= 21); // Restriction from RootData::CoordToKey

		// Iterate over tiles. We'll initially write them in linear order, then sort them by
		// key afterwards.
		for (int tileIdx = 0; tileIdx < numNode2s; tileIdx++) {
			RootDataT::Tile& tile = rootData->tile(tileIdx);

			NodeRangeData rangeData = rangeFunc(node2Start, tileIdx);

			// For the moment, we assume that all nodes are active. (This would come from
			// gvdbNode->mFlags).
			// Please note: These next two lines are especially a hack! They reimplement
			// Tile::setChild and RootData::CoordToKey (we include some compile-time test cases to
			// evaluate if this ever breaks). This is to avoid having to do template specialization
			// over the value of ChildT::TOTAL.
			// Equivalent to tile.setChild(rangeData.aabb.min(), tileIdx), if TOTAL was correctly
			// specified
			Coord ijk = rangeData.aabb.min();
			tile.key = uint64_t(ijk[2] >> totalLog2Dim) |
				(uint64_t(ijk[1] >> totalLog2Dim) << 21) |
				(uint64_t(ijk[0] >> totalLog2Dim) << 42);
			tile.childID = tileIdx;

			// Update the bounding box and min and max values.
			for (int c = 0; c < 3; c++) {
				indexAABB.min()[c] = std::min(indexAABB.min()[c], rangeData.aabb.min()[c]);
				indexAABB.max()[c] = std::max(indexAABB.max()[c], rangeData.aabb.max()[c]);
			}
			valueMin = ExportToNanoVDB_Min(valueMin, *getValueUnion<ValueT>(rangeData.valueMin));
			valueMax = ExportToNanoVDB_Max(valueMax, *getValueUnion<ValueT>(rangeData.valueMax));
		}

		// Set the bounding box and min/max values for the whole volume:
		rootData->mBBox = indexAABB;
		rootData->mMinimum = valueMin;
		rootData->mMaximum = valueMax;

		// Sort the tiles so that their keys are in ascending order. This makes it so that
		// RootNode::findTile can efficiently find them.
		{
			RootDataT::Tile* startTile = &rootData->tile(0);
			RootDataT::Tile* endTile = &rootData->tile(numNode2s - 1) + 1; // Note that this is one after the last tile

			// This says "Treat startTile and endTile as random access iterators. Sort them using
			// this comparison operator: two tiles b...a in order are sorted if a.key < b.key."
			std::sort(startTile, endTile, [](const RootDataT::Tile& a, const RootDataT::Tile& b) {
				return a.key < b.key;
			});
		}

		// For the world bounding box, we compute a bounding box containing GVDB's bounding box
		// after transformation by GVDB's matrix.
		if (indexAABB.min() == indexAABB.max()) {
			gprintf("Warning from ExportToNanoVDB: Bounding box had zero volume!\n");
		}

		BBox<Vec3R> worldAABB(
			Vec3R( FLT_MAX, FLT_MAX, FLT_MAX ), // Initial AABB min
			Vec3R( -FLT_MAX, -FLT_MAX, -FLT_MAX ) // Initial AABB max
		);

		// Cast indexAABB to double-precision
		BBox<Vec3R> indexAABBReal;
		for (int minMax = 0; minMax < 2; minMax++) {
			for (int c = 0; c < 3; c++) {
				indexAABBReal[minMax][c] = static_cast<double>(indexAABB[minMax][c]);
			}
		}

		for (int choiceFlags = 0; choiceFlags < 8; choiceFlags++) {
			nanovdb::Vec3R vertex;
			vertex[0] = indexAABB[(choiceFlags & 1)][0];
			vertex[1] = indexAABB[(choiceFlags & 2) >> 1][1];
			vertex[2] = indexAABB[(choiceFlags & 4) >> 2][2];
			vertex = gridData->mMap.applyMap(vertex);
			worldAABB.expand(vertex);
		}

		gridData->mWorldBBox = worldAABB;

#ifndef GVDB_EXPORT_NANOVDB_SKIP_COORD_TEST
		// Some tests for the implementation of CoordToKey, randomly generated
		// This is to watch out for incompatibilities in CoordToKey, since we don't do partial
		// specialization for it:
		uint64_t testKey = RootData<InternalNode<InternalNode<LeafNode<float, Coord, Mask, 3>, 4>, 5>>
			::CoordToKey({ 438603478, 101217144, 861900436 });
		assert(testKey == ((861900436ULL >> 12) | ((101217144ULL >> 12) << 21) | ((438603478ULL >> 12) << 42)));
		testKey = RootData<InternalNode<InternalNode<LeafNode<float, Coord, Mask, 7>, 4>, 6>>
			::CoordToKey({ 35463336, 183524282, 84996283 });
		assert(testKey == ((84996283ULL >> 17) | ((183524282ULL >> 17) << 21) | ((35463336ULL >> 17) << 42)));
#endif
	}

	CUdeviceptr ExportToNanoVDB(VolumeGVDB& gvdb, uchar channel, void* backgroundPtr,
		const char gridName[nanovdb::GridData::MaxNameSize], nanovdb::GridClass gridClass, size_t* outTotalSize)
	{
		// Validate input
		if (backgroundPtr == nullptr) {
			gprintf("Error in ExportToNanoVDB: backgroundPtr was nullptr!\n");
			return GVDB_EXPORT_NANOVDB_NULL;
		}
		if (outTotalSize == nullptr) {
			gprintf("Error in ExportToNanoVDB: outTotalSize was nullptr!\n");
			return GVDB_EXPORT_NANOVDB_NULL;
		}

		// This function works by splitting its work between the GPU and CPU. While the GPU exports
		// leaves and internal nodes, the CPU fills in the grid data. The CPU then receives the
		// level-2 nodes, sorts them, and then copies its data to the GPU. However, note that the
		// GPU is fully capable of doing this work on its own, using e.g. Thrust's parallel sorting
		// algorithms.

		// In order for this function to be efficient, we output a NanoVDB tree whose lower levels
		// match the GVDB tree. In NanoVDB, these are different templated types, so how do we
		// handle all of the possibilities (since we have to generate all our code in advance)?
		// Well, a NanoVDB volume is essentially a single block of memory, storing data and offsets
		// into this memory. In memory, it can be viewed like this:
		//   GridData
		//   number of GridBlindMetaData objects (contains offsets to contents)
		//   TreeData (contains pointers to root, internal nodes, and leaves)
		//   root
		//     (number of level-2 nodes) Tiles
		//   level-2 nodes
		//   level-1 nodes
		//   leaves
		//   contents of GridBlindMetaData
		//
		// If we're careful, we can break down writing each of these sections into handling a
		// relatively small number of types, and instantiate all of the functions we need.
		// Also, in this function, we ignore GridBlindMetaData.
		//
		// To sum this all up, this function works like this:
		// - Compute region sizes.
		// - Allocate memory.
		// - Start exporting leaves and nodes on the GPU.
		// - On the CPU, fill in most of the grid, tree, and root structures.
		// - Wait for the GPu to finish, and retrieve the level-2 nodes from the GPU. Sort them,
		// then populate the remaining root and GridData fields.
		// - Copy the grid, tree, root, and tiles to the GPU. 
		// - Clean up.

		ValueUnion backgroundUnion = *reinterpret_cast<ValueUnion*>(backgroundPtr);

		// Get template parameters from GVDB
		int brickLog2Dim = gvdb.getLD(0);
		int node1Log2Dim = gvdb.getLD(1);
		int node2Log2Dim = gvdb.getLD(2);
		uchar gvdbType = gvdb.GetChannelType(channel);
		// Make sure the node log dimensions are in the range [1,8], to keep the number of types small
		if (brickLog2Dim < 1 || brickLog2Dim > 8) {
			gprintf("Error in ExportToNanoVDB: The brick log2dim (%d) was outside of the range [1,8]. "
				"Consider using a different tree structure or adding this case to the supported types.\n",
				brickLog2Dim);
			return GVDB_EXPORT_NANOVDB_NULL;
		}
		if (node1Log2Dim < 1 || node1Log2Dim > 8) {
			gprintf("Error in ExportToNanoVDB: The level-1 node log2dim (%d) was outside of the range [1,8]. "
				"Consider using a different tree structure or adding this case to the supported types.\n",
				node1Log2Dim);
			return GVDB_EXPORT_NANOVDB_NULL;
		}
		if (node2Log2Dim < 1 || node2Log2Dim > 8) {
			gprintf("Error in ExportToNanoVDB: The level-2 node log2dim (%d) was outside of the range [1,8]. "
				"Consider using a different tree structure or adding this case to the supported types.\n",
				node2Log2Dim);
			return GVDB_EXPORT_NANOVDB_NULL;
		}
		// Make sure that the gvdbType is a type that can be converted to a NanoVDB volume.
		switch (gvdbType) {
		case T_FLOAT:
		case T_FLOAT3:
		case T_INT:
			break;
		default:
			gprintf("Error in ExportToNanoVDB:  The type of GVDB channel %u was %u, which is not "
				"supported for NanoVDB export.",
				static_cast<unsigned int>(channel), static_cast<unsigned int>(gvdbType));
			return GVDB_EXPORT_NANOVDB_NULL;
		}

		// Denotes the different regions of a NanoVDB file/memory representation.
		enum Region {
			R_GRID,
			R_TREE,
			R_ROOT,
			R_NODE2,
			R_NODE1,
			R_LEAF,
			R_COUNT
		};

		// Count the number of nodes at each level. At the moment, limit the number of nodes of each
		// type to INT_MAX (2^31-1).
		int numNode2s, numNode1s, numLeaves;
		if (!ExportToNanoVDB_GetNumNodes(gvdb, 2, numNode2s)) {
			return GVDB_EXPORT_NANOVDB_NULL;
		}
		if (!ExportToNanoVDB_GetNumNodes(gvdb, 1, numNode1s)) {
			return GVDB_EXPORT_NANOVDB_NULL;
		}
		if (!ExportToNanoVDB_GetNumNodes(gvdb, 0, numLeaves)) {
			return GVDB_EXPORT_NANOVDB_NULL;
		}

		// Compute the size of each region.
		NanoVDBTypeSizes typeSizes = ComputeTypeSizes(gvdbType, brickLog2Dim, node1Log2Dim, node2Log2Dim);
		size_t dataSizes[R_COUNT];
		dataSizes[R_GRID] = typeSizes.grid;
		dataSizes[R_TREE] = typeSizes.tree;
		dataSizes[R_ROOT] = typeSizes.root + typeSizes.rootTile * numNode2s;
		dataSizes[R_NODE2] = numNode2s * typeSizes.node2;
		dataSizes[R_NODE1] = numNode1s * typeSizes.node1;
		dataSizes[R_LEAF] = numLeaves * typeSizes.leaf;

		// Compute offsets into memory using an exclusive prefix sum; the last element in this array
		// will hold the size of all of the memory we need to allocate.
		// (e.g. this turns {3, 5, 2, 5} into {0, 3, 8, 10, 15}.)
		size_t dataOffsetsBytes[R_COUNT + 1];
		dataOffsetsBytes[0] = 0;
		for (int i = 1; i <= R_COUNT; i++) {
			dataOffsetsBytes[i] = dataOffsetsBytes[i - 1] + dataSizes[i - 1];
		}

		// Switch to GVDB's context
		const CUcontext gvdbContext = gvdb.getContext();
		cudaCheck(cuCtxPushCurrent(gvdbContext), "", "nvdb", "ExportToNanoVDB", "gvdbContext", DEBUG_EXPORT_NANOVDB);

		// Allocate the memory on the CPU and GPU!
		// The GPU will need space for the entire NanoVDB volume, while the CPU will only need
		// space to store up to and including the level-2 nodes.
		CUdeviceptr bufferGPU;
		cudaCheck(cuMemAlloc(&bufferGPU, dataOffsetsBytes[R_COUNT]), "nvdb", "ExportToNanoVDB", "cuMemAlloc", "bufferGPU", DEBUG_EXPORT_NANOVDB);
		cudaCheck(cuMemsetD8Async(bufferGPU, 0, dataOffsetsBytes[R_COUNT], 0), "nvdb", "ExportToNanoVDB", "cuMemsetD8Async", "bufferGPU", DEBUG_EXPORT_NANOVDB);

		uint8_t* bufferCPU = new uint8_t[dataOffsetsBytes[R_NODE1]]; // i.e. up to but not including R_NODE1
		memset(bufferCPU, 0, dataOffsetsBytes[R_NODE2]); // Zero out the data up to but not including R_NODE2 for reproducibility

		// We now fill in the data in this block of memory manually.
		// In order to compute bounding boxes and min/max values (which exist in NanoVDB, but aren't
		// stored in GVDB), we work from the leaf nodes up to the level-2 nodes.
		// We start by launching all of the GPU work, then continue working on the CPU while the GPU
		// processes nodes. (Note that this CPU work could also be performed on the GPU.)

		//---------------------------------------------------------------------------------------------
		// Leaves (GVDB bricks)

		// Device function signature: gvdbToNanoVDBProcessLeaves(
		//   VDBInfo* gvdb, void* nanoVDBLeafNodes, uchar channel, int numLeaves)
		gvdb.PrepareVDB(); // Is this necessary?
		VDBInfo* vdbInfo = reinterpret_cast<VDBInfo*>(gvdb.getVDBInfo());
		CUdeviceptr cuVDBInfo = gvdb.getCUVDBInfo();
		CUdeviceptr cuLeafNodesStart = bufferGPU + dataOffsetsBytes[R_LEAF];
		int typeTableIndex = TypeTableIndex(gvdbType);
		{
			LoadFunction(cuFuncProcessLeaves, "gvdbToNanoVDBProcessLeaves");
			// We use a linear launch here. There are no requirements at the moment on block size.
			const uint blockSize = 32;
			const uint numBlocks = (numLeaves + blockSize - 1) / blockSize;
			CUsurfObject atlas = vdbInfo->volOut[channel];
			void* args[5] = { &cuVDBInfo, &cuLeafNodesStart, &typeTableIndex, &atlas, &numLeaves };

			cudaCheck(cuLaunchKernel(cuFuncProcessLeaves, // Function
				numBlocks, 1, 1, // Grid
				blockSize, 1, 1, // Block dimensions
				0, 0, args, nullptr), "nvdb", "ExportToNanoVDB", "cuLaunchKernel", "cuFuncProcessLeaves", DEBUG_EXPORT_NANOVDB);
		}

		//---------------------------------------------------------------------------------------------
		// Level-1 nodes
		LoadFunction(cuFuncProcessInternalNodes, "gvdbToNanoVDBProcessInternalNodes");

		// Device function signature: gvdbToNanoVDBProcessInternalNodes(
		//  VDBInfo * gvdb,
		//  uint8_t* nanoVDBNodes, uint8_t* nanoVDBChildNodes,
		//  int numNodes, int level, int nodeLog2Dim, int childLog2Dim,
		//  ValueUnion backgroundUnion, int typeTableIndex)
		CUdeviceptr cuLevel1Nodes = bufferGPU + dataOffsetsBytes[R_NODE1];
		{
			// Linear launch, no requirements on block size.
			const uint blockSize = 32;
			const uint numBlocks = (numNode1s + blockSize - 1) / blockSize;
			int level = 1;
			int nodeLog2Dim = gvdb.getLD(level);
			int childLog2Dim = gvdb.getLD(level - 1);
			void* args[9] = { &cuVDBInfo,
				&cuLevel1Nodes, &cuLeafNodesStart,
				&numNode1s, &level, &nodeLog2Dim, &childLog2Dim,
				&backgroundUnion, &typeTableIndex};
			cudaCheck(cuLaunchKernel(cuFuncProcessInternalNodes, // Function
				numBlocks, 1, 1, // Grid
				blockSize, 1, 1, // Block dimensions
				0, 0, args, nullptr), "nvdb", "ExportToNanoVDB", "cuLaunchKernel", "cuFuncProcessInternalNodes, 1", DEBUG_EXPORT_NANOVDB);
		}

		//---------------------------------------------------------------------------------------------
		// Level-2 nodes
		// Same device function as above, but called with different arguments
		CUdeviceptr cuLevel2Nodes = bufferGPU + dataOffsetsBytes[R_NODE2];
		{
			// Linear launch, no requirements on block size.
			const uint blockSize = 32;
			const uint numBlocks = (numNode2s + blockSize - 1) / blockSize;
			int level = 2;
			int nodeLog2Dim = gvdb.getLD(level);
			int childLog2Dim = gvdb.getLD(level - 1);
			void* args[9] = { &cuVDBInfo,
				&cuLevel2Nodes, &cuLevel1Nodes,
				&numNode2s, &level, &nodeLog2Dim, &childLog2Dim,
				&backgroundUnion, &typeTableIndex };
			cudaCheck(cuLaunchKernel(cuFuncProcessInternalNodes, // Function
				numBlocks, 1, 1, // Grid
				blockSize, 1, 1, // Block dimensions
				0, 0, args, nullptr), "nvdb", "ExportToNanoVDB", "cuLaunchKernel", "cuFuncProcessInternalNodes, 2", DEBUG_EXPORT_NANOVDB);
		}

		//---------------------------------------------------------------------------------------------
		// Grid (CPU)
		nanovdb::GridData* gridData = reinterpret_cast<nanovdb::GridData*>(bufferCPU);
		{
			gridData->mMagic = NANOVDB_MAGIC_NUMBER;

			memcpy(gridData->mGridName, gridName, nanovdb::GridData::MaxNameSize);

			// Get the GVDB index-to-world transform and copy it to a format Map can read
			Matrix4F xform = gvdb.getTransform(); // Make a copy (note that once this is integrated
			// into the main library, we can access the inverse directly):
			{
				float indexToWorld[4][4];
				for (int row = 0; row < 4; row++) {
					for (int col = 0; col < 4; col++) {
						indexToWorld[row][col] = xform(row, col);
					}
				}
				float worldToIndex[4][4];
				xform.InvertTRS();
				for (int row = 0; row < 4; row++) {
					for (int col = 0; col < 4; col++) {
						worldToIndex[row][col] = xform(row, col);
					}
				}
				gridData->mMap.set(indexToWorld, worldToIndex, 1.0); // mTaper seems to be unused
			}

			// Skip over the world bounding box for now - we'll fill it in later.

			// GridData would like a uniform scale, but that's not really possible to provide, since
			// GVDB supports arbitrary voxel transforms (e.g. think of skewed voxels).
			// For now, we use the approach GridBuilder uses, which is scale_i = ||map(e_i) - map((0,0,0))||.
			// However, for a different approximation, we could use something like sqrt(tr(A*A)/3),
			// where A is the upper-left 3x3 block of xform; if A is normal, this gives the root mean
			// square of the singular values of A.
			const nanovdb::Vec3d mapAt0 = gridData->applyMap(nanovdb::Vec3d(0, 0, 0));
			gridData->mVoxelSize = Vec3R(
				(gridData->applyMap(nanovdb::Vec3d(1, 0, 0)) - mapAt0).length(),
				(gridData->applyMap(nanovdb::Vec3d(0, 1, 0)) - mapAt0).length(),
				(gridData->applyMap(nanovdb::Vec3d(0, 0, 1)) - mapAt0).length()
			);

			gridData->mGridClass = gridClass;

			switch (gvdbType) {
			case T_FLOAT:
				gridData->mGridType = nanovdb::GridType::Float;
				break;
			case T_FLOAT3:
				gridData->mGridType = nanovdb::GridType::Vec3f;
				break;
			case T_INT:
				gridData->mGridType = nanovdb::GridType::Int32;
			}

			gridData->mBlindMetadataCount = 0;
			gridData->mBlindMetadataOffset = 0;
		}
		assert(sizeof(nanovdb::GridData) == dataSizes[R_GRID]); // Consistency check

		//---------------------------------------------------------------------------------------------
		// Tree (CPU)
		using TreeDataT = TreeData<TREE_DEPTH>; // The root is always at level 3 in NanoVDB
		TreeDataT* treeData = reinterpret_cast<TreeDataT*>(bufferCPU + dataOffsetsBytes[R_TREE]);
		{
			// Filling in the tree is much simpler; we simply give the offsets from treePtr to each of
			// the regions, and the number of nodes in each region. Note that the indices of mBytes
			// and mCount refer to the level of the nodes.
			treeData->mBytes[0] = dataOffsetsBytes[R_LEAF] - dataOffsetsBytes[R_TREE];
			treeData->mBytes[1] = dataOffsetsBytes[R_NODE1] - dataOffsetsBytes[R_TREE];
			treeData->mBytes[2] = dataOffsetsBytes[R_NODE2] - dataOffsetsBytes[R_TREE];
			treeData->mBytes[3] = dataOffsetsBytes[R_ROOT] - dataOffsetsBytes[R_TREE];

			treeData->mCount[0] = numLeaves;
			treeData->mCount[1] = numNode1s;
			treeData->mCount[2] = numNode2s;
			treeData->mCount[3] = 1; // There's only one root
		}

		// Now, wait for the GPU to finish by issuing a synchronizing operation to copy its level-2
		// nodes to the CPU:
		cudaCheck(cuMemcpyDtoH(bufferCPU + dataOffsetsBytes[R_NODE2], // CPU pointer
			bufferGPU + dataOffsetsBytes[R_NODE2], // GPU pointer
			dataSizes[R_NODE2]), // Data size
			"nvdb", "ExportToNanoVDB", "cuMemcpyDtoH", "Level-2 Nodes", DEBUG_EXPORT_NANOVDB);

		//---------------------------------------------------------------------------------------------
		// Root and grid extents
		// This computes the bounding box and min and max values of the grid from the level-2 nodes.
		// It also computes the grid's world-space AABB.
		uint8_t* rootDataPtr = bufferCPU + dataOffsetsBytes[R_ROOT];
		{
			uint8_t* node2Start = bufferCPU + dataOffsetsBytes[R_NODE2];

			// All voxels in the leaves of the GVDB volume are active, so this is the total volume of
			// of the leaves:
			uint64_t activeVoxelCount = numLeaves * gvdb.getVoxCnt(0);
			const int totalLog2Dim = node2Log2Dim + node1Log2Dim + brickLog2Dim;

			switch (gvdbType) {
			case T_FLOAT:
				ProcessGridExtents<float>(gridData, rootDataPtr, node2Start,
					activeVoxelCount, backgroundPtr, numNode2s, node2Log2Dim, totalLog2Dim);
				break;
			case T_FLOAT3:
				ProcessGridExtents<Vec3f>(gridData, rootDataPtr, node2Start,
					activeVoxelCount, backgroundPtr, numNode2s, node2Log2Dim, totalLog2Dim);
				break;
			case T_INT:
				ProcessGridExtents<int>(gridData, rootDataPtr, node2Start,
					activeVoxelCount, backgroundPtr, numNode2s, node2Log2Dim, totalLog2Dim);
				break;
			}
		}

		// Finally, copy the updated data - i.e. grid, tree, and root, no level-2 nodes! - back to
		// the GPU.
		cuMemcpyHtoD(bufferGPU, bufferCPU, dataOffsetsBytes[R_NODE2]); // i.e. up to but not including level-2 nodes

		// Pop the context and return.
		CUcontext pctx;
		cudaCheck(cuCtxPopCurrent(&pctx), "", "main", "cuCtxPopCurrent", "tempContext", DEBUG_EXPORT_NANOVDB);

		*outTotalSize = dataOffsetsBytes[R_COUNT];
		return bufferGPU;
	}

	void RenderNanoVDB(CUcontext context, CUdeviceptr nanoVDB, Camera3D* camera,
		uint width, uint height, uchar* outImage)
	{
		// Switch to GVDB's CUDA context
		cuCtxPushCurrent(context);

		nvdb::LoadFunction(cuFuncRender, "gvdbExportNanoVDBRender");

		// Allocate space for the image
		const size_t imageSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4 * sizeof(char);
		CUdeviceptr deviceImage;
		cudaCheck(cuMemAlloc(&deviceImage, imageSize),
			"nvdb", "RenderNanoVDB", "cuMemAlloc", "", DEBUG_EXPORT_NANOVDB);

		// Partition the image into blocks of size 8x8.
		const uint blockSize = 8;
		const uint gridX = (width + blockSize - 1) / blockSize;
		const uint gridY = (width + blockSize - 1) / blockSize;

		// Get the camera origin and directions in world-space.
		// Camera origin in world-space
		nanovdb::Vec3f eye(camera->from_pos.x, camera->from_pos.y, camera->from_pos.z);
		// Camera directions in world-space
		Vector4DF camTopLeftWS = camera->tlRayWorld;
		Vector4DF camRightWS = camera->trRayWorld - camera->tlRayWorld;
		Vector4DF camDownWS = camera->blRayWorld - camera->tlRayWorld;

		static_assert(sizeof(eye) == 3 * sizeof(float), "Vec3f must be packed!");
		static_assert(sizeof(Vector4DF) == 4 * sizeof(float), "Vector4DF must be packed!");

		// Launch the render.
		// The function signature is
		// (ptr to NanoVDB grid, Vec3f eye, Vector4DF topLeftWS, rightWS, downWS,
		//  ptr to image, uint width, height).
		void* args[8] = { &nanoVDB, &eye, &camTopLeftWS, &camRightWS, &camDownWS,
			&deviceImage, &width, &height };
		cudaCheck(cuLaunchKernel(
			cuFuncRender, // Function to call
			gridX, gridY, 1, // Grid size
			blockSize, blockSize, 1, // Block size
			0, // Shared memory in bytes
			NULL, // Default stream
			args, // Kernel parameters
			NULL // Extra options
		), "nvdb", "RenderNanoVDB", "cuLaunchKernel", "", DEBUG_EXPORT_NANOVDB);

		// Copy the data back to the CPU
		cudaCheck(cuMemcpyDtoH(outImage, deviceImage, imageSize),
			"nvdb", "RenderNanoVDB", "cuMemcpyDtoH", "", DEBUG_EXPORT_NANOVDB);

		// Free temporary buffer
		cudaCheck(cuMemFree(deviceImage), "nvdb", "RenderNanoVDB", "cuMemFree", "deviceImage", DEBUG_EXPORT_NANOVDB);

		CUcontext pctx;
		cuCtxPopCurrent(&pctx);
	}
}