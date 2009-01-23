#ifndef _fei_impl_utils_hpp_
#define _fei_impl_utils_hpp_

/*--------------------------------------------------------------------*/
/*    Copyright 2008 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#include <fei_macros.hpp>
#include <fei_fwd.hpp>
#include <fei_mpi.h>

#include <Teuchos_ParameterList.hpp>

#include <string>
#include <vector>
#include <map>


/** The fei namespace contains public functions, classes and interfaces.
*/
namespace fei {

/** The impl_utils namespace contains implementation-utilities. Helpers
  for implementation code, not part of the public API.
*/
namespace impl_utils {

/** pack an fei::FillableMat object into a pair of std::vector objects.
*/
void pack_FillableMat(const fei::FillableMat& mat,
                      std::vector<int>& intdata,
                      std::vector<double>& doubledata);

/** unpack a pair of std::vector objects into an fei::FillableMat object.
    The std::vector objects are assumed to have been produced by the
    function pack_FillableMat(...).
*/
void unpack_FillableMat(const std::vector<int>& intdata,
                        const std::vector<double>& doubledata,
                        fei::FillableMat& mat,
                        bool clear_mat_on_entry = true,
                        bool overwrite_entries = true);

void separate_BC_eqns(const fei::FillableMat& mat,
                    std::vector<int>& bcEqns,
                    std::vector<double>& bcVals);

void create_col_to_row_map(const fei::FillableMat& mat,
                           std::multimap<int,int>& crmap);

int remove_couplings(fei::FillableMat& mat);

void global_union(MPI_Comm comm,
                  const fei::FillableMat& localMatrix,
                  fei::FillableMat& globalUnionMatrix);

void global_union(MPI_Comm comm,
                  const fei::CSVec& localVec,
                  fei::CSVec& globalUnionVec);

void translate_to_reduced_eqns(const fei::Reducer& reducer, fei::CSRMat& mat);

void translate_to_reduced_eqns(const fei::Reducer& reducer, fei::CSVec& vec);

void add_to_graph(const fei::CSRMat& inmat, fei::Graph& graph);

void add_to_matrix(const fei::CSRMat& inmat, bool sum_into, fei::Matrix& matrix);

}//namespace impl_utils
}//namespace fei

#endif

