#pragma once

#include "builder/qbvh6.h"
#include "builder/gpu/AABB3f.h"
#include "builder/gpu/sort.h"
#include "builder/gpu/morton.h"
#include "builder/gpu/lcgbp.h"
#include <memory>
#include "../common/scene.h" 
#include "rthwif_builder.h"


#if defined(EMBREE_SYCL_SUPPORT)

#define BVH_BRANCHING_FACTOR         6
#define FATLEAF_THRESHOLD            6
#define PAIR_OFFSET_SHIFT            28
#define GEOMID_MASK                  (((uint32_t)1<<PAIR_OFFSET_SHIFT)-1)
#define LARGE_WG_SIZE                1024
#define TRIANGLE_QUAD_BLOCK_SIZE     64
#define QBVH6_HEADER_OFFSET          128
#define HOST_DEVICE_COMM_BUFFER_SIZE 16*sizeof(uint32_t)
#define EQUAL_DISTANCES_WORKAROUND   1
#define REBALANCE_BVH2_MINIMUM_DEPTH 30

#if 1
#define TOP_LEVEL_RATIO              5.0f
#define BOTTOM_LEVEL_RATIO           5.0f
#else
#define TOP_LEVEL_RATIO              0.0f
#define BOTTOM_LEVEL_RATIO           0.0f
#endif

namespace embree
{  
  // ===================================================================================================================================================================================
  // =============================================================================== General ===========================================================================================
  // ===================================================================================================================================================================================
  
  struct __aligned(64) PLOCGlobals
  {
    // === first 64 bytes ===       
    gpu::AABB3f geometryBounds;
    gpu::AABB3f centroidBounds; 
    char *qbvh_base_pointer;
    uint32_t node_mem_allocator_start;
    uint32_t node_mem_allocator_cur;
    // === second 64 bytes ===    
    uint32_t numTriangles;
    uint32_t numQuads;
    uint32_t numMergedTrisQuads;
    uint32_t numProcedurals;
    uint32_t numInstances;
    uint32_t numQuadBlocks;
    uint32_t numLossyCompressedGeometries;    
    uint32_t bvh2_index_allocator;    
    uint32_t leaf_mem_allocator_start;
    uint32_t leaf_mem_allocator_cur;
    uint32_t range_start;
    uint32_t range_end;
    uint32_t sync;
    uint32_t rootIndex;
    uint32_t wgID;
    uint32_t numLeaves;

    __forceinline void reset()
    {
      geometryBounds.init();
      centroidBounds.init();
      qbvh_base_pointer = nullptr;
      numTriangles               = 0;
      numQuads                   = 0;
      numMergedTrisQuads         = 0;
      numProcedurals             = 0;
      numInstances               = 0;
      numQuadBlocks              = 0;
      numLossyCompressedGeometries = 0;
      node_mem_allocator_cur     = 0;
      node_mem_allocator_start   = 0;
      bvh2_index_allocator       = 0;
      leaf_mem_allocator_cur     = 0;
      leaf_mem_allocator_start   = 0;
      range_start                = 0;
      range_end                  = 0;
      sync                       = 0;      
      rootIndex                  = 0;
      wgID                       = 0;
      numLeaves                  = 0;
    }
      
    /* allocate data in the node memory section */
    __forceinline char* atomic_allocNode(const uint32_t bytes)
    {
      uint32_t blocks = (uint32_t)bytes / 64;
      const uint32_t current = gpu::atomic_add_global(&node_mem_allocator_cur,blocks);      
      char* ptr = qbvh_base_pointer + 64 * (size_t)current;
      return ptr;
    }

    /* allocate memory in the leaf memory section */
    __forceinline char* atomic_allocLeaf(const uint32_t bytes)
    {
      uint32_t blocks = (uint32_t)bytes / 64;      
      const uint32_t current = gpu::atomic_add_global(&leaf_mem_allocator_cur,blocks);            
      char* ptr = qbvh_base_pointer + 64 * (size_t)current;
      return ptr;
    }
      
    __forceinline char* basePtr() {
      return qbvh_base_pointer;
    }

    __forceinline char* nodeBlockPtr(const uint32_t blockID) {
      return qbvh_base_pointer + 64 * size_t(blockID);
    }

    __forceinline char* leafLocalPtr(const uint32_t localID=0) {
      return qbvh_base_pointer + 64 * size_t(leaf_mem_allocator_start + localID);
    }

    __forceinline uint32_t getBlockIDFromPtr(const char *addr) {
      return (addr - qbvh_base_pointer)/64;
    }
    
    __forceinline uint32_t atomic_add_sub_group_varying_allocNodeBlocks(const uint32_t blocks)
    {
      return gpu::atomic_add_global_sub_group_varying(&node_mem_allocator_cur,blocks);      
    }    
    
    __forceinline char* atomic_add_sub_group_varying_allocLeaf(const uint32_t bytes)
    {
      uint32_t blocks = (uint32_t)bytes / 64;      
      const uint32_t current = gpu::atomic_add_global_sub_group_varying(&leaf_mem_allocator_cur,blocks);            
      char* ptr = qbvh_base_pointer + 64 * (size_t)current;
      return ptr;
    }      
            
    __forceinline void resetGlobalCounters()
    {
      node_mem_allocator_cur = node_mem_allocator_start;
      leaf_mem_allocator_cur = leaf_mem_allocator_start;
    }
            
  };

  static_assert(sizeof(PLOCGlobals) == 128, "PLOCGlobals must be 128 bytes large");

  struct BVH2SubTreeState
  {
    static const uint32_t DEPTH_BITS = 7;
    static const uint32_t LEAVES_BITS = 32 - DEPTH_BITS - 1;

    uint32_t depth  : DEPTH_BITS;
    uint32_t leaves : LEAVES_BITS;
    uint32_t mark   : 1;
    
    static const uint32_t MAX_LEAVES = ((uint)1 << LEAVES_BITS)-1;
    static const uint32_t MAX_DEPTH  = ((uint)1 << DEPTH_BITS)-1;

    __forceinline BVH2SubTreeState() {}

    __forceinline BVH2SubTreeState(const uint32_t leaves, const uint32_t depth) : leaves(leaves), depth(depth) {}
    
    __forceinline BVH2SubTreeState(const BVH2SubTreeState &left, const BVH2SubTreeState &right)
    {
      leaves = sycl::min((uint)(left.leaves)+(uint)right.leaves,MAX_LEAVES);
      const uint32_t leftFatLeaf  = left.leaves  <= FATLEAF_THRESHOLD ? 1 : 0;
      const uint32_t rightFatLeaf = right.leaves <= FATLEAF_THRESHOLD ? 1 : 0;
      const uint32_t sum = leftFatLeaf + rightFatLeaf;
#if 1     
      if (sum == 0) depth = 0;
      else if (sum == 2) depth = 1;
      else depth = sycl::max(left.depth,right.depth)+1;
      
      if (sum == 0 && sycl::max(left.depth,right.depth) >= REBALANCE_BVH2_MINIMUM_DEPTH)
        mark = 1;
#else      
      depth = sycl::max(left.depth+(uint)1,right.depth+(uint)1);
#endif      
    }
    
    __forceinline bool isMarked() const
    {
      return mark == 1;
    }
    
  };
        
  struct __aligned(64) QBVHNodeN
  {
    float bounds_lower[3];
    int offset;

    struct {
      uint8_t type;
      uint8_t pad;
      char exp[3];
      uint8_t instMask;
      uint8_t childData[6];
    };

    struct {
      uint8_t lower_x[BVH_BRANCHING_FACTOR];
      uint8_t upper_x[BVH_BRANCHING_FACTOR];
      uint8_t lower_y[BVH_BRANCHING_FACTOR];
      uint8_t upper_y[BVH_BRANCHING_FACTOR];
      uint8_t lower_z[BVH_BRANCHING_FACTOR];
      uint8_t upper_z[BVH_BRANCHING_FACTOR];
    };

    __forceinline float3 start() const { return float3(bounds_lower[0],bounds_lower[1],bounds_lower[2]); }
    
    static __forceinline const gpu::AABB3f quantize_bounds(const float3 &start, const char exp_x, const char exp_y, const char exp_z, const gpu::AABB3f &fbounds) 
    {
      const float3 lower = fbounds.lower()-start;
      const float3 upper = fbounds.upper()-start;
      float qlower_x = ldexpf(lower.x(), -exp_x + 8); 
      float qlower_y = ldexpf(lower.y(), -exp_y + 8); 
      float qlower_z = ldexpf(lower.z(), -exp_z + 8); 
      float qupper_x = ldexpf(upper.x(), -exp_x + 8); 
      float qupper_y = ldexpf(upper.y(), -exp_y + 8); 
      float qupper_z = ldexpf(upper.z(), -exp_z + 8); 
      assert(qlower_x >= 0.0f && qlower_x <= 255.0f);
      assert(qlower_y >= 0.0f && qlower_y <= 255.0f);
      assert(qlower_z >= 0.0f && qlower_z <= 255.0f);
      assert(qupper_x >= 0.0f && qupper_x <= 255.0f);
      assert(qupper_y >= 0.0f && qupper_y <= 255.0f);
      assert(qupper_z >= 0.0f && qupper_z <= 255.0f); 
      qlower_x = min(max(sycl::floor(qlower_x),0.0f),255.0f);
      qlower_y = min(max(sycl::floor(qlower_y),0.0f),255.0f);
      qlower_z = min(max(sycl::floor(qlower_z),0.0f),255.0f);
      qupper_x = min(max(sycl::ceil(qupper_x),0.0f),255.0f);
      qupper_y = min(max(sycl::ceil(qupper_y),0.0f),255.0f);
      qupper_z = min(max(sycl::ceil(qupper_z),0.0f),255.0f);
      gpu::AABB3f qbounds(float3(qlower_x, qlower_y, qlower_z), float3(qupper_x, qupper_y, qupper_z));
      return qbounds;
    }    
  };

  struct __aligned(64) QuadLeafData {
    unsigned int shaderIndex;   
    unsigned int geomIndex;     
    unsigned int primIndex0;
    unsigned int primIndex1Delta;
    float v[4][3];

    __forceinline QuadLeafData() {}

    __forceinline QuadLeafData(const Vec3f &v0, const Vec3f &v1, const Vec3f &v2, const Vec3f &v3,
                               const uint32_t j0, const uint32_t j1, const uint32_t j2,
                               const uint32_t shaderID, const uint32_t geomID, const uint32_t primID0, const uint32_t primID1, const GeometryFlags geomFlags, const uint32_t geomMask)
    {
      shaderIndex = (geomMask << 24) | shaderID;
      geomIndex = geomID | ((uint)geomFlags << 30);
      primIndex0 = primID0;
      const uint32_t delta = primID1 - primID0;
      const uint32_t j = (((j0) << 0) | ((j1) << 2) | ((j2) << 4));
      primIndex1Delta = delta | (j << 16) | (1 << 22); // single prim in leaf            
      v[0][0] = v0.x;
      v[0][1] = v0.y;
      v[0][2] = v0.z;
      v[1][0] = v1.x;
      v[1][1] = v1.y;
      v[1][2] = v1.z;
      v[2][0] = v2.x;
      v[2][1] = v2.y;
      v[2][2] = v2.z;
      v[3][0] = v3.x;
      v[3][1] = v3.y;
      v[3][2] = v3.z;

    }
  };

  struct LeafGenerationData {

    __forceinline LeafGenerationData() {}
    __forceinline LeafGenerationData(const uint32_t blockID, const uint32_t primID, const uint32_t geomID) : blockID(blockID),primID(primID),geomID(geomID) {}
    
    uint32_t blockID;
    union {
      uint32_t primID;
      uint32_t bvh2_index;
    };
    union {
      uint32_t geomID;
      uint32_t data;
    };
  };

  struct __aligned(16) TmpNodeState { 
    uint32_t header;
    uint32_t bvh2_index;

    __forceinline TmpNodeState() {}
    __forceinline TmpNodeState(const uint32_t _bvh2_index) : header(0x7fffffff), bvh2_index(_bvh2_index) {}
    __forceinline void init(const uint32_t _bvh2_index) { header = 0x7fffffff; bvh2_index = _bvh2_index;  }
  };


  struct GeometryTypeRanges
  {
    uint32_t triQuad_end;
    uint32_t procedural_end;    
    uint32_t instances_end;
    uint32_t lossy_compressed_geometries_end;

    __forceinline GeometryTypeRanges(const uint32_t triQuads, const uint32_t numProcedurals, const uint32_t numInstances, const uint32_t numLossyCompressedGeometries) 
    {
      triQuad_end    = triQuads;
      procedural_end = triQuads + numProcedurals;      
      instances_end  = triQuads + numProcedurals + numInstances;
      lossy_compressed_geometries_end = triQuads + numProcedurals + numInstances + numLossyCompressedGeometries;
    }
    
    __forceinline bool isTriQuad(const uint32_t index)    const { return index >= 0              && index < triQuad_end;    }
    __forceinline bool isProcedural(const uint32_t index) const { return index >= triQuad_end    && index < procedural_end; }    
    __forceinline bool isInstance(const uint32_t index)   const { return index >= procedural_end && index < instances_end;  }
    __forceinline bool isLossyCompressedGeometry(const uint32_t index)   const { return index >= instances_end && index < lossy_compressed_geometries_end;  }
    
    
  };
  
  // ===================================================================================================================================================================================
  // ================================================================================= BVH2 ============================================================================================
  // ===================================================================================================================================================================================
  
  class __aligned(32) BVH2Ploc
  {
  public:
    
    static const uint32_t FATLEAF_SHIFT0     =  31;
    static const uint32_t FATLEAF_SHIFT1     =  30;    
    static const uint32_t FATLEAF_BIT0       =  (uint)1<<FATLEAF_SHIFT0;
    static const uint32_t FATLEAF_MASK       = ~(FATLEAF_BIT0);
    
    uint32_t left;   // 4 bytes
    uint32_t right;  // 4 bytes

    gpu::AABB3f bounds;       // 24 bytes
    
    __forceinline BVH2Ploc() {}

    __forceinline uint32_t leftIndex()      const { return left & FATLEAF_MASK;  }
    __forceinline uint32_t getLeafIndex()   const { return left & FATLEAF_MASK;  }
    __forceinline uint32_t rightIndex()     const { return right & FATLEAF_MASK; }
    
    __forceinline void init(const uint32_t _left, const uint32_t _right, const gpu::AABB3f &_bounds, const BVH2SubTreeState &subtree_size_left, const BVH2SubTreeState &subtree_size_right)
    {
      left    = _left  | ((subtree_size_left.leaves  <= FATLEAF_THRESHOLD ? 1 : 0)<<FATLEAF_SHIFT0);
      right   = _right | ((subtree_size_right.leaves <= FATLEAF_THRESHOLD ? 1 : 0)<<FATLEAF_SHIFT0);

      // === better coalescing ===             
      bounds.lower_x = _bounds.lower_x;
      bounds.lower_y = _bounds.lower_y;
      bounds.lower_z = _bounds.lower_z;
      bounds.upper_x = _bounds.upper_x;
      bounds.upper_y = _bounds.upper_y;
      bounds.upper_z = _bounds.upper_z;                  
    }
    
    __forceinline void initLeaf(const uint32_t _geomID, const uint32_t _primID, const gpu::AABB3f &_bounds)
    {
      left    = _geomID;      
      right   = _primID;

      // === better coalescing ===       
      bounds.lower_x = _bounds.lower_x;
      bounds.lower_y = _bounds.lower_y;
      bounds.lower_z = _bounds.lower_z;
      bounds.upper_x = _bounds.upper_x;
      bounds.upper_y = _bounds.upper_y;
      bounds.upper_z = _bounds.upper_z;                        
    }

    __forceinline void store(BVH2Ploc *dest)
    {
      dest->left  = left;
      dest->right = right;
      dest->bounds.lower_x = bounds.lower_x;
      dest->bounds.lower_y = bounds.lower_y;
      dest->bounds.lower_z = bounds.lower_z;
      dest->bounds.upper_x = bounds.upper_x;
      dest->bounds.upper_y = bounds.upper_y;
      dest->bounds.upper_z = bounds.upper_z;            
    }

    static  __forceinline bool isFatLeaf     (const uint32_t index, const uint32_t numPrimitives) { return (index & FATLEAF_BIT0) || (index & FATLEAF_MASK) < numPrimitives;  }
    static  __forceinline uint32_t getIndex      (const uint32_t index)                           { return index & FATLEAF_MASK;  }
    static  __forceinline bool isLeaf        (const uint32_t index, const uint32_t numPrimitives) { return getIndex(index) < numPrimitives;  }
    static  __forceinline uint32_t makeFatLeaf   (const uint32_t index, const uint32_t numChildren)   { return index | (1<<FATLEAF_SHIFT0);      }

    __forceinline operator const gpu::AABB3f &() const { return bounds; }

    friend __forceinline embree_ostream operator<<(embree_ostream cout, const BVH2Ploc &n)
    {
      cout << "left " << n.leftIndex() << " right " << n.rightIndex() << " left " << n.left << " right " << n.right << " AABB3f { ";
      cout << "  lower = (" << n.bounds.lower_x << ", " << n.bounds.lower_y << ", " << n.bounds.lower_z << ") ";
      cout << "  upper = (" << n.bounds.upper_x << ", " << n.bounds.upper_y << ", " << n.bounds.upper_z << ") ";
      return cout << "}";
    }    
      
  };


  struct __aligned(64) InstancePrimitive
  {
    uint32_t shaderIndex_geomMask; // : 24 shader index used to calculate instancing shader in case of software instancing
                                   // : 8; geometry mask used for ray masking
      
    uint32_t instanceContributionToHitGroupIndex; // : 24;

    // uint32_t type : 1;          // enables/disables opaque culling
    // uint32_t geomFlags : 2;     // unused for instances
      
    uint64_t startNodePtr_instFlags; // : 48;  start node where to continue traversal of the instanced object
      
    Vec3f world2obj_vx;              // 1st column of Worl2Obj transform
    Vec3f world2obj_vy;              // 2nd column of Worl2Obj transform
    Vec3f world2obj_vz;              // 3rd column of Worl2Obj transform
    Vec3f obj2world_p;               // translation of Obj2World transform (on purpose in first 64 bytes)

    uint64_t bvhPtr;                 // pointer to BVH where start node belongs too
      
    uint32_t instanceID;             // user defined value per DXR spec
    uint32_t instanceIndex;          // geometry index of the instance (n'th geometry in scene)
      
    Vec3f obj2world_vx;              // 1st column of Obj2World transform
    Vec3f obj2world_vy;              // 2nd column of Obj2World transform
    Vec3f obj2world_vz;              // 3rd column of Obj2World transform
    Vec3f world2obj_p;               // translation of World2Obj transform

    __forceinline InstancePrimitive() {}
    __forceinline InstancePrimitive(AffineSpace3f obj2world, uint64_t startNodePtr, uint32_t instID, uint32_t geometry_index, uint8_t instMask)
    {
      shaderIndex_geomMask = instMask << 24; 
      instanceContributionToHitGroupIndex = ((uint)PrimLeafDesc::TYPE_OPACITY_CULLING_ENABLED << 29) | ((uint)GeometryFlags::NONE << 30);
    
      startNodePtr_instFlags = startNodePtr;
      
      instanceID = instID;
      instanceIndex = geometry_index;
      bvhPtr = (uint64_t) 0;

      obj2world_vx = obj2world.l.vx;
      obj2world_vy = obj2world.l.vy;
      obj2world_vz = obj2world.l.vz;
      obj2world_p = obj2world.p;
      
      const AffineSpace3f world2obj = rcp(obj2world);
      world2obj_vx = world2obj.l.vx;
      world2obj_vy = world2obj.l.vy;
      world2obj_vz = world2obj.l.vz;
      world2obj_p = world2obj.p;      
    }
  };
  
  static_assert(sizeof(InstancePrimitive) == 128, "InstanceLeaf must be 128 bytes large");
 
  // ===================================================================================================================================================================================
  // ============================================================================== Quadifier ==========================================================================================
  // ===================================================================================================================================================================================


  __forceinline const RTHWIF_TRIANGLE_INDICES &getTriangleDesc(const RTHWIF_GEOMETRY_TRIANGLES_DESC& mesh, const uint32_t triID)
  {
    return *(RTHWIF_TRIANGLE_INDICES*)((char*)mesh.triangleBuffer + mesh.triangleStride * uint64_t(triID));    
  }

  __forceinline const RTHWIF_QUAD_INDICES &getQuadDesc(const RTHWIF_GEOMETRY_QUADS_DESC& mesh, const uint32_t triID)
  {
    return *(RTHWIF_QUAD_INDICES*)((char*)mesh.quadBuffer + mesh.quadStride * uint64_t(triID));    
  }
    
  __forceinline Vec3f getVec3f(const RTHWIF_GEOMETRY_TRIANGLES_DESC& mesh, const uint32_t vtxID)
  {
    return *(Vec3f*)((char*)mesh.vertexBuffer + mesh.vertexStride * uint64_t(vtxID));
    
  }

  __forceinline Vec3f getVec3f(const RTHWIF_GEOMETRY_QUADS_DESC& mesh, const uint32_t vtxID)
  {
    return *(Vec3f*)((char*)mesh.vertexBuffer + mesh.vertexStride * uint64_t(vtxID));
    
  }
    
  __forceinline bool isValidTriangle(const RTHWIF_GEOMETRY_TRIANGLES_DESC& mesh, const uint32_t i, uint3 &indices, gpu::AABB3f &bounds)
  {
    const RTHWIF_TRIANGLE_INDICES &tri = getTriangleDesc(mesh,i);
    const uint32_t numVertices = mesh.vertexCount;
    indices.x() = tri.v0;
    indices.y() = tri.v1;
    indices.z() = tri.v2;    
    if (max(tri.v0,max(tri.v1,tri.v2)) >= numVertices) return false;
    const Vec3fa v0 = getVec3f(mesh,tri.v0);
    const Vec3fa v1 = getVec3f(mesh,tri.v1);
    const Vec3fa v2 = getVec3f(mesh,tri.v2);
    const float max_v0 = max(fabsf(v0.x),fabsf(v0.y),fabsf(v0.z));
    const float max_v1 = max(fabsf(v1.x),fabsf(v1.y),fabsf(v1.z));
    const float max_v2 = max(fabsf(v2.x),fabsf(v2.y),fabsf(v2.z));    
    const static float FLT_LARGE = 1.844E18f;
    const float max_value = max(max_v0,max(max_v1,max_v2));
    if (max_value >= FLT_LARGE || !sycl::isfinite(max_value)) return false;
    float3 vtx0(v0.x,v0.y,v0.z);
    float3 vtx1(v1.x,v1.y,v1.z);
    float3 vtx2(v2.x,v2.y,v2.z);

    bounds.extend(vtx0);
    bounds.extend(vtx1);
    bounds.extend(vtx2);    
    return true;
  }
  
  __forceinline bool isValidQuad(const RTHWIF_GEOMETRY_QUADS_DESC& mesh, const uint32_t i, uint4 &indices, gpu::AABB3f &bounds)
  {
    const RTHWIF_QUAD_INDICES &quad = getQuadDesc(mesh,i);
    const uint32_t numVertices = mesh.vertexCount;
    indices.x() = quad.v0;
    indices.y() = quad.v1;
    indices.z() = quad.v2;
    indices.w() = quad.v3;    
    if (max(max(quad.v0,quad.v1),max(quad.v2,quad.v3)) >= numVertices) return false;
    
    const Vec3fa v0 = getVec3f(mesh,quad.v0);
    const Vec3fa v1 = getVec3f(mesh,quad.v1);
    const Vec3fa v2 = getVec3f(mesh,quad.v2);
    const Vec3fa v3 = getVec3f(mesh,quad.v3);
    
    const float max_v0 = max(fabsf(v0.x),fabsf(v0.y),fabsf(v0.z));
    const float max_v1 = max(fabsf(v1.x),fabsf(v1.y),fabsf(v1.z));
    const float max_v2 = max(fabsf(v2.x),fabsf(v2.y),fabsf(v2.z));
    const float max_v3 = max(fabsf(v3.x),fabsf(v3.y),fabsf(v3.z));    
    
    const static float FLT_LARGE = 1.844E18f;
    const float max_value = max(max(max_v0,max_v1),max(max_v2,max_v3));
    if (max_value >= FLT_LARGE && !sycl::isfinite(max_value)) return false;
    float3 vtx0(v0.x,v0.y,v0.z);
    float3 vtx1(v1.x,v1.y,v1.z);
    float3 vtx2(v2.x,v2.y,v2.z);
    float3 vtx3(v3.x,v3.y,v3.z);    
    bounds.extend(vtx0);
    bounds.extend(vtx1);
    bounds.extend(vtx2);    
    bounds.extend(vtx3);
    return true;
  }

