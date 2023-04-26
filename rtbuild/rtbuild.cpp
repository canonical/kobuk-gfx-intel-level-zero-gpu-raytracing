// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#define RTHWIF_EXPORT_API

#include "rtbuild.h"
#include "qbvh6_builder_sah.h"

#if defined(EMBREE_LEVEL_ZERO)
#include <level_zero/ze_api.h>
#endif

namespace embree
{
  using namespace embree::isa;

  static std::unique_ptr<tbb::task_arena> g_arena;
  
  typedef enum _ze_raytracing_accel_format_internal_t {
    ZE_RTAS_DEVICE_FORMAT_EXP_INVALID = 0,      // invalid acceleration structure format
    ZE_RTAS_DEVICE_FORMAT_EXP_VERSION_1 = 1, // acceleration structure format version 1
    ZE_RTAS_DEVICE_FORMAT_EXP_VERSION_2 = 2, // acceleration structure format version 2
    ZE_RTAS_DEVICE_FORMAT_EXP_VERSION_MAX = 2
  } ze_raytracing_accel_format_internal_t;
  
  inline ze_rtas_triangle_indices_uint32_exp_t getPrimitive(const ze_rtas_builder_triangles_geometry_info_exp_t* geom, uint32_t primID) {
    assert(primID < geom->triangleCount);
    return *(ze_rtas_triangle_indices_uint32_exp_t*)((char*)geom->pTriangleBuffer + uint64_t(primID)*geom->triangleStride);
  }
  
  inline Vec3f getVertex(const ze_rtas_builder_triangles_geometry_info_exp_t* geom, uint32_t vertexID) {
    assert(vertexID < geom->vertexCount);
    return *(Vec3f*)((char*)geom->pVertexBuffer + uint64_t(vertexID)*geom->vertexStride);
  }
  
  inline ze_rtas_quad_indices_uint32_exp_t getPrimitive(const ze_rtas_builder_quads_geometry_info_exp_t* geom, uint32_t primID) {
    assert(primID < geom->quadCount);
    return *(ze_rtas_quad_indices_uint32_exp_t*)((char*)geom->pQuadBuffer + uint64_t(primID)*geom->quadStride);
  }
  
  inline Vec3f getVertex(const ze_rtas_builder_quads_geometry_info_exp_t* geom, uint32_t vertexID) {
    assert(vertexID < geom->vertexCount);
    return *(Vec3f*)((char*)geom->pVertexBuffer + uint64_t(vertexID)*geom->vertexStride);
  }

  inline AffineSpace3fa getTransform(const ze_rtas_builder_instance_geometry_info_exp_t* geom)
  {
    switch (geom->transformFormat)
    {
    case ZE_RTAS_DATA_BUFFER_FORMAT_EXP_FLOAT3X4_COLUMN_MAJOR: {
      const ze_rtas_transform_float3x4_column_major_exp_t* xfm = (const ze_rtas_transform_float3x4_column_major_exp_t*) geom->pTransformBuffer;
      return {
        { xfm->vx_x, xfm->vx_y, xfm->vx_z },
        { xfm->vy_x, xfm->vy_y, xfm->vy_z },
        { xfm->vz_x, xfm->vz_y, xfm->vz_z },
        { xfm-> p_x, xfm-> p_y, xfm-> p_z }
      };
    }
    case ZE_RTAS_DATA_BUFFER_FORMAT_EXP_FLOAT3X4_ALIGNED_COLUMN_MAJOR: {
      const ze_rtas_transform_float3x4_aligned_column_major_exp_t* xfm = (const ze_rtas_transform_float3x4_aligned_column_major_exp_t*) geom->pTransformBuffer;
      return {
        { xfm->vx_x, xfm->vx_y, xfm->vx_z },
        { xfm->vy_x, xfm->vy_y, xfm->vy_z },
        { xfm->vz_x, xfm->vz_y, xfm->vz_z },
        { xfm-> p_x, xfm-> p_y, xfm-> p_z }
      };
    }
    case ZE_RTAS_DATA_BUFFER_FORMAT_EXP_FLOAT3X4_ROW_MAJOR: {
      const ze_rtas_transform_float3x4_row_major_exp_t* xfm = (const ze_rtas_transform_float3x4_row_major_exp_t*) geom->pTransformBuffer;
      return {
        { xfm->vx_x, xfm->vx_y, xfm->vx_z },
        { xfm->vy_x, xfm->vy_y, xfm->vy_z },
        { xfm->vz_x, xfm->vz_y, xfm->vz_z },
        { xfm-> p_x, xfm-> p_y, xfm-> p_z }
      };
    }
    default:
      throw std::runtime_error("invalid transform format");
    }
  }
  
