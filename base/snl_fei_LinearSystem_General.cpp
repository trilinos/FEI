/*--------------------------------------------------------------------*/
/*    Copyright 2005 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#include <fei_macros.hpp>
#include <fei_utils.hpp>

#include <cmath>

#include <snl_fei_CommUtils.hpp>
#include <snl_fei_LinearSystem_General.hpp>
#include <fei_MatrixReducer.hpp>
#include <fei_Matrix_Impl.hpp>
#include <fei_VectorSpace.hpp>
#include <fei_MatrixGraph.hpp>
#include <fei_SparseRowGraph.hpp>
#include <snl_fei_Constraint.hpp>
#include <fei_Record.hpp>
#include <fei_utils.hpp>
#include <fei_LogManager.hpp>

#include <fei_BCRecord.hpp>
#include <fei_BCManager.hpp>
#include <fei_EqnBuffer.hpp>
#include <fei_LinSysCoreFilter.hpp>

#undef fei_file
#define fei_file "snl_fei_LinearSystem_General.cpp"
#include <fei_ErrMacros.hpp>

//----------------------------------------------------------------------------
snl_fei::LinearSystem_General::LinearSystem_General(fei::SharedPtr<fei::MatrixGraph>& matrixGraph)
  : commUtilsInt_(),
    matrixGraph_(matrixGraph),
    bcManager_(NULL),
    essBCvalues_(NULL),
    resolveConflictRequested_(false),
    bcs_trump_slaves_(false),
    explicitBCenforcement_(false),
    BCenforcement_no_column_mod_(false),
    localProc_(0),
    numProcs_(1),
    name_(),
    named_loadcomplete_counter_(),
    iwork_(),
    dwork_(),
    dbgprefix_("LinSysG: ")
{
  commUtilsInt_ = matrixGraph->getRowSpace()->getCommUtils();

  localProc_ = commUtilsInt_->localProc();
  numProcs_  = commUtilsInt_->numProcs();

  fei::SharedPtr<fei::VectorSpace> vecSpace = matrixGraph->getRowSpace();

  int* offsets = new int[numProcs_+1];
  int err = vecSpace->getGlobalIndexOffsets(numProcs_+1, offsets);
  if (err != 0) {
    voidERReturn;
  }

  firstLocalOffset_ = offsets[localProc_];
  lastLocalOffset_ = offsets[localProc_+1]-1;

  delete [] offsets;

  setName("dbg");
}

//----------------------------------------------------------------------------
snl_fei::LinearSystem_General::~LinearSystem_General()
{
  delete bcManager_;

  delete essBCvalues_;

  for(unsigned i=0; i<attributeNames_.size(); ++i) {
    delete [] attributeNames_[i];
  }
}

//----------------------------------------------------------------------------
int snl_fei::LinearSystem_General::parameters(int numParams,
				   const char* const* paramStrings)
{
  if (numParams == 0 || paramStrings == NULL) return(0);

  const char* param = snl_fei::getParam("name", numParams, paramStrings);
  if (param != NULL) {
    if (strlen(param) < 6) ERReturn(-1);

    setName(&(param[5]));
  }

  param = snl_fei::getParam("resolveConflict",numParams,paramStrings);
  if (param != NULL){
    resolveConflictRequested_ = true;
  }

  param = snl_fei::getParam("BCS_TRUMP_SLAVE_CONSTRAINTS",
                            numParams,paramStrings);
  if (param != NULL) {
    bcs_trump_slaves_ = true;
  }

  param = snl_fei::getParam("EXPLICIT_BC_ENFORCEMENT",numParams,paramStrings);
  if (param != NULL){
    explicitBCenforcement_ = true;
  }

  param = snl_fei::getParam("BC_ENFORCEMENT_NO_COLUMN_MOD",numParams,paramStrings);
  if (param != NULL){
    BCenforcement_no_column_mod_ = true;
  }

  param = snl_fei::getParamValue("FEI_OUTPUT_LEVEL",numParams,paramStrings);
  if (param != NULL) {
    setOutputLevel(fei::utils::string_to_output_level(param));
  }

  if (matrix_.get() != NULL) {
    fei::Matrix* matptr = matrix_.get();
    fei::MatrixReducer* matred = dynamic_cast<fei::MatrixReducer*>(matptr);
    if (matred != NULL) {
      matptr = matred->getTargetMatrix().get();
    }
    fei::Matrix_Impl<LinearSystemCore>* lscmatrix =
      dynamic_cast<fei::Matrix_Impl<LinearSystemCore>*>(matptr);
    if (lscmatrix != NULL) {
      lscmatrix->getMatrix()->parameters(numParams, (char**)paramStrings);
    }
  }

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei::LinearSystem_General::parameters(const fei::ParameterSet& params)
{
  int numParams = 0;
  const char** paramStrings = NULL;
  std::vector<std::string> stdstrings;
  fei::utils::convert_ParameterSet_to_strings(&params, stdstrings);
  fei::utils::strings_to_char_ptrs(stdstrings, numParams, paramStrings);

  int err = parameters(numParams, paramStrings);

  delete [] paramStrings;

  return(err);
}

//----------------------------------------------------------------------------
void snl_fei::LinearSystem_General::setName(const char* name)
{
  if (name == NULL) return;

  if (name_ == name) return;

  name_ = name;

  std::map<std::string,unsigned>::iterator
    iter = named_loadcomplete_counter_.find(name_);
  if (iter == named_loadcomplete_counter_.end()) {
    named_loadcomplete_counter_.insert(std::make_pair(name_, 0));
  }
}

//----------------------------------------------------------------------------
int snl_fei::LinearSystem_General::loadEssentialBCs(int numIDs,
			 const int* IDs,
			 int idType,
			 int fieldID,
			 int fieldSize,
			 const double *const *gammaValues,
                         const double *const *alphaValues)
{
  if (output_level_ >= fei::BRIEF_LOGS && output_stream_ != 0) {
    FEI_OSTREAM& os = *output_stream_;
    os << "loadEssentialBCs, numIDs: "<<numIDs<<", idType: " <<idType
       <<", fieldID: "<<fieldID<<", fieldSize: "<<fieldSize<<FEI_ENDL;
  }

  if (bcManager_ == NULL) {
    bcManager_ = new BCManager;
  }

  try {
    bcManager_->addBCRecords(idType, numIDs, IDs, fieldID, fieldSize,
			     gammaValues, alphaValues);
  }
  catch(fei::Exception& exc) {
    FEI_CERR << exc.what()<<FEI_ENDL;
    return(-1);
  }
  return(0);
}

//----------------------------------------------------------------------------
int snl_fei::LinearSystem_General::loadComplete(bool applyBCs,
                                                bool globalAssemble)
{
  if (output_level_ >= fei::BRIEF_LOGS && output_stream_ != 0) {
    FEI_OSTREAM& os = *output_stream_;
    os << dbgprefix_<<"loadComplete"<<FEI_ENDL;
  }

  if (bcManager_ == NULL) {
    bcManager_ = new BCManager;
  }

  if (globalAssemble) {

    if (matrix_.get() != NULL) {
      CHK_ERR( matrix_->gatherFromOverlap() );
    }

    if (rhs_.get() != NULL) {
      CHK_ERR( rhs_->gatherFromOverlap() );
    }

  }

  unsigned counter = 0;

  std::map<std::string,unsigned>::iterator
    iter = named_loadcomplete_counter_.find(name_);
  if (iter == named_loadcomplete_counter_.end()) {
    FEI_COUT << "fei::LinearSystem::loadComplete internal error, name "
      << name_ << " not found." << FEI_ENDL;
  }
  else {
    counter = iter->second++;
  }

  if (output_level_ >= fei::FULL_LOGS) {
    std::string opath = fei::LogManager::getLogManager().getOutputPath();
    if (opath == "") opath = ".";

    FEI_OSTRINGSTREAM Aname;
    FEI_OSTRINGSTREAM bname;
    FEI_OSTRINGSTREAM xname;
    Aname << opath << "/";
    bname << opath << "/";
    xname << opath << "/";

    Aname << "A_"<<name_<<".preBC.np"<<numProcs_<<".slv"<<counter<< ".mtx";

    bname << "b_"<<name_<<".preBC.np"<<numProcs_<<".slv"<<counter<< ".vec";

    std::string Aname_str = Aname.str();
    const char* Aname_c_str = Aname_str.c_str();
    CHK_ERR( matrix_->writeToFile(Aname_c_str) );

    std::string bname_str = bname.str();
    const char* bname_c_str = bname_str.c_str();
    CHK_ERR( rhs_->writeToFile(bname_c_str) );
  }

  CHK_ERR( implementBCs(applyBCs) );

  if (globalAssemble) {
    CHK_ERR( matrix_->globalAssemble() );
  }

  if (output_level_ == fei::STATS || output_level_ == fei::ALL) {
    int globalNumSlaveCRs = matrixGraph_->getGlobalNumSlaveConstraints();
    if (commUtilsInt_->localProc() == 0) {
      FEI_COUT << "Global Neqns: " << matrix_->getGlobalNumRows();
      if (globalNumSlaveCRs > 0) {
	FEI_COUT << ", Global NslaveCRs: " << globalNumSlaveCRs;
      }
      FEI_COUT << FEI_ENDL;
    }
  }

  if (output_level_ >= fei::BRIEF_LOGS && output_stream_ != NULL) {
    FEI_OSTREAM& os = *output_stream_;
    os << dbgprefix_<<"Neqns=" << matrix_->getGlobalNumRows();
    int globalNumSlaveCRs = matrixGraph_->getGlobalNumSlaveConstraints();
    if (globalNumSlaveCRs > 0) {
      os << ", Global NslaveCRs=" << globalNumSlaveCRs;
    }
    os << FEI_ENDL;
  }

  if (output_level_ >= fei::MATRIX_FILES) {
    std::string opath = fei::LogManager::getLogManager().getOutputPath();
    if (opath == "") opath = ".";

    FEI_OSTRINGSTREAM Aname;
    FEI_OSTRINGSTREAM bname;
    FEI_OSTRINGSTREAM xname;
    Aname << opath << "/";
    bname << opath << "/";
    xname << opath << "/";

    Aname << "A_" <<name_<<".np"<<numProcs_<< ".slv" << counter << ".mtx";

    bname << "b_" <<name_<<".np"<<numProcs_<< ".slv" << counter << ".vec";

    xname << "x0_" <<name_<<".np"<<numProcs_<< ".slv" << counter << ".vec";

    std::string Aname_str = Aname.str();
    const char* Aname_c_str = Aname_str.c_str();
    CHK_ERR( matrix_->writeToFile(Aname_c_str) );

    std::string bname_str = bname.str();
    const char* bname_c_str = bname_str.c_str();
    CHK_ERR( rhs_->writeToFile(bname_c_str) );

    std::string xname_str = xname.str();
    const char* xname_c_str = xname_str.c_str();
    CHK_ERR( soln_->writeToFile(xname_c_str) );
  }

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei::LinearSystem_General::setBCValuesOnVector(fei::Vector* vector)
{
  if (essBCvalues_ == NULL) {
    return(0);
  }

  CHK_ERR( vector->copyIn(essBCvalues_->length(),
			  essBCvalues_->indices().dataPtr(),
			  essBCvalues_->coefs().dataPtr()) );

  return(0);
}

//----------------------------------------------------------------------------
bool snl_fei::LinearSystem_General::eqnIsEssentialBC(int globalEqnIndex) const
{
  if (essBCvalues_ == NULL) return(false);

  feiArray<int>& indices = essBCvalues_->indices();
  int offset = snl_fei::binarySearch(globalEqnIndex, indices);
  return( offset < 0 ? false : true);
}

//----------------------------------------------------------------------------
void snl_fei::LinearSystem_General::getEssentialBCs(std::vector<int>& bcEqns,
                                             std::vector<double>& bcVals) const
{
  bcEqns.clear();
  bcVals.clear();
  if (essBCvalues_ == NULL) return;

  int num = essBCvalues_->length();
  bcEqns.resize(num);
  bcVals.resize(num);
  int* essbcs = essBCvalues_->indices().dataPtr();
  double* vals = essBCvalues_->coefs().dataPtr();
  for(int i=0; i<num; ++i) {
    bcEqns[i] = essbcs[i];
    bcVals[i] = vals[i];
  }
}

//----------------------------------------------------------------------------
void snl_fei::LinearSystem_General::getConstrainedEqns(std::vector<int>& crEqns) const
{
  matrixGraph_->getConstrainedIndices(crEqns);
}

//----------------------------------------------------------------------------
int snl_fei::LinearSystem_General::implementBCs(bool applyBCs)
{
  int numLocalBCs = bcManager_->getNumBCs();
  int globalNumBCs = 0;
  matrixGraph_->getRowSpace()->getCommUtils()->GlobalSum(numLocalBCs, globalNumBCs);
  if (globalNumBCs == 0) {
    return(0);
  }

  feiArray<int> essEqns(0, 512), otherEqns(0, 512);
  feiArray<double> essAlpha(0, 512), essGamma(0, 512);
  feiArray<double> otherAlpha(0, 512), otherBeta(0, 512), otherGamma(0, 512);

  fei::SharedPtr<SSMat> localBCeqns(new SSMat);
  fei::SharedPtr<fei::Matrix_Impl<SSMat> > bcEqns;
  matrixGraph_->getRowSpace()->initComplete();
  int numSlaves = matrixGraph_->getGlobalNumSlaveConstraints();
  fei::SharedPtr<fei::Reducer> reducer = matrixGraph_->getReducer();

  int numIndices = numSlaves>0 ?
    reducer->getLocalReducedEqns().size() :
    matrixGraph_->getRowSpace()->getNumIndices_Owned();

  bcEqns.reset(new fei::Matrix_Impl<SSMat>(localBCeqns, matrixGraph_, numIndices));
  fei::SharedPtr<fei::Matrix> bcEqns_reducer;
  if (numSlaves > 0) {
    bcEqns_reducer.reset(new fei::MatrixReducer(reducer, bcEqns));
  }

  fei::Matrix& bcEqns_mat = bcEqns_reducer.get()==NULL ?
      *bcEqns : *bcEqns_reducer;

  fei::SharedPtr<fei::VectorSpace> vecSpace = matrixGraph_->getRowSpace();

  CHK_ERR( bcManager_->finalizeBCEqns(*vecSpace, bcEqns_mat, bcs_trump_slaves_) );

  if (resolveConflictRequested_) {
    fei::SharedPtr<SSMat> ssmat = bcEqns->getMatrix();
    feiArray<int>& bcEqnNumbers = ssmat->getRowNumbers();
    CHK_ERR( snl_fei::resolveConflictingCRs(*matrixGraph_, bcEqns_mat,
                                            bcEqnNumbers) );
  }

  std::vector<SSMat*>& remote = bcEqns->getRemotelyOwnedMatrix();
  for(unsigned p=0; p<remote.size(); ++p) {
    CHK_ERR( snl_fei::separateBCEqns( *(remote[p]), essEqns, essAlpha,
                                     essGamma, otherEqns, otherAlpha,
                                     otherBeta, otherGamma) );
  }

  CHK_ERR( bcEqns->gatherFromOverlap(false) );

  CHK_ERR( snl_fei::separateBCEqns( *(bcEqns->getMatrix()),
				    essEqns, essAlpha, essGamma,
				    otherEqns, otherAlpha, otherBeta, otherGamma) );

  if (otherEqns.length() > 0) {
    FEI_OSTRINGSTREAM osstr;
    osstr << "snl_fei::LinearSystem_General::implementBCs: ERROR, unexpected "
	  << "'otherEqns', (meaning non-dirichlet or non-essential BCs).";
    throw fei::Exception(osstr.str());
  }

  if (essBCvalues_ != NULL) {
    delete essBCvalues_;
  }

  essBCvalues_ = new SSVec;

  int* essEqnsPtr = essEqns.dataPtr();
  double* gammaPtr = essGamma.dataPtr();
  double* alphaPtr = essAlpha.dataPtr();

  for(int i=0; i<essEqns.length(); ++i) {
    int eqn = essEqnsPtr[i];
    double value = gammaPtr[i]/alphaPtr[i];
    CHK_ERR( essBCvalues_->putEntry(eqn, value) );
  }

  if (output_level_ >= fei::BRIEF_LOGS && output_stream_ != NULL) {
    FEI_OSTREAM& os = *output_stream_;
    feiArray<int>& indices = essBCvalues_->indices();
    feiArray<double>& coefs= essBCvalues_->coefs();
    for(int i=0; i<essBCvalues_->length(); ++i) {
      os << "essBCeqns["<<i<<"]: "<<indices[i]<<", "<<coefs[i]<<FEI_ENDL;
    }
  }

  //If the underlying matrix is a LinearSystemCore instance, then this
  //function will return 0, and we're done. A non-zero return-code means
  //we should continue and enforce the BCs assuming a general matrix.

  int returncode = enforceEssentialBC_LinSysCore();
  if (returncode == 0) {
    return(0);
  }

  SSVec allEssBCs;
  if (!BCenforcement_no_column_mod_) {
    snl_fei::globalUnion(commUtilsInt_->getCommunicator(),
                         *essBCvalues_, allEssBCs);

    if (output_level_ >= fei::BRIEF_LOGS && output_stream_ != NULL) {
      FEI_OSTREAM& os = *output_stream_;
      os << "  implementBCs, essEqns.length(): "<<essEqns.length()
         << ", allEssBCs.length(): " << allEssBCs.length()<<FEI_ENDL;
    }
  }

  if (essBCvalues_->length() > 0) {
    enforceEssentialBC_step_1(*essBCvalues_);
  }

  if (!BCenforcement_no_column_mod_ && allEssBCs.length() > 0) {
    enforceEssentialBC_step_2(allEssBCs);
  }

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei::LinearSystem_General::enforceEssentialBC_LinSysCore()
{
  fei::Matrix* matptr = matrix_.get();
  fei::MatrixReducer* matred = dynamic_cast<fei::MatrixReducer*>(matptr);
  if (matred != NULL) {
    matptr = matred->getTargetMatrix().get();
  }

  fei::Matrix_Impl<LinearSystemCore>* lscmatrix =
    dynamic_cast<fei::Matrix_Impl<LinearSystemCore>*>(matptr);
  if (lscmatrix == 0) {
    return(-1);
  }

  int localsize = matrixGraph_->getRowSpace()->getNumIndices_Owned();
  fei::SharedPtr<fei::Reducer> reducer = matrixGraph_->getReducer();
  if (matrixGraph_->getGlobalNumSlaveConstraints() > 0) {
    localsize = reducer->getLocalReducedEqns().size();
  }

  fei::SharedPtr<SSMat> inner(new SSMat);
  fei::SharedPtr<fei::Matrix_Impl<SSMat> > matrix;
  matrix.reset(new fei::Matrix_Impl<SSMat>(inner, matrixGraph_, localsize));

  fei::SharedPtr<fei::SparseRowGraph> remoteGraph =
    matrixGraph_->getRemotelyOwnedGraphRows();

  CHK_ERR( snl_fei::gatherRemoteEssBCs(*essBCvalues_, remoteGraph.get(), *matrix) );

  feiArray<int>& rowNumbers = inner->getRowNumbers();
  if (output_stream_ != NULL) {
    if (output_level_ >= fei::BRIEF_LOGS) {
      FEI_OSTREAM& os = *output_stream_;
      os << "#enforceEssentialBC_LinSysCore RemEssBCs to enforce: "
         << rowNumbers.length() << FEI_ENDL;
    }
  }

  if (rowNumbers.length() > 0) {
    feiArray<SSVec*>& rows    = inner->getRows();

    feiArray<int*> colIndices(rows.length());
    feiArray<double*> coefs(rows.length());
    feiArray<int> colIndLengths(rows.length());

    for(int i=0; i<rows.length(); ++i) {
      SSVec* row = rows[i];
      colIndices[i] = row->indices().dataPtr();
      coefs[i] = row->coefs().dataPtr();
      colIndLengths[i] = row->indices().length();
    }

    int numEqns = rows.length();
    int* eqns = rowNumbers.dataPtr();
    int** colInds = colIndices.dataPtr();
    int* colIndLens = colIndLengths.dataPtr();
    double** BCcoefs = coefs.dataPtr();

    if (output_stream_ != NULL) {
      if (output_level_ > fei::BRIEF_LOGS) {
        FEI_OSTREAM& os = *output_stream_;
        for(int i=0; i<numEqns; ++i) {
          os << "remBCeqn: " << eqns[i] << ", inds/coefs: ";
          for(int j=0; j<colIndLens[i]; ++j) {
            os << "("<<colInds[i][j]<<","<<BCcoefs[i][j]<<") ";
          }
          os << FEI_ENDL;
        }
      }
    }

    int errcode = lscmatrix->getMatrix()->enforceRemoteEssBCs(numEqns,
							      eqns,
							      colInds,
							      colIndLens,
							      BCcoefs);
    if (errcode != 0) {
      return(errcode);
    }
  }

  int numEqns = essBCvalues_->length();
  int* eqns = essBCvalues_->indices().dataPtr();
  double* bccoefs = essBCvalues_->coefs().dataPtr();
  feiArray<double> ones(numEqns);
  ones = 1.0;

  return(lscmatrix->getMatrix()->enforceEssentialBC(eqns, ones.dataPtr(),
						    bccoefs, numEqns));
}

//----------------------------------------------------------------------------
void snl_fei::LinearSystem_General::enforceEssentialBC_step_1(SSVec& essBCs)
{
  //to enforce essential boundary conditions, we do the following:
  //
  //  1.  for each eqn (== essBCs.indices()[n]), {
  //        put zeros in row A[eqn], but leave 1.0 on the diagonal
  //        set b[eqn] = essBCs.coefs()[n]
  //      }
  //
  //  2.  for i in 1..numRows (i.e., all rows) {
  //        if (i in bcEqns) continue;
  //        b[i] -= A[i,eqn] * essBCs.coefs()[n]
  //        A[i,eqn] = 0.0;
  //      }
  //
  //It is important to note that for step 1, essBCs need only contain
  //local eqns, but for step 2 it should contain *ALL* bc eqns.
  //
  //This function performs step 1.

  int numEqns = essBCs.length();
  int* eqns = essBCs.indices().dataPtr();
  double* bcCoefs = essBCs.coefs().dataPtr();

  std::vector<double> coefs;
  std::vector<int> indices;

  fei::SharedPtr<fei::Reducer> reducer = matrixGraph_->getReducer();
  bool haveSlaves = reducer.get()!=NULL;

  try {
  for(int i=0; i<numEqns; i++) {
    int eqn = eqns[i];

    //if slave-constraints are present, the incoming bc-eqns are in
    //the reduced equation space. so we actually have to translate them back
    //to the unreduced space before passing them into the fei::Matrix object,
    //because the fei::Matrix object has machinery to translate unreduced eqns
    //to the reduced space.
    //Also, our firstLocalOffset_ and lastLocalOffset_ attributes are in the
    //unreduced space.
    if (haveSlaves) {
      eqn = reducer->translateFromReducedEqn(eqn);
    }

    if (eqn < firstLocalOffset_ || eqn > lastLocalOffset_) continue;

    //put gamma/alpha on the rhs for this ess-BC equation.
    double bcValue = bcCoefs[i];
    int err = rhs_->copyIn(1, &eqn, &bcValue);
    if (err != 0) {
      FEI_OSTRINGSTREAM osstr;
      osstr <<"snl_fei::LinearSystem_General::enforceEssentialBC_step_1 ERROR: "
	    << "err="<<err<<" returned from rhs_->copyIn row="<<eqn;
      throw fei::Exception(osstr.str());
    }

    err = getMatrixRow(matrix_.get(), eqn, coefs, indices);
    if (err != 0 || indices.size() < 1) {
      continue;
    }

    int rowLen = indices.size();
    int* indPtr = &indices[0];

    //first, put zeros in the row and 1.0 on the diagonal...
    for(int j=0; j<rowLen; j++) {
      if (indPtr[j] == eqn) coefs[j] = 1.0;
      else coefs[j] = 0.0;
    }

    double* coefPtr = &coefs[0];

    err = matrix_->copyIn(1, &eqn, rowLen, indPtr, &coefPtr);
    if (err != 0) {
      FEI_OSTRINGSTREAM osstr;
      osstr <<"snl_fei::LinearSystem_General::enforceEssentialBC_step_1 ERROR: "
	    << "err="<<err<<" returned from matrix_->copyIn row="<<eqn;
      throw fei::Exception(osstr.str());
    }
  }//for i
  }
  catch(fei::Exception& exc) {
    FEI_OSTRINGSTREAM osstr;
    osstr << "fei::LinearSystem::enforceEssentialBC: ERROR, caught exception: "
        << exc.what();
    throw fei::Exception(osstr.str());
  }
}

//----------------------------------------------------------------------------
void snl_fei::LinearSystem_General::enforceEssentialBC_step_2(SSVec& essBCs)
{
  //to enforce essential boundary conditions, we do the following:
  //
  //  1.  for each eqn (== essBCs.indices()[n]), {
  //        put zeros in row A[eqn], but leave 1.0 on the diagonal
  //        set b[eqn] = essBCs.coefs()[n]
  //      }
  //
  //  2.  for i in 1..numRows (i.e., all rows) {
  //        if (i in bcEqns) continue;
  //        b[i] -= A[i,eqn] * essBCs.coefs()[n]
  //        A[i,eqn] = 0.0;
  //      }
  //
  //It is important to note that for step 1, essBCs need only contain
  //local eqns, but for step 2 it should contain *ALL* bc eqns.
  //
  //This function performs step 2.

  int numBCeqns = essBCs.length();
  if (numBCeqns < 1) {
    return;
  }

  int* bcEqns = essBCs.indices().dataPtr();
  double* bcCoefs = essBCs.coefs().dataPtr();

  fei::SharedPtr<fei::Reducer> reducer = matrixGraph_->getReducer();
  bool haveSlaves = reducer.get()!=NULL;
  if (haveSlaves) {
    for(int i=0; i<numBCeqns; ++i) {
      bcEqns[i] = reducer->translateFromReducedEqn(bcEqns[i]);
    }
  }

  int firstBCeqn = bcEqns[0];
  int lastBCeqn = bcEqns[numBCeqns-1];

  std::vector<double> coefs;
  std::vector<int> indices;

  int insertPoint;

  int nextBCeqnOffset = 0;
  int nextBCeqn = bcEqns[nextBCeqnOffset];

  for(int i=firstLocalOffset_; i<=lastLocalOffset_; ++i) {
    if (haveSlaves) {
      if (reducer->isSlaveEqn(i)) continue;
    }

    bool should_continue = false;
    if (i >= nextBCeqn) {
      if (i == nextBCeqn) {
	++nextBCeqnOffset;
	if (nextBCeqnOffset < numBCeqns) {
	  nextBCeqn = bcEqns[nextBCeqnOffset];
	}
	else {
	  nextBCeqn = lastLocalOffset_+1;
	}

	should_continue = true;
      }
      else {
	while(nextBCeqn <= i) {
	  if (nextBCeqn == i) should_continue = true;
	  ++nextBCeqnOffset;
	  if (nextBCeqnOffset < numBCeqns) {
	    nextBCeqn = bcEqns[nextBCeqnOffset];
	  }
	  else {
	    nextBCeqn = lastLocalOffset_+1;
	  }
	}
      }
    }

    if (should_continue) continue;

    int err = getMatrixRow(matrix_.get(), i, coefs, indices);
    if (err != 0 || indices.size() < 1) {
      continue;
    }

    int numIndices = indices.size();
    int* indicesPtr = &indices[0];
    double* coefsPtr = &coefs[0];
    bool modifiedCoef = false;

    snl_fei::insertion_sort_with_companions(numIndices, indicesPtr, coefsPtr);

    if (indicesPtr[0] > lastBCeqn || indicesPtr[numIndices-1] < firstBCeqn) {
      continue;
    }

    double value = 0.0;
    int offset = 0;

    for(int j=0; j<numIndices; ++j) {
      int idx = indicesPtr[j];
      offset = snl_fei::binarySearch(idx, bcEqns, numBCeqns,
				     insertPoint);
      if (offset > -1) {
	value -= bcCoefs[offset]*coefsPtr[j];

	coefsPtr[j] = 0.0;
	modifiedCoef = true;
      }
    }

    if (modifiedCoef) {
      err = matrix_->copyIn(1, &i, numIndices, indicesPtr, &coefsPtr);
      if (err != 0) {
	FEI_OSTRINGSTREAM osstr;
	osstr <<"snl_fei::LinearSystem_General::enforceEssentialBC_step_2 ERROR: "
	      << "err="<<err<<" returned from matrix_->copyIn, row="<<i;
	throw fei::Exception(osstr.str());
      }
    }

    const double fei_eps = 1.e-49;
    if (std::abs(value) > fei_eps) {
      rhs_->sumIn(1, &i, &value);

      if (output_level_ >= fei::FULL_LOGS && output_stream_ != 0) {
	FEI_OSTREAM& os = *output_stream_;
	os << "enfEssBC_step2: rhs["<<i<<"] += "<<value<<FEI_ENDL;
      }
    }
  }
}

//----------------------------------------------------------------------------
int snl_fei::LinearSystem_General::getMatrixRow(fei::Matrix* matrix, int row,
						std::vector<double>& coefs,
						std::vector<int>& indices)
{
  int len = 0;
  int err = matrix->getRowLength(row, len);
  if (err != 0) {
    coefs.resize(0);
    indices.resize(0);
    return(err);
  }

  coefs.resize(len);
  indices.resize(len);

  CHK_ERR( matrix->copyOutRow(row, len, &coefs[0], &indices[0]));

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei::LinearSystem_General::loadLagrangeConstraint(int constraintID,
							  const double *weights,
							  double rhsValue)
{
  if (output_level_ >= fei::BRIEF_LOGS && output_stream_ != NULL) {
    FEI_OSTREAM& os = *output_stream_;
    os << "loadLagrangeConstraint crID: "<<constraintID<<FEI_ENDL;
  }

  Constraint<fei::Record*,fei::record_lessthan>* cr =
    matrixGraph_->getLagrangeConstraint(constraintID);
  if (cr == NULL) {
    return(-1);
  }

  CHK_ERR( matrixGraph_->getConstraintConnectivityIndices(cr, iwork_) );

  //Let's attach the weights to the constraint-record now.
  feiArray<double>* cr_weights = cr->getMasterWeights();
  cr_weights->resize(iwork_.size());
  for(unsigned i=0; i<iwork_.size(); ++i) {
    cr_weights->append(weights[i]);
  }

  fei::SharedPtr<fei::VectorSpace> vecSpace = matrixGraph_->getRowSpace();

  int crEqn = -1;
  CHK_ERR( vecSpace->getGlobalIndex(cr->getIDType(),
				     cr->getConstraintID(),
				     crEqn) );

  //now add the row contribution to the matrix and rhs
  int numIndices = iwork_.size();
  int* indicesPtr = &(iwork_[0]);

  CHK_ERR( matrix_->sumIn(1, &crEqn, numIndices, indicesPtr, &weights) );

  CHK_ERR( rhs_->sumIn(1, &crEqn, &rhsValue) );

  //now add the column contributions to the matrix
  for(int k=0; k<numIndices; ++k) {
    double* thisWeight = (double*)(&(weights[k]));
    CHK_ERR( matrix_->sumIn(1, &(indicesPtr[k]), 1, &crEqn, &thisWeight) );
  }

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei::LinearSystem_General::loadPenaltyConstraint(int constraintID,
							 const double *weights,
							 double penaltyValue,
							 double rhsValue)
{
  if (output_level_ >= fei::BRIEF_LOGS && output_stream_ != NULL) {
    FEI_OSTREAM& os = *output_stream_;
    os << "loadPenaltyConstraint crID: "<<constraintID<<FEI_ENDL;
  }

  Constraint<fei::Record*,fei::record_lessthan>* cr =
    matrixGraph_->getPenaltyConstraint(constraintID);
  if (cr == NULL) {
    return(-1);
  }

  CHK_ERR( matrixGraph_->getConstraintConnectivityIndices(cr, iwork_) );

  fei::SharedPtr<fei::VectorSpace> vecSpace = matrixGraph_->getRowSpace();

  int numIndices = iwork_.size();
  int* indicesPtr = &(iwork_[0]);

  //now add the contributions to the matrix and rhs
  feiArray<double> coefs(numIndices);
  double* coefPtr = coefs.dataPtr();
  for(int i=0; i<numIndices; ++i) {
    for(int j=0; j<numIndices; ++j) {
      coefPtr[j] = weights[i]*weights[j]*penaltyValue;
    }
    CHK_ERR( matrix_->sumIn(1, &(indicesPtr[i]), numIndices, indicesPtr,
			    &coefPtr) );

    double rhsCoef = weights[i]*penaltyValue*rhsValue;
    CHK_ERR( rhs_->sumIn(1, &(indicesPtr[i]), &rhsCoef) );
  }

  return(0);
}