  __forceinline uint32_t try_pair_triangles(const uint3 &a, const uint3 &b, uint& lb0, uint& lb1, uint32_t &lb2)    
  {
    lb0 = 3;
    lb1 = 3;
    lb2 = 3;
    
    lb0 = ( b.x() == a.x() ) ? 0 : lb0;
    lb1 = ( b.y() == a.x() ) ? 0 : lb1;
    lb2 = ( b.z() == a.x() ) ? 0 : lb2;
    
    lb0 = ( b.x() == a.y() ) ? 1 : lb0;
    lb1 = ( b.y() == a.y() ) ? 1 : lb1;
    lb2 = ( b.z() == a.y() ) ? 1 : lb2;
    
    lb0 = ( b.x() == a.z() ) ? 2 : lb0;
    lb1 = ( b.y() == a.z() ) ? 2 : lb1;
    lb2 = ( b.z() == a.z() ) ? 2 : lb2;    
    if ((lb0 == 3) + (lb1 == 3) + (lb2 == 3) <= 1)
    {
      uint32_t p3_index = 0;
      p3_index = (lb1 == 3) ? 1 : p3_index;
      p3_index = (lb2 == 3) ? 2 : p3_index;
      return p3_index;
    }
    else
      return -1;
  }

  __forceinline bool try_pair_triangles(const uint3 &a, const uint3 &b)    
  {
    uint32_t lb0,lb1,lb2;
    lb0 = 3;
    lb1 = 3;
    lb2 = 3;
    
    lb0 = ( b.x() == a.x() ) ? 0 : lb0;
    lb1 = ( b.y() == a.x() ) ? 0 : lb1;
    lb2 = ( b.z() == a.x() ) ? 0 : lb2;
    
    lb0 = ( b.x() == a.y() ) ? 1 : lb0;
    lb1 = ( b.y() == a.y() ) ? 1 : lb1;
    lb2 = ( b.z() == a.y() ) ? 1 : lb2;
    
    lb0 = ( b.x() == a.z() ) ? 2 : lb0;
    lb1 = ( b.y() == a.z() ) ? 2 : lb1;
    lb2 = ( b.z() == a.z() ) ? 2 : lb2;    
    if ((lb0 == 3) + (lb1 == 3) + (lb2 == 3) <= 1)
      return true;
    else
      return false;
  }

  __forceinline uint32_t getBlockQuadificationCount(const RTHWIF_GEOMETRY_TRIANGLES_DESC* const triMesh, const uint32_t localID, const uint32_t startPrimID, const uint32_t endPrimID)
  {
    const uint32_t subgroupLocalID = get_sub_group_local_id();
    const uint32_t subgroupSize    = get_sub_group_size();            
    const uint32_t ID           = (startPrimID + localID) < endPrimID ? startPrimID + localID : -1;
    uint3 tri_indices;
    gpu::AABB3f tmp_bounds;
    bool valid = ID < endPrimID ? isValidTriangle(*triMesh,ID,tri_indices,tmp_bounds) : false;
    bool paired = false;
    uint32_t numQuads = 0;
    uint32_t active_mask = sub_group_ballot(valid);

    while(active_mask)
    {
      active_mask = sub_group_broadcast(active_mask,0);
                                                                              
      const uint32_t broadcast_lane = sycl::ctz(active_mask);
      if (subgroupLocalID == broadcast_lane) valid = false;

      active_mask &= active_mask-1;
                                                                              
      const bool broadcast_paired = sub_group_broadcast(paired, broadcast_lane);
      const uint32_t broadcast_ID     = sub_group_broadcast(ID    , broadcast_lane);

      if (!broadcast_paired)
      {
        const uint3 tri_indices_broadcast(sub_group_broadcast(tri_indices.x(),broadcast_lane),
                                          sub_group_broadcast(tri_indices.y(),broadcast_lane),
                                          sub_group_broadcast(tri_indices.z(),broadcast_lane));
        bool pairable = false;
        if (ID != broadcast_ID && !paired && valid)
          pairable = try_pair_triangles(tri_indices_broadcast,tri_indices);
                                                                            
        const uint32_t first_paired_lane = sycl::ctz(sub_group_ballot(pairable));
        if (first_paired_lane < subgroupSize)
        {
          active_mask &= ~((uint)1 << first_paired_lane);
          if (subgroupLocalID == first_paired_lane) { valid = false; }
        }
      }
      numQuads++;
    }
    return numQuads;
  }

  __forceinline uint32_t getMergedQuadBounds(const RTHWIF_GEOMETRY_TRIANGLES_DESC* const triMesh, const uint32_t ID, const uint32_t endPrimID, gpu::AABB3f &bounds)
  {
    const uint32_t subgroupLocalID = get_sub_group_local_id();
    const uint32_t subgroupSize    = get_sub_group_size();        
    
    uint3 tri_indices;
    bool valid = ID < endPrimID ? isValidTriangle(*triMesh,ID,tri_indices,bounds) : false;
    bool paired = false;
    uint32_t paired_ID = -1;
    uint32_t active_mask = sub_group_ballot(valid);

    while(active_mask)
    {
      active_mask = sub_group_broadcast(active_mask,0);
                                                                              
      const uint32_t broadcast_lane = sycl::ctz(active_mask);

      if (subgroupLocalID == broadcast_lane) valid = false;
                                                                            
      active_mask &= active_mask-1;
                                                                              
      const bool broadcast_paired = sub_group_broadcast(paired, broadcast_lane);
      const uint32_t broadcast_ID     = sub_group_broadcast(ID    , broadcast_lane);

      if (!broadcast_paired)
      {
        const uint3 tri_indices_broadcast(sub_group_broadcast(tri_indices.x(),broadcast_lane),
                                          sub_group_broadcast(tri_indices.y(),broadcast_lane),
                                          sub_group_broadcast(tri_indices.z(),broadcast_lane));
        bool pairable = false;
        if (ID != broadcast_ID && !paired && valid)
          pairable = try_pair_triangles(tri_indices_broadcast,tri_indices);
                                                                            
        const uint32_t first_paired_lane = sycl::ctz(sub_group_ballot(pairable));
        if (first_paired_lane < subgroupSize)
        {
          active_mask &= ~((uint)1 << first_paired_lane);
          if (subgroupLocalID == first_paired_lane) { valid = false; }
          const uint32_t secondID = sub_group_broadcast(ID,first_paired_lane);
          gpu::AABB3f second_bounds = bounds.sub_group_broadcast(first_paired_lane);
          if (subgroupLocalID == broadcast_lane)  {
            paired_ID = secondID;
            bounds.extend(second_bounds);
          }
        }
        else
          if (subgroupLocalID == broadcast_lane)
            paired_ID = ID;
                                                                                
      }
    }
    return paired_ID;
  }  
  
  // ===================================================================================================================================================================================
  // ============================================================================== Instances ==========================================================================================
  // ===================================================================================================================================================================================

  __forceinline AffineSpace3fa getTransform(const RTHWIF_GEOMETRY_INSTANCE_DESC* geom)
  {
    switch (geom->transformFormat)
    {
    case RTHWIF_TRANSFORM_FORMAT_FLOAT3X4_COLUMN_MAJOR: {
      const RTHWIF_TRANSFORM_FLOAT3X4_COLUMN_MAJOR* xfm = (const RTHWIF_TRANSFORM_FLOAT3X4_COLUMN_MAJOR*) geom->transform;
      return {
        { xfm->vx_x, xfm->vx_y, xfm->vx_z },
        { xfm->vy_x, xfm->vy_y, xfm->vy_z },
        { xfm->vz_x, xfm->vz_y, xfm->vz_z },
        { xfm-> p_x, xfm-> p_y, xfm-> p_z }
      };
    }
    case RTHWIF_TRANSFORM_FORMAT_FLOAT4X4_COLUMN_MAJOR: {
      const RTHWIF_TRANSFORM_FLOAT4X4_COLUMN_MAJOR* xfm = (const RTHWIF_TRANSFORM_FLOAT4X4_COLUMN_MAJOR*) geom->transform;
      return {
        { xfm->vx_x, xfm->vx_y, xfm->vx_z },
        { xfm->vy_x, xfm->vy_y, xfm->vy_z },
        { xfm->vz_x, xfm->vz_y, xfm->vz_z },
        { xfm-> p_x, xfm-> p_y, xfm-> p_z }
      };
    }
    case RTHWIF_TRANSFORM_FORMAT_FLOAT3X4_ROW_MAJOR: {
      const RTHWIF_TRANSFORM_FLOAT3X4_ROW_MAJOR* xfm = (const RTHWIF_TRANSFORM_FLOAT3X4_ROW_MAJOR*) geom->transform;
      return {
        { xfm->vx_x, xfm->vx_y, xfm->vx_z },
        { xfm->vy_x, xfm->vy_y, xfm->vy_z },
        { xfm->vz_x, xfm->vz_y, xfm->vz_z },
        { xfm-> p_x, xfm-> p_y, xfm-> p_z }
      };      
    }
    }
  }  

  __forceinline gpu::AABB3f getInstanceBounds(const RTHWIF_GEOMETRY_INSTANCE_DESC &instance)
  {
    const Vec3fa lower(instance.bounds->lower.x,instance.bounds->lower.y,instance.bounds->lower.z);
    const Vec3fa upper(instance.bounds->upper.x,instance.bounds->upper.y,instance.bounds->upper.z);
    const BBox3fa org_bounds(lower,upper);
    const AffineSpace3fa local2world = getTransform(&instance);    
    const BBox3fa instance_bounds = xfmBounds(local2world,org_bounds);
    const gpu::AABB3f bounds(instance_bounds.lower.x,instance_bounds.lower.y,instance_bounds.lower.z,
                       instance_bounds.upper.x,instance_bounds.upper.y,instance_bounds.upper.z);       
    return bounds;
  }
  
  __forceinline bool isValidInstance(const RTHWIF_GEOMETRY_INSTANCE_DESC& instance, gpu::AABB3f &bounds)
  {
    if (instance.bounds == nullptr) return false;    
    bounds = getInstanceBounds(instance);
    if (bounds.empty()) return false;
    if (!bounds.checkNumericalBounds()) return false;    
    return true;
  }

  // =====================================================================================================================================================================================
  // ======================================================================= Lossy Compressed Geometries =================================================================================
  // =====================================================================================================================================================================================


  __forceinline void copyCLs_from_SLM_to_GlobalMemory(void *dest, void *source, const uint32_t numCachelines)
  {
    const uint32_t subgroupLocalID = get_sub_group_local_id();
    uint32_t *s = (uint*)source;    
    uint32_t *d = (uint*)dest;
    for (uint32_t i=0;i<numCachelines;i++,s+=16,d+=16)
      sub_group_store(d,s[subgroupLocalID]);
  }
  
  __forceinline uint32_t estimateLossyCompressedGeometriesSize(const uint32_t numLossyCompressedGeometries)
  {
#if 0    
    const uint32_t numQuads = numLossyCompressedGeometries * (RTC_LOSSY_COMPRESSED_GRID_VERTEX_RES-1) * (RTC_LOSSY_COMPRESSED_GRID_VERTEX_RES-1) * 64;
    const uint32_t numInnerNodes = numLossyCompressedGeometries * (4*3+2+1) * 64;
    return numQuads + numInnerNodes;
#else
    return  numLossyCompressedGeometries * LossyCompressedMeshCluster::getDecompressedSizeInBytes(LossyCompressedMeshCluster::MAX_QUADS_PER_CLUSTER);    
#endif    
  }  
  

  // =====================================================================================================================================================================================
  // ============================================================================== Prefix Sums ==========================================================================================
  // =====================================================================================================================================================================================