  inline void verifyGeometryDesc(const ze_rtas_builder_triangles_geometry_info_exp_t* geom)
  {
    if (geom->triangleBufferFormat != ZE_RTAS_DATA_BUFFER_FORMAT_EXP_TRIANGLE_INDICES_UINT32)
      throw std::runtime_error("triangle format must be ZE_RTAS_DATA_BUFFER_FORMAT_EXP_TRIANGLE_INDICES_UINT32");
    
    if (geom->vertexBufferFormat != ZE_RTAS_DATA_BUFFER_FORMAT_EXP_FLOAT3)
      throw std::runtime_error("vertex format must be ZE_RTAS_DATA_BUFFER_FORMAT_EXP_FLOAT3");
 
    if (geom->reserved0 != 0) throw std::runtime_error("reserved member must be 0");
    if (geom->reserved1 != 0) throw std::runtime_error("reserved member must be 0");
    if (geom->reserved2 != 0) throw std::runtime_error("reserved member must be 0");
    
    if (geom->triangleCount && geom->pTriangleBuffer == nullptr) throw std::runtime_error("no triangle buffer specified");
    if (geom->vertexCount   && geom->pVertexBuffer   == nullptr) throw std::runtime_error("no vertex buffer specified");
  }

  inline void verifyGeometryDesc(const ze_rtas_builder_quads_geometry_info_exp_t* geom)
  {
    if (geom->quadBufferFormat != ZE_RTAS_DATA_BUFFER_FORMAT_EXP_QUAD_INDICES_UINT32)
      throw std::runtime_error("quad format must be ZE_RTAS_DATA_BUFFER_FORMAT_EXP_QUAD_INDICES_UINT32");
    
    if (geom->vertexBufferFormat != ZE_RTAS_DATA_BUFFER_FORMAT_EXP_FLOAT3)
      throw std::runtime_error("vertex format must be ZE_RTAS_DATA_BUFFER_FORMAT_EXP_FLOAT3");
 
    if (geom->reserved0 != 0) throw std::runtime_error("reserved member must be 0");
    if (geom->reserved1 != 0) throw std::runtime_error("reserved member must be 0");
    if (geom->reserved2 != 0) throw std::runtime_error("reserved member must be 0");
    
    if (geom->quadCount   && geom->pQuadBuffer   == nullptr) throw std::runtime_error("no quad buffer specified");
    if (geom->vertexCount && geom->pVertexBuffer == nullptr) throw std::runtime_error("no vertex buffer specified");
  }

  inline void verifyGeometryDesc(const ze_rtas_builder_procedural_geometry_info_exp_t* geom)
  {
    if (geom->reserved != 0) throw std::runtime_error("reserved member must be 0");
    if (geom->primCount   && geom->pfnGetBoundsCb == nullptr) throw std::runtime_error("no bounds function specified");
  }

  inline void verifyGeometryDesc(const ze_rtas_builder_instance_geometry_info_exp_t* geom)
  {
    if (geom->pTransformBuffer == nullptr) throw std::runtime_error("no instance transformation specified");
    if (geom->pBounds == nullptr) throw std::runtime_error("no acceleration structure bounds specified");
    if (geom->pAccelerationStructure == nullptr) throw std::runtime_error("no acceleration structure to instanciate specified");
  }

  inline bool buildBounds(const ze_rtas_builder_triangles_geometry_info_exp_t* geom, uint32_t primID, BBox3fa& bbox, void* buildUserPtr)
  {
    if (primID >= geom->triangleCount) return false;
    const ze_rtas_triangle_indices_uint32_exp_t tri = getPrimitive(geom,primID);
    if (unlikely(tri.v0 >= geom->vertexCount)) return false;
    if (unlikely(tri.v1 >= geom->vertexCount)) return false;
    if (unlikely(tri.v2 >= geom->vertexCount)) return false;
    
    const Vec3f p0 = getVertex(geom,tri.v0);
    const Vec3f p1 = getVertex(geom,tri.v1);
    const Vec3f p2 = getVertex(geom,tri.v2);
    if (unlikely(!isvalid(p0))) return false;
    if (unlikely(!isvalid(p1))) return false;
    if (unlikely(!isvalid(p2))) return false;
    
    bbox = BBox3fa(min(p0,p1,p2),max(p0,p1,p2));
    return true;
  }

  inline bool buildBounds(const ze_rtas_builder_quads_geometry_info_exp_t* geom, uint32_t primID, BBox3fa& bbox, void* buildUserPtr)
  {
    if (primID >= geom->quadCount) return false;
    const ze_rtas_quad_indices_uint32_exp_t tri = getPrimitive(geom,primID);
    if (unlikely(tri.v0 >= geom->vertexCount)) return false;
    if (unlikely(tri.v1 >= geom->vertexCount)) return false;
    if (unlikely(tri.v2 >= geom->vertexCount)) return false;
    if (unlikely(tri.v3 >= geom->vertexCount)) return false;
    
    const Vec3f p0 = getVertex(geom,tri.v0);
    const Vec3f p1 = getVertex(geom,tri.v1);
    const Vec3f p2 = getVertex(geom,tri.v2);
    const Vec3f p3 = getVertex(geom,tri.v3);
    if (unlikely(!isvalid(p0))) return false;
    if (unlikely(!isvalid(p1))) return false;
    if (unlikely(!isvalid(p2))) return false;
    if (unlikely(!isvalid(p3))) return false;
    
    bbox = BBox3fa(min(p0,p1,p2,p3),max(p0,p1,p2,p3));
    return true;
  }

