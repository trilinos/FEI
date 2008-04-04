
/*--------------------------------------------------------------------*/
/*    Copyright 2005 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#include "fei_CSRMat.hpp"
#include "snl_fei_ArrayUtils.hpp"
#include <limits>
#include <cmath>

namespace fei {

CSRMat::CSRMat()
 : srg_(),
   packedcoefs_()
{
}

CSRMat::CSRMat(const FillableMat& fmat)
 : srg_(),
   packedcoefs_()
{
  *this = fmat;
}

CSRMat::~CSRMat()
{
}

CSRMat&
CSRMat::operator=(const fei::FillableMat& src)
{
  FillableMat::const_iterator iter = src.begin(), iter_end = src.end();

  unsigned nrows = src.getNumRows();

  srg_.rowNumbers.resize(nrows);
  srg_.rowOffsets.resize(nrows+1);

  unsigned nnz = 0;
  unsigned i = 0;
  for(; iter != iter_end; ++iter, ++i) {
    srg_.rowNumbers[i] = iter->first;
    srg_.rowOffsets[i] = nnz;
    nnz += iter->second->size();
  }

  srg_.rowOffsets[nrows] = nnz;

  srg_.packedColumnIndices.resize(nnz);
  packedcoefs_.resize(nnz);

  iter = src.begin();

  unsigned offset = 0;
  for(; iter != iter_end; ++iter) {
    const FillableVec* v = iter->second;
    FillableVec::const_iterator viter = v->begin(), vend = v->end();
    for(; viter != vend; ++viter) {
      srg_.packedColumnIndices[offset] = viter->first;
      packedcoefs_[offset++] = viter->second;
    }
  }

  return *this;
}

void multiply_CSRMat_CSVec(const CSRMat& A, const CSVec& x, CSVec& y)
{
  const std::vector<int>& rows = A.getGraph().rowNumbers;
  const int* rowoffs = &(A.getGraph().rowOffsets[0]);
  const std::vector<int>& colinds = A.getGraph().packedColumnIndices;
  const double* Acoef = &(A.getPackedCoefs()[0]);

  const std::vector<int>& xind = x.indices();
  const std::vector<double>& xcoef = x.coefs();

  const double* xcoef_ptr = &xcoef[0];
  const int* xind_ptr = &xind[0];
  int xlen = xcoef.size();

  std::vector<int>& yind = y.indices();
  std::vector<double>& ycoef = y.coefs();

  unsigned nrows = A.getNumRows();

  yind.resize(nrows);
  ycoef.resize(nrows);

  int* yind_ptr = &yind[0];
  double* ycoef_ptr = &ycoef[0];

  int jbeg = *rowoffs++;
  for(unsigned i=0; i<nrows; ++i) {
    int jend = *rowoffs++;

    double sum = 0.0;
    while(jbeg<jend) {
      int xoff = snl_fei::binarySearch(colinds[jbeg++], xind_ptr, xlen);

      if (xoff > -1) {
        sum += *Acoef++*xcoef_ptr[xoff];
      }
    }

    yind_ptr[i] = rows[i];
    ycoef_ptr[i] = sum;
  }
}

void multiply_trans_CSRMat_CSVec(const CSRMat& A, const CSVec& x, CSVec& y)
{
  const std::vector<int>& rows = A.getGraph().rowNumbers;
  const int* rowoffs = &(A.getGraph().rowOffsets[0]);
  const std::vector<int>& colinds = A.getGraph().packedColumnIndices;
  const double* Acoef = &(A.getPackedCoefs()[0]);

  const std::vector<int>& xind = x.indices();
  const std::vector<double>& xcoef = x.coefs();

  const double* xcoef_ptr = &xcoef[0];
  const int* xind_ptr = &xind[0];
  int xlen = xcoef.size();

  unsigned nrows = A.getNumRows();

  static fei::FillableVec fy;
  fy.zero();

  int jbeg = *rowoffs++;
  for(unsigned i=0; i<nrows; ++i) {
    int jend = *rowoffs++;

    int xoff = snl_fei::binarySearch(rows[i], xind_ptr, xlen);
    if (xoff < 0) {
      continue;
    }

    double xcoeff = xcoef_ptr[xoff];

    while(jbeg<jend) {
       fy.addEntry(colinds[jbeg++],*Acoef++*xcoeff);
    }
  }

  y = fy;
}

void multiply_CSRMat_CSRMat(const CSRMat& A, const CSRMat& B, CSRMat& C,
                            bool storeResultZeros)
{
  const std::vector<int>& Arows = A.getGraph().rowNumbers;
  const int* Arowoffs = &(A.getGraph().rowOffsets[0]);
  const std::vector<int>& Acols = A.getGraph().packedColumnIndices;
  const double* Acoefs = &(A.getPackedCoefs()[0]);

  const std::vector<int>& Brows = B.getGraph().rowNumbers;
  const int* Browoffs = &(B.getGraph().rowOffsets[0]);
  const std::vector<int>& Bcols = B.getGraph().packedColumnIndices;
  const double* Bcoefs = &(B.getPackedCoefs()[0]);

  fei::FillableMat fc;
  fc.zero();

  static double fei_eps = std::numeric_limits<double>::epsilon();

  int jbeg = *Arowoffs++;
  for(unsigned i=0; i<Arows.size(); ++i) {
    int row = Arows[i];
    int jend = *Arowoffs++;

    fei::FillableVec* fc_row = fc.hasRow(row) ? fc.getRow(row) : NULL;

    while(jbeg<jend) {
      int Acol = Acols[jbeg++];
      double Acoef = *Acoefs++;

      int Brow_offset =
        snl_fei::binarySearch(Acol, &Brows[0], Brows.size());

      if (Brow_offset < 0) {
        continue;
      }

      const int* Brow_cols = &(Bcols[Browoffs[Brow_offset]]);
      const double* Brow_coefs = &(Bcoefs[Browoffs[Brow_offset]]);
      int Brow_len = Browoffs[Brow_offset+1]-Browoffs[Brow_offset];

      for(int k=0; k<Brow_len; ++k) {
        double resultCoef = Acoef*Brow_coefs[k];
        int resultCol = Brow_cols[k];

        if (std::abs(resultCoef) > fei_eps || storeResultZeros) {
          if (fc_row == NULL) {
            fc.sumInCoef(row, resultCol, resultCoef);
            fc_row = fc.getRow(row);
          }
          else fc_row->addEntry(resultCol, resultCoef);
        }
      }
    }
  }

  C = fc;
}

void multiply_trans_CSRMat_CSRMat(const CSRMat& A, const CSRMat& B, CSRMat& C,
                                  bool storeResultZeros)
{
  const std::vector<int>& Arows = A.getGraph().rowNumbers;
  const int* Arowoffs = &(A.getGraph().rowOffsets[0]);
  const std::vector<int>& Acols = A.getGraph().packedColumnIndices;
  const double* Acoefs = &(A.getPackedCoefs()[0]);

  const std::vector<int>& Brows = B.getGraph().rowNumbers;
  const int* Browoffs = &(B.getGraph().rowOffsets[0]);
  const std::vector<int>& Bcols = B.getGraph().packedColumnIndices;
  const double* Bcoefs = &(B.getPackedCoefs()[0]);

  fei::FillableMat fc;

  std::vector<double> row_coefs;

  int jbeg = *Arowoffs++;
  for(unsigned i=0; i<Arows.size(); ++i) {
    int row = Arows[i];
    int jend = *Arowoffs++;

    int Brow_offset =
      snl_fei::binarySearch(row, &Brows[0], Brows.size());

    if (Brow_offset < 0) {
      continue;
    }

    const int* Brow_cols = &(Bcols[Browoffs[Brow_offset]]);
    const double* Brow_coefs = &(Bcoefs[Browoffs[Brow_offset]]);
    int Brow_len = Browoffs[Brow_offset+1]-Browoffs[Brow_offset];

    if ((int)row_coefs.size() < Brow_len) row_coefs.resize(Brow_len*2);
    double* row_coefs_ptr = &row_coefs[0];

    while(jbeg<jend) {
      int Acol = Acols[jbeg++];
      double Acoef = *Acoefs++;

      for(int k=0; k<Brow_len; ++k) {
        row_coefs_ptr[k] = Acoef*Brow_coefs[k];
      }

      fc.sumInRow(Acol, Brow_cols, row_coefs_ptr, Brow_len);
    }
  }

  C = fc;
}

}//namespace fei