  __forceinline void clearScratchMem(sycl::queue &gpu_queue, uint32_t *scratch_mem, const uint32_t value, const uint32_t numEntries, double &iteration_time, const bool verbose)
   {
     const uint32_t wgSize = 256;
     const sycl::nd_range<1> nd_range1(gpu::alignTo(numEntries,wgSize),sycl::range<1>(wgSize)); 
     sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
         cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(32)
                          {
                            const uint32_t globalID         = item.get_global_id(0);
                            if (globalID < numEntries)
                              scratch_mem[globalID] = value;
                          });                                                         
       });
     gpu::waitOnEventAndCatchException(queue_event);
     if (unlikely(verbose)) iteration_time += gpu::getDeviceExecutionTiming(queue_event); 
   }

  __forceinline uint32_t prefixSumWorkgroup(const uint32_t count, const uint32_t WG_SIZE, uint32_t *const counts, uint32_t *const counts_prefix_sum, const sycl::nd_item<1> &item, uint32_t &total_reduction)
  {
    const uint32_t subgroupID      = get_sub_group_id();
    const uint32_t subgroupSize    = get_sub_group_size();
    const uint32_t subgroupLocalID = get_sub_group_local_id();                                                        
    const uint32_t exclusive_scan  = sub_group_exclusive_scan(count, std::plus<uint>());
    const uint32_t reduction       = sub_group_reduce(count, std::plus<uint>());
    counts[subgroupID]         = reduction;
                             
    item.barrier(sycl::access::fence_space::local_space);

    /* -- prefix sum over reduced sub group counts -- */        
    total_reduction = 0;
    for (uint32_t j=subgroupLocalID;j<WG_SIZE/subgroupSize;j+=subgroupSize)
    {
      const uint32_t subgroup_counts = counts[j];
      const uint32_t sums_exclusive_scan = sub_group_exclusive_scan(subgroup_counts, std::plus<uint>());
      const uint32_t reduction = sub_group_broadcast(subgroup_counts,subgroupSize-1) + sub_group_broadcast(sums_exclusive_scan,subgroupSize-1);
      counts_prefix_sum[j] = sums_exclusive_scan + total_reduction;
      total_reduction += reduction;
    }

    item.barrier(sycl::access::fence_space::local_space);

    const uint32_t sums_prefix_sum = counts_prefix_sum[subgroupID];                                                                 
    return sums_prefix_sum + exclusive_scan;
  }

  __forceinline uint32_t prefixSumWorkgroup(const uint32_t ps, const uint32_t WG_SIZE, uint32_t *const counts, const sycl::nd_item<1> &item)
  {
    const uint32_t subgroupID      = get_sub_group_id();
    const uint32_t subgroupSize    = get_sub_group_size();    
    const uint32_t exclusive_scan = sub_group_exclusive_scan(ps, std::plus<uint>());
    const uint32_t reduction = sub_group_reduce(ps, std::plus<uint>());
    counts[subgroupID] = reduction;
                                                                          
    item.barrier(sycl::access::fence_space::local_space);

    /* -- prefix sum over reduced sub group counts -- */        
    uint32_t p_sum = 0;
    for (uint32_t j=0;j<TRIANGLE_QUAD_BLOCK_SIZE/subgroupSize;j++)
      if (j<subgroupID) p_sum += counts[j];
                                                                          
    return p_sum + exclusive_scan;
  }

  
  
  sycl::event prefixSumOverCounts (sycl::queue &gpu_queue, sycl::event &input_event, const uint32_t numGeoms, uint32_t *const counts_per_geom_prefix_sum, uint32_t *host_device_tasks, const bool verbose) // FIXME: general, host_device_tasks
  {
    static const uint32_t GEOM_PREFIX_SUB_GROUP_WIDTH = 16;
    static const uint32_t GEOM_PREFIX_WG_SIZE  = LARGE_WG_SIZE;

    sycl::event queue_event =  gpu_queue.submit([&](sycl::handler &cgh) {
        sycl::local_accessor< uint32_t       , 1> counts(sycl::range<1>((GEOM_PREFIX_WG_SIZE/GEOM_PREFIX_SUB_GROUP_WIDTH)),cgh);
        sycl::local_accessor< uint32_t       , 1> counts_prefix_sum(sycl::range<1>((GEOM_PREFIX_WG_SIZE/GEOM_PREFIX_SUB_GROUP_WIDTH)),cgh);
        const sycl::nd_range<1> nd_range(GEOM_PREFIX_WG_SIZE,sycl::range<1>(GEOM_PREFIX_WG_SIZE));
        cgh.depends_on(input_event);
        cgh.parallel_for(nd_range,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(GEOM_PREFIX_SUB_GROUP_WIDTH) {
            const uint32_t localID        = item.get_local_id(0);
            const uint32_t localSize       = item.get_local_range().size();            
            const uint32_t aligned_numGeoms = gpu::alignTo(numGeoms,GEOM_PREFIX_WG_SIZE);

            uint32_t total_offset = 0;
            for (uint32_t t=localID;t<aligned_numGeoms;t+=localSize)
            {
              item.barrier(sycl::access::fence_space::local_space);
                                                          
              uint32_t count = 0;
              if (t < numGeoms)
                count = counts_per_geom_prefix_sum[t];

              uint32_t total_reduction = 0;
              const uint32_t p_sum = total_offset + prefixSumWorkgroup(count,GEOM_PREFIX_WG_SIZE,counts.get_pointer(),counts_prefix_sum.get_pointer(),item,total_reduction);
              total_offset += total_reduction;              

              if (t < numGeoms)
                counts_per_geom_prefix_sum[t] = p_sum;

            }
                                                        
            if (localID == 0)
            {
              counts_per_geom_prefix_sum[numGeoms] = total_offset;                                                          
              *host_device_tasks = total_offset;
            }
                                                        
          });
      });
    return queue_event;
  }

  // =====================================================================================================================================================================================
  // ========================================================================== Counting Primitives ======================================================================================
  // =====================================================================================================================================================================================
  
  struct __aligned(64) PrimitiveCounts {
    uint32_t numTriangles;
    uint32_t numQuads;
    uint32_t numProcedurals;
    uint32_t numInstances;
    uint32_t numMergedTrisQuads;
    uint32_t numQuadBlocks;
    uint32_t numLossyCompressedGeometries;

    __forceinline void reset()
    {
      numTriangles = numQuads = numProcedurals = numInstances = numMergedTrisQuads = numQuadBlocks = numLossyCompressedGeometries = 0;
    }
    __forceinline PrimitiveCounts() 
    {
      reset();
    }
  };

  __forceinline PrimitiveCounts operator +(const PrimitiveCounts& a, const PrimitiveCounts& b) {
    PrimitiveCounts c;
    c.numTriangles       = a.numTriangles       + b.numTriangles;
    c.numQuads           = a.numQuads           + b.numQuads;
    c.numProcedurals     = a.numProcedurals     + b.numProcedurals;
    c.numInstances       = a.numInstances       + b.numInstances;
    c.numMergedTrisQuads = a.numMergedTrisQuads + b.numMergedTrisQuads;    
    c.numQuadBlocks      = a.numQuadBlocks      + b.numQuadBlocks;
    c.numLossyCompressedGeometries = a.numLossyCompressedGeometries + b.numLossyCompressedGeometries;
    return c;
  }

  __forceinline bool operator ==(const PrimitiveCounts& a, const PrimitiveCounts& b) {
    if (a.numTriangles       == b.numTriangles &&
        a.numQuads           == b.numQuads &&
        a.numProcedurals     == b.numProcedurals &&
        a.numInstances       == b.numInstances &&
        a.numMergedTrisQuads == b.numMergedTrisQuads &&
        a.numQuadBlocks      == b.numQuadBlocks &&
        a.numLossyCompressedGeometries == b.numLossyCompressedGeometries)
      return true;
    return false;
  }
    
  __forceinline bool operator !=(const PrimitiveCounts& a, const PrimitiveCounts& b) {
    return !(a==b);
  }
    
  __forceinline uint32_t find_geomID_from_blockID(const uint32_t *const prefix_sum, const int numItems, const int blockID)
  {
    int l = 0;
    int r = numItems-1;
    while (r-l>1)
    {
      const int m = (l + r) / 2;
      if ( prefix_sum[m] > blockID)
        r = m;
      else if (prefix_sum[m] < blockID)
        l = m;
      else
      {
        if (m == numItems-1 || prefix_sum[m] != prefix_sum[m+1]) return m;
        l = m + 1;
       }
    }
    const int final = prefix_sum[r] <= blockID ? r : l;
    return final;
  }
  

  PrimitiveCounts countPrimitives(sycl::queue &gpu_queue, const RTHWIF_GEOMETRY_DESC **const geometries, const uint32_t numGeometries, PLOCGlobals *const globals, uint32_t *blocksPerGeom, uint32_t *host_device_tasks, double &iteration_time, const bool verbose)    
  {
    PrimitiveCounts count;
    const uint32_t wgSize = LARGE_WG_SIZE;
    const sycl::nd_range<1> nd_range1(gpu::alignTo(numGeometries,wgSize),sycl::range<1>(wgSize));          
    sycl::event count_event = gpu_queue.submit([&](sycl::handler &cgh) {
        sycl::local_accessor< uint, 0> _numTriangles(cgh);
        sycl::local_accessor< uint, 0> _numQuads(cgh);
        sycl::local_accessor< uint, 0> _numProcedurals(cgh);
        sycl::local_accessor< uint, 0> _numInstances(cgh);
        sycl::local_accessor< uint, 0> _numQuadBlocks(cgh);
        sycl::local_accessor< uint, 0> _numLossyCompressedGeometries(cgh);
        
        cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)
                         {
                           const uint32_t geomID    = item.get_global_id(0);
                           const uint32_t localID   = item.get_local_id(0);
                           
                           uint32_t &numTriangles   = *_numTriangles.get_pointer();
                           uint32_t &numQuads       = *_numQuads.get_pointer();
                           uint32_t &numProcedurals = *_numProcedurals.get_pointer();
                           uint32_t &numInstances   = *_numInstances.get_pointer();
                           uint32_t &numQuadBlocks  = *_numQuadBlocks.get_pointer();
                           uint32_t &numLossyCompressedGeometries = *_numLossyCompressedGeometries.get_pointer();
                           
                           if (localID == 0)
                           {
                             numTriangles   = 0;
                             numQuads       = 0;
                             numProcedurals = 0;
                             numInstances   = 0;
                             numQuadBlocks  = 0;
                             numLossyCompressedGeometries = 0;
                           }
                           item.barrier(sycl::access::fence_space::local_space);

                           if (geomID < numGeometries)
                           {
                             uint32_t numBlocks = 0;                             
                             const RTHWIF_GEOMETRY_DESC* geom = geometries[geomID];
                             if (geom != nullptr) {
                               switch (geom->geometryType)
                               {
                               case RTHWIF_GEOMETRY_TYPE_TRIANGLES  :
                               {
                                 gpu::atomic_add_local(&numTriangles  , ((RTHWIF_GEOMETRY_TRIANGLES_DESC *)geom)->triangleCount);
                                 gpu::atomic_add_local(&numQuadBlocks ,(((RTHWIF_GEOMETRY_TRIANGLES_DESC *)geom)->triangleCount+TRIANGLE_QUAD_BLOCK_SIZE-1)/TRIANGLE_QUAD_BLOCK_SIZE);
                                 numBlocks += (((RTHWIF_GEOMETRY_TRIANGLES_DESC *)geom)->triangleCount+TRIANGLE_QUAD_BLOCK_SIZE-1)/TRIANGLE_QUAD_BLOCK_SIZE;
                                 break;
                               }
                               case RTHWIF_GEOMETRY_TYPE_QUADS      :
                               {
                                 gpu::atomic_add_local(&numQuads      , ((RTHWIF_GEOMETRY_QUADS_DESC     *)geom)->quadCount);
                                 gpu::atomic_add_local(&numQuadBlocks ,(((RTHWIF_GEOMETRY_QUADS_DESC *)geom)->quadCount+TRIANGLE_QUAD_BLOCK_SIZE-1)/TRIANGLE_QUAD_BLOCK_SIZE);
                                 numBlocks += (((RTHWIF_GEOMETRY_QUADS_DESC *)geom)->quadCount+TRIANGLE_QUAD_BLOCK_SIZE-1)/TRIANGLE_QUAD_BLOCK_SIZE;
                                 break;
                               }
                               case RTHWIF_GEOMETRY_TYPE_AABBS_FPTR :  gpu::atomic_add_local(&numProcedurals,((RTHWIF_GEOMETRY_AABBS_FPTR_DESC*)geom)->primCount);     break;
                               case RTHWIF_GEOMETRY_TYPE_INSTANCE   :  gpu::atomic_add_local(&numInstances  ,(uint)1); break;
                               case RTHWIF_GEOMETRY_TYPE_LOSSY_COMPRESSED_GEOMETRY : gpu::atomic_add_local(&numLossyCompressedGeometries,((RTHWIF_GEOMETRY_LOSSY_COMPRESSED_GEOMETRY_DESC*)geom)->numLCGs + ((RTHWIF_GEOMETRY_LOSSY_COMPRESSED_GEOMETRY_DESC*)geom)->numLCMs); break;                     
                               };
                             }
                             blocksPerGeom[geomID] = numBlocks;
                           }
                           
                           item.barrier(sycl::access::fence_space::local_space);
                           if (localID == 0)
                           {
                             gpu::atomic_add_global(&globals->numTriangles,numTriangles);
                             gpu::atomic_add_global(&globals->numQuads,numQuads);
                             gpu::atomic_add_global(&globals->numProcedurals,numProcedurals);
                             gpu::atomic_add_global(&globals->numInstances,numInstances);
                             gpu::atomic_add_global(&globals->numQuadBlocks,numQuadBlocks);
                             gpu::atomic_add_global(&globals->numLossyCompressedGeometries,numLossyCompressedGeometries);
                           }
                         });
		  
      });    
    sycl::event copy_event =  gpu_queue.submit([&](sycl::handler &cgh) {
        cgh.depends_on(count_event);
        cgh.single_task([=]() {
            host_device_tasks[0] = globals->numTriangles;
            host_device_tasks[1] = globals->numQuads;
            host_device_tasks[2] = globals->numProcedurals;
            host_device_tasks[3] = globals->numInstances;
            host_device_tasks[4] = globals->numQuadBlocks;
            host_device_tasks[5] = globals->numLossyCompressedGeometries;
          });
      });
    gpu::waitOnEventAndCatchException(copy_event);

    if (unlikely(verbose))
    {
      iteration_time += gpu::getDeviceExecutionTiming(count_event);    
      iteration_time += gpu::getDeviceExecutionTiming(copy_event);
    }
    
    count.numTriangles   = host_device_tasks[0];
    count.numQuads       = host_device_tasks[1];
    count.numProcedurals = host_device_tasks[2];
    count.numInstances   = host_device_tasks[3];
    count.numQuadBlocks  = host_device_tasks[4];
    count.numLossyCompressedGeometries = host_device_tasks[5];

    return count;
  }  


  uint32_t countQuadsPerGeometryUsingBlocks(sycl::queue &gpu_queue, PLOCGlobals *const globals, const RTHWIF_GEOMETRY_DESC **const geometries, const uint32_t numGeoms, const uint32_t numQuadBlocks, uint32_t *const blocksPerGeom, uint32_t *const quadsPerBlock, uint32_t *const host_device_tasks, double &iteration_time, const bool verbose)    
  {
    sycl::event initial;
    sycl::event prefix_sum_blocks_event = prefixSumOverCounts(gpu_queue,initial,numGeoms,blocksPerGeom,host_device_tasks,verbose);     

    // ================================================================================================================

    const sycl::nd_range<1> nd_range2(numQuadBlocks*TRIANGLE_QUAD_BLOCK_SIZE,sycl::range<1>(TRIANGLE_QUAD_BLOCK_SIZE));
    sycl::event count_quads_event = gpu_queue.submit([&](sycl::handler &cgh) {
        sycl::local_accessor< uint32_t      ,  0> _active_counter(cgh);
        sycl::local_accessor< uint32_t      ,  0> _geomID(cgh);
        cgh.depends_on(prefix_sum_blocks_event);
        cgh.parallel_for(nd_range2,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)      
                         {
                           const uint32_t localID         = item.get_local_id(0);                                                                      
                           const uint32_t subgroupLocalID = get_sub_group_local_id();
                           const uint32_t globalBlockID   = item.get_group(0);
                           uint32_t &active_counter       = *_active_counter.get_pointer();
                           uint32_t &geomID               = *_geomID.get_pointer();
                           active_counter = 0;
                           if (localID == 0)
                             geomID = find_geomID_from_blockID(blocksPerGeom,numGeoms,globalBlockID);
                           
                           item.barrier(sycl::access::fence_space::local_space);
                           const uint32_t blockID        = globalBlockID - blocksPerGeom[geomID];
                           const RTHWIF_GEOMETRY_DESC *const geometryDesc = geometries[geomID];
                           
                           // ====================                           
                           // === TriangleMesh ===
                           // ====================
                           
                           if (geometryDesc->geometryType == RTHWIF_GEOMETRY_TYPE_TRIANGLES)
                           {
                             RTHWIF_GEOMETRY_TRIANGLES_DESC* triMesh = (RTHWIF_GEOMETRY_TRIANGLES_DESC*)geometryDesc;                             
                             const uint32_t numTriangles   = triMesh->triangleCount;
                             {                                                                          
                               const uint32_t startPrimID = blockID*TRIANGLE_QUAD_BLOCK_SIZE;
                               const uint32_t endPrimID   = min(startPrimID+TRIANGLE_QUAD_BLOCK_SIZE,numTriangles);
                               const uint32_t numQuads    = getBlockQuadificationCount(triMesh,localID,startPrimID,endPrimID);
                               if (subgroupLocalID == 0)                                 
                                 gpu::atomic_add_local(&active_counter,numQuads);
                             }
                           }
                           // ================                           
                           // === QuadMesh ===
                           // ================
                           else if (geometryDesc->geometryType == RTHWIF_GEOMETRY_TYPE_QUADS)
                           {
                             RTHWIF_GEOMETRY_QUADS_DESC* quadMesh = (RTHWIF_GEOMETRY_QUADS_DESC*)geometryDesc;                                                          
                             const uint32_t numQuads  = quadMesh->quadCount;
                             {                                                                          
                               const uint32_t startPrimID  = blockID*TRIANGLE_QUAD_BLOCK_SIZE;
                               const uint32_t endPrimID    = min(startPrimID+TRIANGLE_QUAD_BLOCK_SIZE,numQuads);
                               const uint32_t ID           = (startPrimID + localID) < endPrimID ? startPrimID + localID : -1;
                               uint4 quad_indices;
                               gpu::AABB3f tmp_bounds;
                               bool valid = ID < endPrimID ? isValidQuad(*quadMesh,ID,quad_indices,tmp_bounds) : false;
                               uint32_t active_mask = sub_group_ballot(valid);
                               uint32_t numQuads = sycl::popcount(active_mask);
                               if (subgroupLocalID == 0)
                                 gpu::atomic_add_local(&active_counter,numQuads);
                             }
                           }
                             
                           item.barrier(sycl::access::fence_space::local_space);
                                                                        
                           if (localID == 0)
                           {
                             gpu::atomic_add_global(&globals->numMergedTrisQuads,active_counter);
                             if (quadsPerBlock)
                               quadsPerBlock[globalBlockID] = active_counter;
                           }
                           
                         });
		  
      });

    // ================================================================================================================
    
    sycl::event copy_event =  gpu_queue.submit([&](sycl::handler &cgh) {
        cgh.depends_on(count_quads_event);
        cgh.single_task([=]() {
            host_device_tasks[0] = globals->numMergedTrisQuads;
          });
         
      });

    // ================================================================================================================
    
    if (quadsPerBlock)
    {
      sycl::event prefix_sum_quads_per_block_event = prefixSumOverCounts(gpu_queue,copy_event,numQuadBlocks,quadsPerBlock,host_device_tasks,verbose);    
      gpu::waitOnEventAndCatchException(prefix_sum_quads_per_block_event);
      if (unlikely(verbose))
        iteration_time += gpu::getDeviceExecutionTiming(prefix_sum_quads_per_block_event);
    }
    else
      gpu::waitOnEventAndCatchException(copy_event);

    if (unlikely(verbose))
    {
      iteration_time += gpu::getDeviceExecutionTiming(prefix_sum_blocks_event);             
      iteration_time += gpu::getDeviceExecutionTiming(count_quads_event);        
      iteration_time += gpu::getDeviceExecutionTiming(copy_event);
    }

    const uint32_t numMergedTrisQuads = host_device_tasks[0];
    return numMergedTrisQuads;
  }
   
   PrimitiveCounts getEstimatedPrimitiveCounts(sycl::queue &gpu_queue, const RTHWIF_GEOMETRY_DESC **const geometries, const uint32_t numGeoms, const bool verbose)    
  {
    if (numGeoms == 0) return PrimitiveCounts();

    const bool verbose2 = verbose;
    uint32_t *host_device_tasks = (uint*)sycl::aligned_alloc(64,HOST_DEVICE_COMM_BUFFER_SIZE,gpu_queue.get_device(),gpu_queue.get_context(),sycl::usm::alloc::host);    
    char *scratch           = (char*)sycl::aligned_alloc(64,sizeof(PrimitiveCounts)+sizeof(PLOCGlobals)+numGeoms*sizeof(uint),gpu_queue.get_device(),gpu_queue.get_context(),sycl::usm::alloc::device);
    
    PLOCGlobals *globals = (PLOCGlobals*)scratch;   
    uint32_t  *blocksPerGeom = (uint*)(scratch + sizeof(PLOCGlobals));

    sycl::event queue_event =  gpu_queue.submit([&](sycl::handler &cgh) {
        cgh.single_task([=]() {
            globals->reset();
          });
      });
    gpu::waitOnEventAndCatchException(queue_event);
    
    double device_prim_counts_time;
    PrimitiveCounts primCounts = countPrimitives(gpu_queue,geometries,numGeoms,globals,blocksPerGeom,host_device_tasks,device_prim_counts_time,verbose2); 
    const uint32_t numQuadBlocks = primCounts.numQuadBlocks;
    if (numQuadBlocks)
    {
      // === get accurate quadification count ===
      double device_quadification_time = 0.0;    
      primCounts.numMergedTrisQuads = countQuadsPerGeometryUsingBlocks(gpu_queue,globals,geometries,numGeoms,numQuadBlocks,blocksPerGeom,nullptr,host_device_tasks,device_quadification_time,verbose2);
    }

    sycl::free(scratch          ,gpu_queue.get_context());
    sycl::free(host_device_tasks,gpu_queue.get_context());
    
    return primCounts;
  }
   

  // =========================================================================================================================================================================================
  // ============================================================================== Create Primrefs ==========================================================================================
  // =========================================================================================================================================================================================

   void createQuads_initPLOCPrimRefs(sycl::queue &gpu_queue, PLOCGlobals *const globals, const RTHWIF_GEOMETRY_DESC **const geometries, const uint32_t numGeoms, const uint32_t numQuadBlocks, const uint32_t *const scratch, BVH2Ploc *const bvh2, const uint32_t prim_type_offset, double &iteration_time, const bool verbose)    
  {
    const uint32_t *const blocksPerGeom          = scratch;
    const uint32_t *const quadsPerBlockPrefixSum = scratch + numGeoms;
    static const uint32_t MERGE_TRIANGLES_TO_QUADS_SUB_GROUP_WIDTH = 16;


    
    const sycl::nd_range<1> nd_range1(numQuadBlocks*TRIANGLE_QUAD_BLOCK_SIZE,sycl::range<1>(TRIANGLE_QUAD_BLOCK_SIZE));
    sycl::event quadification_event = gpu_queue.submit([&](sycl::handler &cgh) {
        sycl::local_accessor< uint32_t      , 0> _active_counter(cgh);
        sycl::local_accessor< uint32_t      , 0> _geomID(cgh);                
        sycl::local_accessor< uint32_t      , 1> counts(sycl::range<1>((TRIANGLE_QUAD_BLOCK_SIZE/MERGE_TRIANGLES_TO_QUADS_SUB_GROUP_WIDTH)),cgh);       
        cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(MERGE_TRIANGLES_TO_QUADS_SUB_GROUP_WIDTH)      
                         {
                           const uint32_t localID         = item.get_local_id(0);
                           const uint32_t globalBlockID   = item.get_group(0);
                           uint32_t &active_counter       = *_active_counter.get_pointer();
                           uint32_t &geomID               = *_geomID.get_pointer();
                           active_counter = 0;
                           if (localID == 0)
                             geomID = find_geomID_from_blockID(blocksPerGeom,numGeoms,globalBlockID);
                           
                           item.barrier(sycl::access::fence_space::local_space);
                           const uint32_t blockID        = globalBlockID - blocksPerGeom[geomID];
                           const uint32_t startQuadOffset = quadsPerBlockPrefixSum[globalBlockID] + prim_type_offset;
                           const RTHWIF_GEOMETRY_DESC *const geometryDesc = geometries[geomID];
                                                      
                           // ====================                           
                           // === TriangleMesh ===
                           // ====================
                           if (geometryDesc->geometryType == RTHWIF_GEOMETRY_TYPE_TRIANGLES)
                           {
                             RTHWIF_GEOMETRY_TRIANGLES_DESC* triMesh = (RTHWIF_GEOMETRY_TRIANGLES_DESC*)geometryDesc;                             
                             const uint32_t numTriangles = triMesh->triangleCount;                                                                                                     
                             {                                                                          
                               const uint32_t startPrimID  = blockID*TRIANGLE_QUAD_BLOCK_SIZE;
                               const uint32_t endPrimID    = min(startPrimID+TRIANGLE_QUAD_BLOCK_SIZE,numTriangles);
                               const uint32_t ID           = (startPrimID + localID) < endPrimID ? startPrimID + localID : -1;
                               {
                                 gpu::AABB3f bounds;
                                 bounds.init();                                 
                                 const uint32_t paired_ID = getMergedQuadBounds(triMesh,ID,endPrimID,bounds);                                                                          
                                 const uint32_t flag = paired_ID != -1 ? 1 : 0;
                                 const uint32_t ps = ID < endPrimID ? flag : 0;
                                 const uint32_t dest_offset = startQuadOffset + prefixSumWorkgroup(ps,TRIANGLE_QUAD_BLOCK_SIZE,counts.get_pointer(),item);

                                 /* --- store cluster representative into destination array --- */
                                 if (ID < endPrimID)
                                   if (paired_ID != -1)
                                   {
                                     const uint32_t pair_offset = paired_ID - ID;
                                     const uint32_t pair_geomID = (pair_offset << PAIR_OFFSET_SHIFT) | geomID;
                                     BVH2Ploc node;
                                     node.initLeaf(pair_geomID,ID,bounds); // need to consider pair_offset
                                     node.store(&bvh2[dest_offset]);
                                   }                                                                          
                               }
                             }                                                                        
                           }
                           // ================                           
                           // === QuadMesh ===
                           // ================                           
                           else if (geometryDesc->geometryType == RTHWIF_GEOMETRY_TYPE_QUADS)
                           {
                             RTHWIF_GEOMETRY_QUADS_DESC* quadMesh = (RTHWIF_GEOMETRY_QUADS_DESC*)geometryDesc;                                                          
                             const uint32_t numQuads  = quadMesh->quadCount;                             
                             {                                                                          
                               const uint32_t startPrimID  = blockID*TRIANGLE_QUAD_BLOCK_SIZE;
                               const uint32_t endPrimID    = min(startPrimID+TRIANGLE_QUAD_BLOCK_SIZE,numQuads);
                               const uint32_t ID           = (startPrimID + localID) < endPrimID ? startPrimID + localID : -1;
                               {
                                 uint4 quad_indices;
                                 gpu::AABB3f bounds;
                                 bounds.init();
                                 const bool valid = ID < endPrimID ? isValidQuad(*quadMesh,ID,quad_indices,bounds) : false;                                                                          
                                 const uint32_t ps = valid ? 1 : 0;
                                 const uint32_t dest_offset = startQuadOffset + prefixSumWorkgroup(ps,TRIANGLE_QUAD_BLOCK_SIZE,counts.get_pointer(),item);

                                 /* --- store cluster representative into destination array --- */
                                 if (ID < endPrimID)
                                   if (valid)
                                   {
                                     BVH2Ploc node;
                                     node.initLeaf(geomID,ID,bounds); // need to consider pair_offset
                                     node.store(&bvh2[dest_offset]);
                                   }                                                                          
                               }
                             }                                                                                                     
                           }
                         });
		  
      });
    gpu::waitOnEventAndCatchException(quadification_event);
    if (unlikely(verbose)) iteration_time += gpu::getDeviceExecutionTiming(quadification_event);
  }
       
   uint32_t createInstances_initPLOCPrimRefs(sycl::queue &gpu_queue, const RTHWIF_GEOMETRY_DESC **const geometry_desc, const uint32_t numGeoms, uint32_t *scratch_mem, const uint32_t MAX_WGS, BVH2Ploc *const bvh2, const uint32_t prim_type_offset, uint32_t *host_device_tasks, double &iteration_time, const bool verbose)    
  {
    const uint32_t numWGs = min((numGeoms+LARGE_WG_SIZE-1)/LARGE_WG_SIZE,(uint)MAX_WGS);
    clearScratchMem(gpu_queue,scratch_mem,0,numWGs,iteration_time,verbose);
   
    static const uint32_t CREATE_INSTANCES_SUB_GROUP_WIDTH = 16;
    static const uint32_t CREATE_INSTANCES_WG_SIZE  = LARGE_WG_SIZE;
    const sycl::nd_range<1> nd_range1(numWGs*CREATE_INSTANCES_WG_SIZE,sycl::range<1>(CREATE_INSTANCES_WG_SIZE));
    sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
        sycl::local_accessor< uint32_t      , 1> counts(sycl::range<1>((CREATE_INSTANCES_WG_SIZE/CREATE_INSTANCES_SUB_GROUP_WIDTH)),cgh);
        sycl::local_accessor< uint32_t      , 1> counts_prefix_sum(sycl::range<1>((CREATE_INSTANCES_WG_SIZE/CREATE_INSTANCES_SUB_GROUP_WIDTH)),cgh);                
        sycl::local_accessor< uint32_t      , 0> _active_counter(cgh);
        sycl::local_accessor< uint32_t      , 0> _global_count_prefix_sum(cgh);
        
        cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(CREATE_INSTANCES_SUB_GROUP_WIDTH)      
                         {
                           const uint32_t groupID         = item.get_group(0);
                           const uint32_t numGroups       = item.get_group_range(0);                                                        
                           const uint32_t localID         = item.get_local_id(0);
                           const uint32_t step_local      = item.get_local_range().size();
                           const uint32_t startID         = ((size_t)groupID + 0)*(size_t)numGeoms / numWGs;
                           const uint32_t endID           = ((size_t)groupID + 1)*(size_t)numGeoms / numWGs;
                           const uint32_t sizeID          = endID-startID;
                           const uint32_t aligned_sizeID  = gpu::alignTo(sizeID,CREATE_INSTANCES_WG_SIZE);
                           
                           uint32_t &active_counter          = *_active_counter.get_pointer();
                           uint32_t &global_count_prefix_sum = *_global_count_prefix_sum.get_pointer();
                           
                           active_counter = 0;
                           global_count_prefix_sum = 0;            

                           sycl::atomic_ref<uint, sycl::memory_order::relaxed, sycl::memory_scope::work_group,sycl::access::address_space::local_space> counter(active_counter);
                           
                           item.barrier(sycl::access::fence_space::local_space);

                           
                           for (uint32_t ID = localID; ID < aligned_sizeID; ID += step_local)
                           {
                             if (ID < sizeID)
                             {
                               const uint32_t instID = startID + ID;
                               uint32_t count = 0;
                               if (geometry_desc[instID]->geometryType == RTHWIF_GEOMETRY_TYPE_INSTANCE)
                               {
                                 RTHWIF_GEOMETRY_INSTANCE_DESC *geom = (RTHWIF_GEOMETRY_INSTANCE_DESC *)geometry_desc[instID];
                                 gpu::AABB3f bounds;
                                 if (isValidInstance(*geom,bounds)) count = 1;
                               }
                               if (count)
                                 gpu::atomic_add_local(&active_counter,count);
                             }
                           }                             

                           item.barrier(sycl::access::fence_space::local_space);

                           const uint32_t flag = (uint)1<<31;            
                           const uint32_t mask = ~flag;
            
                           if (localID == 0)
                           {
                             sycl::atomic_ref<uint,sycl::memory_order::relaxed, sycl::memory_scope::device,sycl::access::address_space::global_space> scratch_mem_counter(scratch_mem[groupID]);
                             scratch_mem_counter.store(active_counter | flag);              
                           }
            
                           item.barrier(sycl::access::fence_space::global_and_local);

                           // =======================================            
                           // wait until earlier WGs finished as well
                           // =======================================

                           if (localID < groupID)
                           {
                             sycl::atomic_ref<uint, sycl::memory_order::acq_rel, sycl::memory_scope::device,sycl::access::address_space::global_space> global_state(scratch_mem[localID]);
                             uint32_t c = 0;
                             while( ((c = global_state.load()) & flag) == 0 );
                             if (c) gpu::atomic_add_local(&global_count_prefix_sum,(uint)c & mask);
                           }
            
                           item.barrier(sycl::access::fence_space::local_space);

                           uint32_t total_offset = 0;
                           gpu::AABB3f bounds;
                           for (uint32_t ID = localID; ID < aligned_sizeID; ID += step_local)
                           {
                             item.barrier(sycl::access::fence_space::local_space);

                             uint32_t count = 0;
                             if (ID < sizeID)
                             {
                               const uint32_t instID = startID + ID;
                               if (geometry_desc[instID]->geometryType == RTHWIF_GEOMETRY_TYPE_INSTANCE)
                               {
                                 RTHWIF_GEOMETRY_INSTANCE_DESC *geom = (RTHWIF_GEOMETRY_INSTANCE_DESC *)geometry_desc[instID];                                                                  
                                 if (isValidInstance(*geom,bounds)) count = 1;
                               }
                             }

                             uint32_t total_reduction = 0;
                             const uint32_t p_sum = global_count_prefix_sum + total_offset + prefixSumWorkgroup(count,CREATE_INSTANCES_WG_SIZE,counts.get_pointer(),counts_prefix_sum.get_pointer(),item,total_reduction);
                             total_offset += total_reduction;
                             
                             if (ID < sizeID)
                             {
                               const uint32_t instID = startID + ID;
                               if (geometry_desc[instID]->geometryType == RTHWIF_GEOMETRY_TYPE_INSTANCE && count == 1)
                               {
                                 BVH2Ploc node;                                                                                           
                                 node.initLeaf(instID,0,bounds);                               
                                 node.store(&bvh2[prim_type_offset + p_sum]);                                                                
                               }                               
                             }

                             if (groupID == numGroups-1 && localID == 0)
                               *host_device_tasks = global_count_prefix_sum + (scratch_mem[groupID] & mask);
                           }                                                      
                         });
		  
      });
    gpu::waitOnEventAndCatchException(queue_event);
    if (unlikely(verbose)) iteration_time += gpu::getDeviceExecutionTiming(queue_event);
    const uint32_t numInstances = *host_device_tasks;
    return numInstances;
  }

  
   __forceinline bool buildBounds(const RTHWIF_GEOMETRY_AABBS_FPTR_DESC* geom, uint32_t primID, BBox3fa& bbox, void* buildUserPtr)
  {
    if (primID >= geom->primCount) return false;
    if (geom->getBounds == nullptr) return false;

    BBox3f bounds;
    (geom->getBounds)(primID,1,geom->geomUserPtr,buildUserPtr,(RTHWIF_AABB*)&bounds);
    if (unlikely(!isvalid(bounds.lower))) return false;
    if (unlikely(!isvalid(bounds.upper))) return false;
    if (unlikely(bounds.empty())) return false;
    
    bbox = (BBox3f&) bounds;
    return true;
  }
  
   uint32_t createProcedurals_initPLOCPrimRefs(sycl::queue &gpu_queue, const RTHWIF_GEOMETRY_DESC **const geometry_desc, const uint32_t numGeoms, uint32_t *scratch_mem, const uint32_t MAX_WGS, BVH2Ploc *const bvh2, const uint32_t prim_type_offset, uint32_t *host_device_tasks, double &iteration_time, const bool verbose)    
  {
    //FIXME: GPU version
    
    uint32_t ID = 0;
    for (uint32_t userGeomID=0;userGeomID<numGeoms;userGeomID++)
    {
      if (unlikely(geometry_desc[userGeomID] == nullptr)) continue;
      if (geometry_desc[userGeomID]->geometryType == RTHWIF_GEOMETRY_TYPE_AABBS_FPTR)
      {
        RTHWIF_GEOMETRY_INSTANCE_DESC *geom = (RTHWIF_GEOMETRY_INSTANCE_DESC *)geometry_desc[userGeomID];
        if (geom->geometryType == RTHWIF_GEOMETRY_TYPE_AABBS_FPTR)
        {
          RTHWIF_GEOMETRY_AABBS_FPTR_DESC *procedural = (RTHWIF_GEOMETRY_AABBS_FPTR_DESC *)geom;
          for (uint32_t i=0;i<procedural->primCount;i++)
          {
            BBox3fa procedural_bounds;
            
            if (!buildBounds(procedural,i,procedural_bounds,procedural->geomUserPtr)) continue;

            gpu::AABB3f bounds = gpu::AABB3f(procedural_bounds.lower.x,procedural_bounds.lower.y,procedural_bounds.lower.z,
                                             procedural_bounds.upper.x,procedural_bounds.upper.y,procedural_bounds.upper.z);
            
            BVH2Ploc node;                             
            node.initLeaf(userGeomID,i,bounds);                               
            node.store(&bvh2[prim_type_offset + ID]);
            ID++;              
          }          
        }
      }
    }
    return ID;
  }


   __forceinline void writeNodeFast(void *_dest, const int relative_block_offset, const gpu::AABB3f &parent_bounds, const uint32_t numChildren, const gpu::AABB3f child_bounds[6], const NodeType default_type)
  {
    uint32_t *dest = (uint*)_dest;    
    //PRINT3(dest,numChildren,relative_block_offset);

    const float _ulp = std::numeric_limits<float>::epsilon();
    const float up = 1.0f + float(_ulp);  
    const gpu::AABB3f conservative_bounds = parent_bounds.conservativeBounds();
    const float3 len = conservative_bounds.size() * up;
      
    int _exp_x; float mant_x = frexp(len.x(), &_exp_x); _exp_x += (mant_x > 255.0f / 256.0f);
    int _exp_y; float mant_y = frexp(len.y(), &_exp_y); _exp_y += (mant_y > 255.0f / 256.0f);
    int _exp_z; float mant_z = frexp(len.z(), &_exp_z); _exp_z += (mant_z > 255.0f / 256.0f);
    _exp_x = max(-128,_exp_x); // enlarge too tight bounds
    _exp_y = max(-128,_exp_y);
    _exp_z = max(-128,_exp_z);

    const float3 lower(conservative_bounds.lower_x,conservative_bounds.lower_y,conservative_bounds.lower_z);
    
    dest[0]  = gpu::as_uint(lower.x());
    dest[1]  = gpu::as_uint(lower.y());
    dest[2]  = gpu::as_uint(lower.z());
    dest[3]  = relative_block_offset; 

    uint8_t tmp[48];
    
    tmp[0]         = NODE_TYPE_MIXED;
    tmp[1]         = 0;    // pad 
    tmp[2]         = _exp_x; assert(_exp_x >= -128 && _exp_x <= 127); 
    tmp[3]         = _exp_y; assert(_exp_y >= -128 && _exp_y <= 127);
    tmp[4]         = _exp_z; assert(_exp_z >= -128 && _exp_z <= 127);
    tmp[5]         = 0xff; //instanceMode ? 1 : 0xff;
    
#pragma nounroll
    for (uint32_t i=0;i<BVH_BRANCHING_FACTOR;i++)
    {
      //const uint32_t index = BVH2Ploc::getIndex(indices[sycl::min(i,numChildren-1)]);
      // === default is invalid ===
      uint8_t lower_x = 0x80;
      uint8_t lower_y = 0x80;
      uint8_t lower_z = 0x80;    
      uint8_t upper_x = 0x00;
      uint8_t upper_y = 0x00;
      uint8_t upper_z = 0x00;
      uint8_t data    = 0x00;      
      // === determine leaf type ===
      const bool isLeaf = true; // && !forceFatLeaves;      
      const uint32_t numBlocks    =  1;
      NodeType leaf_type = default_type;
      data = (i<numChildren) ? numBlocks : 0;
      data |= (isLeaf ? leaf_type : NODE_TYPE_INTERNAL) << 2;
      const gpu::AABB3f childBounds = child_bounds[i]; //.conservativeBounds();
      // === bounds valid ? ====
      uint32_t equal_dims = childBounds.lower_x == childBounds.upper_x ? 1 : 0;
      equal_dims += childBounds.lower_y == childBounds.upper_y ? 1 : 0;
      equal_dims += childBounds.lower_z == childBounds.upper_z ? 1 : 0;
      const bool write = (i<numChildren) && equal_dims <= 1;
      // === quantize bounds ===
      const gpu::AABB3f  qbounds    = QBVHNodeN::quantize_bounds(lower, _exp_x, _exp_y, _exp_z, childBounds);
      // === updated discretized bounds ===
      lower_x = write ? (uint8_t)qbounds.lower_x : lower_x;
      lower_y = write ? (uint8_t)qbounds.lower_y : lower_y;
      lower_z = write ? (uint8_t)qbounds.lower_z : lower_z;
      upper_x = write ? (uint8_t)qbounds.upper_x : upper_x;
      upper_y = write ? (uint8_t)qbounds.upper_y : upper_y;
      upper_z = write ? (uint8_t)qbounds.upper_z : upper_z;
      // === init child in node ===
      tmp[ 6+i] = data;
      tmp[12+i] = lower_x;
      tmp[18+i] = upper_x;      
      tmp[24+i] = lower_y;
      tmp[30+i] = upper_y;      
      tmp[36+i] = lower_z;
      tmp[42+i] = upper_z;      
    }
    // === write out second part of 64 bytes node ===
    for (uint32_t i=0;i<12;i++)
      dest[4+i] = tmp[i*4+0] | (tmp[i*4+1]<<8) | (tmp[i*4+2]<<16) | (tmp[i*4+3]<<24);
  }
   
   struct __aligned(64) LocalNodeData_subgroup { 
     uint32_t lower[4];
     uint8_t tmp[48];
   }; 

   __forceinline void writeNode_subgroup(LocalNodeData_subgroup &dest, const uint32_t relative_block_offset, const gpu::AABB3f &parent_bounds, const gpu::AABB3f &childBounds, const uint32_t numChildren, const uint32_t localID, const NodeType default_type = NODE_TYPE_INTERNAL)
  {    
    const float _ulp = std::numeric_limits<float>::epsilon();
    const float up = 1.0f + float(_ulp);  
    const gpu::AABB3f conservative_bounds = parent_bounds.conservativeBounds();
    const float3 len = conservative_bounds.size() * up;
      
    int _exp_x; float mant_x = frexp(len.x(), &_exp_x); _exp_x += (mant_x > 255.0f / 256.0f);
    int _exp_y; float mant_y = frexp(len.y(), &_exp_y); _exp_y += (mant_y > 255.0f / 256.0f);
    int _exp_z; float mant_z = frexp(len.z(), &_exp_z); _exp_z += (mant_z > 255.0f / 256.0f);
    _exp_x = max(-128,_exp_x); // enlarge too tight bounds
    _exp_y = max(-128,_exp_y);
    _exp_z = max(-128,_exp_z);

    const float3 lower(conservative_bounds.lower_x,conservative_bounds.lower_y,conservative_bounds.lower_z);
    
    dest.lower[0]  = gpu::as_uint(lower.x());
    dest.lower[1]  = gpu::as_uint(lower.y());
    dest.lower[2]  = gpu::as_uint(lower.z());
    dest.lower[3]  = relative_block_offset; 
    
    dest.tmp[0]         = NODE_TYPE_MIXED; // type
    dest.tmp[1]         = 0;    // pad 
    dest.tmp[2]         = _exp_x; assert(_exp_x >= -128 && _exp_x <= 127); 
    dest.tmp[3]         = _exp_y; assert(_exp_y >= -128 && _exp_y <= 127);
    dest.tmp[4]         = _exp_z; assert(_exp_z >= -128 && _exp_z <= 127);
    dest.tmp[5]         = 0xff; //instanceMode ? 1 : 0xff;

    for (uint32_t i=numChildren;i<6;i++)
    {
      uint8_t lower_x = 0x80;
      uint8_t lower_y = 0x80;
      uint8_t lower_z = 0x80;    
      uint8_t upper_x = 0x00;
      uint8_t upper_y = 0x00;
      uint8_t upper_z = 0x00;
      uint8_t data    = 0x00;      
      // === init child in node ===
      dest.tmp[ 6+i] = data;
      dest.tmp[12+i] = lower_x;
      dest.tmp[18+i] = upper_x;      
      dest.tmp[24+i] = lower_y;
      dest.tmp[30+i] = upper_y;      
      dest.tmp[36+i] = lower_z;
      dest.tmp[42+i] = upper_z;      
    }
    
    if (localID < BVH_BRANCHING_FACTOR)
    {
      const uint32_t i = localID;
      // === default is invalid ===
      uint8_t lower_x = 0x80;
      uint8_t lower_y = 0x80;
      uint8_t lower_z = 0x80;    
      uint8_t upper_x = 0x00;
      uint8_t upper_y = 0x00;
      uint8_t upper_z = 0x00;
      uint8_t data    = 0x00;      
      // === determine leaf type ===
      const bool isLeaf = true; // && !forceFatLeaves;      
      const uint32_t numBlocks    =  1;
      NodeType leaf_type = default_type;
      data = (i<numChildren) ? numBlocks : 0;
      data |= (isLeaf ? leaf_type : NODE_TYPE_INTERNAL) << 2;
      // === bounds valid ? ====
      uint32_t equal_dims = childBounds.lower_x == childBounds.upper_x ? 1 : 0;
      equal_dims += childBounds.lower_y == childBounds.upper_y ? 1 : 0;
      equal_dims += childBounds.lower_z == childBounds.upper_z ? 1 : 0;
      const bool write = (i<numChildren) && equal_dims <= 1;
      // === quantize bounds ===
      const gpu::AABB3f  qbounds    = QBVHNodeN::quantize_bounds(lower, _exp_x, _exp_y, _exp_z, childBounds);
      // === updated discretized bounds ===
      lower_x = write ? (uint8_t)qbounds.lower_x : lower_x;
      lower_y = write ? (uint8_t)qbounds.lower_y : lower_y;
      lower_z = write ? (uint8_t)qbounds.lower_z : lower_z;
      upper_x = write ? (uint8_t)qbounds.upper_x : upper_x;
      upper_y = write ? (uint8_t)qbounds.upper_y : upper_y;
      upper_z = write ? (uint8_t)qbounds.upper_z : upper_z;
      
      // === init child in node ===
      dest.tmp[ 6+i] = data;
      dest.tmp[12+i] = lower_x;
      dest.tmp[18+i] = upper_x;      
      dest.tmp[24+i] = lower_y;
      dest.tmp[30+i] = upper_y;      
      dest.tmp[36+i] = lower_z;
      dest.tmp[42+i] = upper_z;      
    }
  }

   
   __forceinline gpu::AABB3f to_AABB3f(const BBox3f &v)
  {
    return gpu::AABB3f(to_float3(v.lower),to_float3(v.upper));
  }

  struct QBVHReference
  {
    float3 lower;
    int _exp_x;
    int _exp_y;
    int _exp_z;

    __forceinline QBVHReference(const gpu::AABB3f &bounds)
    {
     const float _ulp = std::numeric_limits<float>::epsilon();
     const float up = 1.0f + float(_ulp);        
      const gpu::AABB3f conservative_bounds = bounds.conservativeBounds();
      const float3 len = conservative_bounds.size() * up;      
      float mant_x = frexp(len.x(), &_exp_x); _exp_x += (mant_x > 255.0f / 256.0f);
      float mant_y = frexp(len.y(), &_exp_y); _exp_y += (mant_y > 255.0f / 256.0f);
      float mant_z = frexp(len.z(), &_exp_z); _exp_z += (mant_z > 255.0f / 256.0f);
      _exp_x = max(-128,_exp_x); // enlarge too tight bounds
      _exp_y = max(-128,_exp_y);
      _exp_z = max(-128,_exp_z);
      lower = float3(conservative_bounds.lower_x,conservative_bounds.lower_y,conservative_bounds.lower_z);      
    }
    
  };
  
  struct QBVH    
  {
    uint8_t lower_x,lower_y,lower_z;
    uint8_t upper_x,upper_y,upper_z;

    __forceinline QBVH() {}
    
    __forceinline QBVH(const QBVH &q) : lower_x(q.lower_x),lower_y(q.lower_y),lower_z(q.lower_z),upper_x(q.upper_x),upper_y(q.upper_y),upper_z(q.upper_z)
    {
    }
    
    __forceinline QBVH(const gpu::AABB3f &bounds,const QBVHReference &ref)
    {
      const gpu::AABB3f  qbounds    = QBVHNodeN::quantize_bounds(ref.lower, ref._exp_x, ref._exp_y, ref._exp_z, bounds);
      lower_x = (uint8_t)qbounds.lower_x;
      lower_y = (uint8_t)qbounds.lower_y;
      lower_z = (uint8_t)qbounds.lower_z;
      upper_x = (uint8_t)qbounds.upper_x;
      upper_y = (uint8_t)qbounds.upper_y;
      upper_z = (uint8_t)qbounds.upper_z;     
    }

    __forceinline void init()
    {
      lower_x = 255;
      lower_y = 255;
      lower_z = 255;

      upper_x = 0;
      upper_y = 0;
      upper_z = 0;      
    }
    
    __forceinline void extend(const QBVH &v)
    {
      lower_x = min(lower_x,v.lower_x);
      lower_y = min(lower_y,v.lower_y);
      lower_z = min(lower_z,v.lower_z);

      upper_x = max(upper_x,v.upper_x);
      upper_y = max(upper_y,v.upper_y);
      upper_z = max(upper_z,v.upper_z);
    }

    __forceinline QBVH sub_group_reduce() const
    {
      QBVH v;
      v.lower_x = embree::sub_group_reduce(lower_x, SYCL_EXT_ONEAPI::minimum<uint8_t>());
      v.lower_y = embree::sub_group_reduce(lower_y, SYCL_EXT_ONEAPI::minimum<uint8_t>());
      v.lower_z = embree::sub_group_reduce(lower_z, SYCL_EXT_ONEAPI::minimum<uint8_t>());

      v.upper_x = embree::sub_group_reduce(upper_x, SYCL_EXT_ONEAPI::maximum<uint8_t>());
      v.upper_y = embree::sub_group_reduce(upper_y, SYCL_EXT_ONEAPI::maximum<uint8_t>());
      v.upper_z = embree::sub_group_reduce(upper_z, SYCL_EXT_ONEAPI::maximum<uint8_t>());      
      return v;
    }
    
    
  };
   


   __forceinline void writeNodeFast(void *_dest, const int relative_block_offset, const uint32_t numChildren, const QBVH child_bounds[6], const NodeType default_type, const QBVHReference &ref)
   {
     uint32_t *dest = (uint*)_dest;    
    
     dest[0]  = gpu::as_uint(ref.lower.x());
     dest[1]  = gpu::as_uint(ref.lower.y());
     dest[2]  = gpu::as_uint(ref.lower.z());
     dest[3]  = relative_block_offset; 

     uint8_t tmp[48];
    
     tmp[0]         = NODE_TYPE_MIXED;
     tmp[1]         = 0;    // pad 
     tmp[2]         = ref._exp_x; assert(_exp_x >= -128 && _exp_x <= 127); 
     tmp[3]         = ref._exp_y; assert(_exp_y >= -128 && _exp_y <= 127);
     tmp[4]         = ref._exp_z; assert(_exp_z >= -128 && _exp_z <= 127);
     tmp[5]         = 0xff; //instanceMode ? 1 : 0xff;
    
#pragma nounroll
     for (uint32_t i=0;i<BVH_BRANCHING_FACTOR;i++)
     {
       //const uint32_t index = BVH2Ploc::getIndex(indices[sycl::min(i,numChildren-1)]);
       // === default is invalid ===
       uint8_t lower_x = 0x80;
       uint8_t lower_y = 0x80;
       uint8_t lower_z = 0x80;    
       uint8_t upper_x = 0x00;
       uint8_t upper_y = 0x00;
       uint8_t upper_z = 0x00;
       uint8_t data    = 0x00;      
       // === determine leaf type ===
       const bool isLeaf = true; // && !forceFatLeaves;      
       const uint32_t numBlocks    =  1;
       NodeType leaf_type = default_type;
       data = (i<numChildren) ? numBlocks : 0;
       data |= (isLeaf ? leaf_type : NODE_TYPE_INTERNAL) << 2;
       const QBVH &childBounds = child_bounds[i];
       // === bounds valid ? ====
       uint32_t equal_dims = childBounds.lower_x == childBounds.upper_x ? 1 : 0;
       equal_dims += childBounds.lower_y == childBounds.upper_y ? 1 : 0;
       equal_dims += childBounds.lower_z == childBounds.upper_z ? 1 : 0;
       const bool write = (i<numChildren) && equal_dims <= 1;
       // === updated discretized bounds ===
       lower_x = write ? (uint8_t)childBounds.lower_x : lower_x;
       lower_y = write ? (uint8_t)childBounds.lower_y : lower_y;
       lower_z = write ? (uint8_t)childBounds.lower_z : lower_z;
       upper_x = write ? (uint8_t)childBounds.upper_x : upper_x;
       upper_y = write ? (uint8_t)childBounds.upper_y : upper_y;
       upper_z = write ? (uint8_t)childBounds.upper_z : upper_z;
       // === init child in node ===
       tmp[ 6+i] = data;
       tmp[12+i] = lower_x;
       tmp[18+i] = upper_x;      
       tmp[24+i] = lower_y;
       tmp[30+i] = upper_y;      
       tmp[36+i] = lower_z;
       tmp[42+i] = upper_z;      
     }
     // === write out second part of 64 bytes node ===
     for (uint32_t i=0;i<12;i++)
       dest[4+i] = tmp[i*4+0] | (tmp[i*4+1]<<8) | (tmp[i*4+2]<<16) | (tmp[i*4+3]<<24);
   }
  

   __forceinline void writeNodeFast(void *_dest, const int relative_block_offset, const CompressedAABB3f &parent_bounds, const uint32_t numChildren, const CompressedAABB3f child_bounds[6], const NodeType default_type, const Vec3f &start, const Vec3f &diag)
   {
     uint32_t *dest = (uint*)_dest;    
     //PRINT3(dest,numChildren,relative_block_offset);

     const float _ulp = std::numeric_limits<float>::epsilon();
     const float up = 1.0f + float(_ulp);  
     const gpu::AABB3f conservative_bounds = to_AABB3f(parent_bounds.decompress(start,diag)).conservativeBounds();
     const float3 len = conservative_bounds.size() * up;
      
     int _exp_x; float mant_x = frexp(len.x(), &_exp_x); _exp_x += (mant_x > 255.0f / 256.0f);
     int _exp_y; float mant_y = frexp(len.y(), &_exp_y); _exp_y += (mant_y > 255.0f / 256.0f);
     int _exp_z; float mant_z = frexp(len.z(), &_exp_z); _exp_z += (mant_z > 255.0f / 256.0f);
     _exp_x = max(-128,_exp_x); // enlarge too tight bounds
     _exp_y = max(-128,_exp_y);
     _exp_z = max(-128,_exp_z);

     const float3 lower(conservative_bounds.lower_x,conservative_bounds.lower_y,conservative_bounds.lower_z);
    
     dest[0]  = gpu::as_uint(lower.x());
     dest[1]  = gpu::as_uint(lower.y());
     dest[2]  = gpu::as_uint(lower.z());
     dest[3]  = relative_block_offset; 

     uint8_t tmp[48];
    
     tmp[0]         = NODE_TYPE_MIXED;
     tmp[1]         = 0;    // pad 
     tmp[2]         = _exp_x; assert(_exp_x >= -128 && _exp_x <= 127); 
     tmp[3]         = _exp_y; assert(_exp_y >= -128 && _exp_y <= 127);
     tmp[4]         = _exp_z; assert(_exp_z >= -128 && _exp_z <= 127);
     tmp[5]         = 0xff; //instanceMode ? 1 : 0xff;
    
#pragma nounroll
     for (uint32_t i=0;i<BVH_BRANCHING_FACTOR;i++)
     {
       //const uint32_t index = BVH2Ploc::getIndex(indices[sycl::min(i,numChildren-1)]);
       // === default is invalid ===
       uint8_t lower_x = 0x80;
       uint8_t lower_y = 0x80;
       uint8_t lower_z = 0x80;    
       uint8_t upper_x = 0x00;
       uint8_t upper_y = 0x00;
       uint8_t upper_z = 0x00;
       uint8_t data    = 0x00;      
       // === determine leaf type ===
       const bool isLeaf = true; // && !forceFatLeaves;      
       const uint32_t numBlocks    =  1;
       NodeType leaf_type = default_type;
       data = (i<numChildren) ? numBlocks : 0;
       data |= (isLeaf ? leaf_type : NODE_TYPE_INTERNAL) << 2;
       const gpu::AABB3f childBounds = to_AABB3f(child_bounds[i].decompress(start,diag)); //.conservativeBounds();
       // === bounds valid ? ====
       uint32_t equal_dims = childBounds.lower_x == childBounds.upper_x ? 1 : 0;
       equal_dims += childBounds.lower_y == childBounds.upper_y ? 1 : 0;
       equal_dims += childBounds.lower_z == childBounds.upper_z ? 1 : 0;
       const bool write = (i<numChildren) && equal_dims <= 1;
       // === quantize bounds ===
       const gpu::AABB3f  qbounds    = QBVHNodeN::quantize_bounds(lower, _exp_x, _exp_y, _exp_z, childBounds);
       // === updated discretized bounds ===
       lower_x = write ? (uint8_t)qbounds.lower_x : lower_x;
       lower_y = write ? (uint8_t)qbounds.lower_y : lower_y;
       lower_z = write ? (uint8_t)qbounds.lower_z : lower_z;
       upper_x = write ? (uint8_t)qbounds.upper_x : upper_x;
       upper_y = write ? (uint8_t)qbounds.upper_y : upper_y;
       upper_z = write ? (uint8_t)qbounds.upper_z : upper_z;
       // === init child in node ===
       tmp[ 6+i] = data;
       tmp[12+i] = lower_x;
       tmp[18+i] = upper_x;      
       tmp[24+i] = lower_y;
       tmp[30+i] = upper_y;      
       tmp[36+i] = lower_z;
       tmp[42+i] = upper_z;      
     }
     // === write out second part of 64 bytes node ===
     for (uint32_t i=0;i<12;i++)
       dest[4+i] = tmp[i*4+0] | (tmp[i*4+1]<<8) | (tmp[i*4+2]<<16) | (tmp[i*4+3]<<24);
   }
   
   
   uint32_t createLossyCompressedGeometries_initPLOCPrimRefs(sycl::queue &gpu_queue, const RTHWIF_GEOMETRY_DESC **const geometry_desc, const uint32_t numGeoms, uint32_t *scratch_mem, const uint32_t MAX_WGS, BVH2Ploc *const bvh2, const uint32_t prim_type_offset, uint32_t *host_device_tasks, char* lcg_bvh_mem, uint32_t *const lcg_bvh_mem_allocator, double &iteration_time, const bool verbose)    
  {    
    uint32_t numTotalLCGs = 0;
    //char* dest = lcg_bvh_mem;
    for (uint32_t lcgID=0;lcgID<numGeoms;lcgID++)
    {
      if (unlikely(geometry_desc[lcgID] == nullptr)) continue;
      if (geometry_desc[lcgID]->geometryType == RTHWIF_GEOMETRY_TYPE_LOSSY_COMPRESSED_GEOMETRY)
      {
        
        RTHWIF_GEOMETRY_LOSSY_COMPRESSED_GEOMETRY_DESC *geom = (RTHWIF_GEOMETRY_LOSSY_COMPRESSED_GEOMETRY_DESC *)geometry_desc[lcgID];        
        if (geom->numLCMs)
        {          
#if 1
          const uint32_t wgSize = 16;        
          const sycl::nd_range<1> nd_range1(wgSize*geom->numLCMs,sycl::range<1>(wgSize));          
          sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
              sycl::local_accessor< CompressedAABB3f, 1> _clusterPrimBounds(sycl::range<1>(LossyCompressedMeshCluster::MAX_QUADS_PER_CLUSTER),cgh);
              //sycl::local_accessor< QBVH, 1> _clusterPrimBounds(sycl::range<1>(LossyCompressedMeshCluster::MAX_QUADS_PER_CLUSTER),cgh);
              
              cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)
                               {
                                 const uint32_t subgroupLocalID = get_sub_group_local_id();
                                 const uint32_t subgroupSize    = get_sub_group_size();                                           
                                 const uint32_t ID         = item.get_group(0);
                                 const LossyCompressedMeshCluster* const base = (LossyCompressedMeshCluster*)(geom->pLCMs);
                                 const uint32_t clusterID = geom->pLCMIDs[ID];
                                 const LossyCompressedMeshCluster &cluster = base[ clusterID ];                                 
                                 const LossyCompressedMesh &mesh = *cluster.mesh;

                                 const CompressedVertex *const compressedVertices = mesh.compressedVertices + cluster.offsetVertices; 
                                 const CompressedQuadIndices *const compressedIndices = mesh.compressedIndices + cluster.offsetIndices;
          
                                 const Vec3f lower = mesh.bounds.lower;
                                 const Vec3f diag = mesh.bounds.size() * (1.0f / CompressedVertex::RES_PER_DIM);

                                 uint32_t globalBlockID = 0;
                                 if (subgroupLocalID == 0)
                                   globalBlockID = gpu::atomic_add_global(lcg_bvh_mem_allocator,(uint)cluster.numBlocks);
                                 globalBlockID = sub_group_broadcast(globalBlockID,0);
                                 char *const dest = lcg_bvh_mem + globalBlockID*64;

                                 //QuadLeafData *const local_leaf = (QuadLeafData *)_local_leaf.get_pointer();
                                 
                                 QuadLeafData *const leaf = (QuadLeafData*)(dest + (cluster.numBlocks-cluster.numQuads)*64);
                                 CompressedAABB3f *const clusterPrimBounds = _clusterPrimBounds.get_pointer();
                                 //QBVH *const clusterPrimBounds = _clusterPrimBounds.get_pointer();

                                 //const QBVHReference qbvh_ref(to_AABB3f(cluster.bounds.decompress(lower,diag)));

                                 const CompressedAABB3f &clusterBounds = cluster.bounds;
                                 
                                 BVH2Ploc node;                             
                                 node.initLeaf(lcgID,((int64_t)dest-(int64_t)lcg_bvh_mem)/64,to_AABB3f(clusterBounds.decompress(lower,diag)));                               
                                 node.store(&bvh2[prim_type_offset + ID]);
                                 
                                 for (uint32_t q=subgroupLocalID;q<cluster.numQuads;q+=subgroupSize)
                                 {
                                   const uint32_t v0 = compressedIndices[q].v0();
                                   const uint32_t v1 = compressedIndices[q].v1();
                                   const uint32_t v2 = compressedIndices[q].v2();
                                   const uint32_t v3 = compressedIndices[q].v3();

                                   const Vec3f vtx0 = compressedVertices[v0].decompress(lower,diag);
                                   const Vec3f vtx1 = compressedVertices[v1].decompress(lower,diag);
                                   const Vec3f vtx2 = compressedVertices[v2].decompress(lower,diag);
                                   const Vec3f vtx3 = compressedVertices[v3].decompress(lower,diag);

                                   CompressedAABB3f quad_bounds( compressedVertices[v0] );  
                                   quad_bounds.extend( compressedVertices[v1] );  
                                   quad_bounds.extend( compressedVertices[v2] );  
                                   quad_bounds.extend( compressedVertices[v3] );

                                   //BBox3f quad_bounds( vtx0 );  
                                   //quad_bounds.extend( vtx1 );  
                                   //quad_bounds.extend( vtx2 );  
                                   //quad_bounds.extend( vtx3 );                                   
                                   
                                   
                                   const uint32_t geomID = lcgID;
                                   const uint32_t primID0 = clusterID;
                                   const uint32_t primID1 = clusterID;
            
                                   leaf[q] = QuadLeafData( vtx0,vtx1,vtx3,vtx2, 3,2,1, 0, geomID, primID0, primID1, GeometryFlags::OPAQUE, -1);
                                   clusterPrimBounds[q] = quad_bounds;
                                   //clusterPrimBounds[q] = QBVH(to_AABB3f(quad_bounds),qbvh_ref);

                                   //const uint32_t cls = std::min(cluster.numQuads - sub_group_broadcast(q,0),(uint)16);
                                   //copyCLs_from_SLM_to_GlobalMemory(&leaf[q],&local_leaf[q],cls);                                   
                                 }
                                 sub_group_barrier();                                                                    
                                 
                                 uint32_t numPrims = cluster.numQuads;
                                 uint32_t numNodes = ((numPrims+BVH_BRANCHING_FACTOR-1)/BVH_BRANCHING_FACTOR);
                                 char *prev = (char*)leaf;
                                 char *cur  = prev - numNodes*64;
                                 NodeType node_type = NODE_TYPE_QUAD;
                                 while(numPrims > BVH_BRANCHING_FACTOR)
                                 {
                                   for (uint32_t i=subgroupLocalID;i<numNodes;i+=subgroupSize)
                                   {
                                     CompressedAABB3f nodeBounds;
                                     //QBVH nodeBounds;
                                     nodeBounds.init();
                                     const uint32_t offset = i*BVH_BRANCHING_FACTOR;
                                     for (uint32_t j=0;j<BVH_BRANCHING_FACTOR;j++)
                                     {
                                       const uint32_t index = min(offset+j,numPrims-1);
                                       nodeBounds.extend( clusterPrimBounds[index] );
                                     }
                                     const uint32_t numChildren = min(numPrims-offset,(uint)BVH_BRANCHING_FACTOR);
                                     writeNodeFast(&cur[i*64],((int64_t)&prev[64*offset]-(int64_t)&cur[i*64])/64,nodeBounds,numChildren,&clusterPrimBounds[offset],node_type,lower,diag);
                                     //writeNodeFast(&cur[i*64],((int64_t)&prev[64*offset]-(int64_t)&cur[i*64])/64,numChildren,&clusterPrimBounds[offset],node_type,qbvh_ref);
                                     sub_group_barrier();                                   
                                     clusterPrimBounds[i] = nodeBounds;
                                   }
                                   sub_group_barrier();
                                 
                                   node_type = NODE_TYPE_INTERNAL;
                                   numPrims = numNodes;
                                   numNodes = ((numPrims+BVH_BRANCHING_FACTOR-1)/BVH_BRANCHING_FACTOR);
                                   prev = cur;
                                   cur -= numNodes*64;            
                                 }
                                 writeNodeFast(dest,((int64_t)prev-(int64_t)dest)/64,clusterBounds,numPrims,clusterPrimBounds,node_type,lower,diag);
                                 //writeNodeFast(dest,((int64_t)prev-(int64_t)dest)/64,numPrims,clusterPrimBounds,node_type,qbvh_ref);
                                
                               });
            });