  inline bool buildBounds(const ze_rtas_builder_procedural_geometry_info_exp_t* geom, uint32_t primID, BBox3fa& bbox, void* buildUserPtr)
  {
    if (primID >= geom->primCount) return false;
    if (geom->pfnGetBoundsCb == nullptr) return false;

    BBox3f bounds;
    (geom->pfnGetBoundsCb)(primID,1,geom->pGeomUserPtr,buildUserPtr,(ze_rtas_aabb_exp_t*)&bounds);
    if (unlikely(!isvalid(bounds.lower))) return false;
    if (unlikely(!isvalid(bounds.upper))) return false;
    if (unlikely(bounds.empty())) return false;
    
    bbox = (BBox3f&) bounds;
    return true;
  }

  inline bool buildBounds(const ze_rtas_builder_instance_geometry_info_exp_t* geom, uint32_t primID, BBox3fa& bbox, void* buildUserPtr)
  {
    if (primID >= 1) return false;
    if (geom->pAccelerationStructure == nullptr) return false;
    if (geom->pTransformBuffer == nullptr) return false;
    
    const AffineSpace3fa local2world = getTransform(geom);
    const Vec3fa lower(geom->pBounds->lower.x,geom->pBounds->lower.y,geom->pBounds->lower.z);
    const Vec3fa upper(geom->pBounds->upper.x,geom->pBounds->upper.y,geom->pBounds->upper.z);
    const BBox3fa bounds = xfmBounds(local2world,BBox3fa(lower,upper));
     
    if (unlikely(!isvalid(bounds.lower))) return false;
    if (unlikely(!isvalid(bounds.upper))) return false;
    if (unlikely(bounds.empty())) return false;
    
    bbox = bounds;
    return true;
  }

  template<typename GeometryType>
  PrimInfo createGeometryPrimRefArray(const GeometryType* geom, void* buildUserPtr, evector<PrimRef>& prims, const range<size_t>& r, size_t k, unsigned int geomID)
  {
    PrimInfo pinfo(empty);
    for (uint32_t primID=r.begin(); primID<r.end(); primID++)
    {
      BBox3fa bounds = empty;
      if (!buildBounds(geom,primID,bounds,buildUserPtr)) continue;
      const PrimRef prim(bounds,geomID,primID);
      pinfo.add_center2(prim);
      prims[k++] = prim;
    }
    return pinfo;
  }
  
  RTHWIF_API void zeRTASInitExp()
  {
    uint32_t numThreads = tbb::this_task_arena::max_concurrency();
    g_arena.reset(new tbb::task_arena(numThreads,numThreads));
  }
  
  RTHWIF_API void zeRTASExitExp()
  {
    g_arena.reset();
  }

  typedef struct _zet_base_desc_t
  {
    /** [in] type of this structure */
    ze_structure_type_t_ stype;
    
    /** [in,out][optional] must be null or a pointer to an extension-specific structure */
    const void* pNext;
    
  } zet_base_desc_t_;

  #define VALIDATE(arg) \
  {\
  ze_result_t_ result = validate(arg);\
  if (result != ZE_RESULT_SUCCESS_) return result; \
  }

#define VALIDATE_PTR(arg)                       \
  {                                                                     \
    if ((arg) == nullptr) return ZE_RESULT_ERROR_INVALID_NULL_POINTER_; \
  }                                                                     \

   ze_result_t_ validate(ze_driver_handle_t hDriver)
  {
    if (hDriver == nullptr)
      return ZE_RESULT_ERROR_INVALID_NULL_HANDLE_;
    
    return ZE_RESULT_SUCCESS_;
  }

  ze_result_t_ validate(ze_device_handle_t hDevice)
  {
    if (hDevice == nullptr)
      return ZE_RESULT_ERROR_INVALID_NULL_HANDLE_;
    
    return ZE_RESULT_SUCCESS_;
  }
  
  bool checkDescChain(zet_base_desc_t_* desc)
  {
    /* supporting maximal 1024 to also detect cycles */
    for (size_t i=0; i<1024; i++) {
      if (desc->pNext == nullptr) return true;
      desc = (zet_base_desc_t_*) desc->pNext;
    }
    return false;
  }

  struct ze_rtas_builder
  {
    ze_rtas_builder () {
    }
    
    ~ze_rtas_builder() {
      magick = 0x0;
    }

    bool verify() const {
      return magick == MAGICK;
    }
    
    enum { MAGICK = 0x45FE67E1 };
    uint32_t magick = MAGICK;
  };

  ze_result_t_ validate(ze_rtas_builder_exp_handle_t hBuilder)
  {
    if (hBuilder == nullptr)
      return ZE_RESULT_ERROR_INVALID_NULL_HANDLE_;
    
    if (!((ze_rtas_builder*)hBuilder)->verify())
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;

    return ZE_RESULT_SUCCESS_;
  }

