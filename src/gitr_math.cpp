#ifndef _GITRMATH_
#define _GITRMATH_

#ifdef __CUDACC__
#define CUDA_CALLABLE_MEMBER_DEVICE __device__
#define CUDA_CALLABLE_MEMBER_HOST __host__
#else
#define CUDA_CALLABLE_MEMBER_DEVICE
#define CUDA_CALLABLE_MEMBER_HOST
#endif

#ifdef __CUDACC__
#else
#include <cmath>
#endif
namespace gitr_math {
  template<typename T>
  class function {
  public:
#ifdef __CUDACC__
#if USE_DOUBLE > 0
  CUDA_CALLABLE_MEMBER_DEVICE	  
  double sqrt(double x){ return sqrt(x);};
#else
  CUDA_CALLABLE_MEMBER_DEVICE
  float sqrt(float x){ return sqrtf(x);};
#endif
#else
#if USE_DOUBLE > 0
  double sqrt(double x){ return std::sqrt(x);};
#else
  float sqrt(float x){ return sqrtf(x);};
#endif
#endif
  };
}

#endif
