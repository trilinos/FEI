/*--------------------------------------------------------------------*/
/*    Copyright 2007 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#include "fei_Vector_Local.hpp"
#include "fei_sstream.hpp"
#include "fei_fstream.hpp"

#include <algorithm>

namespace fei {

Vector_Local::Vector_Local(fei::SharedPtr<fei::VectorSpace> vecSpace)
 : vecSpace_(vecSpace),
   coefs_(),
   global_to_local_(),
   work_indices_()
{
  int numCoefs = vecSpace_->getNumIndices_SharedAndOwned();
  coefs_.resize(numCoefs);

  std::vector<int> indices(numCoefs);
  vecSpace_->getIndices_SharedAndOwned(numCoefs, &indices[0], numCoefs);

  std::sort(indices.begin(), indices.end());

  for(int i=0; i<numCoefs; ++i) {
    global_to_local_.insert(std::make_pair(indices[i], i));
  }
}

Vector_Local::~Vector_Local()
{
}

int
Vector_Local::update(double a,
               fei::Vector* x,
               double b)
{
  FEI_CERR << "Vector_Local::update NOT IMPLEMENTED."<<FEI_ENDL;
  return(-1);
}

int
Vector_Local::scatterToOverlap()
{ return(0); }

int
Vector_Local::gatherFromOverlap(bool accumulate)
{ (void)accumulate; return(0); }

int
Vector_Local::putScalar(double scalar)
{
  for(unsigned i=0; i<coefs_.size(); ++i) coefs_[i] = scalar;
  return(0);
}

int
Vector_Local::giveToVector(int numValues, const int* indices,
                           const double* values,
                           bool sumInto, int vectorIndex)
{
  if (vectorIndex != 0) {
    FEI_CERR << "fei::Vector_Local ERROR, vectorIndex!=0. Report to Alan Williams."<<FEI_ENDL;
    return(-1);
  }

  for(int i=0; i<numValues; ++i) {
    std::map<int,int>::iterator
     iter = global_to_local_.find(indices[i]);
    if (iter == global_to_local_.end()) {
      FEI_CERR << "fei::Vector_Local ERROR, eqn "<<indices[i]<<" not found "
         << "locally."<<FEI_ENDL;
      return(-1);
    }

    if (sumInto) {
      coefs_[iter->second] += values[i];
    }
    else {
      coefs_[iter->second] = values[i];
    }
  }

  return(0);
}

int
Vector_Local::sumIn(int numValues, const int* indices, const double* values,
              int vectorIndex)
{
  return( giveToVector(numValues, indices, values, true, vectorIndex) );
}

int
Vector_Local::copyIn(int numValues, const int* indices, const double* values,
               int vectorIndex)
{
  return( giveToVector(numValues, indices, values, false, vectorIndex) );
}

fei::SharedPtr<fei::VectorSpace>
Vector_Local::getVectorSpace()
{ return( vecSpace_ ); }

void
Vector_Local::setVectorSpace(fei::SharedPtr<fei::VectorSpace> vecSpace)
{ vecSpace_ = vecSpace; }

int
Vector_Local::assembleFieldData(int fieldID,
                       int idType,
                       int numIDs,
                       const int* IDs,
                       const double* data,
                       bool sumInto,
                       int vectorIndex)
{
  int fieldSize = vecSpace_->getFieldSize(fieldID);

  work_indices_.resize(numIDs*fieldSize);
  int* indicesPtr = &work_indices_[0];

  CHK_ERR( vecSpace_->getGlobalIndices(numIDs, IDs, idType, fieldID,
                                        indicesPtr) );

  CHK_ERR( giveToVector(numIDs*fieldSize, indicesPtr, data, sumInto, vectorIndex) );

  return(0);
}

int
Vector_Local::sumInFieldData(int fieldID,
                       int idType,
                       int numIDs,
                       const int* IDs,
                       const double* data,
                       int vectorIndex)
{
  return(assembleFieldData(fieldID, idType, numIDs, IDs,
                           data, true, vectorIndex));
}

int
Vector_Local::copyInFieldData(int fieldID,
                        int idType,
                        int numIDs,
                        const int* IDs,
                        const double* data,
                        int vectorIndex)
{
  return(assembleFieldData(fieldID, idType, numIDs, IDs,
                           data, false, vectorIndex));
}

int
Vector_Local::copyOutFieldData(int fieldID,
                         int idType,
                         int numIDs,
                         const int* IDs,
                         double* data,
                         int vectorIndex)
{
  int fieldSize = vecSpace_->getFieldSize(fieldID);

  work_indices_.resize(numIDs*fieldSize);
  int* indicesPtr = &work_indices_[0];

  CHK_ERR( vecSpace_->getGlobalIndices(numIDs, IDs, idType, fieldID,
                                        indicesPtr) );

  for(int i=0; i<(int)work_indices_.size(); ++i) {
    std::map<int,int>::iterator
      iter = global_to_local_.find(work_indices_[i]);
    if (iter == global_to_local_.end()) {
      FEI_CERR << "fei::Vector_Local::copyOut ERROR, eqn "<<work_indices_[i]<<" not found "
         << "locally."<<FEI_ENDL;
      return(-1);
    }

    data[i] = coefs_[iter->second];
  }

  return(0);
}

int
Vector_Local::copyOut(int numValues, const int* indices,
                      double* values, int vectorIndex) const
{
  if (vectorIndex != 0) {
    FEI_CERR << "fei::Vector_Local ERROR, vectorIndex!=0. Report to Alan Williams."<<FEI_ENDL;
    return(-1);
  }

  for(int i=0; i<numValues; ++i) {
    std::map<int,int>::const_iterator
     iter = global_to_local_.find(indices[i]);
    if (iter == global_to_local_.end()) {
      FEI_CERR << "fei::Vector_Local::copyOut ERROR, eqn "<<indices[i]<<" not found "
         << "locally."<<FEI_ENDL;
      return(-1);
    }

    values[i] = coefs_[iter->second];
  }

  return(0);
}

std::vector<double>&
Vector_Local::getCoefs()
{
  return(coefs_);
}

int
Vector_Local::writeToFile(const char* filename,
                    bool matrixMarketFormat)
{
  int localProc = vecSpace_->getCommUtils()->localProc();
  FEI_OSTRINGSTREAM osstr;
  osstr << filename << "." << localProc;
  std::string fullname = osstr.str();
  FEI_OFSTREAM ofstr(fullname.c_str(), IOS_OUT);

  return( writeToStream(ofstr, matrixMarketFormat) );
}

int
Vector_Local::writeToStream(FEI_OSTREAM& ostrm,
                      bool matrixMarketFormat)
{
  static char mmbanner[] = "%%MatrixMarket matrix array real general";

  if (matrixMarketFormat) {
    ostrm << mmbanner << FEI_ENDL;
    ostrm << coefs_.size() << " 1" << FEI_ENDL;
  }
  else {
    ostrm << coefs_.size() << FEI_ENDL;
  }

  ostrm.setf(IOS_SCIENTIFIC, IOS_FLOATFIELD);
  ostrm.precision(13);

  std::map<int,int>::iterator
    iter = global_to_local_.begin();

  for(unsigned i=0; i<coefs_.size(); ++i) {
    if (matrixMarketFormat) {
      ostrm << coefs_[i] << FEI_ENDL;
    }
    else {
      ostrm << iter->first << " " << coefs_[i] << FEI_ENDL;
      ++iter;
    }
  }

  return(0);
}

}//namespace fei