#else          
          const uint32_t wgSize = 16;        
          const sycl::nd_range<1> nd_range1(wgSize*geom->numLCMs,sycl::range<1>(wgSize));          
          sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
              sycl::local_accessor< gpu::AABB3f, 1> _clusterPrimBounds(sycl::range<1>(LossyCompressedMeshCluster::MAX_QUADS_PER_CLUSTER),cgh);
              cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)
                               {
                                 const uint32_t subgroupLocalID = get_sub_group_local_id();
                                 const uint32_t subgroupSize    = get_sub_group_size();                                           
                                 const uint32_t ID         = item.get_group(0);
                                 const LossyCompressedMeshCluster* const base = (LossyCompressedMeshCluster*)(geom->pLCMs);
                                 const uint32_t clusterID = geom->pLCMIDs[ID];
                                 const LossyCompressedMeshCluster &cluster = base[ clusterID ];
                                 
                                 const LossyCompressedMesh &mesh = *cluster.mesh;
                                 const CompressedVertex *const compressedVertices = mesh.compressedVertices + cluster.offsetVertices; 
                                 const CompressedQuadIndices *const compressedIndices = mesh.compressedIndices + cluster.offsetIndices;
          
                                 const Vec3f lower = mesh.bounds.lower;
                                 const Vec3f diag = mesh.bounds.size() * (1.0f / CompressedVertex::RES_PER_DIM);

                                 uint32_t globalBlockID = 0;
                                 if (subgroupLocalID == 0)
                                   globalBlockID = gpu::atomic_add_global(lcg_bvh_mem_allocator,(uint)cluster.numBlocks);
                                 globalBlockID = sub_group_broadcast(globalBlockID,0);
                                 char *const dest = lcg_bvh_mem + globalBlockID*64;
          
                                 QuadLeafData *const leaf = (QuadLeafData*)(dest + (cluster.numBlocks-cluster.numQuads)*64);
                                 gpu::AABB3f *const clusterPrimBounds = _clusterPrimBounds.get_pointer();
                                 gpu::AABB3f clusterBounds;
                                 clusterBounds.init();
                                 for (uint32_t q=subgroupLocalID;q<cluster.numQuads;q+=subgroupSize)
                                 {
                                   const uint32_t v0 = compressedIndices[q].v0();
                                   const uint32_t v1 = compressedIndices[q].v1();
                                   const uint32_t v2 = compressedIndices[q].v2();
                                   const uint32_t v3 = compressedIndices[q].v3();

                                   const Vec3f vtx0 = compressedVertices[v0].decompress(lower,diag);
                                   const Vec3f vtx1 = compressedVertices[v1].decompress(lower,diag);
                                   const Vec3f vtx2 = compressedVertices[v2].decompress(lower,diag);
                                   const Vec3f vtx3 = compressedVertices[v3].decompress(lower,diag);

                                   gpu::AABB3f quad_bounds( to_float3(vtx0) );
                                   quad_bounds.extend( to_float3(vtx1) );
                                   quad_bounds.extend( to_float3(vtx2) );
                                   quad_bounds.extend( to_float3(vtx3) );                                 
                                   clusterBounds.extend(quad_bounds);

                                   const uint32_t geomID = lcgID;
                                   const uint32_t primID0 = clusterID;
                                   const uint32_t primID1 = clusterID;
            
                                   leaf[q] = QuadLeafData( vtx0,vtx1,vtx3,vtx2, 3,2,1, 0, geomID, primID0, primID1, GeometryFlags::OPAQUE, -1);
                                   clusterPrimBounds[q] = quad_bounds;
                                 }

                                 clusterBounds = clusterBounds.sub_group_reduce();                                
                                 uint32_t numPrims = cluster.numQuads;
                                 uint32_t numNodes = ((numPrims+BVH_BRANCHING_FACTOR-1)/BVH_BRANCHING_FACTOR);
                                 char *prev = (char*)leaf;
                                 char *cur  = prev - numNodes*64;
                                 NodeType node_type = NODE_TYPE_QUAD;
                                 while(numPrims > BVH_BRANCHING_FACTOR)
                                 {
                                   for (uint32_t i=subgroupLocalID;i<numNodes;i+=subgroupSize)
                                   {
                                     gpu::AABB3f nodeBounds;
                                     nodeBounds.init();
                                     const uint32_t offset = i*BVH_BRANCHING_FACTOR;
                                     for (uint32_t j=0;j<BVH_BRANCHING_FACTOR;j++)
                                     {
                                       const uint32_t index = min(offset+j,numPrims-1);
                                       nodeBounds.extend( clusterPrimBounds[index] );
                                     }
                                     const uint32_t numChildren = min(numPrims-offset,(uint)BVH_BRANCHING_FACTOR);
                                     writeNodeFast(&cur[i*64],((int64_t)&prev[64*offset]-(int64_t)&cur[i*64])/64,nodeBounds,numChildren,&clusterPrimBounds[offset],node_type);
                                     sub_group_barrier();                                   
                                     clusterPrimBounds[i] = nodeBounds;
                                   }
                                   sub_group_barrier();
                                 
                                   node_type = NODE_TYPE_INTERNAL;
                                   numPrims = numNodes;
                                   numNodes = ((numPrims+BVH_BRANCHING_FACTOR-1)/BVH_BRANCHING_FACTOR);
                                   prev = cur;
                                   cur -= numNodes*64;            
                                 }
                                 writeNodeFast(dest,((int64_t)prev-(int64_t)dest)/64,clusterBounds,numPrims,clusterPrimBounds,node_type);          
                                
                                 BVH2Ploc node;                             
                                 node.initLeaf(lcgID,((int64_t)dest-(int64_t)lcg_bvh_mem)/64,clusterBounds);                               
                                 node.store(&bvh2[prim_type_offset + ID]);
                               });
            });
