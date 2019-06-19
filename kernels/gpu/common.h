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

#include "../common/device.h"

#if defined(EMBREE_DPCPP_SUPPORT)

#include <CL/sycl.hpp>

#ifdef __SYCL_DEVICE_ONLY__
#define GLOBAL __global
#define LOCAL  __local

extern int   work_group_reduce_add(int x);
extern float work_group_reduce_min(float x);
extern float work_group_reduce_max(float x);

extern float atomic_min(volatile __global float *p, float val);
extern float atomic_min(volatile __local  float *p, float val);
extern float atomic_max(volatile __global float *p, float val);
extern float atomic_max(volatile __local  float *p, float val);

#else
#define GLOBAL 
#define LOCAL 

/* dummy functions for host */
int   work_group_reduce_add(int x) { return x; }
float work_group_reduce_min(float x) { return x; }
float work_group_reduce_max(float x) { return x; }

float atomic_min(volatile float *p, float val) { return val; };
float atomic_max(volatile float *p, float val) { return val; };

#endif

namespace embree
{
  namespace gpu
  {

    inline float halfarea(const cl::sycl::float3 &d)
    {
      return fma((float)d.x(),((float)d.y()+(float)d.z()),(float)d.y()*(float)d.z());
    }
    
    inline float area(const cl::sycl::float3 &d)
    {
      return halfarea(d) * 2.0f;
    }
  };
};


#endif