  struct ze_rtas_parallel_operation_t
  {
    ze_rtas_parallel_operation_t(ze_rtas_builder_exp_handle_t hBuilder)
      : hBuilder(hBuilder) {}

    ~ze_rtas_parallel_operation_t() {
      magick = 0x0;
      hBuilder = nullptr;
    }

    bool verify() const {
      return (magick == MAGICK) && (validate(hBuilder) == ZE_RESULT_SUCCESS_);
    }
    
    enum { MAGICK = 0xE84567E1 };
    uint32_t magick = MAGICK;
    ze_rtas_builder_exp_handle_t hBuilder = nullptr;
    ze_result_t_ errorCode = ZE_RESULT_SUCCESS_;
    tbb::task_group group;
  };

  ze_result_t_ validate(ze_rtas_parallel_operation_exp_handle_t hParallelOperation)
  {
    if (hParallelOperation == nullptr)
      return ZE_RESULT_ERROR_INVALID_NULL_HANDLE_;
    
    if (!((ze_rtas_parallel_operation_t*)hParallelOperation)->verify())
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;

    return ZE_RESULT_SUCCESS_;
  }

  ze_result_t_ validate(const ze_rtas_builder_exp_desc_t* pDescriptor)
  {
    if (pDescriptor == nullptr)
      return ZE_RESULT_ERROR_INVALID_NULL_POINTER_;

    if (!checkDescChain((zet_base_desc_t_*)pDescriptor))
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;

    if (uint32_t(ZE_RTAS_BUILDER_EXP_VERSION_CURRENT) < uint32_t(pDescriptor->builderVersion))
      return ZE_RESULT_ERROR_INVALID_ENUMERATION_;
    
    return ZE_RESULT_SUCCESS_;
  }

  ze_result_t_ validate(ze_rtas_device_exp_properties_t* pProperties)
  { 
    if (pProperties == nullptr)
      return ZE_RESULT_ERROR_INVALID_NULL_POINTER_;

    if (!checkDescChain((zet_base_desc_t_*)pProperties))
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;
    
    if (pProperties->stype != ZE_STRUCTURE_TYPE_RTAS_DEVICE_EXP_PROPERTIES)
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;

    return ZE_RESULT_SUCCESS_;
  }

  ze_result_t_ validate(ze_rtas_device_format_exp_t rtasFormat)
  {
    if (uint32_t(rtasFormat) > uint32_t(ZE_RTAS_DEVICE_FORMAT_EXP_VERSION_MAX))
      return ZE_RESULT_ERROR_INVALID_ENUMERATION_;

    return ZE_RESULT_SUCCESS_;
  }
  
  ze_result_t_ validate(const ze_rtas_builder_build_op_exp_desc_t* args)
  {
    /* check for valid pointers */
    if (args == nullptr)
      return ZE_RESULT_ERROR_INVALID_NULL_POINTER_;

    /* check if input descriptor has proper type */
    if (args->stype != ZE_STRUCTURE_TYPE_RTAS_BUILDER_BUILD_OP_EXP_DESC)
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;

    /* check valid pNext chain */
    if (!checkDescChain((zet_base_desc_t_*)args))
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;

    /* check if acceleration structure format is supported */
    VALIDATE(args->rtasFormat);

    /* check for valid geometries array */
    if (args->ppGeometries == nullptr && args->numGeometries > 0)
      return ZE_RESULT_ERROR_INVALID_NULL_POINTER_;

    /* validate build quality */
    if (args->buildQuality < 0 || ZE_RTAS_BUILDER_BUILD_QUALITY_HINT_EXP_HIGH < args->buildQuality)
      return ZE_RESULT_ERROR_INVALID_ENUMERATION_;

    /* validate build flags */
    if (args->buildFlags >= (ZE_RTAS_BUILDER_BUILD_OP_EXP_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION<<1))
      return ZE_RESULT_ERROR_INVALID_ENUMERATION_;
    
    return ZE_RESULT_SUCCESS_;
  }

  ze_result_t_ validate(ze_rtas_builder_exp_properties_t* pProp)
  {
    /* check for valid pointers */
    if (pProp == nullptr)
      return ZE_RESULT_ERROR_INVALID_NULL_POINTER_;
    
    /* check if return property has proper type */
    if (pProp->stype != ZE_STRUCTURE_TYPE_RTAS_DEVICE_EXP_PROPERTIES)
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;

    /* check valid pNext chain */
    if (!checkDescChain((zet_base_desc_t_*)pProp))
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;

    return ZE_RESULT_SUCCESS_;
  }

