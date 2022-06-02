#ifndef __TACHO_LU_ON_DEVICE_HPP__
#define __TACHO_LU_ON_DEVICE_HPP__


/// \file  Tacho_LU_OnDevice.hpp
/// \brief LU device solver
/// \author Kyungjoo Kim (kyukim@sandia.gov)

#include "Tacho_LU_External.hpp"

namespace Tacho {

  template<>
  struct LU<Algo::OnDevice> {
    template<typename ViewTypeA,
             typename ViewTypeP>
    inline
    static int
    lapack_invoke(const ViewTypeA &A,
                  const ViewTypeP &P) {
      return LU<Algo::External>::invoke(A, P);
    }

#if defined(KOKKOS_ENABLE_CUDA)
    template<typename ViewTypeA,
             typename ViewTypeP,
             typename ViewTypeW>
    inline
    static int
    cusolver_invoke(cusolverDnHandle_t &handle,
                    const ViewTypeA &A,
                    const ViewTypeP &P,
                    const ViewTypeW &W) {
      typedef typename ViewTypeA::non_const_value_type value_type;
      typedef typename ViewTypeW::non_const_value_type work_value_type;
      const ordinal_type 
        m = A.extent(0), n = A.extent(1);
      
      int r_val(0);
      if (m > 0 && n > 0) {
        int *devInfo = (int*)W.data();
        value_type *workspace = W.data() + 1;
        //int lwork = (W.span()-1);
        r_val = Lapack<value_type>::getrf(handle,
                                          m, n, 
                                          A.data(), A.stride_1(),
                                          workspace,
                                          P.data(), 
                                          devInfo);
      }
      return r_val;
    }

    template<typename ViewTypeA>
    inline
    static int
    cusolver_buffer_size(cusolverDnHandle_t &handle,
                         const ViewTypeA &A,
                         int *lwork) {
      typedef typename ViewTypeA::non_const_value_type value_type;
      const ordinal_type 
        m = A.extent(0), n = A.extent(1);

      int r_val(0);
      if (m > 0) 
        r_val = Lapack<value_type>::getrf_buffersize(handle,
                                                     m, n, 
                                                     A.data(), A.stride_1(),
                                                     lwork);
      return r_val;
    }
#endif
      
    template<typename MemberType,
             typename ViewTypeA,
             typename ViewTypeP,
             typename ViewTypeW>
    inline
    static int
    invoke(MemberType &member,
           const ViewTypeA &A,
           const ViewTypeP &P,
           const ViewTypeW &W) {
      typedef typename ViewTypeA::non_const_value_type value_type;
      typedef typename ViewTypeW::non_const_value_type value_type_w;

      typedef typename ViewTypeA::memory_space memory_space;
      typedef typename ViewTypeW::memory_space memory_space_w;
        
      static_assert(ViewTypeA::rank == 2,"A is not rank 2 view.");
      static_assert(ViewTypeP::rank == 1,"P is not rank 1 view.");
      static_assert(ViewTypeW::rank == 1,"W is not rank 1 view.");
        
      static_assert(std::is_same<value_type,value_type_w>::value,
                    "A and W do not have the same value type.");

      static_assert(std::is_same<memory_space,memory_space_w>::value,
                    "A and W do not have the same memory space.");
      int r_val(0);
      if (std::is_same<memory_space,Kokkos::HostSpace>::value) {
        r_val = lapack_invoke(A, P);
      }
        
#if defined (KOKKOS_ENABLE_CUDA)
      if (std::is_same<memory_space,Kokkos::CudaSpace>::value ||
          std::is_same<memory_space,Kokkos::CudaUVMSpace>::value) {
        if (W.span() == 0) {
          int lwork;
          r_val = cusolver_buffer_size(member, A, &lwork);
          r_val = (lwork+sizeof(value_type_w))/sizeof(value_type_w) + 1; 
        } else 
          r_val = cusolver_invoke(member, A, P, W);
      }
#endif
      return r_val;
    }

    template<typename ViewTypeA,
             typename ViewTypeP,
             typename ViewTypeD>
    inline
    static int
    lapack_modify(const ordinal_type m,
                  const ViewTypeP &P) {
      return LU<Algo::External>::modify(m, P);
    }

#if defined(KOKKOS_ENABLE_CUDA)
    template<typename ExecSpaceType,
             typename ViewTypeP>
    inline
    static int
    cusolver_modify(ExecSpaceType &exec_instance,
                    const ordinal_type m,
                    const ViewTypeP &P) {
      using exec_space = ExecSpaceType;
        
      int r_val(0);
      if (m > 0) {
        ordinal_type 
          *__restrict__ ipiv = P.data(),
          *__restrict__ fpiv = ipiv + m, 
          *__restrict__ perm = fpiv + m, 
          *__restrict__ peri = perm + m;
        
        Kokkos::RangePolicy<exec_space> range_policy(exec_instance, 0, m);
        Kokkos::RangePolicy<exec_space> single_policy(exec_instance, 0, 1);
        Kokkos::parallel_for
          (range_policy,
           KOKKOS_LAMBDA(const ordinal_type i) {
            perm[i] = i; 
            fpiv[i] = ipiv[i]-i-1;
          });
        exec_instance.fence();
        Kokkos::parallel_for
          (single_policy,
           KOKKOS_LAMBDA(const ordinal_type j) {
            for (ordinal_type i=0;i<m;++i) {
              if (fpiv[i]) {
                const ordinal_type pidx = i+fpiv[i];
                swap(perm[i], perm[pidx]);
              }
            }
          });
        exec_instance.fence();
        Kokkos::parallel_for
          ("PermutationInverse", range_policy,
           KOKKOS_LAMBDA(const ordinal_type i) {
            peri[perm[i]] = i;        
          });        
      }
      return r_val;
    }
#endif

    template<typename MemberType,
             typename ViewTypeP>
    inline
    static int
    modify(MemberType &member,
           const ordinal_type m,
           const ViewTypeP &P) {
      typedef typename ViewTypeP::memory_space memory_space;
      
      static_assert(ViewTypeP::rank == 1,"P is not rank 1 view.");
      
      int r_val(0);
      if (std::is_same<memory_space,Kokkos::HostSpace>::value) {
        r_val = lapack_modify(m, P);
      }
      
#if defined (KOKKOS_ENABLE_CUDA)
      if (std::is_same<memory_space,Kokkos::CudaSpace>::value ||
          std::is_same<memory_space,Kokkos::CudaUVMSpace>::value) {
        r_val = cusolver_modify(member, m, P);
      }
#endif
      return r_val;
    }
  };  
}
#endif
