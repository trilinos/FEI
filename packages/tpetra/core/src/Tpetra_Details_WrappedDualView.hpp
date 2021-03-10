// @HEADER
// ***********************************************************************
//
//          Tpetra: Templated Linear Algebra Services Package
//                 Copyright (2008) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// ************************************************************************
// @HEADER

#ifndef TPETRA_DETAILS_WRAPPEDDUALVIEW_HPP
#define TPETRA_DETAILS_WRAPPEDDUALVIEW_HPP

#include <Tpetra_Access.hpp>
#include <Kokkos_DualView.hpp>

//! Namespace for Tpetra classes and methods
namespace Tpetra {

/// \brief Namespace for Tpetra implementation details.
/// \warning Do NOT rely on the contents of this namespace.
namespace Details {

template <typename DualViewType>
class WrappedDualView {
public:

  WrappedDualView(DualViewType dv)
    : dualView(dv)
  { }

  WrappedDualView() {}

  KOKKOS_INLINE_FUNCTION size_t extent(const int i) const
  {
    return dualView.extent(i);
  }

  typename DualViewType::t_host::const_type
  getHostView(Access::ReadOnlyStruct) const {
    throwIfDeviceViewAlive();
    dualView.sync_host();
    return dualView.view_host();
  }

  typename DualViewType::t_host
  getHostView(Access::ReadWriteStruct) {
    throwIfDeviceViewAlive();
    dualView.sync_host();
    dualView.modify_host();
    return dualView.view_host();
  }

  typename DualViewType::t_host
  getHostView(Access::WriteOnlyStruct) {
    throwIfDeviceViewAlive();
    dualView.clear_sync_state();
    dualView.modify_host();
    return dualView.view_host();
  }

  typename DualViewType::t_dev::const_type
  getDeviceView(Access::ReadOnlyStruct) const {
    throwIfHostViewAlive();
    dualView.sync_device();
    return dualView.view_device();
  }

  typename DualViewType::t_dev
  getDeviceView(Access::ReadWriteStruct) {
    throwIfHostViewAlive();
    dualView.sync_device();
    dualView.modify_device();
    return dualView.view_device();
  }

  typename DualViewType::t_dev
  getDeviceView(Access::WriteOnlyStruct) {
    throwIfHostViewAlive();
    dualView.clear_sync_state();
    dualView.modify_device();
    return dualView.view_device();
  }

private:
  void throwIfHostViewAlive() const {
    if (dualView.h_view.use_count() > dualView.d_view.use_count()) {
      const char msg[] = "Tpetra::Details::WrappedDualView: Cannot access data on "
                         "device while a host view is alive";
      throw std::runtime_error(msg);
    }
  }

  void throwIfDeviceViewAlive() const {
    if (dualView.d_view.use_count() > dualView.h_view.use_count()) {
      const char msg[] = "Tpetra::Details::WrappedDualView: Cannot access data on "
                         "host while a device view is alive";
      throw std::runtime_error(msg);
    }
  }

  mutable DualViewType dualView;
};

} // namespace Details

} // namespace Tpetra

#endif
