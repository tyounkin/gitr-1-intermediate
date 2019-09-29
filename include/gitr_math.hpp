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
//#include <cmath>
#endif
namespace gitr_math {
  template<typename T>
  class function {
  public:
  CUDA_CALLABLE_MEMBER_DEVICE	  
  T sqrt(T x);
  };
}

#endif