  ze_result_t_ validate(ze_rtas_parallel_operation_exp_properties_t* pProperties)
  {
    /* check for valid pointer */
    if (pProperties == nullptr)
      return ZE_RESULT_ERROR_INVALID_NULL_POINTER_;

    /* check for proper property */
    if (pProperties->stype != ZE_STRUCTURE_TYPE_RTAS_PARALLEL_OPERATION_EXP_PROPERTIES)
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;

    /* check valid pNext chain */
    if (!checkDescChain((zet_base_desc_t_*)pProperties))
      return ZE_RESULT_ERROR_INVALID_ARGUMENT_;

    return ZE_RESULT_SUCCESS_;
  }
  
  RTHWIF_API ze_result_t_ zeRTASBuilderCreateExp(ze_driver_handle_t hDriver, const ze_rtas_builder_exp_desc_t *pDescriptor, ze_rtas_builder_exp_handle_t *phBuilder)
  {
    /* input validation */
    VALIDATE(hDriver);
    VALIDATE(pDescriptor);
    VALIDATE_PTR(phBuilder);

    *phBuilder = (ze_rtas_builder_exp_handle_t) new ze_rtas_builder();
    return ZE_RESULT_SUCCESS_;
  }

  RTHWIF_API ze_result_t_ zeRTASBuilderDestroyExp(ze_rtas_builder_exp_handle_t hBuilder)
  {
    VALIDATE(hBuilder);
    delete (ze_rtas_builder*) hBuilder;
    return ZE_RESULT_SUCCESS_;
  }

  RTHWIF_API ze_result_t_ zeDeviceGetRTASPropertiesExp( const ze_device_handle_t hDevice, ze_rtas_device_exp_properties_t* pProperties )
  {
    /* input validation */
    VALIDATE(hDevice);
    VALIDATE(pProperties);

    /* fill properties */
    pProperties->flags = ZE_RTAS_DEVICE_EXP_FLAG_NONE;
    pProperties->rtasDeviceFormat = (ze_rtas_device_format_exp_t) ZE_RTAS_DEVICE_FORMAT_EXP_INVALID;
    pProperties->rtasBufferAlignment = 128;

#if defined(EMBREE_LEVEL_ZERO)

    /* check for supported device ID */
    ze_device_properties_t device_props{ ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES };
    ze_result_t status = zeDeviceGetProperties(hDevice, &device_props);
    if (status != ZE_RESULT_SUCCESS)
      return ZE_RESULT_ERROR_UNKNOWN_;

    /* check for Intel vendor */
    const uint32_t vendor_id = device_props.vendorId;
    const uint32_t device_id = device_props.deviceId;
    if (vendor_id != 0x8086) return ZE_RESULT_ERROR_UNKNOWN_;
    
    bool dg2 =
      (0x4F80 <= device_id && device_id <= 0x4F88) ||
      (0x5690 <= device_id && device_id <= 0x5698) ||
      (0x56A0 <= device_id && device_id <= 0x56A6) ||
      (0x56B0 <= device_id && device_id <= 0x56B3) ||
      (0x56C0 <= device_id && device_id <= 0x56C1);

    bool pvc =
      (0x0BD0 <= device_id && device_id <= 0x0BDB) ||
      (device_id == 0x0BE5                       );

    if (dg2 || pvc) {
      pProperties->rtasDeviceFormat = (ze_rtas_device_format_exp_t) ZE_RTAS_DEVICE_FORMAT_EXP_VERSION_1;
      return ZE_RESULT_SUCCESS_;
    }        

    return ZE_RESULT_ERROR_UNKNOWN_;

#else

    pProperties->rtasDeviceFormat = (ze_rtas_device_format_exp_t) ZE_RTAS_DEVICE_FORMAT_EXP_VERSION_1;
    return ZE_RESULT_SUCCESS_;
    
#endif
  }
  
  RTHWIF_API ze_result_t_ zeRTASBuilderDeviceFormatCompatibilityCheckExp( ze_rtas_builder_exp_handle_t hBuilder,
                                                                          const ze_rtas_device_format_exp_t accelFormat,
                                                                          const ze_rtas_device_format_exp_t otherAccelFormat )
  {
    /* input validation */
    VALIDATE(hBuilder);
    VALIDATE(accelFormat);
    VALIDATE(otherAccelFormat);

    /* check if rtas formats are compatible */
    if (accelFormat == otherAccelFormat)
      return ZE_RESULT_SUCCESS_;

    /* report incompatible format */
    return ZE_RESULT_EXP_ERROR_OPERANDS_INCOMPATIBLE_;
  }

  uint32_t getNumPrimitives(const ze_rtas_builder_geometry_info_exp_t* geom)
  {
    switch (geom->geometryType) {
    case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_TRIANGLES  : return ((ze_rtas_builder_triangles_geometry_info_exp_t*) geom)->triangleCount;
    case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_PROCEDURAL : return ((ze_rtas_builder_procedural_geometry_info_exp_t*) geom)->primCount;
    case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_QUADS      : return ((ze_rtas_builder_quads_geometry_info_exp_t*) geom)->quadCount;
    case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_INSTANCE   : return 1;
    default                              : return 0;
    };
  }
  