#endif        
          gpu::waitOnEventAndCatchException(queue_event);
        
          if (unlikely(verbose))
            iteration_time += gpu::getDeviceExecutionTiming(queue_event);


#if 0
    static double total_sum = 0.0f;
    static uint32_t entries = 0;
    total_sum += gpu::getDeviceExecutionTiming(queue_event);
    entries++;
    if (entries % (4*4096)) PRINT2("Decomp",total_sum / entries);
    //PRINT4(gpu::getDeviceExecutionTiming(memset_event),gpu::getDeviceExecutionTiming(compute_lod_event),gpu::getDeviceExecutionTiming(select_clusterIDs_event),total);
#endif    

          //PRINT(gpu::getDeviceExecutionTiming(queue_event));
          //*lcg_bvh_mem_allocator = 0;
          // }
          numTotalLCGs += geom->numLCMs;          
        }

        if (geom->numLCGs)
        {
          const uint32_t SIZE_LCG_BVH = estimateLossyCompressedGeometriesSize(1);        
          const uint32_t wgSize = 16;        
          const sycl::nd_range<1> nd_range1(wgSize*geom->numLCGs,sycl::range<1>(wgSize));          
          sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
              sycl::local_accessor< LocalNodeData_subgroup, 1> _local_node(sycl::range<1>(3),cgh); //FIXME: reuse _local_leaf
              sycl::local_accessor< gpu::AABB3f, 1> _local_bounds(sycl::range<1>(12),cgh);
              sycl::local_accessor< Vec3f, 1> _gridPos(sycl::range<1>(9*9),cgh);
            
              cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)
                               {
                                 const uint32_t subgroupLocalID = get_sub_group_local_id();
                                 const uint32_t blockID = subgroupLocalID <  6 ? 0 : (subgroupLocalID <  12 ? 1 : 2);
                               
                                 const uint32_t ID         = item.get_group(0);
                                 const uint32_t start_x[16] = { 0,1,2, 0,1,2, 3,4,5, 3,4,5, 6,7,6,7};
                                 const uint32_t start_y[16] = { 0,0,0, 1,1,1, 0,0,0, 1,1,1, 0,0,1,1};

                                 LocalNodeData_subgroup *local_node = _local_node.get_pointer();                               
                                 gpu::AABB3f *local_bounds          = _local_bounds.get_pointer();
                                 Vec3f *gridPos                     = _gridPos.get_pointer();

                                 LCGBP_State &state = ((LCGBP_State*)(geom->pLCGs))[ID];
                                 const LCGBP *const lcgbp = state.lcgbp;
                                 const uint32_t lgcbp_start_x = state.start_x;
                                 const uint32_t lgcbp_start_y = state.start_y;                               
                                 const uint32_t lgcbp_step = state.step;
                                 
                                 char* dest = lcg_bvh_mem + ID * SIZE_LCG_BVH;
                                 LocalNodeData_subgroup *node = (LocalNodeData_subgroup*)(dest + 3 * 64);
                                 QuadLeafData *leaf = (QuadLeafData*)(dest + (4*3+2+1)*64);
                               
                                 const uint32_t sx = start_x[subgroupLocalID];
                                 const uint32_t sy = start_y[subgroupLocalID];

                                 for (uint32_t y=0;y<9;y++)
                                 {
                                   const uint32_t x = subgroupLocalID;
                                   if (x < 9)
                                     gridPos[y*9+x] = lcgbp->decode(lgcbp_start_x + x*lgcbp_step, lgcbp_start_y + y*lgcbp_step);
                                 }
                                 sub_group_barrier();                               
                               
                                 /* ---- fix cracks if necessary ---- */
                                 if (unlikely(state.lod_diff_levels))
                                 {
                                   const uint32_t i = subgroupLocalID;
                                   const uint32_t diff_top    = state.get_lod_diff_level(0);
                                   const uint32_t diff_right  = state.get_lod_diff_level(1);
                                   const uint32_t diff_bottom = state.get_lod_diff_level(2);
                                   const uint32_t diff_left   = state.get_lod_diff_level(3);
                                   
                                   if (unlikely(diff_top))
                                   {
                                     const uint32_t index = (i>>diff_top)<<diff_top;
                                     const Vec3f p = gridPos[index];
                                     sub_group_barrier();
                                     if (i>0 && i<8)
                                       gridPos[i] = p; 
                                   }
                                   if (unlikely(diff_right))
                                   {
                                     const uint32_t index = (i>>diff_right)<<diff_right;
                                     const Vec3f p = gridPos[index*9+8];
                                     if (i>0 && i<8)
                                       gridPos[i*9+8] = p; 
                                   }
                                   if (unlikely(diff_bottom))
                                   {
                                     const uint32_t index = (i>>diff_bottom)<<diff_bottom;
                                     const Vec3f p = gridPos[8*9+index];
                                     if (i>0 && i<8)
                                       gridPos[8*9+i] = p;                                  
                                   }
                                   if (unlikely(diff_left))
                                   {
                                     const uint32_t index = (i>>diff_left)<<diff_left;
                                     const Vec3f p = gridPos[index*9];
                                     if (i>0 && i<8)
                                       gridPos[i*9] = p;                                 
                                   }                                  
                                 }


                                 /* ---- continous LOD support ---- */
                                 if (unlikely(state.blend))
                                 {
                                   sub_group_barrier();

                                   const uint32_t x = subgroupLocalID;
#if 0
                                   const uint32_t diff_top    = state.get_lod_diff_level(0);
                                   const uint32_t diff_right  = state.get_lod_diff_level(1);
                                   const uint32_t diff_bottom = state.get_lod_diff_level(2);
                                   const uint32_t diff_left   = state.get_lod_diff_level(3);
                                 
                                   const uint32_t sy = diff_top ? 1 : 0; 
                                   const uint32_t ey = diff_bottom ? 7 : 8; 
                                   const uint32_t sx = diff_left ? 1 : 0; 
                                   const uint32_t ex = diff_right ? 7 : 8;
#else
                                   const uint32_t sy = (state.flags & TOP_BORDER) ? 1 : 0; 
                                   const uint32_t ey = (state.flags & BOTTOM_BORDER) ? 7 : 8; 
                                   const uint32_t sx = (state.flags & LEFT_BORDER) ? 1 : 0; 
                                   const uint32_t ex = (state.flags & RIGHT_BORDER) ? 7 : 8;
                                 
#endif                                 
                                 
                                   if (x>=sx && x<=ex)
                                     for (uint32_t y=sy;y<=ey;y++)
                                     {
                                       uint32_t state_blend = state.blend;
                                       const float blend_factor = (float)state_blend * 1.0f / 255.0f;                                     
                                     
                                       uint32_t blend_x = x + ((x < 4) ? 1 : -1);
                                       uint32_t blend_y = y + ((y < 4) ? 1 : -1);
                                       blend_x = (x%2) ? blend_x : x;
                                       blend_y = (y%2) ? blend_y : y;
                                       if (blend_x != x || blend_y != y)
                                       {
                                         const Vec3f blend_v = lerp(gridPos[y*9+x],gridPos[blend_y*9+blend_x],blend_factor);
                                         gridPos[y*9+x] = blend_v;
                                       }
                                     }
                                   sub_group_barrier();                                                                      
                                 }
                               

                              
                                 /* --------------------------------- */
                               
                                 for (uint32_t rows=0;rows<4;rows++,leaf+=16,node+=3)
                                 {
                                   const uint32_t x = sx; 
                                   const uint32_t y = sy + 2*rows;
                                   const Vec3f p0 = gridPos[9*(y+0)+(x+0)];
                                   const Vec3f p1 = gridPos[9*(y+0)+(x+1)];
                                   const Vec3f p2 = gridPos[9*(y+1)+(x+1)];
                                   const Vec3f p3 = gridPos[9*(y+1)+(x+0)];

                                   const uint32_t geomID = lcgID;
                                   const uint32_t primID = (ID << RTC_LOSSY_COMPRESSED_GRID_LOCAL_ID_SHIFT) | 2*(y*RTC_LOSSY_COMPRESSED_GRID_QUAD_RES+x) ; //y*8+x; 

                                   leaf[subgroupLocalID] = QuadLeafData( p0,p1,p3,p2, 3,2,1, 0, geomID, primID, primID+1, GeometryFlags::OPAQUE, -1);
                                 
                                   gpu::AABB3f quad_bounds( to_float3(p0) );
                                   quad_bounds.extend( to_float3(p1) );
                                   quad_bounds.extend( to_float3(p2) );
                                   quad_bounds.extend( to_float3(p3) );
                                 
                                   if (blockID == 0)
                                     local_bounds[rows*3+0] = quad_bounds.sub_group_reduce();
                                   else if (blockID == 1)
                                     local_bounds[rows*3+1] = quad_bounds.sub_group_reduce();
                                   else if (blockID == 2)
                                     local_bounds[rows*3+2] = quad_bounds.sub_group_reduce();
                                 
                                   sub_group_barrier();
                                 
                                   const uint32_t offset = ((char*)(leaf+blockID*6) - (char*)(node+blockID))/64;                                 
                                   writeNode_subgroup(local_node[blockID],offset,local_bounds[rows*3+blockID],quad_bounds,blockID == 2 ? 4 : 6,subgroupLocalID-blockID*6,NODE_TYPE_QUAD);
                                   sub_group_barrier();                                 
                                   copyCLs_from_SLM_to_GlobalMemory(node,local_node,3);                                 
                                 }

                                 sub_group_barrier();

                                 // === two internal full 6-wide nodes ===
                                 node = (LocalNodeData_subgroup*)(dest + 1 * 64);
                                 {
                                   gpu::AABB3f child_bounds;
                                   child_bounds.init();
                                 
                                   if (subgroupLocalID < 12)
                                     child_bounds = local_bounds[subgroupLocalID];                               
                                   const gpu::AABB3f parent_bounds = child_bounds.sub_group_reduce();
                                 
                                   if (subgroupLocalID < 12)                               
                                     writeNode_subgroup(local_node[blockID],blockID == 0 ? 2 : 7,parent_bounds,child_bounds,6,subgroupLocalID-blockID*6,NODE_TYPE_INTERNAL);
                                   sub_group_barrier();
                                   copyCLs_from_SLM_to_GlobalMemory(node,local_node,2);
                                   sub_group_barrier();

                                   if (blockID == 0)
                                     local_bounds[0] = child_bounds.sub_group_reduce();
                                   else if (blockID == 1)
                                     local_bounds[1] = child_bounds.sub_group_reduce();
                                   sub_group_barrier();                                 
                                 }
                              
                                 // === one internal 2-wide root node ===

                                 node = (LocalNodeData_subgroup*)(dest + 0 * 64);
                                 gpu::AABB3f child_bounds;
                                 child_bounds.init();
                                 
                                 if (subgroupLocalID < 2)
                                   child_bounds = local_bounds[subgroupLocalID];                               
                                 const gpu::AABB3f parent_bounds = child_bounds.sub_group_reduce();
                                 
                                 if (subgroupLocalID < 2)                               
                                   writeNode_subgroup(local_node[0],1,parent_bounds,child_bounds,2,subgroupLocalID,NODE_TYPE_INTERNAL);
                                 sub_group_barrier();
                                 copyCLs_from_SLM_to_GlobalMemory(node,local_node,1);
                               
                                 if (subgroupLocalID == 0)
                                 {
                                   BVH2Ploc node;                             
                                   node.initLeaf(lcgID,ID*SIZE_LCG_BVH/64,parent_bounds);                               
                                   node.store(&bvh2[prim_type_offset + ID]);
                                 }
                               
                               });
            });
        
          gpu::waitOnEventAndCatchException(queue_event);
        
          if (unlikely(verbose))
            iteration_time += gpu::getDeviceExecutionTiming(queue_event);
          //PRINT( gpu::getDeviceExecutionTiming(queue_event) );
          numTotalLCGs += geom->numLCGs;          
        }
      }
    }
    return numTotalLCGs;
  }
   

  // ===================================================================================================================================================================================
  // =========================================================================== DISTANCE FUNCTION =====================================================================================
  // ===================================================================================================================================================================================

   float distanceFct(const gpu::AABB3f &bounds0,const gpu::AABB3f &bounds1)
  {
    const gpu::AABB3f bounds = gpu::merge(bounds0,bounds1);
    return bounds.halfArea();
  }
  

  // ====================================================================================================================================================================================
  // ================================================================================= SETUP ============================================================================================
  // ====================================================================================================================================================================================
  

   void computeCentroidGeometryBounds(sycl::queue &gpu_queue, gpu::AABB3f *geometryBounds, gpu::AABB3f *centroidBounds, const BVH2Ploc *const bvh2, const uint32_t numPrimitives, double &iteration_time, const bool verbose)
  {
    const uint32_t wgSize = LARGE_WG_SIZE;
    const sycl::nd_range<1> nd_range1(gpu::alignTo(numPrimitives,wgSize),sycl::range<1>(wgSize));          
    sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
        sycl::local_accessor< gpu::AABB3f, 0> _local_geometry_aabb(cgh);
        sycl::local_accessor< gpu::AABB3f, 0> _local_centroid_aabb(cgh);
                                                       
        cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)
                         {
                           const uint32_t localID        = item.get_local_id(0);
                           const uint32_t subgroupLocalID = get_sub_group_local_id();
                           const uint32_t ID = item.get_global_id(0);                                                                                                               
                           gpu::AABB3f *local_geometry_aabb = _local_geometry_aabb.get_pointer();
                           gpu::AABB3f *local_centroid_aabb = _local_centroid_aabb.get_pointer();
                                                                                    
                           gpu::AABB3f geometry_aabb;
                           gpu::AABB3f centroid_aabb;
                           geometry_aabb.init();
                           centroid_aabb.init();

                           if (ID < numPrimitives)
                           {                             
                             const gpu::AABB3f aabb_geom = bvh2[ID].bounds;
                             const gpu::AABB3f aabb_centroid(aabb_geom.centroid2()); 
                             geometry_aabb.extend(aabb_geom);
                             centroid_aabb.extend(aabb_centroid);		      
                                                                                    
                             if (localID == 0)
                             {
                               local_geometry_aabb->init();
                               local_centroid_aabb->init();                                                                                      
                             }
                           }
                           item.barrier(sycl::access::fence_space::local_space);

                           geometry_aabb = geometry_aabb.sub_group_reduce(); 
                           centroid_aabb = centroid_aabb.sub_group_reduce(); 

                           if (subgroupLocalID == 0) 
                           {
                             geometry_aabb.atomic_merge_local(*local_geometry_aabb);
                             centroid_aabb.atomic_merge_local(*local_centroid_aabb);
                           }
                           
                           item.barrier(sycl::access::fence_space::local_space);

                           if (localID == 0)
                           {
                             local_geometry_aabb->atomic_merge_global(*geometryBounds);
                             local_centroid_aabb->atomic_merge_global(*centroidBounds);
                           }
                         });
		  
      });
    gpu::waitOnEventAndCatchException(queue_event);
    if (unlikely(verbose)) iteration_time += gpu::getDeviceExecutionTiming(queue_event);
  }
  
  template<typename type>
    __forceinline void computeMortonCodes64Bit(sycl::queue &gpu_queue, const gpu::AABB3f *const _centroidBounds, type *const mc0, const BVH2Ploc *const bvh2, const uint32_t numPrimitives, const uint32_t shift, const uint64_t mask, double &iteration_time, const bool verbose)    
  {
    const uint32_t wgSize = 16;
    const sycl::nd_range<1> nd_range1(gpu::alignTo(numPrimitives,wgSize),sycl::range<1>(wgSize));              
    sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(32)
                         {
                           const uint32_t globalID     = item.get_global_id(0);                                                                    
                           if (globalID < numPrimitives)
                           {
                             const gpu::AABB3f centroidBounds(*_centroidBounds);
                             const uint32_t i = globalID;

                             const float3 lower = centroidBounds.lower();                                                                      
                             const uint32_t   grid_size   = 1 << type::GRID_SHIFT;
                             const float3 grid_base(lower.x(),lower.y(),lower.z());
                             const float3 grid_extend(centroidBounds.maxDiagDim());                             
                             const float3 grid_scale = cselect( int3(grid_extend != 0.0f), (grid_size * 0.99f)/grid_extend, float3(0.0f)); 

                             /* calculate and store morton code */
                             const gpu::AABB3f bounds3f = bvh2[i].bounds;
                             const float3 centroid = bounds3f.centroid2();
                             const float3 gridpos_f = (centroid-grid_base)*grid_scale;                                                                      
                             const sycl::uint3 gridpos = gridpos_f.convert<uint32_t,sycl::rounding_mode::rtz>();                             
                             const uint64_t code = (gpu::bitInterleave3D_64bits(gridpos)>>shift) & mask;
                             mc0[i] = type(code,i); 
                           }
                         });
		  
      });
    gpu::waitOnEventAndCatchException(queue_event);
    if (unlikely(verbose)) iteration_time += gpu::getDeviceExecutionTiming(queue_event);      
  }

  template<typename type>
    __forceinline void computeMortonCodes64Bit_SaveMSBBits(sycl::queue &gpu_queue, const gpu::AABB3f *const _centroidBounds, type *const mc0, const BVH2Ploc *const bvh2, uint32_t *const high, const uint32_t numPrimitives, double &iteration_time, const bool verbose)    
  {
    const uint32_t wgSize = 16; //256;
    const sycl::nd_range<1> nd_range1(gpu::alignTo(numPrimitives,wgSize),sycl::range<1>(wgSize));              
    sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(32)
                         {
                           const uint32_t globalID     = item.get_global_id(0);                                                                    
                           if (globalID < numPrimitives)
                           {
                             const gpu::AABB3f centroidBounds(*_centroidBounds);
                             const uint32_t i = globalID;
                             const float3 lower = centroidBounds.lower();                                                                      
                             const uint32_t   grid_size   = 1 << type::GRID_SHIFT;
                             const float3 grid_base(lower.x(),lower.y(),lower.z());
                             const float3 grid_extend(centroidBounds.maxDiagDim());                             
                             const float3 grid_scale = cselect( int3(grid_extend != 0.0f), (grid_size * 0.99f)/grid_extend, float3(0.0f));
                             /* calculate and store morton code */
                             const gpu::AABB3f bounds3f = bvh2[i].bounds;
                             const float3 centroid = bounds3f.centroid2();
                             const float3 gridpos_f = (centroid-grid_base)*grid_scale;                                                                      
                             const sycl::uint3 gridpos = gridpos_f.convert<uint32_t,sycl::rounding_mode::rtz>();
                             const uint64_t mask = (((uint64_t)1 << 32)-1);
                             const uint64_t code = gpu::bitInterleave3D_64bits(gridpos);
                             high[i] = code >> 32;
                             mc0[i]  = type(code & mask,i); 
                           }
                         });
		  
      });
    gpu::waitOnEventAndCatchException(queue_event);
    if (unlikely(verbose)) iteration_time += gpu::getDeviceExecutionTiming(queue_event);      
  }
  
  template<typename type>
    __forceinline sycl::event restoreMSBBits(sycl::queue &gpu_queue, type *const mc0, uint32_t *const high, const uint32_t numPrimitives, sycl::event &input_event, const bool verbose)    
  {
    const uint32_t wgSize = 16; 
    const sycl::nd_range<1> nd_range1(gpu::alignTo(numPrimitives,wgSize),sycl::range<1>(wgSize));              
    sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
        cgh.depends_on(input_event);        
        cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(32)
                         {
                           const uint32_t globalID     = item.get_global_id(0);                                                                    
                           if (globalID < numPrimitives)
                           {
                             const uint32_t index = mc0[globalID].getIndex();
                             const uint64_t code = high[index];                             
                             mc0[globalID] = type(code,index);                                        
                           }
                         });
		  
      });
    return queue_event;
  }
  

  template<typename type>  
    __forceinline void initClusters(sycl::queue &gpu_queue, type *const mc0, const BVH2Ploc *const bvh2, uint32_t *const cluster_index, BVH2SubTreeState *const bvh2_subtree_size, const uint32_t numPrimitives, double &iteration_time, const bool verbose)    
  {
    static const uint32_t INIT_CLUSTERS_WG_SIZE = 256;    
    const sycl::nd_range<1> nd_range1(sycl::range<1>(gpu::alignTo(numPrimitives,INIT_CLUSTERS_WG_SIZE)),sycl::range<1>(INIT_CLUSTERS_WG_SIZE)); 
    sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)
                         {
                           const uint32_t globalID     = item.get_global_id(0);
                           if (globalID < numPrimitives)
                           {
                             const uint32_t index = mc0[globalID].getIndex();
                             bvh2_subtree_size[globalID] = BVH2SubTreeState(1,1);
                             cluster_index[globalID] = index;
                           }
                         });
                                                       
      });
    gpu::waitOnEventAndCatchException(queue_event);
    if (unlikely(verbose)) iteration_time += gpu::getDeviceExecutionTiming(queue_event);      
  }


  // ====================================================================================================================================================================================
  // ====================================================================================================================================================================================
  // ====================================================================================================================================================================================
  

  __forceinline  uint32_t encodeRelativeOffset(const int localID, const int neighbor)
  {
    const int sOffset = neighbor - localID;
    const uint32_t uOffset = sycl::abs(sOffset)-1;
    return (uOffset<<1) | ((uint)sOffset>>31);
  }

  __forceinline  uint32_t encodeRelativeOffsetFast(const uint32_t ID, const uint32_t neighbor) // === neighbor must be larger than ID ===
  {
    const uint32_t uOffset = neighbor - ID - 1;
    return uOffset<<1;
  }  

  __forceinline int decodeRelativeOffset(const int localID, const uint32_t offset, const uint32_t ID)
  {
    const uint32_t off = (offset>>1)+1;
    return localID + (((offset^ID) % 2 == 0) ? (int)off : -(int)off);
  }


  __forceinline void findNN(const uint32_t localID, const uint32_t ID, const uint32_t local_window_size, const gpu::AABB3f *const cached_bounds, uint32_t *const cached_neighbor, const uint32_t SEARCH_RADIUS_SHIFT,const bool forceNeighborMerge=false)
  {
    /* ---------------------------------------------------------- */                                                       
    /* --- compute nearest neighbor and store result into SLM --- */
    /* ---------------------------------------------------------- */
    const uint32_t SEARCH_RADIUS =    (uint)1<<SEARCH_RADIUS_SHIFT;            
    uint32_t encode_mask   = ~(((uint)1<<(SEARCH_RADIUS_SHIFT+1))-1);

    /* --- only look at exponent if we need to force a merge --- */
    if (forceNeighborMerge)
      encode_mask =~(((uint)1<<(24))-1);
    
    
    uint32_t min_area_index = -1;
    const gpu::AABB3f bounds0 = cached_bounds[localID];              
    for (uint32_t r=1;r<=SEARCH_RADIUS && (localID + r < local_window_size) ;r++)
    {
      const gpu::AABB3f bounds1 = cached_bounds[localID+r];
      const float new_area = distanceFct(bounds0,bounds1);
      uint32_t new_area_i = ((gpu::as_uint(new_area) << 1) & encode_mask);
#if EQUAL_DISTANCES_WORKAROUND == 1
      const uint32_t encode0 = encodeRelativeOffsetFast(localID  ,localID+r);                
      const uint32_t new_area_index0 = new_area_i | encode0 | (ID&1); 
      const uint32_t new_area_index1 = new_area_i | encode0 | (((ID+r)&1)^1); 
#else
      const uint32_t encode0 = encodeRelativeOffset(localID  ,localID+r);                
      const uint32_t encode1 = encodeRelativeOffset(localID+r,localID);   // faster            
      const uint32_t new_area_index0 = new_area_i | encode0;
      const uint32_t new_area_index1 = new_area_i | encode1;            
#endif
      min_area_index = min(min_area_index,new_area_index0);                  
      gpu::atomic_min_local(&cached_neighbor[localID+r],new_area_index1);                  
    }
    gpu::atomic_min_local(&cached_neighbor[localID],min_area_index);

  }


  __forceinline uint32_t getNewClusterIndexCreateBVH2Node(const uint32_t localID, const uint32_t ID, const uint32_t maxID, const uint32_t local_window_start, const uint32_t *const cluster_index_source, const gpu::AABB3f *const cached_bounds, const uint32_t *const cached_neighbor,const uint32_t *const cached_clusterID, BVH2Ploc *const bvh2, uint32_t *const bvh2_index_allocator, BVH2SubTreeState *const bvh2_subtree_size, const uint32_t SEARCH_RADIUS_SHIFT)
  {
    const uint32_t decode_mask =  (((uint)1<<(SEARCH_RADIUS_SHIFT+1))-1);
              
    uint32_t new_cluster_index = -1;
    if (ID < maxID)
    {
      new_cluster_index = cluster_index_source[ID]; // prevents partial writes later 
      //new_cluster_index = cached_clusterID[ID-local_window_start]; //cluster_index_source[ID];
#if EQUAL_DISTANCES_WORKAROUND == 1          
      uint32_t n_i     = decodeRelativeOffset(ID -local_window_start,cached_neighbor[ID -local_window_start] & decode_mask,  ID) + local_window_start;
      uint32_t n_i_n_i = decodeRelativeOffset(n_i-local_window_start,cached_neighbor[n_i-local_window_start] & decode_mask, n_i) + local_window_start;
#else
      uint32_t n_i     = decodeRelativeOffset(ID -local_window_start,cached_neighbor[ID -local_window_start] & decode_mask,  0) + local_window_start;
      uint32_t n_i_n_i = decodeRelativeOffset(n_i-local_window_start,cached_neighbor[n_i-local_window_start] & decode_mask,  0) + local_window_start;          
#endif          
      const gpu::AABB3f bounds = cached_bounds[ID - local_window_start];
      
      if (ID == n_i_n_i)  
      {
        if (ID < n_i)
        {
          const uint32_t leftIndex  = cached_clusterID[ID-local_window_start]; //cluster_index_source[ID];
          const uint32_t rightIndex = cached_clusterID[n_i-local_window_start]; //cluster_index_source[n_i];
          const gpu::AABB3f &leftBounds  = bounds;
          const gpu::AABB3f &rightBounds = cached_bounds[n_i-local_window_start];

          /* --- reduce per subgroup to lower pressure on global atomic counter --- */                                                         
          sycl::atomic_ref<uint, sycl::memory_order::relaxed, sycl::memory_scope::device,sycl::access::address_space::global_space> bvh2_counter(*bvh2_index_allocator);
          const uint32_t bvh2_index = gpu::atomic_add_global_sub_group_shared(bvh2_counter,1);

          /* --- store new BVH2 node --- */
          bvh2[bvh2_index].init(leftIndex,rightIndex,gpu::merge(leftBounds,rightBounds),bvh2_subtree_size[leftIndex],bvh2_subtree_size[rightIndex]);                                                         
          bvh2_subtree_size[bvh2_index] = BVH2SubTreeState(bvh2_subtree_size[leftIndex],bvh2_subtree_size[rightIndex]);
          new_cluster_index = bvh2_index;
        }
        else
          new_cluster_index = -1; /* --- second item of pair with the larger index disables the slot --- */
      }
    }
    return new_cluster_index;
  }
  
  void iteratePLOC (sycl::queue &gpu_queue, PLOCGlobals *const globals, BVH2Ploc *const bvh2, uint32_t *const cluster_index_source, uint32_t *const cluster_index_dest, BVH2SubTreeState *const bvh2_subtree_size, uint32_t *const scratch_mem, const uint32_t numPrims, const uint32_t NN_SEARCH_WG_NUM, uint32_t *host_device_tasks, const uint32_t SEARCH_RADIUS_SHIFT, double &iteration_time, const bool forceNeighborMerge, const bool verbose)    
  {
    static const uint32_t NN_SEARCH_SUB_GROUP_WIDTH = 16;
    static const uint32_t NN_SEARCH_WG_SIZE         = LARGE_WG_SIZE;
    uint32_t *const bvh2_index_allocator            = &globals->bvh2_index_allocator;
    
    sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
        sycl::local_accessor< gpu::AABB3f, 1> cached_bounds  (sycl::range<1>(NN_SEARCH_WG_SIZE),cgh);
        sycl::local_accessor< uint32_t       , 1> cached_neighbor(sycl::range<1>(NN_SEARCH_WG_SIZE),cgh);
        sycl::local_accessor< uint32_t       , 1> cached_clusterID(sycl::range<1>(NN_SEARCH_WG_SIZE),cgh);        
        sycl::local_accessor< uint32_t       , 1> counts(sycl::range<1>((NN_SEARCH_WG_SIZE/NN_SEARCH_SUB_GROUP_WIDTH)),cgh);
        sycl::local_accessor< uint32_t       , 1> counts_prefix_sum(sycl::range<1>((NN_SEARCH_WG_SIZE/NN_SEARCH_SUB_GROUP_WIDTH)),cgh);        
        sycl::local_accessor< uint32_t      ,  0> _wgID(cgh);
        sycl::local_accessor< uint32_t      ,  0> _global_count_prefix_sum(cgh);
                
        const sycl::nd_range<1> nd_range(sycl::range<1>(NN_SEARCH_WG_NUM*NN_SEARCH_WG_SIZE),sycl::range<1>(NN_SEARCH_WG_SIZE));		  
        cgh.parallel_for(nd_range,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(NN_SEARCH_SUB_GROUP_WIDTH) {
            const uint32_t localID        = item.get_local_id(0);
            const uint32_t localSize       = item.get_local_range().size();            
            const uint32_t SEARCH_RADIUS   = (uint)1<<SEARCH_RADIUS_SHIFT;            
            const uint32_t WORKING_WG_SIZE = NN_SEARCH_WG_SIZE - 4*SEARCH_RADIUS; /* reducing working group set size to LARGE_WG_SIZE - 4 * radius to avoid loops */
            

            uint32_t &wgID = *_wgID.get_pointer();
            uint32_t &global_count_prefix_sum = *_global_count_prefix_sum.get_pointer();

            if (localID == 0)
              wgID = gpu::atomic_add_global(&globals->wgID,(uint)1);

            item.barrier(sycl::access::fence_space::local_space);
            
            const uint32_t groupID        = wgID;                                                             
            const uint32_t startID        = ((size_t)groupID + 0)*(size_t)numPrims / NN_SEARCH_WG_NUM;
            const uint32_t endID          = ((size_t)groupID + 1)*(size_t)numPrims / NN_SEARCH_WG_NUM;
            const uint32_t sizeID         = endID-startID;
            const uint32_t aligned_sizeID = gpu::alignTo(sizeID,WORKING_WG_SIZE);
            
            
            uint32_t total_offset = 0;                                                     
            for (uint32_t t=0;t<aligned_sizeID;t+=WORKING_WG_SIZE)
            {
              /* -------------------------------------------------------- */                                                       
              /* --- copy AABBs from cluster representatives into SLM --- */
              /* -------------------------------------------------------- */
                                                       
              const int local_window_start = max((int)(startID+t                )-2*(int)SEARCH_RADIUS  ,(int)0);
              const int local_window_end   = min((int)(startID+t+WORKING_WG_SIZE)+2*(int)SEARCH_RADIUS+1,(int)numPrims);
              const int local_window_size = local_window_end - local_window_start;
              const uint32_t ID = startID + localID + t;
              const uint32_t maxID = min(startID + t + WORKING_WG_SIZE,endID);

              const uint32_t clusterID = cluster_index_source[min(local_window_start+(int)localID,local_window_end-1)];
              cached_bounds[localID] = bvh2[clusterID].bounds;
              cached_clusterID[localID] = clusterID;
              cached_neighbor[localID] = -1;
              
              item.barrier(sycl::access::fence_space::local_space);

              /* ---------------------------------------------------------- */                                                       
              /* --- compute nearest neighbor and store result into SLM --- */
              /* ---------------------------------------------------------- */

              findNN(localID,ID,local_window_size,cached_bounds.get_pointer(),cached_neighbor.get_pointer(),SEARCH_RADIUS_SHIFT,forceNeighborMerge);

              item.barrier(sycl::access::fence_space::local_space);
              
              /* ---------------------------------------------------------- */                                                       
              /* --- merge valid nearest neighbors and create bvh2 node --- */
              /* ---------------------------------------------------------- */

              const uint32_t new_cluster_index = getNewClusterIndexCreateBVH2Node(localID,ID,maxID,local_window_start,cluster_index_source,
                                                                              cached_bounds.get_pointer(),cached_neighbor.get_pointer(),cached_clusterID.get_pointer(),
                                                                              bvh2,bvh2_index_allocator,bvh2_subtree_size,
                                                                              SEARCH_RADIUS_SHIFT);

              const uint32_t flag = new_cluster_index != -1 ? 1 : 0;
              const uint32_t ps = ID < maxID ? flag : 0;
              uint32_t total_reduction = 0;
              const uint32_t p_sum = startID + total_offset + prefixSumWorkgroup(ps,NN_SEARCH_WG_SIZE,counts.get_pointer(),counts_prefix_sum.get_pointer(),item,total_reduction);    

              /* --- store cluster representative into destination array --- */                                                                 
              if (ID < maxID)
                if (new_cluster_index != -1)
                  cluster_index_dest[p_sum] = new_cluster_index;
          
              total_offset += total_reduction;                                          
            }

            /* ----------------------------------------------------------------------------------------- */                                                       
            /* --- store number of valid cluster representatives into scratch mem and set valid flag --- */
            /* ----------------------------------------------------------------------------------------- */

            const uint32_t flag = (uint)1<<31;            
            const uint32_t mask = ~flag;
            
            if (localID == 0)
            {
              sycl::atomic_ref<uint,sycl::memory_order::relaxed, sycl::memory_scope::device,sycl::access::address_space::global_space> scratch_mem_counter(scratch_mem[groupID]);
              scratch_mem_counter.store(total_offset | flag);              
            }

            global_count_prefix_sum = 0;            
            
            item.barrier(sycl::access::fence_space::global_and_local);

            // =======================================            
            // wait until earlier WGs finished as well
            // =======================================

            if (localID < groupID /*NN_SEARCH_WG_NUM */)
            {
              sycl::atomic_ref<uint, sycl::memory_order::acq_rel, sycl::memory_scope::device,sycl::access::address_space::global_space> global_state(scratch_mem[localID]);
              uint32_t c = 0;
              while( ((c = global_state.load()) & flag) == 0 );
              if (c)
                gpu::atomic_add_local(&global_count_prefix_sum,(uint)c & mask);
            }
            
            item.barrier(sycl::access::fence_space::local_space);

            /* ---------------------------------------------------- */                                                       
            /* --- prefix sum over per WG counts in scratch mem --- */
            /* ---------------------------------------------------- */
            const uint32_t active_count = total_offset;
            const uint32_t global_offset = global_count_prefix_sum;            
              
            for (uint32_t t=localID;t<active_count;t+=localSize)
              cluster_index_source[global_offset + t] = cluster_index_dest[startID + t];                                                               

            /* -------------------------------------------------- */                                                       
            /* --- update number of clusters after compaction --- */
            /* -------------------------------------------------- */
                                         
            if (localID == 0 && groupID == NN_SEARCH_WG_NUM-1) // need to be the last group as only this one waits until all previous are done              
              *host_device_tasks = global_offset + active_count;
            
            /* -------------------------------- */                                                       
            /* --- last WG does the cleanup --- */
            /* -------------------------------- */

            if (localID == 0)
            {
              const uint32_t syncID = gpu::atomic_add_global(&globals->sync,(uint)1);
              if (syncID == NN_SEARCH_WG_NUM-1)
              {
                /* --- reset atomics --- */
                globals->wgID = 0;
                globals->sync = 0;
                /* --- reset scratch_mem --- */
                for (uint32_t i=0;i<NN_SEARCH_WG_NUM;i++) 
                  scratch_mem[i] = 0;                
              }
            }            
                                                     
          });		  
      });
    gpu::waitOnEventAndCatchException(queue_event);
    if (unlikely(verbose)) iteration_time += gpu::getDeviceExecutionTiming(queue_event);
  }
  
  // ====================================================================================================================================================================================
  // ====================================================================================================================================================================================
  // ====================================================================================================================================================================================
  
  
  __forceinline uint32_t wgBuild(sycl::nd_item<1> &item,
                             uint32_t *const bvh2_index_allocator,
                             const uint32_t startID,
                             const uint32_t endID,
                             BVH2Ploc *const bvh2,
                             uint32_t *const global_cluster_index_source,
                             uint32_t *const global_cluster_index_dest,
                             BVH2SubTreeState *const bvh2_subtree_size,
                             gpu::AABB3f *const cached_bounds,
                             uint32_t *const cached_neighbor,
                             uint32_t *const cached_clusterID,                             
                             uint32_t *const counts,
                             uint32_t *const counts_prefix_sum,                             
                             uint32_t &active_counter,
                             const uint32_t BOTTOM_UP_THRESHOLD,
                             const uint32_t SEARCH_RADIUS_SHIFT,
                             const uint32_t SINGLE_WG_SIZE = LARGE_WG_SIZE)
  {
    const uint32_t localID         = item.get_local_id(0);
    const uint32_t localSize       = item.get_local_range().size();
    const uint32_t SEARCH_RADIUS   = (uint)1<<SEARCH_RADIUS_SHIFT;
    const uint32_t WORKING_WG_SIZE = SINGLE_WG_SIZE - 4*SEARCH_RADIUS; 
    
    uint32_t *const cluster_index_source = &global_cluster_index_source[startID];
    uint32_t *const cluster_index_dest   = &global_cluster_index_dest[startID];

    float ratio = 100.0f;
    
    uint32_t numPrims = endID-startID; 
    while(numPrims>BOTTOM_UP_THRESHOLD)
    {
      const uint32_t aligned_numPrims = gpu::alignTo(numPrims,WORKING_WG_SIZE);

      uint32_t total_offset = 0;
      for (uint32_t t=0;t<aligned_numPrims;t+=WORKING_WG_SIZE)
      {
        /* -------------------------------------------------------- */                                                       
        /* --- copy AABBs from cluster representatives into SLM --- */
        /* -------------------------------------------------------- */
            
        const int local_window_start = max((int)(t                )-2*(int)SEARCH_RADIUS  ,(int)0);
        const int local_window_end   = min((int)(t+WORKING_WG_SIZE)+2*(int)SEARCH_RADIUS+1,(int)numPrims);
        const int local_window_size = local_window_end - local_window_start;
        
        const uint32_t ID    = localID + t;                                                             
        const uint32_t maxID = min(t + WORKING_WG_SIZE,numPrims);                                                         

        /* --- fill the SLM bounds cache --- */
        
        const uint32_t clusterID = cluster_index_source[min(local_window_start+(int)localID,local_window_end-1)];
        cached_bounds[localID] = bvh2[clusterID].bounds;
        cached_clusterID[localID] = clusterID;        
        cached_neighbor[localID] = -1;              
        

        item.barrier(sycl::access::fence_space::local_space);
        

        /* ---------------------------------------------------------- */                                                       
        /* --- compute nearest neighbor and store result into SLM --- */
        /* ---------------------------------------------------------- */
        findNN(localID,ID,local_window_size,cached_bounds,cached_neighbor,SEARCH_RADIUS_SHIFT,ratio < TOP_LEVEL_RATIO);
        
        item.barrier(sycl::access::fence_space::local_space);
        
        /* ---------------------------------------------------------- */                                                       
        /* --- merge valid nearest neighbors and create bvh2 node --- */
        /* ---------------------------------------------------------- */

        const uint32_t new_cluster_index = getNewClusterIndexCreateBVH2Node(localID,ID,maxID,local_window_start,cluster_index_source,
                                                                        cached_bounds,cached_neighbor,cached_clusterID,
                                                                        bvh2,bvh2_index_allocator,bvh2_subtree_size,
                                                                        SEARCH_RADIUS_SHIFT);
        
        const uint32_t flag = new_cluster_index != -1 ? 1 : 0;
        const uint32_t ps = ID < maxID ? flag : 0;

        uint32_t total_reduction = 0;
        const uint32_t p_sum = total_offset + prefixSumWorkgroup(ps,SINGLE_WG_SIZE,counts,counts_prefix_sum,item,total_reduction);

        /* --- store cluster representative into destination array --- */                                                                 
        if (ID < maxID)
          if (new_cluster_index != -1)
            cluster_index_dest[p_sum] = new_cluster_index;
          
        total_offset += total_reduction;              
      }

      item.barrier(sycl::access::fence_space::global_and_local);

      /*-- copy elements back from dest to source -- */
      
      for (uint32_t t=localID;t<total_offset;t+=localSize)
        cluster_index_source[t] = cluster_index_dest[t];      

      item.barrier(sycl::access::fence_space::local_space);

      const uint32_t new_numPrims = total_offset;
      
      ratio = (float)(numPrims-new_numPrims) / numPrims * 100.0f;
      numPrims = total_offset;
    }
    return numPrims; /* return number of remaining cluster reps */
  }

  // ====================================================================================================================================================================================
  // ====================================================================================================================================================================================
  // ====================================================================================================================================================================================
  
  void singleWGBuild(sycl::queue &gpu_queue, PLOCGlobals *const globals, BVH2Ploc *const bvh2, uint32_t *const cluster_index_source, uint32_t *const cluster_index_dest, BVH2SubTreeState *const bvh2_subtree_size, const uint32_t numPrimitives, const uint32_t SEARCH_RADIUS_SHIFT, double &iteration_time, const bool verbose)
  {
    static const uint32_t SINGLE_WG_SUB_GROUP_WIDTH = 16;
    static const uint32_t SINGLE_WG_SIZE = LARGE_WG_SIZE;
    uint32_t *const bvh2_index_allocator = &globals->bvh2_index_allocator;
    
    sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
        const sycl::nd_range<1> nd_range(SINGLE_WG_SIZE,sycl::range<1>(SINGLE_WG_SIZE));

        /* local variables */
        sycl::local_accessor< gpu::AABB3f, 1> cached_bounds  (sycl::range<1>(SINGLE_WG_SIZE),cgh);
        sycl::local_accessor< uint32_t       , 1> cached_neighbor(sycl::range<1>(SINGLE_WG_SIZE),cgh);
        sycl::local_accessor< uint32_t       , 1> cached_clusterID(sycl::range<1>(SINGLE_WG_SIZE),cgh);
#if 0
        sycl::local_accessor< uint32_t       , 1> local_cluster_index_source(sycl::range<1>(SINGLE_WG_SWITCH_THRESHOLD),cgh);
        sycl::local_accessor< uint32_t       , 1> local_cluster_index_dest(sycl::range<1>(SINGLE_WG_SWITCH_THRESHOLD),cgh);
#endif        
        sycl::local_accessor< uint32_t       , 1> counts(sycl::range<1>((SINGLE_WG_SIZE/SINGLE_WG_SUB_GROUP_WIDTH)),cgh);        
        sycl::local_accessor< uint32_t       , 1> counts_prefix_sum(sycl::range<1>((SINGLE_WG_SIZE/SINGLE_WG_SUB_GROUP_WIDTH)),cgh);
        sycl::local_accessor< uint32_t       , 0> _active_counter(cgh);
                                             
        cgh.parallel_for(nd_range,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(SINGLE_WG_SUB_GROUP_WIDTH) {
            uint32_t &active_counter = *_active_counter.get_pointer();

            const uint32_t localID        = item.get_local_id(0);
            //const uint32_t localSize       = item.get_local_range().size();

#if 0            
            for (uint32_t i=localID;i<numPrimitives;i+=localSize)
              local_cluster_index_source[i] = cluster_index_source[i];

            item.barrier(sycl::access::fence_space::local_space);
#endif
            
            //wgBuild(item, bvh2_index_allocator, 0, numPrimitives, bvh2, local_cluster_index_source.get_pointer(), local_cluster_index_dest.get_pointer(), bvh2_subtree_size, cached_bounds.get_pointer(), cached_neighbor.get_pointer(), cached_clusterID.get_pointer(), counts.get_pointer(),  counts_prefix_sum.get_pointer(), active_counter, 1, SEARCH_RADIUS_SHIFT, SINGLE_WG_SIZE);
            wgBuild(item, bvh2_index_allocator, 0, numPrimitives, bvh2, cluster_index_source, cluster_index_dest, bvh2_subtree_size, cached_bounds.get_pointer(), cached_neighbor.get_pointer(), cached_clusterID.get_pointer(), counts.get_pointer(),  counts_prefix_sum.get_pointer(), active_counter, 1, SEARCH_RADIUS_SHIFT, SINGLE_WG_SIZE);


            if (localID == 0) globals->rootIndex = globals->bvh2_index_allocator-1;
          });		  
      });            
    gpu::waitOnEventAndCatchException(queue_event);
    if (unlikely(verbose)) iteration_time += gpu::getDeviceExecutionTiming(queue_event);      
  }


  // =========================================================================================================================================================================
  // ====================================================================== Rebalance BVH2 ===================================================================================
  // =========================================================================================================================================================================
  
  __forceinline void rebalanceBVH2(BVH2Ploc *bvh2, uint32_t root, const uint32_t numPrimitives,const BVH2SubTreeState *const bvh2_subtree_size, uint32_t *const inner, uint32_t *const leaves, const uint32_t maxEntries)
  {
    uint32_t numLeaves = 0;
    uint32_t numInner = 1;
    inner[0] = root;

    uint32_t start = 0;
    while(start<numInner && numInner < maxEntries)
    {
      const uint32_t num = numInner-start;
      uint32_t plus = 0;
      for (uint32_t i=0;i<num;i++,plus++)
      {
        const uint32_t index = inner[start+i];
        const uint32_t left  = bvh2[index].leftIndex();          
        if (BVH2Ploc::isFatLeaf(bvh2[index].left,numPrimitives))
          leaves[numLeaves++] = left;
        else
          inner[numInner++] = left;

        const uint32_t right = bvh2[index].rightIndex();
        if (BVH2Ploc::isFatLeaf(bvh2[index].right,numPrimitives))
          leaves[numLeaves++] = right;
        else
          inner[numInner++] = right;

        if (numInner >= maxEntries) break;
      }
      start+=plus;
    }

    while(numInner >= numLeaves)
      leaves[numLeaves++] = inner[--numInner];
    
    uint32_t active = numLeaves;
    while(active > 1)
    {
      uint32_t new_active = 0;
      for (uint32_t i=0;i<active;i+=2)
        if (i+1 < active)
        {
          const uint32_t innerID = inner[--numInner];
          const uint32_t leftIndex  = leaves[i+0];
          const uint32_t rightIndex = leaves[i+1];
          const gpu::AABB3f &leftBounds  = bvh2[leftIndex].bounds;
          const gpu::AABB3f &rightBounds = bvh2[rightIndex].bounds;          
          bvh2[innerID].init(leftIndex,rightIndex,gpu::merge(leftBounds,rightBounds),bvh2_subtree_size[leftIndex],bvh2_subtree_size[rightIndex]);                                                         
          //bvh2_subtree_size[innerID] = BVH2SubTreeState(bvh2_subtree_size[leftIndex],bvh2_subtree_size[rightIndex]);
          leaves[new_active++] = innerID;
        }
        else
          leaves[new_active++] = leaves[i];
      active = new_active;      
    }    
  }
  
  __forceinline void rebalanceBVH2(sycl::queue &gpu_queue, BVH2Ploc *const bvh2, const BVH2SubTreeState *const bvh2_subtree_size, const uint32_t numPrimitives, double &iteration_time, const bool verbose)    
  {
    static const uint32_t REBALANCE_BVH2_WG_SIZE = 16;
    static const uint32_t MAX_NUM_REBALANCE_NODES = 256;
    
    const sycl::nd_range<1> nd_range1(gpu::alignTo(numPrimitives,REBALANCE_BVH2_WG_SIZE),sycl::range<1>(REBALANCE_BVH2_WG_SIZE));
    sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
        sycl::local_accessor< uint, 1> _inner(sycl::range<1>((MAX_NUM_REBALANCE_NODES)),cgh);
        sycl::local_accessor< uint, 1> _leaves(sycl::range<1>((MAX_NUM_REBALANCE_NODES)),cgh);                
        
        cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)
                         {
                           const uint32_t ID = item.get_global_id(0);
                           if (ID < numPrimitives)
                           {
                             const uint32_t global_root = numPrimitives+ID;
                             uint32_t *inner  = _inner.get_pointer();
                             uint32_t *leaves = _leaves.get_pointer();
                             uint32_t mask = sub_group_ballot(bvh2_subtree_size[global_root].isMarked());
                             while (mask)
                             {
                               const uint32_t index = sycl::ctz(mask);
                               mask &= mask-1;
                               const uint32_t root = sub_group_broadcast(global_root,index);
                               const uint32_t leftIndex = bvh2[root].leftIndex();
                               const uint32_t rightIndex = bvh2[root].rightIndex();
                               if (bvh2_subtree_size[leftIndex].depth >= REBALANCE_BVH2_MINIMUM_DEPTH)                             
                                 rebalanceBVH2(bvh2,leftIndex,numPrimitives,bvh2_subtree_size,inner,leaves,MAX_NUM_REBALANCE_NODES);
                               if (bvh2_subtree_size[rightIndex].depth >= REBALANCE_BVH2_MINIMUM_DEPTH)                             
                                 rebalanceBVH2(bvh2,rightIndex,numPrimitives,bvh2_subtree_size,inner,leaves,MAX_NUM_REBALANCE_NODES);                             
                             }
                           }
                         });
                                                       
      });
    gpu::waitOnEventAndCatchException(queue_event);
    if (unlikely(verbose)) iteration_time += gpu::getDeviceExecutionTiming(queue_event);
  }
  

  // ===================================================================================================================================================================================
  // ====================================================================== BVH2 -> QBVH6 conversion ===================================================================================
  // ===================================================================================================================================================================================


  __forceinline void getLeafIndices(const uint32_t first_index, const BVH2Ploc *const bvh_nodes, uint32_t *const dest, uint32_t &indexID, const uint32_t numPrimitives)
  {
    dest[0] = first_index;
    uint32_t old_indexID = 0;
    indexID = 1;
    while(old_indexID != indexID)
    {
      old_indexID = indexID;
      for (uint32_t i=0;i<old_indexID;i++)
        if (!BVH2Ploc::isLeaf(dest[i],numPrimitives))
        {
          const uint32_t left = bvh_nodes[BVH2Ploc::getIndex(dest[i])].left;
          const uint32_t right = bvh_nodes[BVH2Ploc::getIndex(dest[i])].right;
          dest[i]         = left;
          dest[indexID++] = right;
        }
    }
  }  

  __forceinline void writeNode(void *_dest, const uint32_t relative_block_offset, const gpu::AABB3f &parent_bounds, const uint32_t numChildren, uint32_t indices[BVH_BRANCHING_FACTOR], const BVH2Ploc *const bvh2, const uint32_t numPrimitives, const NodeType type, const GeometryTypeRanges &geometryTypeRanges)
  {
    uint32_t *dest = (uint*)_dest;
    //dest = (uint*) __builtin_assume_aligned(dest,16);
    
    const float _ulp = std::numeric_limits<float>::epsilon();
    const float up = 1.0f + float(_ulp);  
    const gpu::AABB3f conservative_bounds = parent_bounds.conservativeBounds();
    const float3 len = conservative_bounds.size() * up;
      
    int _exp_x; float mant_x = frexp(len.x(), &_exp_x); _exp_x += (mant_x > 255.0f / 256.0f);
    int _exp_y; float mant_y = frexp(len.y(), &_exp_y); _exp_y += (mant_y > 255.0f / 256.0f);
    int _exp_z; float mant_z = frexp(len.z(), &_exp_z); _exp_z += (mant_z > 255.0f / 256.0f);
    _exp_x = max(-128,_exp_x); // enlarge too tight bounds
    _exp_y = max(-128,_exp_y);
    _exp_z = max(-128,_exp_z);

    const float3 lower(conservative_bounds.lower_x,conservative_bounds.lower_y,conservative_bounds.lower_z);
    
    dest[0]  = gpu::as_uint(lower.x());
    dest[1]  = gpu::as_uint(lower.y());
    dest[2]  = gpu::as_uint(lower.z());
    dest[3]  = relative_block_offset; 

    uint8_t tmp[48];
    
    tmp[0]         = type; // type
    tmp[1]         = 0;    // pad 
    tmp[2]         = _exp_x; assert(_exp_x >= -128 && _exp_x <= 127); 
    tmp[3]         = _exp_y; assert(_exp_y >= -128 && _exp_y <= 127);
    tmp[4]         = _exp_z; assert(_exp_z >= -128 && _exp_z <= 127);
    tmp[5]         = 0xff; //instanceMode ? 1 : 0xff;
    
#pragma nounroll
    for (uint32_t i=0;i<BVH_BRANCHING_FACTOR;i++)
    {
      const uint32_t index = BVH2Ploc::getIndex(indices[sycl::min(i,numChildren-1)]);
      // === default is invalid ===
      uint8_t lower_x = 0x80;
      uint8_t lower_y = 0x80;
      uint8_t lower_z = 0x80;    
      uint8_t upper_x = 0x00;
      uint8_t upper_y = 0x00;
      uint8_t upper_z = 0x00;
      uint8_t data    = 0x00;      
      // === determine leaf type ===
      const bool isLeaf = index < numPrimitives; // && !forceFatLeaves;      
      const bool isInstance   = geometryTypeRanges.isInstance(index);  
      const bool isProcedural = geometryTypeRanges.isProcedural(index);
      const bool isLCG        = geometryTypeRanges.isLossyCompressedGeometry(index);      
      
      const uint32_t numBlocks    = isInstance ? 2 : 1;
      NodeType leaf_type = NODE_TYPE_QUAD;
      leaf_type = isInstance ? NODE_TYPE_INSTANCE   : leaf_type;
      leaf_type = isProcedural ? NODE_TYPE_PROCEDURAL : leaf_type;
      leaf_type = isLCG ? NODE_TYPE_INTERNAL : leaf_type;      
      data = (i<numChildren) ? numBlocks : 0;
      data |= (isLeaf ? leaf_type : NODE_TYPE_INTERNAL) << 2;
      const gpu::AABB3f childBounds = bvh2[index].bounds; //.conservativeBounds();
      // === bounds valid ? ====
      const uint32_t equal_dims = childBounds.numEqualDims();
      const bool write = (i<numChildren) && equal_dims <= 1;
      // === quantize bounds ===
      const gpu::AABB3f  qbounds    = QBVHNodeN::quantize_bounds(lower, _exp_x, _exp_y, _exp_z, childBounds);
      // === updated discretized bounds ===
      lower_x = write ? (uint8_t)qbounds.lower_x : lower_x;
      lower_y = write ? (uint8_t)qbounds.lower_y : lower_y;
      lower_z = write ? (uint8_t)qbounds.lower_z : lower_z;
      upper_x = write ? (uint8_t)qbounds.upper_x : upper_x;
      upper_y = write ? (uint8_t)qbounds.upper_y : upper_y;
      upper_z = write ? (uint8_t)qbounds.upper_z : upper_z;
      // === init child in node ===
      tmp[ 6+i] = data;
      tmp[12+i] = lower_x;
      tmp[18+i] = upper_x;      
      tmp[24+i] = lower_y;
      tmp[30+i] = upper_y;      
      tmp[36+i] = lower_z;
      tmp[42+i] = upper_z;      
    }
    // === write out second part of 64 bytes node ===
    for (uint32_t i=0;i<12;i++)
      dest[4+i] = tmp[i*4+0] | (tmp[i*4+1]<<8) | (tmp[i*4+2]<<16) | (tmp[i*4+3]<<24);
  }

  __forceinline uint32_t getNumLeaves(const uint32_t first_index, const BVH2Ploc *const bvh_nodes, const uint32_t numPrimitives)
  {
    uint32_t dest[BVH_BRANCHING_FACTOR];
    dest[0] = BVH2Ploc::getIndex(first_index);
    uint32_t old_indexID = 0;
    uint32_t indexID = 1;
    while(old_indexID != indexID)
    {
      old_indexID = indexID;
      for (uint32_t i=0;i<old_indexID;i++)
        if (!BVH2Ploc::isLeaf(dest[i],numPrimitives))
        {
          const uint32_t left = bvh_nodes[BVH2Ploc::getIndex(dest[i])].left;
          const uint32_t right = bvh_nodes[BVH2Ploc::getIndex(dest[i])].right;
          dest[i]         = left;
          dest[indexID++] = right;
        }
    }
    return indexID;
  }  
    
  __forceinline uint32_t openBVH2MaxAreaSortChildren(const uint32_t index, uint32_t indices[BVH_BRANCHING_FACTOR], const BVH2Ploc *const bvh2, const uint32_t numPrimitives)
  {
    float areas[BVH_BRANCHING_FACTOR];
                                                                          
    const uint32_t _left  = bvh2[BVH2Ploc::getIndex(index)].left;
    const uint32_t _right = bvh2[BVH2Ploc::getIndex(index)].right;
                                                                          
    indices[0] = _left;
    indices[1] = _right;
    areas[0]  = (!BVH2Ploc::isFatLeaf( _left,numPrimitives)) ? bvh2[BVH2Ploc::getIndex( _left)].bounds.area() : neg_inf;    
    areas[1]  = (!BVH2Ploc::isFatLeaf(_right,numPrimitives)) ? bvh2[BVH2Ploc::getIndex(_right)].bounds.area() : neg_inf; 

    uint32_t numChildren = 2;    
#if 1   
    while (numChildren < BVH_BRANCHING_FACTOR)
    {      
      /*! find best child to split */
      float bestArea = areas[0];
      uint32_t bestChild = 0;
      for (uint32_t i=1;i<numChildren;i++)
        if (areas[i] > bestArea)
        {
          bestArea = areas[i];
          bestChild = i;
        }
      
      if (areas[bestChild] < 0.0f) break;
      
      const uint32_t bestNodeID = indices[bestChild];      
      const uint32_t left  = bvh2[BVH2Ploc::getIndex(bestNodeID)].left;
      const uint32_t right = bvh2[BVH2Ploc::getIndex(bestNodeID)].right;

      areas[bestChild]     = (!BVH2Ploc::isFatLeaf(left,numPrimitives)) ? bvh2[BVH2Ploc::getIndex(left)].bounds.area() : neg_inf; 
      areas[numChildren]   = (!BVH2Ploc::isFatLeaf(right,numPrimitives)) ? bvh2[BVH2Ploc::getIndex(right)].bounds.area() : neg_inf; 
      indices[bestChild]   = left;      
      indices[numChildren] = right;                                                                            
      numChildren++;      
    }
#else
    while(numChildren < BVH_BRANCHING_FACTOR)
    {
      const uint32_t cur_numChildren = numChildren;      
      for (uint32_t i=0;i<cur_numChildren && numChildren < BVH_BRANCHING_FACTOR;i++)
      {
        if (!BVH2Ploc::isFatLeaf(indices[i],numPrimitives))
        {
          const uint32_t left  = bvh2[BVH2Ploc::getIndex(indices[i])].left;
          const uint32_t right = bvh2[BVH2Ploc::getIndex(indices[i])].right;          
          indices[i]   = left;      
          indices[numChildren] = right;                                                                            
          numChildren++;                
        }
      }
      if (cur_numChildren == numChildren) break;
    }
#endif    
    
    for (uint32_t i=0;i<numChildren;i++)
      areas[i] = fabs(areas[i]);

    for (uint32_t m=0; m<numChildren-1; m++)
      for (uint32_t n=m+1; n<numChildren; n++)
        if (areas[m] < areas[n])
        {
          std::swap(areas[m],areas[n]);
          std::swap(indices[m],indices[n]);
        }

    return numChildren;
  }  

  __forceinline void write(const QuadLeaf &q, float16 *out) 
  {
    out[0].s0() = gpu::as_float(q.header[0]);      
    out[0].s1() = gpu::as_float(q.header[1]);
    out[0].s2() = gpu::as_float(q.header[2]);
    out[0].s3() = gpu::as_float(q.header[3]);
    out[0].s4() = q.v0.x;
    out[0].s5() = q.v0.y;
    out[0].s6() = q.v0.z;
    out[0].s7() = q.v1.x;
    out[0].s8() = q.v1.y;
    out[0].s9() = q.v1.z;
    out[0].sA() = q.v2.x;
    out[0].sB() = q.v2.y;
    out[0].sC() = q.v2.z;
    out[0].sD() = q.v3.x;
    out[0].sE() = q.v3.y;
    out[0].sF() = q.v3.z;      
  }

  // =============================================================================================================================================
  // =============================================================================================================================================
  // =============================================================================================================================================


  bool convertBVH2toQBVH6(sycl::queue &gpu_queue, PLOCGlobals *globals, uint32_t *host_device_tasks, const RTHWIF_GEOMETRY_DESC **const geometries, QBVH6 *qbvh, const BVH2Ploc *const bvh2, LeafGenerationData *leafGenData, const uint32_t numPrimitives, const bool instanceMode, const GeometryTypeRanges &geometryTypeRanges, char* const lcg_bvh_mem, float& conversion_device_time,  const bool verbose)
  {
    static const uint32_t STOP_THRESHOLD = 1296;    
    double total_time = 0.0f;    
    conversion_device_time = 0.0f;
    
    const bool forceFatLeaves = numPrimitives <= BVH_BRANCHING_FACTOR;
    
    host_device_tasks[0] = 0;
    host_device_tasks[1] = 0;
    
    /* ---- Phase I: single WG generates enough work for the breadth-first phase --- */
    {
      const uint32_t wgSize = LARGE_WG_SIZE;
      const sycl::nd_range<1> nd_range1(wgSize,sycl::range<1>(wgSize));                    
      sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
          sycl::local_accessor< uint32_t      ,  0> _node_mem_allocator_cur(cgh);                                                   
          cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)      
                           {
                             const uint32_t localID     = item.get_local_id(0);
                             const uint32_t localSize   = item.get_local_range().size();                                                                      
                             uint32_t &node_mem_allocator_cur = *_node_mem_allocator_cur.get_pointer();

                             const uint32_t node_start = 2;
                             const uint32_t node_end   = 3;
                                                                      
                             if (localID == 0)
                             {
                               // === init globals ===
                               globals->node_mem_allocator_start = node_start;
                               globals->node_mem_allocator_cur   = node_end;
                               globals->qbvh_base_pointer        = (char*)qbvh;
                               globals->numLeaves                = 0;
                               // === init intial state ===
                               TmpNodeState *root_state = (TmpNodeState*)((char*)qbvh + 64 * node_start);
                               uint32_t rootIndex = globals->rootIndex;
                               // === make fat leaf if #prims < threshold ===
                               if (numPrimitives <= FATLEAF_THRESHOLD)
                                 rootIndex = BVH2Ploc::makeFatLeaf(rootIndex,numPrimitives);                                                                        
                               root_state->init( rootIndex );                                                                        
                               node_mem_allocator_cur = node_end;
                             }

                             item.barrier(sycl::access::fence_space::global_and_local);

                             uint32_t startBlockID = node_start;
                             uint32_t endBlockID   = node_mem_allocator_cur;

                             while(1)
                             {                                                                        
                               item.barrier(sycl::access::fence_space::local_space);
                               
                               if (startBlockID == endBlockID || endBlockID-startBlockID>STOP_THRESHOLD) break;
                               
                               for (uint32_t innerID=startBlockID+localID;innerID<endBlockID;innerID+=localSize)
                               {
                                 TmpNodeState *state = (TmpNodeState *)globals->nodeBlockPtr(innerID);
                                 const uint32_t header = state->header;                                                                        
                                 const uint32_t index  = state->bvh2_index;
                                 char* curAddr = (char*)state;

                                 if (header == 0x7fffffff)
                                 {
                                   if (!BVH2Ploc::isLeaf(index,numPrimitives))
                                   {                                                                              
                                     if (!BVH2Ploc::isFatLeaf(index,numPrimitives))
                                     {
                                       uint32_t indices[BVH_BRANCHING_FACTOR];                                                                                
                                       const uint32_t numChildren = openBVH2MaxAreaSortChildren(index,indices,bvh2,numPrimitives);
                                       uint32_t numBlocks = 0;
                                       for (uint32_t i=0;i<numChildren;i++)
                                         numBlocks += geometryTypeRanges.isInstance(BVH2Ploc::getIndex(indices[i])) ? 2 : 1; 
                                                                                    
                                       const uint32_t allocID = gpu::atomic_add_local(&node_mem_allocator_cur,numBlocks);
                                                                                
                                       char* childAddr = (char*)globals->qbvh_base_pointer + 64 * allocID;
                                       writeNode(curAddr,allocID-innerID,bvh2[BVH2Ploc::getIndex(index)].bounds,numChildren,indices,bvh2,numPrimitives,NODE_TYPE_MIXED,geometryTypeRanges);
                                       uint32_t offset = 0;
                                       for (uint32_t j=0;j<numChildren;j++)
                                       {
                                         TmpNodeState *childState = (TmpNodeState *)(childAddr + offset*64);
                                         childState->init(indices[j]);                                                                                  
                                         const bool isInstance = geometryTypeRanges.isInstance(BVH2Ploc::getIndex(indices[j]));
                                         // === invalidate header for second cache line in instance case ===
                                         if (isInstance)
                                           *(uint*)(childAddr + offset*64 + 64) = 0; 
                                         offset += isInstance ? 2 : 1;                                                                                  
                                       }
                                     }
                                   }
                                 }
                               }

                               item.barrier(sycl::access::fence_space::global_and_local);
                                                                        
                               startBlockID = endBlockID;
                               endBlockID = node_mem_allocator_cur;
                             }
                             // write out local node allocator to globals 
                             if (localID == 0)
                             {
                               startBlockID = globals->node_mem_allocator_start;
                               globals->range_start = startBlockID;
                               globals->range_end   = endBlockID;                                                                        
                               globals->node_mem_allocator_cur = node_mem_allocator_cur;
                               host_device_tasks[0] = endBlockID - startBlockID;
                               host_device_tasks[1] = endBlockID - globals->node_mem_allocator_start;
                               if (unlikely(globals->node_mem_allocator_cur > globals->leaf_mem_allocator_start))
                                 host_device_tasks[0] = -1;                                 
                             }
                                                                      
                           });
        });
      gpu::waitOnEventAndCatchException(queue_event);
      if (unlikely(verbose)) total_time += gpu::getDeviceExecutionTiming(queue_event);
    }
    
    if (unlikely(host_device_tasks[0] == -1)) return false;

    /* ---- Phase II: full breadth-first phase until only fat leaves or single leaves remain--- */

    struct __aligned(64) LocalNodeData {
      uint32_t v[16];
    };
    while(1)
    {      
      const uint32_t blocks = host_device_tasks[0];
      if (blocks == 0 || blocks == -1) break;
      
      const uint32_t wgSize = 256;
      const sycl::nd_range<1> nd_range1(gpu::alignTo(blocks,wgSize),sycl::range<1>(wgSize));              
      sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
          sycl::local_accessor< LocalNodeData, 1> _localNodeData(sycl::range<1>(wgSize),cgh);                        
          cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)      
                           {
                             const uint32_t localID   = item.get_local_id(0);                                                                      
                             const uint32_t globalID  = item.get_global_id(0);
                             const uint32_t numGroups   = item.get_group_range(0);                             
                             const uint32_t startBlockID = globals->range_start; 
                             const uint32_t endBlockID   = globals->range_end;                              
                             const uint32_t innerID   = startBlockID + globalID;
                             char* curAddr = nullptr;
                             bool valid = false;

                             LocalNodeData *localNodeData = _localNodeData.get_pointer();
                                                                      
                             if (innerID < endBlockID)
                             {
                               TmpNodeState *state = (TmpNodeState *)globals->nodeBlockPtr(innerID);
                               const uint32_t header = state->header;                                                                        
                               const uint32_t index  = state->bvh2_index;
                               curAddr = (char*)state;
                               if (header == 0x7fffffff)
                               {
                                 if (!BVH2Ploc::isLeaf(index,numPrimitives))
                                 {                                                                            
                                   if (!BVH2Ploc::isFatLeaf(index,numPrimitives))
                                   {                                                                              
                                     uint32_t indices[BVH_BRANCHING_FACTOR];
                                     const uint32_t numChildren = openBVH2MaxAreaSortChildren(index,indices,bvh2,numPrimitives);
                                     uint32_t numBlocks = 0;
                                     for (uint32_t i=0;i<numChildren;i++)
                                       numBlocks += geometryTypeRanges.isInstance(BVH2Ploc::getIndex(indices[i])) ? 2 : 1; 
                                                                              
                                     const uint32_t childBlockID = globals->atomic_add_sub_group_varying_allocNodeBlocks(numBlocks);
                                     char *const childAddr = globals->nodeBlockPtr(childBlockID);
                                                                              
                                     valid = true;
                                     writeNode(localNodeData[localID].v,childBlockID-innerID,bvh2[BVH2Ploc::getIndex(index)].bounds,numChildren,indices,bvh2,numPrimitives,NODE_TYPE_MIXED,geometryTypeRanges);
                                     uint32_t offset = 0;
                                     for (uint32_t j=0;j<numChildren;j++)
                                     {
                                       TmpNodeState *childState = (TmpNodeState *)(childAddr + offset*64);
                                       childState->init(indices[j]);
                                       const bool isInstance = geometryTypeRanges.isInstance(BVH2Ploc::getIndex(indices[j]));
                                       // === invalid header for second cache line in instance case ===
                                       if (isInstance)
                                         *(uint*)(childAddr + offset*64 + 64) = 0; 
                                       offset += isInstance ? 2 : 1;                                                                                 
                                     }
                                   }
                                 }
                               }
                             }

                             item.barrier(sycl::access::fence_space::local_space);

                                                                      
                             const uint32_t subgroupLocalID = get_sub_group_local_id();
                             uint32_t mask = sub_group_ballot(valid);
                             while(mask)
                             {
                               const uint32_t index = sycl::ctz(mask);
                               mask &= mask-1;
                               uint32_t ID = sub_group_broadcast(localID,index);
                               uint* dest = sub_group_broadcast((uint*)curAddr,index);
                               const uint32_t v = localNodeData[ID].v[subgroupLocalID];                                                                        
                               sub_group_store(dest,v);
                             }
                                                                      
                             /* -------------------------------- */                                                       
                             /* --- last WG does the cleanup --- */
                             /* -------------------------------- */

                             if (localID == 0)
                             {
                               const uint32_t syncID = gpu::atomic_add_global(&globals->sync,(uint)1);
                               if (syncID == numGroups-1)
                               {
                                 /* --- reset atomics --- */
                                 globals->sync = 0;
                                 const uint32_t new_startBlockID = globals->range_end;
                                 const uint32_t new_endBlockID   = globals->node_mem_allocator_cur;                                 
                                 globals->range_start = new_startBlockID;
                                 globals->range_end   = new_endBlockID;
                                 host_device_tasks[0] = new_endBlockID - new_startBlockID;
                                 host_device_tasks[1] = new_endBlockID - globals->node_mem_allocator_start;
                                 if (unlikely(globals->node_mem_allocator_cur > globals->leaf_mem_allocator_start))
                                   host_device_tasks[0] = -1;                                                                  
                               }
                             }
                             
                           });
		  
        });
      gpu::waitOnEventAndCatchException(queue_event);
      if (unlikely(verbose)) total_time += gpu::getDeviceExecutionTiming(queue_event);
    }
    
    if (unlikely(host_device_tasks[0] == -1)) return false;
    
    /* ---- Phase III: fill in mixed leafs and generate inner node for fatleaves plus storing primID, geomID pairs for final phase --- */
    const uint32_t blocks = host_device_tasks[1];
    
    if (blocks)
    {
      const uint32_t wgSize = 256;
      const sycl::nd_range<1> nd_range1(gpu::alignTo(blocks,wgSize),sycl::range<1>(wgSize));              
      sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
          sycl::local_accessor< uint32_t  ,  0> _local_numBlocks(cgh);
          sycl::local_accessor< uint32_t  ,  0> _local_numLeaves(cgh);                                                   
          sycl::local_accessor< uint32_t  ,  0> _global_blockID(cgh);
          sycl::local_accessor< uint32_t  ,  0> _global_numLeafID(cgh);                                                   
          sycl::local_accessor< LeafGenerationData, 1> _local_leafGenData(sycl::range<1>(wgSize*BVH_BRANCHING_FACTOR),cgh);
          sycl::local_accessor< uint, 1> _local_indices(sycl::range<1>(wgSize*BVH_BRANCHING_FACTOR),cgh);
          cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)      
                           {
                             const uint32_t localID   = item.get_local_id(0);                             
                             const uint32_t globalID  = item.get_global_id(0);
                             const uint32_t numGroups   = item.get_group_range(0);
                             const uint32_t localSize   = item.get_local_range().size();                                                                      
                                                                      
                             const uint32_t startBlockID = globals->node_mem_allocator_start;
                             const uint32_t endBlockID   = globals->node_mem_allocator_cur;                             
                             const uint32_t innerID      = startBlockID + globalID;

                             uint32_t &local_numBlocks   = *_local_numBlocks.get_pointer();
                             uint32_t &local_numLeaves   = *_local_numLeaves.get_pointer();                                                                      
                             uint32_t &global_blockID    = *_global_blockID.get_pointer();
                             uint32_t &global_leafID     = *_global_numLeafID.get_pointer();
                                                                      
                             LeafGenerationData* local_leafGenData = _local_leafGenData.get_pointer();

                             uint32_t *indices = _local_indices.get_pointer() + BVH_BRANCHING_FACTOR * localID;
                                                                      
                             if (localID == 0)
                             {
                               local_numBlocks = 0;
                               local_numLeaves = 0;
                             }
                                                                      
                             item.barrier(sycl::access::fence_space::local_space);
                                                                      
                             char* curAddr      = nullptr;
                             uint32_t numChildren   = 0;
                             bool isFatLeaf     = false;
                             uint32_t local_blockID = 0;
                             uint32_t local_leafID  = 0;
                             uint32_t index         = 0;
                                                                      
                             if (innerID < endBlockID)                                                                        
                             {
                               TmpNodeState *state = (TmpNodeState *)globals->nodeBlockPtr(innerID);
                               index   = state->bvh2_index;

                                                                        
                               curAddr = (char*)state;
                               if (state->header == 0x7fffffff) // not processed yet
                               {
                                 isFatLeaf = !BVH2Ploc::isLeaf(index,numPrimitives) || forceFatLeaves;
                                 uint32_t numBlocks = 0;
                                 if (isFatLeaf) 
                                 {
                                   numChildren = 0;
                                   getLeafIndices(index,bvh2,indices,numChildren,numPrimitives);
                                   for (uint32_t i=0;i<numChildren;i++)
                                     numBlocks += geometryTypeRanges.isInstance(BVH2Ploc::getIndex(indices[i])) ? 2 : 1;
                                 }
                                 else
                                 {
                                   numChildren = 1;
                                   numBlocks = 0; // === already been allocated in inner node ===
                                   indices[0] = index;
                                 }
                                 local_blockID = gpu::atomic_add_local(&local_numBlocks,numBlocks);
                                 local_leafID  = gpu::atomic_add_local(&local_numLeaves,numChildren);
                               }
                             }

                                                                      
                             item.barrier(sycl::access::fence_space::local_space);
                                                                      
                             const uint32_t numBlocks = local_numBlocks;
                             const uint32_t numLeaves = local_numLeaves;                                                                                                                                              
                             if (localID == 0)
                             {
                               global_blockID = gpu::atomic_add_global(&globals->leaf_mem_allocator_cur,numBlocks);
                               global_leafID  = gpu::atomic_add_global(&globals->numLeaves,numLeaves);
                             }
                                                                      
                             item.barrier(sycl::access::fence_space::local_space);

                             const uint32_t blockID = global_blockID + local_blockID;
                             const uint32_t leafID  = global_leafID; //  + local_leafID;
                                                                      
                                                                      
                             if (isFatLeaf)
                               writeNode(curAddr,blockID-innerID,bvh2[BVH2Ploc::getIndex(index)].bounds,numChildren,indices,bvh2,numPrimitives,NODE_TYPE_MIXED,geometryTypeRanges);
                                                                                                                                            
                             /* --- write to SLM frist --- */
                                                                      
                             const uint32_t local_leafDataID = local_leafID;
                             uint32_t node_blockID = 0;
                             for (uint32_t j=0;j<numChildren;j++)
                             {
                               const uint32_t index_j = BVH2Ploc::getIndex(indices[j]);
                               const uint32_t geomID = bvh2[index_j].left;
                               const uint32_t primID = bvh2[index_j].right;
                               const uint32_t bID = isFatLeaf ? (blockID + node_blockID) : innerID;
                               const bool isInstance   = geometryTypeRanges.isInstance(index_j);
                               local_leafGenData[local_leafDataID+j].blockID = bID;
                               local_leafGenData[local_leafDataID+j].primID = primID;
                               local_leafGenData[local_leafDataID+j].geomID = geomID;
                               node_blockID += isInstance  ? 2 : 1;
                             }
                                                                                                                                           
                             item.barrier(sycl::access::fence_space::local_space);

                             /* --- write out all local entries to global memory --- */
                                                                      
                             for (uint32_t i=localID;i<numLeaves;i+=localSize) 
                               leafGenData[leafID+i] = local_leafGenData[i]; 
                                                                      
                             if (localID == 0)
                             {                                                                        
                               const uint32_t syncID = gpu::atomic_add_global(&globals->sync,(uint)1);
                               if (syncID == numGroups-1)
                               {
                                 /* --- reset atomics --- */
                                 globals->sync = 0;
                                 host_device_tasks[0] = globals->numLeaves;
                                 if (unlikely(globals->numLeaves > numPrimitives))
                                   host_device_tasks[0] = -1;
                               }
                             }
                             
                           });
		  
        });
      gpu::waitOnEventAndCatchException(queue_event);
      if (unlikely(verbose)) total_time += gpu::getDeviceExecutionTiming(queue_event);
    }    
    if (unlikely(host_device_tasks[0] == -1)) return false;

    /* ---- Phase IV: for each primID, geomID pair generate corresponding leaf data --- */
    const uint32_t leaves = host_device_tasks[0];

    
    if (leaves)
    {
      const uint32_t wgSize = 256;
      const sycl::nd_range<1> nd_range1(gpu::alignTo(leaves,wgSize),sycl::range<1>(wgSize));              
      sycl::event queue_event = gpu_queue.submit([&](sycl::handler &cgh) {
          cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) EMBREE_SYCL_SIMD(16)      
                           {
                             const uint32_t globalID = item.get_global_id(0);
                             QuadLeafData*qleaf = nullptr;
                             if (globalID < leaves)                                                                        
                             {
                               qleaf = (QuadLeafData *)globals->nodeBlockPtr(leafGenData[globalID].blockID);
                               const uint32_t geomID = leafGenData[globalID].geomID & GEOMID_MASK;
                               const RTHWIF_GEOMETRY_DESC *const geometryDesc = geometries[geomID];                                 
                               if (geometryDesc->geometryType == RTHWIF_GEOMETRY_TYPE_TRIANGLES)
                               {
                                 
                                 // ====================                           
                                 // === TriangleMesh ===
                                 // ====================                                                                        
                                 const uint32_t primID0 = leafGenData[globalID].primID;
                                 const uint32_t primID1 = primID0 + (leafGenData[globalID].geomID >> PAIR_OFFSET_SHIFT);                                   
                                 RTHWIF_GEOMETRY_TRIANGLES_DESC* triMesh = (RTHWIF_GEOMETRY_TRIANGLES_DESC*)geometryDesc;
                                 const RTHWIF_TRIANGLE_INDICES &tri = getTriangleDesc(*triMesh,primID0);
                                 const Vec3f p0 = getVec3f(*triMesh,tri.v0);
                                 const Vec3f p1 = getVec3f(*triMesh,tri.v1);
                                 const Vec3f p2 = getVec3f(*triMesh,tri.v2);                                       
                                 Vec3f p3 = p2;
                                 uint32_t lb0 = 0,lb1 = 0, lb2 = 0;
                                 
                                 /* handle paired triangle */
                                 if (primID0 != primID1)
                                 {
                                   const RTHWIF_TRIANGLE_INDICES &tri1 = getTriangleDesc(*triMesh,primID1);          
                                   const uint32_t p3_index = try_pair_triangles(uint3(tri.v0,tri.v1,tri.v2),uint3(tri1.v0,tri1.v1,tri1.v2),lb0,lb1,lb2);
                                   p3 = getVec3f(*triMesh,((uint*)&tri1)[p3_index]); // FIXME: might cause tri1 to store to mem first
                                 }
                                 *qleaf = QuadLeafData( p0,p1,p2,p3, lb0,lb1,lb2, 0, geomID, primID0, primID1, (GeometryFlags)triMesh->geometryFlags, triMesh->geometryMask);
                               }
                               else if (geometryDesc->geometryType == RTHWIF_GEOMETRY_TYPE_QUADS)
                               {
                                 // ================                           
                                 // === QuadMesh ===
                                 // ================
                                 const uint32_t primID0 = leafGenData[globalID].primID;                                   
                                 RTHWIF_GEOMETRY_QUADS_DESC* quadMesh = (RTHWIF_GEOMETRY_QUADS_DESC*)geometryDesc;                             
                                 const RTHWIF_QUAD_INDICES &quad = getQuadDesc(*quadMesh,primID0);
                                 const Vec3f p0 = getVec3f(*quadMesh,quad.v0);
                                 const Vec3f p1 = getVec3f(*quadMesh,quad.v1);
                                 const Vec3f p2 = getVec3f(*quadMesh,quad.v2);                                       
                                 const Vec3f p3 = getVec3f(*quadMesh,quad.v3);                                                                          
                                 *qleaf = QuadLeafData( p0,p1,p3,p2, 3,2,1, 0, geomID, primID0, primID0, (GeometryFlags)quadMesh->geometryFlags, quadMesh->geometryMask);
                               }
                             
                               else if (geometryDesc->geometryType == RTHWIF_GEOMETRY_TYPE_INSTANCE)
                               {
                                 // ================                           
                                 // === Instance ===
                                 // ================                                 
                                 const uint32_t instID = leafGenData[globalID].geomID;
                                 const RTHWIF_GEOMETRY_INSTANCE_DESC* instance = (const RTHWIF_GEOMETRY_INSTANCE_DESC*)geometryDesc;                                 
                                 InstancePrimitive *dest = (InstancePrimitive *)qleaf;         
                                 const AffineSpace3fa local2world = getTransform(instance);
                                 const uint64_t root = (uint64_t)instance->accel + QBVH6_HEADER_OFFSET;
                                 *dest = InstancePrimitive(local2world,root,instance->instanceUserID,instID,mask32_to_mask8(instance->geometryMask));
                               }
                               else if (geometryDesc->geometryType == RTHWIF_GEOMETRY_TYPE_AABBS_FPTR)
                               {
                                 // ==================                           
                                 // === Procedural ===
                                 // ==================                                
                                 const uint32_t primID0 = leafGenData[globalID].primID;                                 
                                 const RTHWIF_GEOMETRY_AABBS_FPTR_DESC* geom = (const RTHWIF_GEOMETRY_AABBS_FPTR_DESC*)geometryDesc;                                 
                                 const uint32_t mask32 = mask32_to_mask8(geom->geometryMask);
                                 ProceduralLeaf *dest = (ProceduralLeaf *)qleaf;
                                 PrimLeafDesc leafDesc(0,geomID,GeometryFlags::NONE /*(GeometryFlags)geom->geometryFlags*/,mask32,PrimLeafDesc::TYPE_OPACITY_CULLING_ENABLED);
                                 *dest = ProceduralLeaf(leafDesc,primID0,true);
                               }
#if 1
                               else if (geometryDesc->geometryType == RTHWIF_GEOMETRY_TYPE_LOSSY_COMPRESSED_GEOMETRY)
                               {
                                 // =================================                           
                                 // === Lossy Compressed Geometry ===
                                 // =================================                                 
                                 const uint32_t primID = leafGenData[globalID].primID;
                                 InternalNode6Data *lcg_root = (InternalNode6Data*)(lcg_bvh_mem + primID * 64);
                                 const int oldOffset = lcg_root->childOffset;
                                 InternalNode6Data *dest     = (InternalNode6Data *)qleaf;         
                                 *dest = *lcg_root;
                                 dest->childOffset = ((uint64_t)lcg_root + oldOffset*64 - (uint64_t)dest)/64; // FIXME with SLM
                                 //dest->childOffset = ((uint64_t)lcg_root - (uint64_t)dest)/64; // FIXME with SLM
                               }
#endif                               
                             }

                           /* ================================== */                                                                      
                           /* === write out to global memory === */
                           /* ================================== */
                                                                      
                           /* const uint32_t subgroupLocalID = get_sub_group_local_id(); */
                           /* uint32_t mask = sub_group_ballot(valid); */
                           /* while(mask) */
                           /* { */
                           /*   const uint32_t index = sycl::ctz(mask); */
                           /*   mask &= mask-1; */
                           /*   uint32_t ID = sub_group_broadcast(localID,index); */
                           /*   uint* dest = sub_group_broadcast((uint*)qleaf,index); */
                           /*   uint* source = (uint*)&localLeaf[ID]; */
                           /*   const uint32_t v = source[subgroupLocalID]; */
                           /*   sub_group_store(dest,v); */
                           /* } */
                             
                           });
		  
        });
      gpu::waitOnEventAndCatchException(queue_event);
      if (unlikely(verbose)) total_time += gpu::getDeviceExecutionTiming(queue_event);     
    }        
    conversion_device_time = (float)total_time;
    return true;    
  }
  
    
}

#endif
