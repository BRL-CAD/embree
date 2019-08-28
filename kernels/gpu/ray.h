// ======================================================================== //
// Copyright 2009-2019 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //
#pragma once

#if defined(EMBREE_DPCPP_SUPPORT)
#include "common.h"

namespace embree
{
  namespace gpu
  {

    /* Ray structure for a single ray */
    struct RTCRayGPU
    {
      cl::sycl::float3 org; // x,y,z coordinates of ray origin
      float tnear;          // start of ray segment
      cl::sycl::float3 dir; // x,y,z coordinate of ray direction
      float time;           // time of this ray for motion blur
      float tfar;           // end of ray segment (set to hit distance)
      unsigned int mask;    // ray mask
      unsigned int id;      // ray ID
      unsigned int flags;   // ray flags
    };

    /* Hit structure for a single ray */
    struct RTCHitGPU
    {
      cl::sycl::float3 Ng; // x,y,z coordinates of geometry normal
      float u;             // barycentric u coordinate of hit
      float v;             // barycentric v coordinate of hit
      unsigned int primID; // primitive ID
      unsigned int geomID; // geometry ID
      unsigned int instID; // instance ID
    };

    /* Combined ray/hit structure for a single ray */
    struct RTCRayHitGPU
    {
      struct RTCRayGPU ray;
      struct RTCHitGPU hit;
    };

  };
};

#endif