  RTHWIF_API ze_result_t_ zeRTASBuilderGetBuildPropertiesExp(ze_rtas_builder_exp_handle_t hBuilder,
                                                             const ze_rtas_builder_build_op_exp_desc_t* args,
                                                             ze_rtas_parallel_operation_exp_handle_t hParallelOperation,
                                                             ze_rtas_builder_exp_properties_t* pProp)
  {
    /* input validation */
    VALIDATE(hBuilder);
    VALIDATE(args);
    VALIDATE(hParallelOperation);
    VALIDATE(pProp);

    const ze_rtas_builder_geometry_info_exp_t** geometries = args->ppGeometries;
    const size_t numGeometries = args->numGeometries;

    auto getSize = [&](uint32_t geomID) -> size_t {
      const ze_rtas_builder_geometry_info_exp_t* geom = geometries[geomID];
      if (geom == nullptr) return 0;
      return getNumPrimitives(geom);
    };
    
    auto getType = [&](unsigned int geomID)
    {
      const ze_rtas_builder_geometry_info_exp_t* geom = geometries[geomID];
      assert(geom);
      switch (geom->geometryType) {
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_TRIANGLES : return QBVH6BuilderSAH::TRIANGLE;
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_QUADS: return QBVH6BuilderSAH::QUAD;
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_PROCEDURAL: return QBVH6BuilderSAH::PROCEDURAL;
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_INSTANCE: return QBVH6BuilderSAH::INSTANCE;
      default: throw std::runtime_error("invalid geometry type");
      };
    };

    /* query memory requirements from builder */
    size_t expectedBytes = 0;
    size_t worstCaseBytes = 0;
    size_t scratchBytes = 0;
    QBVH6BuilderSAH::estimateSize(numGeometries, getSize, getType, args->buildQuality, args->buildFlags, expectedBytes, worstCaseBytes, scratchBytes);
    
    /* fill return struct */
    pProp->flags = 0;
    pProp->rtasBufferSizeBytesExpected = expectedBytes;
    pProp->rtasBufferSizeBytesMax = worstCaseBytes;
    pProp->scratchBufferSizeBytes = scratchBytes;
    return ZE_RESULT_SUCCESS_;
  }
  
