//
// Written by Nick Gildea (2017)
// Public Domain
//

#include "fast_dc.h"

#include "ng_mesh_simplify.h"
#include "qef_simd.h"

#include "glm/glm.hpp"
#include <stdint.h>
#include "density.h"

// ----------------------------------------------------------------------------

// TODO the winding field could be packed into the normal vec as the unused w component
struct EdgeInfo
{
	vec4 pos;
	vec4 normal;
	bool winding = false;
};

// Ideally we'd use https://github.com/greg7mdp/sparsepp but fall back to STL
#ifdef HAVE_SPARSEPP

#include <sparsepp/spp.h>

using EdgeInfoMap = spp::sparese_hash_map<uint32_t, EdgeInfo>;
using VoxelIDSet = spp::sparse_hash_set<uint32_t>;
using VoxelIndexMap = spp::sparese_hash_map<uint32_t, int>;

#else

#include <unordered_map>
#include <unordered_set>

using EdgeInfoMap = std::unordered_map<uint32_t, EdgeInfo>;
using VoxelIDSet = std::unordered_set<uint32_t>;
using VoxelIndexMap = std::unordered_map<uint32_t, int>;

#endif

// ----------------------------------------------------------------------------

using glm::ivec4;
using glm::vec4;
using glm::vec3;
using glm::vec2;

#ifdef _MSC_VER
#define ALIGN16 __declspec(align(16))
#else
#define ALIGN16 __attribute__((aligned(16))
#endif

// ----------------------------------------------------------------------------

//const int VOXEL_GRID_SIZE = 128;
//const float VOXEL_GRID_OFFSET = (float)VOXEL_GRID_SIZE / 2.f;

// ----------------------------------------------------------------------------

static const vec4 AXIS_OFFSET[3] =
{
	vec4(1.f, 0.f, 0.f, 0.f),
	vec4(0.f, 1.f, 0.f, 0.f),
	vec4(0.f, 0.f, 1.f, 0.f)
};

// ----------------------------------------------------------------------------

static const ivec4 EDGE_NODE_OFFSETS[3][4] =
{
	{ ivec4(0), ivec4(0, 0, 1, 0), ivec4(0, 1, 0, 0), ivec4(0, 1, 1, 0) },
	{ ivec4(0), ivec4(1, 0, 0, 0), ivec4(0, 0, 1, 0), ivec4(1, 0, 1, 0) },
	{ ivec4(0), ivec4(0, 1, 0, 0), ivec4(1, 0, 0, 0), ivec4(1, 1, 0, 0) },
};

// ----------------------------------------------------------------------------

// The two lookup tables below were calculated by expanding the IDs into 3d coordinates
// performing the calcuations in 3d space and then converting back into the compact form 
// and subtracting the base voxel ID. Use of this lookup table means those calculations 
// can be avoided at run-time.

const uint32_t ENCODED_EDGE_NODE_OFFSETS[12] =
{
	0x00000000,
	0x00100000,
	0x00000400,
	0x00100400,
	0x00000000,
	0x00000001,
	0x00100000,
	0x00100001,
	0x00000000,
	0x00000400,
	0x00000001,
	0x00000401,
};

const uint32_t ENCODED_EDGE_OFFSETS[12] =
{
	0x00000000,
	0x00100000,
	0x00000400,
	0x00100400,
	0x40000000,
	0x40100000,
	0x40000001,
	0x40100001,
	0x80000000,
	0x80000400,
	0x80000001,
	0x80000401,
};

// ----------------------------------------------------------------------------

// The "super primitve" -- use the parameters to configure different shapes from a single function
// see https://www.shadertoy.com/view/MsVGWG

float sdSuperprim(vec3 p, vec4 s, vec2 r)
{
	const vec3 d = glm::abs(p) - vec3(s);

	float q = glm::length(vec2(glm::max(d.x + r.x, 0.f), glm::max(d.y + r.x, 0.f)));
	q += glm::min(-r.x, glm::max(d.x, d.y));
	q = (glm::abs((q + s.w)) - s.w);

	return glm::length(vec2(glm::max(q + r.y, 0.f),
		glm::max(d.z + r.y, 0.f))) + glm::min(-r.y, glm::max(q, d.z));
}