  RTHWIF_API ze_result_t_ zeRTASBuilderBuildExpInternal(const ze_rtas_builder_build_op_exp_desc_t* args,
                                                            void *pScratchBuffer, size_t scratchBufferSizeBytes,
                                                            void *pRtasBuffer, size_t rtasBufferSizeBytes,
                                                            void *pBuildUserPtr, ze_rtas_aabb_exp_t *pBounds, size_t *pRtasBufferSizeBytes) try
  {
    const ze_rtas_builder_geometry_info_exp_t** geometries = args->ppGeometries;
    const uint32_t numGeometries = args->numGeometries;

    /* verify input descriptors */
    parallel_for(numGeometries,[&](uint32_t geomID) {
      const ze_rtas_builder_geometry_info_exp_t* geom = geometries[geomID];
      if (geom == nullptr) return;
      
      switch (geom->geometryType) {
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_TRIANGLES  : verifyGeometryDesc((ze_rtas_builder_triangles_geometry_info_exp_t*)geom); break;
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_QUADS      : verifyGeometryDesc((ze_rtas_builder_quads_geometry_info_exp_t*    )geom); break;
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_PROCEDURAL : verifyGeometryDesc((ze_rtas_builder_procedural_geometry_info_exp_t*)geom); break;
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_INSTANCE   : verifyGeometryDesc((ze_rtas_builder_instance_geometry_info_exp_t* )geom); break;
      default: throw std::runtime_error("invalid geometry type");
      };
    });
    
    auto getSize = [&](uint32_t geomID) -> size_t {
      const ze_rtas_builder_geometry_info_exp_t* geom = geometries[geomID];
      if (geom == nullptr) return 0;
      return getNumPrimitives(geom);
    };
    
    auto getType = [&](unsigned int geomID)
    {
      const ze_rtas_builder_geometry_info_exp_t* geom = geometries[geomID];
      assert(geom);
      switch (geom->geometryType) {
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_TRIANGLES : return QBVH6BuilderSAH::TRIANGLE;
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_QUADS: return QBVH6BuilderSAH::QUAD;
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_PROCEDURAL: return QBVH6BuilderSAH::PROCEDURAL;
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_INSTANCE: return QBVH6BuilderSAH::INSTANCE;
      default: throw std::runtime_error("invalid geometry type");
      };
    };
    
    auto createPrimRefArray = [&] (evector<PrimRef>& prims, BBox1f time_range, const range<size_t>& r, size_t k, unsigned int geomID) -> PrimInfo
    {
      const ze_rtas_builder_geometry_info_exp_t* geom = geometries[geomID];
      assert(geom);

      switch (geom->geometryType) {
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_TRIANGLES  : return createGeometryPrimRefArray((ze_rtas_builder_triangles_geometry_info_exp_t*)geom,pBuildUserPtr,prims,r,k,geomID);
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_QUADS      : return createGeometryPrimRefArray((ze_rtas_builder_quads_geometry_info_exp_t*    )geom,pBuildUserPtr,prims,r,k,geomID);
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_PROCEDURAL: return createGeometryPrimRefArray((ze_rtas_builder_procedural_geometry_info_exp_t*)geom,pBuildUserPtr,prims,r,k,geomID);
      case ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_INSTANCE: return createGeometryPrimRefArray((ze_rtas_builder_instance_geometry_info_exp_t* )geom,pBuildUserPtr,prims,r,k,geomID);
      default: throw std::runtime_error("invalid geometry type");
      };
    };
    
    auto getTriangle = [&](unsigned int geomID, unsigned int primID)
    {
      const ze_rtas_builder_triangles_geometry_info_exp_t* geom = (const ze_rtas_builder_triangles_geometry_info_exp_t*) geometries[geomID];
      assert(geom);
      
      const ze_rtas_triangle_indices_uint32_exp_t tri = getPrimitive(geom,primID);
      if (unlikely(tri.v0 >= geom->vertexCount)) return QBVH6BuilderSAH::Triangle();
      if (unlikely(tri.v1 >= geom->vertexCount)) return QBVH6BuilderSAH::Triangle();
      if (unlikely(tri.v2 >= geom->vertexCount)) return QBVH6BuilderSAH::Triangle();
      
      const Vec3f p0 = getVertex(geom,tri.v0);
      const Vec3f p1 = getVertex(geom,tri.v1);
      const Vec3f p2 = getVertex(geom,tri.v2);
      if (unlikely(!isvalid(p0))) return QBVH6BuilderSAH::Triangle();
      if (unlikely(!isvalid(p1))) return QBVH6BuilderSAH::Triangle();
      if (unlikely(!isvalid(p2))) return QBVH6BuilderSAH::Triangle();

      const GeometryFlags gflags = (GeometryFlags) geom->geometryFlags;
      return QBVH6BuilderSAH::Triangle(tri.v0,tri.v1,tri.v2,p0,p1,p2,gflags,geom->geometryMask);
    };
    
    auto getTriangleIndices = [&] (uint32_t geomID, uint32_t primID) {
      const ze_rtas_builder_triangles_geometry_info_exp_t* geom = (const ze_rtas_builder_triangles_geometry_info_exp_t*) geometries[geomID];
      assert(geom);
      const ze_rtas_triangle_indices_uint32_exp_t tri = getPrimitive(geom,primID);
      return Vec3<uint32_t>(tri.v0,tri.v1,tri.v2);
    };
    
    auto getQuad = [&](unsigned int geomID, unsigned int primID)
    {
      const ze_rtas_builder_quads_geometry_info_exp_t* geom = (const ze_rtas_builder_quads_geometry_info_exp_t*) geometries[geomID];
      assert(geom);
                     
      const ze_rtas_quad_indices_uint32_exp_t quad = getPrimitive(geom,primID);
      const Vec3f p0 = getVertex(geom,quad.v0);
      const Vec3f p1 = getVertex(geom,quad.v1);
      const Vec3f p2 = getVertex(geom,quad.v2);
      const Vec3f p3 = getVertex(geom,quad.v3);

      const GeometryFlags gflags = (GeometryFlags) geom->geometryFlags;
      return QBVH6BuilderSAH::Quad(p0,p1,p2,p3,gflags,geom->geometryMask);
    };
    
    auto getProcedural = [&](unsigned int geomID, unsigned int primID) {
      const ze_rtas_builder_procedural_geometry_info_exp_t* geom = (const ze_rtas_builder_procedural_geometry_info_exp_t*) geometries[geomID];
      assert(geom);
      return QBVH6BuilderSAH::Procedural(geom->geometryMask); // FIXME: pass gflags
    };
    
    auto getInstance = [&](unsigned int geomID, unsigned int primID)
    {
      assert(geometries[geomID]);
      assert(geometries[geomID]->geometryType == ZE_RTAS_BUILDER_GEOMETRY_TYPE_EXP_INSTANCE);
      const ze_rtas_builder_instance_geometry_info_exp_t* geom = (const ze_rtas_builder_instance_geometry_info_exp_t*) geometries[geomID];
      void* accel = geom->pAccelerationStructure;
      const AffineSpace3fa local2world = getTransform(geom);
      return QBVH6BuilderSAH::Instance(local2world,accel,geom->geometryMask,geom->instanceUserID); // FIXME: pass instance flags
    };

    /* dispatch globals ptr for debugging purposes */
    void* dispatchGlobalsPtr = nullptr;
#if defined(EMBREE_SYCL_ALLOC_DISPATCH_GLOBALS)
    dispatchGlobalsPtr = args->dispatchGlobalsPtr;
#endif

    bool verbose = false;
    bool success = QBVH6BuilderSAH::build(numGeometries, nullptr, 
                           getSize, getType, 
                           createPrimRefArray, getTriangle, getTriangleIndices, getQuad, getProcedural, getInstance,
                           (char*)pRtasBuffer, rtasBufferSizeBytes,
                           pScratchBuffer, scratchBufferSizeBytes,
                           (BBox3f*) pBounds, pRtasBufferSizeBytes,
                           args->buildQuality, args->buildFlags, verbose, dispatchGlobalsPtr);
    if (!success) {
      return ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY_;
    }
    return ZE_RESULT_SUCCESS_;
  }
  catch (std::exception& e) {
    std::cerr << "caught exception during BVH build: " << e.what() << std::endl;
    return ZE_RESULT_ERROR_UNKNOWN_;
  }
  
  RTHWIF_API ze_result_t_ zeRTASBuilderBuildExp(ze_rtas_builder_exp_handle_t hBuilder,
                                                const ze_rtas_builder_build_op_exp_desc_t* args,
                                                void *pScratchBuffer, size_t scratchBufferSizeBytes,
                                                void *pRtasBuffer, size_t rtasBufferSizeBytes,
                                                ze_rtas_parallel_operation_exp_handle_t hParallelOperation,
                                                void *pBuildUserPtr, ze_rtas_aabb_exp_t *pBounds, size_t *pRtasBufferSizeBytes)
  {
    /* input validation */
    VALIDATE(hBuilder);
    VALIDATE(args);
    VALIDATE_PTR(pScratchBuffer);
    VALIDATE_PTR(pRtasBuffer);
    
    /* if parallel operation is provided then execute using thread arena inside task group ... */
    if (hParallelOperation)
    {
      VALIDATE(hParallelOperation);
      
      ze_rtas_parallel_operation_t* op = (ze_rtas_parallel_operation_t*) hParallelOperation;
      if (op->hBuilder != hBuilder)
        return ZE_RESULT_ERROR_INVALID_ARGUMENT_;
      
      g_arena->execute([&](){ op->group.run([=](){
         op->errorCode = zeRTASBuilderBuildExpInternal(args,
                                                       pScratchBuffer, scratchBufferSizeBytes,
                                                       pRtasBuffer, rtasBufferSizeBytes,
                                                       pBuildUserPtr, pBounds, pRtasBufferSizeBytes);
                                            });
                       });
      return ZE_RESULT_ERROR_HANDLE_OBJECT_IN_USE_;
    }
    /* ... otherwise we just execute inside task arena to avoid spawning of TBB worker threads */
    else
    {
      ze_result_t_ errorCode = ZE_RESULT_SUCCESS_;
      g_arena->execute([&](){ errorCode = zeRTASBuilderBuildExpInternal(args,
                                                                        pScratchBuffer, scratchBufferSizeBytes,
                                                                        pRtasBuffer, rtasBufferSizeBytes,
                                                                        pBuildUserPtr, pBounds, pRtasBufferSizeBytes);
                       });
      return errorCode;
    }
  }

  RTHWIF_API ze_result_t_ zeRTASParallelOperationCreateExp(ze_rtas_builder_exp_handle_t hBuilder, ze_rtas_parallel_operation_exp_handle_t* phParallelOperation)
  {
    /* input validation */
    VALIDATE(hBuilder);
    VALIDATE_PTR(phParallelOperation);

    /* create parallel operation object */
    *phParallelOperation = (ze_rtas_parallel_operation_exp_handle_t) new ze_rtas_parallel_operation_t(hBuilder);
    return ZE_RESULT_SUCCESS_;
  }
  
  RTHWIF_API ze_result_t_ zeRTASParallelOperationDestroyExp( ze_rtas_parallel_operation_exp_handle_t hParallelOperation )
  {
    /* input validation */
    VALIDATE(hParallelOperation);

    /* delete parallel operation */
    delete (ze_rtas_parallel_operation_t*) hParallelOperation;
    return ZE_RESULT_SUCCESS_;
  }
  
  RTHWIF_API ze_result_t_ zeRTASParallelOperationGetPropertiesExp( ze_rtas_parallel_operation_exp_handle_t hParallelOperation, ze_rtas_parallel_operation_exp_properties_t* pProperties )
  {
    /* input validation */
    VALIDATE(hParallelOperation);
    VALIDATE(pProperties);
   
    /* return properties */
    pProperties->flags = ZE_RTAS_PARALLEL_OPERATION_EXP_FLAG_NONE;
    pProperties->maxConcurrency = tbb::this_task_arena::max_concurrency();
    return ZE_RESULT_SUCCESS_;
  }
  
  RTHWIF_API ze_result_t_ zeRTASParallelOperationJoinExp( ze_rtas_parallel_operation_exp_handle_t hParallelOperation)
  {
    /* check for valid handle */
    VALIDATE(hParallelOperation);
    
    ze_rtas_parallel_operation_t* op = (ze_rtas_parallel_operation_t*) hParallelOperation;
    g_arena->execute([&](){ op->group.wait(); });
    return op->errorCode;
  }
}