// ----------------------------------------------------------------------------

float Density(const vec4& p)
{
	return Density_Func(vec3(p));
}

// ----------------------------------------------------------------------------

uint32_t EncodeVoxelUniqueID(const ivec4& idxPos)
{
	return idxPos.x | (idxPos.y << 10) | (idxPos.z << 20);
}

// ----------------------------------------------------------------------------

ivec4 DecodeVoxelUniqueID(const uint32_t id)
{
	return ivec4(
		id & 0x3ff,
		(id >> 10) & 0x3ff,
		(id >> 20) & 0x3ff,
		0);
}

// ----------------------------------------------------------------------------

uint32_t EncodeAxisUniqueID(const int axis, const int x, const int y, const int z)
{
	return (x << 0) | (y << 10) | (z << 20) | (axis << 30);
}

// ----------------------------------------------------------------------------

float FindIntersection(const vec4& p0, const vec4& p1)
{
	const int FIND_EDGE_INFO_STEPS = 16;
	const float FIND_EDGE_INFO_INCREMENT = 1.f / FIND_EDGE_INFO_STEPS;

	float minValue = FLT_MAX;
	float currentT = 0.f;
	float t = 0.f;
	for (int i = 0; i < FIND_EDGE_INFO_STEPS; i++)
	{
		const vec4 p = glm::mix(p0, p1, currentT);
		const float	d = glm::abs(Density(p));
		if (d < minValue)
		{
			t = currentT;
			minValue = d;
		}

		currentT += FIND_EDGE_INFO_INCREMENT;
	}

	return t;
}

// ----------------------------------------------------------------------------

static void FindActiveVoxels(
	VoxelIDSet& activeVoxels,
	EdgeInfoMap& activeEdges,
	int worldX, int worldY, int worldZ, const int voxelGridSize, VertexData& cellData)
{
	printf("FindActiveVoxels ");
	const float gridOffset = voxelGridSize / 2.0f;

	for (int x = 0; x < voxelGridSize; x++)
		for (int y = 0; y < voxelGridSize; y++)
			for (int z = 0; z < voxelGridSize; z++)
			{
				printf("\nindex[%d,%d,%d] \n", x, y, z);
				const ivec4 idxPos(x, y, z, 0);
				const vec4 p = vec4(x - gridOffset + worldX, y - gridOffset + worldY, z - gridOffset + worldZ, 1.f);
				
				//printf("    pos[%f,%f,%f]\n", p.x, p.y, p.z);
				/*cellData.push_back(p.x);
				cellData.push_back(p.y);
				cellData.push_back(p.z);*/

				for (int axis = 0; axis < 3; axis++)
				{
					//printf(" axis[%d]", axis);
					const vec4 q = p + AXIS_OFFSET[axis]; //

					const float pDensity = Density(p);
					const float qDensity = Density(q);

					const bool zeroCrossing =
						pDensity >= 0.f && qDensity < 0.f ||
						pDensity < 0.f && qDensity >= 0.f;
					//printf(" - zeroCrossing[%d]", zeroCrossing);
					if (!zeroCrossing)
					{
						continue;
					}

					const float t = FindIntersection(p, q);
					const vec4 pos = vec4(glm::mix(glm::vec3(p), glm::vec3(q), t), 1.f);
					/*cellData.push_back(pos.x);
					cellData.push_back(pos.y);
					cellData.push_back(pos.z);*/

					const float H = 0.001f;
					const auto normal = glm::normalize(vec4(
						Density(pos + vec4(H, 0.f, 0.f, 0.f)) - Density(pos - vec4(H, 0.f, 0.f, 0.f)),
						Density(pos + vec4(0.f, H, 0.f, 0.f)) - Density(pos - vec4(0.f, H, 0.f, 0.f)),
						Density(pos + vec4(0.f, 0.f, H, 0.f)) - Density(pos - vec4(0.f, 0.f, H, 0.f)),
						0.f));

					EdgeInfo info;
					info.pos = pos;
					info.normal = normal;
					info.winding = pDensity >= 0.f;

					const auto code = EncodeAxisUniqueID(axis, x, y, z);
					activeEdges[code] = info;

					const auto edgeNodes = EDGE_NODE_OFFSETS[axis];
					for (int i = 0; i < 4; i++)
					{
						const auto nodeIdxPos = idxPos - edgeNodes[i];
						const auto nodeID = EncodeVoxelUniqueID(nodeIdxPos);
						
						printf("\n------- \nedge offset[%d,%d,%d]", (int)edgeNodes[i].x, (int)edgeNodes[i].y, (int)edgeNodes[i].z);
						printf("idxPos[%d,%d,%d]", (int)idxPos.x, (int)idxPos.y, (int)idxPos.z);
						printf("nodeIdxPos[%d, %d, %d]\n", (int)nodeIdxPos.x, (int)nodeIdxPos.y, (int)nodeIdxPos.z);
						printf(" - vuID[%d]\n", nodeID);
						vec4 decoded = DecodeVoxelUniqueID(nodeID);
						printf("decoded p [%d, %d, %d]\n", (int)decoded.x, (int)decoded.y, (int)decoded.z);

						if (nodeIdxPos.x < 0 || nodeIdxPos.y < 0 || nodeIdxPos.z < 0) {
							printf("outside bounds, ignoring....\n");
							continue;
						}
						activeVoxels.insert(nodeID);
					}
					printf("\n");
				}
			}
	printf("... DONE\n");
}

// ----------------------------------------------------------------------------

static void GenerateVertexData(
	const VoxelIDSet& voxels,
	const EdgeInfoMap& edges,
	VoxelIndexMap& vertexIndices,
	MeshBuffer* buffer,
	float& debugVal, VertexData& cellData)
{
	printf("GenerateVertexData");
	MeshVertex* vert = &buffer->vertices[0];

	int idxCounter = 0;
	for (const auto& voxelID : voxels)
	{
		ALIGN16 vec4 p[12];
		ALIGN16 vec4 n[12];

		int idx = 0;
		for (int i = 0; i < 12; i++)
		{
			const auto edgeID = voxelID + ENCODED_EDGE_OFFSETS[i];
			const auto iter = edges.find(edgeID);

			if (iter != end(edges))
			{
				const auto& info = iter->second;
				const vec4 pos = info.pos;
				const vec4 normal = info.normal;

				p[idx] = pos;
				n[idx] = normal;
				idx++;
			}
		}

		ALIGN16 vec4 nodePos;
		qef_solve_from_points_4d(&p[0].x, &n[0].x, idx, &nodePos.x);
		vec4 vid = DecodeVoxelUniqueID(voxelID);
		printf("\n - QEF Solved Pos for cell[%d, %d, %d] - [%f, %f, %f] idx: %d", (int)vid.x, (int)vid.y, (int)vid.z, nodePos.x, nodePos.y, nodePos.z, idx);
		cellData.push_back(nodePos.x);
		cellData.push_back(nodePos.y);
		cellData.push_back(nodePos.z);
		vec4 nodeNormal;
		for (int i = 0; i < idx; i++)
		{
			nodeNormal += n[i];
		}
		if (idx != 0) {
			nodeNormal *= (1.f / (float)idx); //this where we are getting normal set to infinity?  
		}

		if (idx > 1) {
			vertexIndices[voxelID] = idxCounter++;

			buffer->numVertices++;
			vert->xyz = nodePos;

			//should we snap position so it's never outside this voxel?
			//I guess?


			vert->normal = nodeNormal;
			vert++;
		}
	}
	printf("... DONE\n");

}

// ----------------------------------------------------------------------------

static void GenerateTriangles(
	const EdgeInfoMap& edges,
	const VoxelIndexMap& vertexIndices,
	MeshBuffer* buffer)
{
	printf("GenerateTriangles");
	MeshTriangle* tri = &buffer->triangles[0];

	for (const auto& pair : edges)
	{
		const auto& edge = pair.first;
		const auto& info = pair.second;

		const ivec4 basePos = DecodeVoxelUniqueID(edge);
		printf("voxelId[%d,%d,%d]\n", (int)basePos.x, (int)basePos.y, (int)basePos.z);
		const int axis = (edge >> 30) & 0xff;

		const int nodeID = edge & ~0xc0000000;
		const uint32_t voxelIDs[4] =
		{
			nodeID - ENCODED_EDGE_NODE_OFFSETS[axis * 4 + 0],
			nodeID - ENCODED_EDGE_NODE_OFFSETS[axis * 4 + 1],
			nodeID - ENCODED_EDGE_NODE_OFFSETS[axis * 4 + 2],
			nodeID - ENCODED_EDGE_NODE_OFFSETS[axis * 4 + 3],
		};

		// attempt to find the 4 voxels which share this edge
		int edgeVoxels[4];
		int numFoundVoxels = 0;
		for (int i = 0; i < 4; i++)
		{
			const auto iter = vertexIndices.find(voxelIDs[i]);
			if (iter != end(vertexIndices))
			{
				edgeVoxels[numFoundVoxels++] = iter->second;
			}
		}

		// we can only generate a quad (or two triangles) if all 4 are found
		if (numFoundVoxels < 4)
		{
			continue;
		}

		if (info.winding)
		{
			tri->indices_[0] = edgeVoxels[0];
			tri->indices_[1] = edgeVoxels[1];
			tri->indices_[2] = edgeVoxels[3];
			tri++;

			tri->indices_[0] = edgeVoxels[0];
			tri->indices_[1] = edgeVoxels[3];
			tri->indices_[2] = edgeVoxels[2];
			tri++;
		}
		else
		{
			tri->indices_[0] = edgeVoxels[0];
			tri->indices_[1] = edgeVoxels[3];
			tri->indices_[2] = edgeVoxels[1];
			tri++;

			tri->indices_[0] = edgeVoxels[0];
			tri->indices_[1] = edgeVoxels[2];
			tri->indices_[2] = edgeVoxels[3];
			tri++;
		}

		buffer->numTriangles += 2;
	}
	printf("..DONE\n");
}

// ----------------------------------------------------------------------------

MeshBuffer* GenerateMesh(int x, int y, int z, int cellSize, float& debugVal, VertexData& cellData)
{
	VoxelIDSet activeVoxels;
	EdgeInfoMap activeEdges;

	FindActiveVoxels(activeVoxels, activeEdges, x, y, z, cellSize, cellData);

	MeshBuffer* buffer = new MeshBuffer;
	buffer->vertices = (MeshVertex*)malloc(activeVoxels.size() * sizeof(MeshVertex));
	buffer->numVertices = 0;

	VoxelIndexMap vertexIndices;
	GenerateVertexData(activeVoxels, activeEdges, vertexIndices, buffer, debugVal, cellData);

	buffer->triangles = (MeshTriangle*)malloc(2 * activeEdges.size() * sizeof(MeshTriangle));
	buffer->numTriangles = 0;
	GenerateTriangles(activeEdges, vertexIndices, buffer);

	printf("mesh verts/tris: %d %d\n", buffer->numVertices, buffer->numTriangles);

	return buffer;
}

// ----------------------------------------------------------------------------

SuperPrimitiveConfig ConfigForShape(const SuperPrimitiveConfig::Type& type)
{
	SuperPrimitiveConfig config;
	switch (type)
	{
	default:
	case SuperPrimitiveConfig::Cube:
		config.s = vec4(1.f);
		config.r = vec2(0.f);
		break;

	case SuperPrimitiveConfig::Cylinder:
		config.s = vec4(1.f);
		config.r = vec2(1.f, 0.f);
		break;

	case SuperPrimitiveConfig::Pill:
		config.s = vec4(1.f, 1.f, 2.f, 1.);
		config.r = vec2(1.f);
		break;

	case SuperPrimitiveConfig::Corridor:
		config.s = vec4(1.f, 1.f, 1.f, 0.25f);
		config.r = vec2(0.1f);
		break;

	case SuperPrimitiveConfig::Torus:
		config.s = vec4(1.f, 1.f, 0.25f, 0.25f);
		config.r = vec2(1.f, 0.25f);
		break;
	}

	return config;
}

// ----------------------------------------------------------------------------