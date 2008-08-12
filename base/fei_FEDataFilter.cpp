/*--------------------------------------------------------------------*/
/*    Copyright 2005 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#include <fei_macros.hpp>

#include <limits>
#include <cmath>

#include <fei_defs.h>

#include <feiArray.hpp>
#include <snl_fei_CommUtils.hpp>
#include <fei_TemplateUtils.hpp>
#include <snl_fei_Constraint.hpp>
typedef snl_fei::Constraint<GlobalID> ConstraintType;

#include <fei_LibraryWrapper.hpp>
#include <SNL_FEI_Structure.hpp>
#include <fei_FiniteElementData.hpp>
#include <fei_Lookup.hpp>
#include <FEI_Implementation.hpp>
#include <fei_EqnCommMgr.hpp>
#include <fei_EqnBuffer.hpp>
#include <fei_NodeDatabase.hpp>
#include <fei_NodeCommMgr.hpp>
#include <fei_ProcEqns.hpp>
#include <fei_SSVec.hpp>
#include <fei_SSMat.hpp>
#include <fei_BlockDescriptor.hpp>
#include <fei_ConnectivityTable.hpp>
#include <snl_fei_Utils.hpp>

#include <fei_FEDataFilter.hpp>

#undef fei_file
#define fei_file "FEDataFilter.cpp"
#include <fei_ErrMacros.hpp>

#define ASSEMBLE_PUT 0
#define ASSEMBLE_SUM 1

//------------------------------------------------------------------------------
FEDataFilter::FEDataFilter(FEI_Implementation* owner,
			   MPI_Comm comm, snl_fei::CommUtils<int>* commUtils, 
			   SNL_FEI_Structure* probStruct,
			   LibraryWrapper* wrapper,
			   int masterRank)
 : Filter(probStruct),
   wrapper_(wrapper),
   feData_(NULL),
   useLookup_(true),
   internalFei_(0),
   newData_(false),
   localStartRow_(0),
   localEndRow_(0),
   numGlobalEqns_(0),
   reducedStartRow_(0),
   reducedEndRow_(0),
   numReducedRows_(0),
   iterations_(0),
   numRHSs_(0),
   currentRHS_(0),
   rhsIDs_(0, 8),
   outputLevel_(0),
   comm_(comm),
   masterRank_(masterRank),
   commUtils_(commUtils),
   deleteCommUtils_(false),
   problemStructure_(probStruct),
   penCRIDs_(0, 1000),
   rowIndices_(),
   rowColOffsets_(0, 256),
   colIndices_(0, 256),
   putRHSVec_(NULL),
   eqnCommMgr_(NULL),
   eqnCommMgr_put_(NULL),
   maxElemRows_(0),
   eStiff_(NULL),
   eStiff1D_(NULL),
   eLoad_(NULL),
   numRegularElems_(0),
   constraintBlocks_(0, 16),
   constraintNodeOffsets_(0, 16),
   packedFieldSizes_(0, 32)
{

#ifndef FEI_SER
  //  initialize a couple of MPI things

  MPI_Comm_rank(comm_, &localRank_);
  MPI_Comm_size(comm_, &numProcs_);
#else
  localRank_ = 0;
  numProcs_ = 1;
#endif

  if (commUtils_ == NULL) {
    commUtils_ = new snl_fei::CommUtils<int>(comm_);
    deleteCommUtils_ = true;
  }

  internalFei_ = 0;

  numRHSs_ = 1;
  rhsIDs_.resize(numRHSs_);
  rhsIDs_[0] = 0;

  eqnCommMgr_ = problemStructure_->getEqnCommMgr().deepCopy();
  createEqnCommMgr_put();

  if (wrapper_->haveFiniteElementData()) {
    feData_ = wrapper_->getFiniteElementData();
  }
  else {
    FEI_CERR << "FEDataFilter::FEDataFilter ERROR, must be constructed with a "
	 << "FiniteElementData interface. Aborting." << FEI_ENDL;
#ifndef FEI_SER
    MPI_Abort(comm_, -1);
#else
    abort();
#endif
  }

  //We need to get the parameters from the owning FEI_Implementation, if we've
  //been given a non-NULL FEI_Implementation...
  if (owner != NULL) {
    int numParams = -1;
    char** paramStrings = NULL;
    int err = owner->getParameters(numParams, paramStrings);

    //Now let's pass them into our own parameter-handling mechanism.
    err = parameters(numParams, paramStrings);
    if (err != 0) {
      FEI_CERR << "FEDataFilter::FEDataFilter ERROR, parameters failed." << FEI_ENDL;
      MPI_Abort(comm_, -1);
    }
  }

  return;
}

//------------------------------------------------------------------------------
FEDataFilter::FEDataFilter(const FEDataFilter& src)
 : Filter(NULL),
   wrapper_(NULL),
   feData_(NULL),
   useLookup_(true),
   internalFei_(0),
   newData_(false),
   localStartRow_(0),
   localEndRow_(0),
   numGlobalEqns_(0),
   reducedStartRow_(0),
   reducedEndRow_(0),
   numReducedRows_(0),
   iterations_(0),
   numRHSs_(0),
   currentRHS_(0),
   rhsIDs_(0, 8),
   outputLevel_(0),
   comm_(0),
   masterRank_(0),
   commUtils_(NULL),
   deleteCommUtils_(false),
   problemStructure_(NULL),
   penCRIDs_(0, 1000),
   rowIndices_(),
   rowColOffsets_(0, 256),
   colIndices_(0, 256),
   putRHSVec_(NULL),
   eqnCommMgr_(NULL),
   eqnCommMgr_put_(NULL),
   maxElemRows_(0),
   eStiff_(NULL),
   eStiff1D_(NULL),
   eLoad_(NULL),
   numRegularElems_(0),
   constraintBlocks_(0, 16),
   constraintNodeOffsets_(0, 16),
   packedFieldSizes_(0, 32)
{
}

//------------------------------------------------------------------------------
FEDataFilter::~FEDataFilter() {
//
//  Destructor function. Free allocated memory, etc.
//
  numRHSs_ = 0;

  delete eqnCommMgr_;
  delete eqnCommMgr_put_;

  delete [] eStiff_;
  delete [] eStiff1D_;
  delete [] eLoad_;

  if (deleteCommUtils_) delete commUtils_;

  delete putRHSVec_;
}

//------------------------------------------------------------------------------
int FEDataFilter::initialize()
{
// Determine final sparsity pattern for setting the structure of the
// underlying sparse matrix.
//
  debugOutput("#  initialize");

  // now, obtain the global equation info, such as how many equations there
  // are globally, and what the local starting and ending row-numbers are.

  // let's also get the number of global nodes, and a first-local-node-number.
  // node-number is a globally 0-based number we are assigning to nodes.
  // node-numbers are contiguous on a processor -- i.e., a processor owns a
  // contiguous block of node-numbers. This provides an easier-to-work-with
  // node numbering than the application-supplied nodeIDs which may not be
  // assumed to be contiguous or 0-based, or anything else.

  feiArray<int>& eqnOffsets = problemStructure_->getGlobalEqnOffsets();
  localStartRow_ = eqnOffsets[localRank_];
  localEndRow_ = eqnOffsets[localRank_+1]-1;
  numGlobalEqns_ = eqnOffsets[numProcs_];

  //--------------------------------------------------------------------------
  //  ----- end active equation calculations -----

  if (eqnCommMgr_ != NULL) delete eqnCommMgr_;
  eqnCommMgr_ = NULL;
  if (eqnCommMgr_put_ != NULL) delete eqnCommMgr_put_;
  eqnCommMgr_put_ = NULL;

  eqnCommMgr_ = problemStructure_->getEqnCommMgr().deepCopy();
  if (eqnCommMgr_ == NULL) ERReturn(-1);

  int err = createEqnCommMgr_put();
  if (err != 0) ERReturn(err);

  //(we need to set the number of RHSs in the eqn comm manager)
  eqnCommMgr_->setNumRHSs(numRHSs_);

  //let's let the underlying linear system know what the global offsets are.
  //While we're dealing with global offsets, we'll also calculate the starting
  //and ending 'reduced' rows, etc.
  CHK_ERR( initLinSysCore() );

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::createEqnCommMgr_put()
{
  if (eqnCommMgr_put_ != NULL) return(0);

  eqnCommMgr_put_  = eqnCommMgr_->deepCopy();
  if (eqnCommMgr_put_ == NULL) ERReturn(-1);

  eqnCommMgr_put_->resetCoefs();
  eqnCommMgr_put_->accumulate_ = false;
  return(0);
}

//==============================================================================
int FEDataFilter::initLinSysCore()
{
  try {

  int err = wrapper_->getFiniteElementData()->setLookup(*problemStructure_);

  if (err != 0) {
    useLookup_ = false;
  }

  reducedStartRow_ = localStartRow_;
  reducedEndRow_ = localEndRow_;

  int numElemBlocks = problemStructure_->getNumElemBlocks();
  NodeDatabase& nodeDB     = problemStructure_->getNodeDatabase();
  NodeCommMgr& nodeCommMgr = problemStructure_->getNodeCommMgr();

  int numNodes = nodeDB.getNumNodeDescriptors();
  int numRemoteNodes = nodeCommMgr.getSharedNodeIDs().size() -
                         nodeCommMgr.getLocalNodeIDs().size();
  numNodes -= numRemoteNodes;

  int numSharedNodes = nodeCommMgr.getNumSharedNodes();

  std::vector<int> numElemsPerBlock(numElemBlocks);
  std::vector<int> numNodesPerElem(numElemBlocks);
  std::vector<int> elemMatrixSizePerBlock(numElemBlocks);

  for(int blk=0; blk<numElemBlocks; blk++) {
    BlockDescriptor* block = NULL;
    CHK_ERR( problemStructure_->getBlockDescriptor_index(blk, block) );

    numElemsPerBlock[blk] = block->getNumElements();
    numNodesPerElem[blk]  = block->numNodesPerElement;

    int* fieldsPerNode = block->fieldsPerNodePtr();
    int** fieldIDsTable = block->fieldIDsTablePtr();

    elemMatrixSizePerBlock[blk] = 0;

    for(int nn=0; nn<numNodesPerElem[blk]; nn++) {
      if (fieldsPerNode[nn] <= 0) ERReturn(-1);
      
      for(int nf=0; nf<fieldsPerNode[nn]; nf++) {
	elemMatrixSizePerBlock[blk] +=
	  problemStructure_->getFieldSize(fieldIDsTable[nn][nf]);
      }
    }
  }

  //Now we need to run the penalty constraint records and figure out how many
  //extra "element-blocks" to describe. (A penalty constraint will be treated 
  //exactly like an element.) So first, we need to figure out how many different
  //sizes of constraint connectivities there are, because the constraints with
  //the same numbers of constrained nodes will be grouped together in blocks.

  if (problemStructure_==NULL) {
    FEI_COUT << "problemStructrue_ NULL"<<FEI_ENDL;
    ERReturn(-1);
  }

  std::map<GlobalID,ConstraintType*>::const_iterator
    cr_iter = problemStructure_->getPenConstRecords().begin(),
    cr_end  = problemStructure_->getPenConstRecords().end();

  int i;

  //constraintBlocks will be a sorted list with each "block-id" being the
  //num-nodes-per-constraint for constraints in that block.

  //numConstraintsPerBlock is the same length as constraintBlocks
  feiArray<int> numConstraintsPerBlock;
  feiArray<int> numDofPerConstraint;
  penCRIDs_.resize(problemStructure_->getNumPenConstRecords());
  GlobalID* penCRIDsPtr = penCRIDs_.dataPtr();

  int counter = 0;
  while(cr_iter != cr_end) {
    penCRIDsPtr[counter++] = (*cr_iter).first;
    ConstraintType& cr = *((*cr_iter).second);
    int numNodes = cr.getMasters()->length();

    int insertPoint = -1;
    int offset = snl_fei::binarySearch(numNodes, constraintBlocks_, insertPoint);

    int nodeOffset = 0;
    if (offset < 0) {
      constraintBlocks_.insert(numNodes, insertPoint);
      numConstraintsPerBlock.insert(1, insertPoint);
      numDofPerConstraint.insert(0, insertPoint);

      if (insertPoint > 0) {
	nodeOffset = constraintNodeOffsets_[insertPoint-1] +
	   constraintBlocks_[insertPoint-1];
      }
      constraintNodeOffsets_.insert(nodeOffset, insertPoint);
      offset = insertPoint;
    }
    else {
      numConstraintsPerBlock[offset]++;
      ++cr_iter;
      continue;
    }

    int* fieldIDs = cr.getMasterFieldIDs()->dataPtr();
    for(int k=0; k<numNodes; ++k) {
      int fieldSize = problemStructure_->getFieldSize(fieldIDs[k]);
      packedFieldSizes_.insert(fieldSize, nodeOffset+k);
      numDofPerConstraint[offset] += fieldSize;
    }
    ++cr_iter;
  }

  //now combine the elem-block info with the penalty-constraint info.
  int numBlocksTotal = numElemBlocks + constraintBlocks_.length();
  for(i=0; i<constraintBlocks_.length(); ++i) {
    numElemsPerBlock.push_back(numConstraintsPerBlock[i]);
    numNodesPerElem.push_back(constraintBlocks_[i]);
    elemMatrixSizePerBlock.push_back(numDofPerConstraint[i]);
  }

  int numMultCRs = problemStructure_->getNumMultConstRecords();

  CHK_ERR( feData_->describeStructure(numBlocksTotal,
				      &numElemsPerBlock[0],
				      &numNodesPerElem[0],
				      &elemMatrixSizePerBlock[0],
				      numNodes,
				      numSharedNodes,
				      numMultCRs) );

  numRegularElems_ = 0;
  feiArray<int> numDofPerNode(0, 32);

  for(i=0; i<numElemBlocks; i++) {
    BlockDescriptor* block = NULL;
    CHK_ERR( problemStructure_->getBlockDescriptor_index(i, block) );

    if (block->getNumElements() == 0) continue;

    ConnectivityTable& ctbl =
      problemStructure_->getBlockConnectivity(block->getGlobalBlockID());

    std::vector<int> cNodeList(block->numNodesPerElement);

    int* fieldsPerNode = block->fieldsPerNodePtr();
    int** fieldIDsTable = block->fieldIDsTablePtr();

    numDofPerNode.resize(0);
    for(int nn=0; nn<numNodesPerElem[i]; nn++) {
      if (fieldsPerNode[nn] <= 0) ERReturn(-1);
      numDofPerNode.append(0);
      int indx = numDofPerNode.length()-1;

      for(int nf=0; nf<fieldsPerNode[nn]; nf++) {
	numDofPerNode[indx] += problemStructure_->getFieldSize(fieldIDsTable[nn][nf]);
      }
    }

    int nodesPerElement = block->numNodesPerElement;
    NodeDescriptor** elemConn = ctbl.elem_conn_ptrs->dataPtr();
    int offset = 0;
    int numElems = block->getNumElements();
    numRegularElems_ += numElems;
    for(int j=0; j<numElems; j++) {

      for(int k=0; k<nodesPerElement; k++) {
	NodeDescriptor* node = elemConn[offset++];
	cNodeList[k] = node->getNodeNumber();
      }

      CHK_ERR( feData_->setConnectivity(i, ctbl.elemNumbers[j],
					block->numNodesPerElement,
					&cNodeList[0],
					numDofPerNode.dataPtr()) );
    }
  }

  std::vector<int> nodeNumbers;
  cr_iter = problemStructure_->getPenConstRecords().begin();
  i = 0;
  while(cr_iter != cr_end) {
    ConstraintType& cr = *((*cr_iter).second);
    GlobalID* nodeIDs = cr.getMasters()->dataPtr();
    int numNodes = cr.getMasters()->length();
    int index = snl_fei::binarySearch(numNodes, constraintBlocks_);
    if (index < 0) {
      ERReturn(-1);
    }

    int blockNum = numElemBlocks + index;

    nodeNumbers.resize(numNodes);

    for(int k=0; k<numNodes; ++k) {
      NodeDescriptor& node = Filter::findNodeDescriptor(nodeIDs[k]);
      nodeNumbers[k] = node.getNodeNumber();
    }

    int offset = constraintNodeOffsets_[index];
    CHK_ERR( feData_->setConnectivity(blockNum, numRegularElems_+i++,
				      numNodes, &nodeNumbers[0],
				      &(packedFieldSizes_.dataPtr()[offset])) );
    ++cr_iter;
  }

  }
  catch(fei::Exception& exc) {
    FEI_CERR << exc.what() << FEI_ENDL;
    ERReturn(-1);
  }

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::resetSystem(double s)
{
  //
  //  This puts the value s throughout both the matrix and the vector.
  //
  if (Filter::logStream() != NULL) {
    (*logStream()) << "FEI: resetSystem" << FEI_ENDL << s << FEI_ENDL;
  }

  CHK_ERR( feData_->reset() );
 
  debugOutput("#FEDataFilter leaving resetSystem");

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::deleteMultCRs()
{

  debugOutput("#FEDataFilter::deleteMultCRs");

  int err = feData_->deleteConstraints();

  debugOutput("#FEDataFilter leaving deleteMultCRs");

  return(err);
}

//------------------------------------------------------------------------------
int FEDataFilter::resetTheMatrix(double s)
{
  //FiniteElementData implementations can't currently reset the matrix without
  //resetting the rhs vector too. 
  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::resetTheRHSVector(double s)
{
  //FiniteElementData implementations can't currently reset the rhs vector
  //without resetting the matrix too.
  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::resetMatrix(double s)
{
  //
  //  This puts the value s throughout both the matrix and the vector.
  //

  debugOutput("FEI: resetMatrix");

  CHK_ERR( resetTheMatrix(s) );

  eqnCommMgr_->resetCoefs();

  debugOutput("#FEDataFilter leaving resetMatrix");

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::resetRHSVector(double s)
{
  //
  //  This puts the value s throughout the rhs vector.
  //

  debugOutput("FEI: resetRHSVector");

  CHK_ERR( resetTheRHSVector(s) );

  eqnCommMgr_->resetCoefs();

  debugOutput("#FEDataFilter leaving resetRHSVector");

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::resetInitialGuess(double s)
{
  //
  //  This puts the value s throughout the initial guess (solution) vector.
  //
  if (Filter::logStream() != NULL) {
    FEI_OSTREAM& os = *logStream();
    os << "FEI: resetInitialGuess" << FEI_ENDL;
    os << "#value to which initial guess is to be set" << FEI_ENDL;
    os << s << FEI_ENDL;
  }

  //Actually, the FiniteElementData doesn't currently allow us to alter
  //values in any initial guess or solution vector.

  debugOutput("#FEDataFilter leaving resetInitialGuess");

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::loadNodeBCs(int numNodes,
			      const GlobalID *nodeIDs,
			      int fieldID,
			      const double *const *alpha,
			      const double *const *beta,
			      const double *const *gamma)
{
  //
  //  load boundary condition information for a given set of nodes
  //
  int size = problemStructure_->getFieldSize(fieldID);
  if (size < 1) {
    FEI_CERR << "FEI Warning: loadNodeBCs called for fieldID "<<fieldID
	 <<", which was defined with size "<<size<<" (should be positive)."<<FEI_ENDL;
    return(0);
  }

   if (Filter::logStream() != NULL) {
    (*logStream())<<"FEI: loadNodeBCs"<<FEI_ENDL
		     <<"#num-nodes"<<FEI_ENDL<<numNodes<<FEI_ENDL
		     <<"#fieldID"<<FEI_ENDL<<fieldID<<FEI_ENDL
		     <<"#field-size"<<FEI_ENDL<<size<<FEI_ENDL;
    (*logStream())<<"#following lines: nodeID alpha beta gamma "<<FEI_ENDL;

    for(int j=0; j<numNodes; j++) {
      int nodeID = nodeIDs[j];
      (*logStream())<<nodeID<<"  ";
      int k;
      for(k=0; k<size; k++) {
        (*logStream())<< alpha[j][k]<<" ";
      }
      (*logStream())<<"  ";
      for(k=0; k<size; k++) {
        (*logStream())<< beta[j][k]<<" ";
      }
      (*logStream())<<"  ";
      for(k=0; k<size; k++) {
        (*logStream())<<gamma[j][k]<<" ";
      }
      (*logStream())<<FEI_ENDL;
    }
   }

   feiArray<int> essEqns(0, 256), otherEqns(0, 256);
   feiArray<double> essAlpha(0, 256), essGamma(0, 256);
   feiArray<double> otherAlpha(0, 256), otherBeta(0, 256), otherGamma(0, 256);
   NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

   double fei_eps = std::numeric_limits<double>::epsilon();

   for(int i=0; i<numNodes; ++i) {
     NodeDescriptor* node = 0;
     nodeDB.getNodeWithID(nodeIDs[i], node);
     int eqn = -1;
     if (!node->getFieldEqnNumber(fieldID, eqn)) {
       ERReturn(-1);
     }

     for(int j=0; j<size; ++j) {
       double thisAlpha = alpha[i][j];
       double thisBeta = beta[i][j];
       double thisGamma = gamma[i][j];

       if (std::abs(thisAlpha) > fei_eps && std::abs(thisBeta) <= fei_eps) {
	 //it's an essential (dirichlet) BC...
	 essEqns.append(eqn+j);
	 essAlpha.append(thisAlpha);
	 essGamma.append(thisGamma);
       }
       else {
	 if (std::abs(thisBeta) > fei_eps) {
	   //it's a natural or mixed BC...
	   otherEqns.append(eqn+j);
	   otherAlpha.append(thisAlpha);
	   otherBeta.append(thisBeta);
	   otherGamma.append(thisGamma);
	 }
       }
     }
   }

   if (essEqns.length() > 0) {
      CHK_ERR( enforceEssentialBCs(essEqns.dataPtr(),
				   essAlpha.dataPtr(),
				   essGamma.dataPtr(), essEqns.length()) );
   }

   if (otherEqns.length() > 0) {
      CHK_ERR( enforceOtherBCs(otherEqns.dataPtr(),
			       otherAlpha.dataPtr(),
			       otherBeta.dataPtr(), otherGamma.dataPtr(),
			       otherEqns.length()) );
   }

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::loadNodeBCs(int numNodes,
                              const GlobalID *nodeIDs,
                              int fieldID,
                              const int* offsetsIntoField,
                              const double* prescribedValues)
{
  //
  //  load boundary condition information for a given set of nodes
  //
  int size = problemStructure_->getFieldSize(fieldID);
  if (size < 1) {
    FEI_CERR << "FEI Warning: loadNodeBCs called for fieldID "<<fieldID
         <<", which was defined with size "<<size<<" (should be positive)."<<FEI_ENDL;
    return(0);
  }

   if (Filter::logStream() != NULL) {
    (*logStream())<<"FEI: loadNodeBCs"<<FEI_ENDL
                     <<"#num-nodes"<<FEI_ENDL<<numNodes<<FEI_ENDL
                     <<"#fieldID"<<FEI_ENDL<<fieldID<<FEI_ENDL
                     <<"#field-size"<<FEI_ENDL<<size<<FEI_ENDL;
    (*logStream())<<"#following lines: nodeID offsetIntoField value "<<FEI_ENDL;

    for(int j=0; j<numNodes; j++) {
      int nodeID = nodeIDs[j];
      (*logStream())<<nodeID<<"  ";
      (*logStream())<< offsetsIntoField[j]<<" ";
      (*logStream())<< prescribedValues[j]<<FEI_ENDL;
    }
   }

   NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

   std::vector<int> essEqns(numNodes);
   std::vector<double> alpha(numNodes);
   std::vector<double> gamma(numNodes);

   for(int i=0; i<numNodes; ++i) {
     NodeDescriptor* node = 0;
     nodeDB.getNodeWithID(nodeIDs[i], node);
     int eqn = -1;
     if (!node->getFieldEqnNumber(fieldID, eqn)) {
       ERReturn(-1);
     }

     essEqns[i] = eqn + offsetsIntoField[i];
     gamma[i] = prescribedValues[i];
     alpha[i] = 1.0;
   }

   if (essEqns.size() > 0) {
      CHK_ERR( enforceEssentialBCs(&essEqns[0], &alpha[0],
                                   &gamma[0], essEqns.size()) );
   }

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::loadElemBCs(int numElems,
			      const GlobalID *elemIDs,
			      int fieldID,
			      const double *const *alpha,
			      const double *const *beta,
			      const double *const *gamma)
{
   return(-1);
}

//------------------------------------------------------------------------------
void FEDataFilter::allocElemStuff()
{
   int nb = problemStructure_->getNumElemBlocks();

   for(int i=0; i<nb; i++) {
     BlockDescriptor* block = NULL;
     int err = problemStructure_->getBlockDescriptor_index(i, block);
     if (err) voidERReturn;

      int numEqns = block->getNumEqnsPerElement();
      if (maxElemRows_ < numEqns) maxElemRows_ = numEqns;
   }

   eStiff_ = new double*[maxElemRows_];
   eStiff1D_ = new double[maxElemRows_*maxElemRows_];

   if (eStiff_ == NULL || eStiff1D_ == NULL) voidERReturn

   for(int r=0; r<maxElemRows_; r++) {
      eStiff_[r] = eStiff1D_ + r*maxElemRows_;
   }

   eLoad_ = new double[maxElemRows_];

   if (eLoad_ == NULL) voidERReturn
}

//------------------------------------------------------------------------------
int FEDataFilter::sumInElem(GlobalID elemBlockID,
                        GlobalID elemID,
                        const GlobalID* elemConn,
                        const double* const* elemStiffness,
                        const double* elemLoad,
                        int elemFormat)
{
  if (Filter::logStream() != NULL) {
    (*logStream()) << "FEI: sumInElem" << FEI_ENDL <<"# elemBlockID " << FEI_ENDL
		      << static_cast<int>(elemBlockID) << FEI_ENDL
		      << "# elemID " << FEI_ENDL << static_cast<int>(elemID) << FEI_ENDL;
    BlockDescriptor* block = NULL;
    CHK_ERR( problemStructure_->getBlockDescriptor(elemBlockID, block) );
    int numNodes = block->numNodesPerElement;
    (*logStream()) << "#num-nodes" << FEI_ENDL << numNodes << FEI_ENDL;
    (*logStream()) << "#connected nodes" << FEI_ENDL;
    for(int i=0; i<numNodes; ++i) {
      GlobalID nodeID = elemConn[i];
      (*logStream())<<static_cast<int>(nodeID)<<" ";
    }
    (*logStream())<<FEI_ENDL;
  }

  return(generalElemInput(elemBlockID, elemID, elemConn, elemStiffness,
			  elemLoad, elemFormat));
}

//------------------------------------------------------------------------------
int FEDataFilter::sumInElemMatrix(GlobalID elemBlockID,
                              GlobalID elemID,
                              const GlobalID* elemConn,
                              const double* const* elemStiffness,
                              int elemFormat)
{
  if (Filter::logStream() != NULL) {
    (*logStream()) << "FEI: sumInElemMatrix"<<FEI_ENDL
		      << "#elemBlockID" << FEI_ENDL << static_cast<int>(elemBlockID)
		      << "# elemID" << FEI_ENDL << static_cast<int>(elemID) << FEI_ENDL;
    BlockDescriptor* block = NULL;
    CHK_ERR( problemStructure_->getBlockDescriptor(elemBlockID, block) );
    int numNodes = block->numNodesPerElement;
    (*logStream()) << "#num-nodes" << FEI_ENDL << numNodes << FEI_ENDL;
    (*logStream()) << "#connected nodes" << FEI_ENDL;
    for(int i=0; i<numNodes; ++i) {
      GlobalID nodeID = elemConn[i];
      (*logStream())<<static_cast<int>(nodeID)<<" ";
    }
    (*logStream())<<FEI_ENDL;
  }

  return(generalElemInput(elemBlockID, elemID, elemConn, elemStiffness,
			  NULL, elemFormat));
}

//------------------------------------------------------------------------------
int FEDataFilter::sumInElemRHS(GlobalID elemBlockID,
                           GlobalID elemID,
                           const GlobalID* elemConn,
                           const double* elemLoad)
{
  if (Filter::logStream() != NULL) {
    (*logStream()) << "FEI: sumInElemRHS"<<FEI_ENDL<<"# elemBlockID " << FEI_ENDL
		      <<static_cast<int>(elemBlockID)
		      << "# elemID " << FEI_ENDL << static_cast<int>(elemID) << FEI_ENDL;
    BlockDescriptor* block = NULL;
    CHK_ERR( problemStructure_->getBlockDescriptor(elemBlockID, block) );
    int numNodes = block->numNodesPerElement;
    (*logStream()) << "#num-nodes" << FEI_ENDL << numNodes << FEI_ENDL;
    (*logStream()) << "#connected nodes" << FEI_ENDL;
    for(int i=0; i<numNodes; ++i) {
      GlobalID nodeID = elemConn[i];
      (*logStream())<<static_cast<int>(nodeID)<<" ";
    }
    (*logStream())<<FEI_ENDL;
  }

  return(generalElemInput(elemBlockID, elemID, elemConn, NULL,
			  elemLoad, -1));
}

//------------------------------------------------------------------------------
int FEDataFilter::generalElemInput(GlobalID elemBlockID,
                        GlobalID elemID,
                        const GlobalID* elemConn,
                        const double* const* elemStiffness,
                        const double* elemLoad,
                        int elemFormat)
{
  (void)elemConn;
  return(generalElemInput(elemBlockID, elemID, elemStiffness, elemLoad,
			  elemFormat) );
}

//------------------------------------------------------------------------------
int FEDataFilter::generalElemInput(GlobalID elemBlockID,
                        GlobalID elemID,
                        const double* const* elemStiffness,
                        const double* elemLoad,
                        int elemFormat)
{
  //first get the block-descriptor for this elemBlockID...

  BlockDescriptor* block = NULL;
  CHK_ERR( problemStructure_->getBlockDescriptor(elemBlockID, block) );

  //now allocate our local stiffness/load copy if we haven't already.

  if (maxElemRows_ <= 0) allocElemStuff();

  int numElemRows = block->getNumEqnsPerElement();

  //an feiArray.resize operation is free if the size is either shrinking or
  //staying the same.

  const double* const* stiff = NULL;
  if (elemStiffness != NULL) stiff = elemStiffness;

  const double* load = NULL;
  if (elemLoad != NULL) load = elemLoad;

  //we'll make a local dense copy of the element stiffness array
  //if the stiffness array was passed in using one of the "weird"
  //element formats, AND if the stiffness array is non-null.
  if (elemFormat != FEI_DENSE_ROW && stiff != NULL) {
    Filter::copyStiffness(stiff, numElemRows, elemFormat, eStiff_);
    stiff = eStiff_;
  }

  if (stiff != NULL || load != NULL) newData_ = true;

  if (Filter::logStream() != NULL) {
    if (stiff != NULL) {
      (*logStream())
	<< "#numElemRows"<< FEI_ENDL << numElemRows << FEI_ENDL
	<< "#elem-stiff (after being copied into dense-row format)"
	<< FEI_ENDL;
      for(int i=0; i<numElemRows; i++) {
	const double* stiff_i = stiff[i];
	for(int j=0; j<numElemRows; j++) {
	  (*logStream()) << stiff_i[j] << " ";
	}
	(*logStream()) << FEI_ENDL;
      }
    }

    if (load != NULL) {
      (*logStream()) << "#elem-load" << FEI_ENDL;
      for(int i=0; i<numElemRows; i++) {
	(*logStream()) << load[i] << " ";
      }
      (*logStream())<<FEI_ENDL;
    }
  }

  //Now we'll proceed to gather the stuff we need to pass the stiffness
  //data through to the FiniteElementData interface...

  int blockNumber = problemStructure_->getIndexOfBlock(elemBlockID);

  ConnectivityTable& connTable = problemStructure_->
    getBlockConnectivity(elemBlockID);

  std::map<GlobalID,int>::iterator
    iter = connTable.elemIDs.find(elemID);
  if (iter == connTable.elemIDs.end()) {
    ERReturn(-1);
  }

  int elemIndex = iter->second;

  int elemNumber = connTable.elemNumbers[elemIndex];

  int numNodes = block->numNodesPerElement;
  int* fieldsPerNode = block->fieldsPerNodePtr();
  int** fieldIDsTable = block->fieldIDsTablePtr();

  int numDistinctFields = block->getNumDistinctFields();
  int fieldSize = 0;
  if (numDistinctFields == 1) {
    fieldSize = problemStructure_->getFieldSize(fieldIDsTable[0][0]);
  }

  static feiArray<int> iwork(0, 256);
  iwork.resize(2*numNodes);

  int* dofsPerNode = iwork.dataPtr();
  for(int i=0; i<numNodes; ++i) {
    dofsPerNode[i] = 0;
  }

  int* nodeNumbers = dofsPerNode+numNodes;

  NodeDescriptor** elemNodes =
    connTable.elem_conn_ptrs->dataPtr()+elemIndex*numNodes;

  for(int nn=0; nn<numNodes; nn++) {
    NodeDescriptor* node = elemNodes[nn];
    nodeNumbers[nn] = node->getNodeNumber();

    if (numDistinctFields == 1) {
      for(int nf=0; nf<fieldsPerNode[nn]; nf++) {
	dofsPerNode[nn] += fieldSize;
      }
    }
    else {
      for(int nf=0; nf<fieldsPerNode[nn]; nf++) {
	dofsPerNode[nn] += problemStructure_->getFieldSize(fieldIDsTable[nn][nf]);
      }
    }
  }

  if (stiff != NULL) {
    CHK_ERR( feData_->setElemMatrix(blockNumber, elemNumber, numNodes,
				    nodeNumbers, dofsPerNode, stiff) );
  }

  if (load != NULL) {
    CHK_ERR( feData_->setElemVector(blockNumber, elemNumber, numNodes,
				    nodeNumbers, dofsPerNode, load) );
  }

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::sumIntoMatrix(int patternID,
			    const int* rowIDTypes,
                         const GlobalID* rowIDs,
			    const int* colIDTypes,
                         const GlobalID* colIDs,
                         const double* const* matrixEntries)
{
  if (Filter::logStream() != NULL) {
    (*logStream()) << "FEI: sumIntoMatrix" << FEI_ENDL;
  }
  return(generalCoefInput(patternID, rowIDTypes, rowIDs, colIDTypes, colIDs,
			  matrixEntries, NULL, ASSEMBLE_SUM));
}

//------------------------------------------------------------------------------
int FEDataFilter::sumIntoRHS(int patternID,
			 const int* rowIDTypes,
                         const GlobalID* rowIDs,
                         const double* vectorEntries)
{
  if (Filter::logStream() != NULL) {
    (*logStream()) << "FEI: sumIntoRHS" << FEI_ENDL;
  }
  return(generalCoefInput(patternID, rowIDTypes, rowIDs, NULL, NULL,
			  NULL, vectorEntries, ASSEMBLE_SUM));
}

//------------------------------------------------------------------------------
int FEDataFilter::putIntoMatrix(int patternID,
			    const int* rowIDTypes,
			    const GlobalID* rowIDs,
			    const int* colIDTypes,
			    const GlobalID* colIDs,
			    const double* const* matrixEntries)
{
  if (Filter::logStream() != NULL) {
    (*logStream()) << "FEI: putIntoMatrix" << FEI_ENDL;
  }
  return(generalCoefInput(patternID, rowIDTypes, rowIDs, colIDTypes, colIDs,
			  matrixEntries, NULL, ASSEMBLE_PUT));
}

//------------------------------------------------------------------------------
int FEDataFilter::getFromMatrix(int patternID,
			    const int* rowIDTypes,
			    const GlobalID* rowIDs,
			    const int* colIDTypes,
			    const GlobalID* colIDs,
			    double** matrixEntries)
{
   feiArray<int> rowIndices;
   feiArray<int> rowColOffsets, colIndices;
   int numColsPerRow;

   //We're going to supply a little non-standard behavior here that is
   //"extra for experts". If the colIDs argument is supplied as NULL, then
   //this implementation will provide matrix entries for the whole row, for
   //each of the matrix rows referenced by rowIDs and the associated fields
   //stored in 'patternID'.
   //Important note: this assumes that the caller has allocated enough memory
   //in 'matrixEntries'. The caller can find out how much memory is required
   //in a round-about way, by using the 'getFieldSize' and 'getEqnNumbers'
   //functions, and then querying the LinearSystemCore object for row-lengths.
   //This is very unpleasant, but will get us by until the next FEI update
   //addresses this (hopefully).

   if (colIDs == NULL) {
     CHK_ERR( problemStructure_->getPatternScatterIndices(patternID, 
							  rowIDs, rowIndices) );
   }
   else {
     CHK_ERR( problemStructure_->getPatternScatterIndices(patternID,
					     	  rowIDs, colIDs,
					           rowIndices, rowColOffsets,
						   numColsPerRow, colIndices) );
   }

   int err = 0;
   if (colIDs == NULL) {
     err = getFromMatrix(rowIndices.size(), &rowIndices[0],
			 NULL, NULL, 0, matrixEntries);
   }
   else {
     err = getFromMatrix(rowIndices.size(), &rowIndices[0],
			 rowColOffsets.dataPtr(), colIndices.dataPtr(),
			 numColsPerRow, matrixEntries);
   }

   return(err);
}

//------------------------------------------------------------------------------
int FEDataFilter::putIntoRHS(int patternID,
			 const int* rowIDTypes,
                         const GlobalID* rowIDs,
                         const double* vectorEntries)
{
  if (Filter::logStream() != NULL) {
    (*logStream()) << "FEI: putIntoRHS" << FEI_ENDL;
  }

  return(generalCoefInput(patternID, rowIDTypes, rowIDs, NULL, NULL,
			  NULL, vectorEntries, ASSEMBLE_PUT));
}

//------------------------------------------------------------------------------
int FEDataFilter::putIntoRHS(int IDType,
			  int fieldID,
			  int numIDs,
			  const GlobalID* IDs,
			  const double* rhsEntries)
{
  int fieldSize = problemStructure_->getFieldSize(fieldID);

  rowIndices_.resize(fieldSize*numIDs);
  int checkNumEqns;

  CHK_ERR( problemStructure_->getEqnNumbers(numIDs, IDs, IDType, fieldID,
					    checkNumEqns,
					    &rowIndices_[0]));
  if (checkNumEqns != numIDs*fieldSize) {
    ERReturn(-1);
  }

  if (putRHSVec_ == NULL) {
    putRHSVec_ = new SSVec(rowIndices_.size(),
			   &rowIndices_[0], rhsEntries);
  }
  else {
    putRHSVec_->setInternalData(rowIndices_.size(),
				&rowIndices_[0], rhsEntries);
  }

  CHK_ERR( exchangeRemoteEquations() );

  CHK_ERR( assembleRHS(*putRHSVec_, ASSEMBLE_PUT) );

  return(0);
}

//------------------------------------------------------------------------------
int FEDataFilter::sumIntoRHS(int IDType,
			  int fieldID,
			  int numIDs,
			  const GlobalID* IDs,
			  const double* rhsEntries)
{
  int fieldSize = problemStructure_->getFieldSize(fieldID);

  rowIndices_.resize(fieldSize*numIDs);
  int checkNumEqns;

  CHK_ERR( problemStructure_->getEqnNumbers(numIDs, IDs, IDType, fieldID,
					    checkNumEqns,
					    &rowIndices_[0]));
  if (checkNumEqns != numIDs*fieldSize) {
    ERReturn(-1);
  }

  if (putRHSVec_ == NULL) {
    putRHSVec_ = new SSVec(rowIndices_.size(),
			   &rowIndices_[0], rhsEntries);
  }
  else {
    putRHSVec_->setInternalData(rowIndices_.size(),
				&rowIndices_[0], rhsEntries);
  }

  CHK_ERR( assembleRHS(*putRHSVec_, ASSEMBLE_SUM) );

  return(0);
}

//------------------------------------------------------------------------------
int FEDataFilter::getFromRHS(int patternID,
			 const int* rowIDTypes,
                         const GlobalID* rowIDs,
                         double* vectorEntries)
{
  feiArray<int> rowIndices;

  CHK_ERR( problemStructure_->getPatternScatterIndices(patternID, 
						       rowIDs, rowIndices) );

  CHK_ERR( getFromRHS(rowIndices.size(), vectorEntries, &rowIndices[0]))

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::generalCoefInput(int patternID,
			       const int* rowIDTypes,
			       const GlobalID* rowIDs,
			       const int* colIDTypes,
			       const GlobalID* colIDs,
			       const double* const* matrixEntries,
			       const double* vectorEntries,
			       int assemblyMode)
{
//
//We will give these rowIDs and colIDs to the problemStructure_ object,
//and it will return the scatter indices in the feiArray<int> objects rowIndices
//and colIndices. We will then use those indices to put the contents of
//matrixEntries and/or vectorEntries into the linear system core object.
//
//Those equations (corresponding to rowIDs) that are remotely owned, will
//be packed up so they can be sent to the owning processor.
//
  rowIndices_.resize(0);
  rowColOffsets_.resize(0);
  colIndices_.resize(0);

   int numColsPerRow;

   int error = 0;
   if (matrixEntries != NULL && vectorEntries == NULL) {
      error = problemStructure_->getPatternScatterIndices(patternID,
                                           rowIDs, colIDs,
                                           rowIndices_, rowColOffsets_,
					     numColsPerRow, colIndices_);
   }
   else if (matrixEntries == NULL && vectorEntries != NULL) {
      error = problemStructure_->getPatternScatterIndices(patternID,
                                           rowIDs, rowIndices_);
   }
   else {
      FEI_CERR << "FEDataFilter::generalCoefInput: ERROR, both matrixEntries "
	   << "and vectorEntries are NULL." << FEI_ENDL;
      ERReturn(-1);
   }

   if (assemblyMode == ASSEMBLE_PUT) {
     int globalError = 0;
     CHK_ERR( commUtils_->GlobalSum(error, globalError) );
     if (globalError != 0) {
       return(-1);
     }
   }

   const double* const* coefs = NULL;
   if (matrixEntries != NULL) coefs = matrixEntries;

   const double* rhsCoefs = NULL;
   if (vectorEntries != NULL) rhsCoefs = vectorEntries;

   if (coefs != NULL || rhsCoefs != NULL) newData_ = true;

   //Recall that for a pattern, the list of column-entities is packed, we have
   //a list of column-entities for each row-entities. Thus, we now have a list
   //of column-indices for each row index...
   int numRows = rowIndices_.size();
   int numCols = colIndices_.length();

   if (Filter::logStream() != NULL) {
     if (coefs != NULL) {
       (*logStream()) << "#num-rows num-cols"<<FEI_ENDL
			  <<numRows<<" "<<numCols << FEI_ENDL;
       for(int i=0; i<numRows; i++) {
	 const double* coefs_i = coefs[i];
	 for(int j=0; j<numCols; j++) {
	   (*logStream()) << coefs_i[j] << " ";
	 }
	 (*logStream()) << FEI_ENDL;
       }
     }

     if (rhsCoefs != NULL) {
       (*logStream()) << "#num-rows"<<FEI_ENDL<<numRows << FEI_ENDL;
       for(int i=0; i<numRows; i++) {
	 (*logStream()) << rhsCoefs[i] << FEI_ENDL;
       }
     }
   }

   if (assemblyMode == ASSEMBLE_PUT) CHK_ERR( exchangeRemoteEquations() );

   if (coefs != NULL) {
     //wrap a super-sparse-matrix object around the coefs data
     SSMat mat(numRows, &rowIndices_[0],
	       numColsPerRow, rowColOffsets_.dataPtr(),
	       colIndices_.dataPtr(), coefs);

     CHK_ERR( assembleEqns(mat, assemblyMode) );
   }

   if (rhsCoefs != NULL) {
     if (putRHSVec_ == NULL) {
       putRHSVec_ = new SSVec(rowIndices_.size(),
			      &rowIndices_[0], rhsCoefs);
     }
     else {
       putRHSVec_->setInternalData(rowIndices_.size(),
				   &rowIndices_[0], rhsCoefs);
     }

     CHK_ERR( assembleRHS(*putRHSVec_, assemblyMode) );
   }

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::enforceEssentialBCs(const int* eqns, 
                                      const double* alpha,
                                      const double* gamma, 
                                      int numEqns)
{
  feiArray<double> values(0,512);
  feiArray<int> nodeNumbers(0,512);
  feiArray<int> dofOffsets(0,512);

  int i;
  for(i=0; i<numEqns; i++) {
    int reducedEqn = -1;
    bool isSlave = problemStructure_->
      translateToReducedEqn(eqns[i], reducedEqn);
    if (isSlave) continue;

    int nodeNumber = problemStructure_->getAssociatedNodeNumber(eqns[i]);

    nodeNumbers.append(nodeNumber);

    NodeDescriptor* node = NULL;
    CHK_ERR( problemStructure_->getNodeDatabase().
             getNodeWithNumber(nodeNumber, node));

    int firstEqn = node->getFieldEqnNumbers()[0];
    dofOffsets.append(eqns[i] - firstEqn);

    values.append(gamma[i]/alpha[i]);
  }

  CHK_ERR( feData_->setDirichletBCs(nodeNumbers.length(),
				    nodeNumbers.dataPtr(),
				    dofOffsets.dataPtr(),
				    values.dataPtr()) );

  newData_ = true;

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::enforceOtherBCs(const int* eqns, const double* alpha,
			      const double* beta, const double* gamma,
			      int numEqns)
{
  //This function is for enforcing natural (Neumann) or mixed boundary 
  //conditions. This is a simple operation:
  //for i in 0 .. numEqns-1 {
  //  A[eqns[i], eqns[i]] += alpha[i]/beta[i];
  //  b[eqns[i]] += gamma[i]/beta[i]
  //}

  feiArray<double> matValues(0,256), rhsValues(0,256);
  feiArray<int> nodeNumbers(0,256);
  feiArray<int> dofOffsets(0,256);
  int i;
  for(i=0; i<numEqns; i++) {
    int reducedEqn = -1;
    bool isSlave = problemStructure_->
      translateToReducedEqn(eqns[i], reducedEqn);
    if (isSlave) continue;
    int nodeNumber = problemStructure_->getAssociatedNodeNumber(reducedEqn);
    nodeNumbers.append(nodeNumber);

    NodeDescriptor* node = NULL;
    CHK_ERR( problemStructure_->getNodeDatabase().
	     getNodeWithNumber(nodeNumbers[i], node));

    int firstEqn = node->getFieldEqnNumbers()[0];
    dofOffsets.append(eqns[i] - firstEqn);

    matValues.append(alpha[i]/beta[i]);
    rhsValues.append(gamma[i]/beta[i]);
  }

  feiArray<int> numColsPerRow(nodeNumbers.length());
  numColsPerRow = 1; //feiArray::operator=

  CHK_ERR( feData_->sumIntoMatrix(nodeNumbers.length(),
				  nodeNumbers.dataPtr(),
				  dofOffsets.dataPtr(),
				  numColsPerRow.dataPtr(),
				  nodeNumbers.dataPtr(),
				  dofOffsets.dataPtr(),
				  matValues.dataPtr()) );

  CHK_ERR( feData_->sumIntoRHSVector(nodeNumbers.length(),
				     nodeNumbers.dataPtr(),
				     dofOffsets.dataPtr(),
				     rhsValues.dataPtr()) );

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::exchangeRemoteEquations(){
//
// This function is where processors send local contributions to remote
// equations to the owners of those equations, and receive remote
// contributions to local equations.
//
   //debugOutput("#FEDataFilter::exchangeRemoteEquations");

   //CHK_ERR( eqnCommMgr_->exchangeEqns() );

   //so now the remote contributions should be available, let's get them out
   //of the eqn comm mgr and put them into our local matrix structure.

   //debugOutput("#   putting remote contributions into linear system...");

   //CHK_ERR( unpackRemoteContributions(*eqnCommMgr_, ASSEMBLE_SUM) );

   //eqnCommMgr_->resetCoefs();

   //debugOutput("#FEDataFilter leaving exchangeRemoteEquations");

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::unpackRemoteContributions(EqnCommMgr& eqnCommMgr,
					int assemblyMode)
{
   bool newCoefs = eqnCommMgr.newCoefData();
   bool newRHSs = eqnCommMgr.newRHSData();

   if (!newCoefs && !newRHSs) {
     return(0);
   }

   int numRecvEqns = eqnCommMgr.getNumLocalEqns();

   feiArray<int>& recvEqnNumbers = eqnCommMgr.localEqnNumbersPtr();
   feiArray<SSVec*>& recvEqns = eqnCommMgr.localEqns();
   feiArray<feiArray<double>*>& recvRHSs = *(eqnCommMgr.localRHSsPtr());

   int i;
   double** coefs = new double*[numRecvEqns];

   for(i=0; i<numRecvEqns; i++) {
      coefs[i] = recvEqns[i]->coefs().dataPtr();
   }

   for(i=0; i<numRecvEqns; i++) {

      int eqn = recvEqnNumbers[i];
      if ((reducedStartRow_ > eqn) || (reducedEndRow_ < eqn)) {
         FEI_CERR << "FEDataFilter::unpackRemoteContributions: ERROR, recvEqn " << eqn
              << " out of range. (localStartRow_: " << reducedStartRow_
              << ", localEndRow_: " << reducedEndRow_ << ", localRank_: "
              << localRank_ << ")" << FEI_ENDL;
         MPI_Abort(comm_, -1);
      }

      for(int ii=0; ii<recvEqns[i]->length(); ii++) {
         if (coefs[i][ii] > 1.e+200) {
            FEI_CERR << localRank_ << ": FEDataFilter::unpackRemoteContributions: "
                 << "WARNING, coefs["<<i<<"]["<<ii<<"]: " << coefs[i][ii]
                 << FEI_ENDL;
            MPI_Abort(comm_, -1);
         }
      }

      if (recvEqns[i]->length() > 0 && newCoefs) {
	//sum this equation into the matrix,
	CHK_ERR( giveToLocalReducedMatrix(1, &(recvEqnNumbers[i]),
					  recvEqns[i]->length(),
					  recvEqns[i]->indices().dataPtr(),
					  &(coefs[i]), assemblyMode ) );
      }

      //and now the RHS contributions.
      if (newRHSs) {
	for(int j=0; j<numRHSs_; j++) {
	  CHK_ERR( giveToLocalReducedRHS(1, &( (*(recvRHSs[i]))[j] ),
					 &eqn, assemblyMode) );
	}
      }
   }

   delete [] coefs;

   return(0);
}

//------------------------------------------------------------------------------
int FEDataFilter::loadFEDataMultCR(int CRID,
			       int numCRNodes,
			       const GlobalID* CRNodes, 
			       const int* CRFields,
			       const double* CRWeights,
			       double CRValue)
{
  if (Filter::logStream() != NULL) {
    FEI_OSTREAM& os = *logStream();
    os<<"FEI: loadCRMult"<<FEI_ENDL;
    os<<"#num-nodes"<<FEI_ENDL<<numCRNodes<<FEI_ENDL;
    os<<"#CRNodes:"<<FEI_ENDL;
    int i;
    for(i=0; i<numCRNodes; ++i) {
      GlobalID nodeID = CRNodes[i];
      os << static_cast<int>(nodeID) << " ";
    }
    os << FEI_ENDL << "#fields:"<<FEI_ENDL;
    for(i=0; i<numCRNodes; ++i) os << CRFields[i] << " ";
    os << FEI_ENDL << "#field-sizes:"<<FEI_ENDL;
    for(i=0; i<numCRNodes; ++i) {
      int size = problemStructure_->getFieldSize(CRFields[i]);
      os << size << " ";
    }
    os << FEI_ENDL<<"#weights:"<<FEI_ENDL;
    int offset = 0;
    for(i=0; i<numCRNodes; ++i) {
      int size = problemStructure_->getFieldSize(CRFields[i]);
      for(int j=0; j<size; ++j) {
	os << CRWeights[offset++] << " ";
      }
    }
    os << FEI_ENDL<<"#CRValue:"<<FEI_ENDL<<CRValue<<FEI_ENDL;
  }

  if (numCRNodes <= 0) return(0);

  feiArray<int> nodeNumbers;
  feiArray<int> dofOffsets;
  feiArray<double> weights;

  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

  double fei_eps = std::numeric_limits<double>::epsilon();

  int offset = 0;
  for(int i=0; i<numCRNodes; i++) {
    NodeDescriptor* node = NULL;
    CHK_ERR( nodeDB.getNodeWithID(CRNodes[i], node) );

    int firstEqn = node->getFieldEqnNumbers()[0];
    int fieldEqn = -1;
    bool hasField = node->getFieldEqnNumber(CRFields[i], fieldEqn);
    if (!hasField) ERReturn(-1);

    int fieldSize = problemStructure_->getFieldSize(CRFields[i]);

    for(int f=0; f<fieldSize; f++) {
      double weight = CRWeights[offset++];
      if (std::abs(weight) > fei_eps) {
	nodeNumbers.append(node->getNodeNumber());
	dofOffsets.append((fieldEqn+f)-firstEqn);
	weights.append(weight);
      }
    }
  }

  CHK_ERR( feData_->setMultiplierCR(CRID,
				    nodeNumbers.length(),
				    nodeNumbers.dataPtr(),
				    dofOffsets.dataPtr(),
				    weights.dataPtr(),
				    CRValue) );
  newData_ = true;

  return(0);
}

//------------------------------------------------------------------------------
int FEDataFilter::loadFEDataPenCR(int CRID,
			      int numCRNodes,
			      const GlobalID* CRNodes, 
			      const int* CRFields,
			      const double* CRWeights,
			      double CRValue, 
			      double penValue)
{
  if (numCRNodes <= 0) return(0);

  feiArray<int> nodeNumbers;
  feiArray<int> dofsPerNode;
  feiArray<double> weights;

  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

  int i, j, offset = 0;
  for(i=0; i<numCRNodes; i++) {
    NodeDescriptor* node = NULL;
    CHK_ERR( nodeDB.getNodeWithID(CRNodes[i], node) );

    int fieldEqn = -1;
    bool hasField = node->getFieldEqnNumber(CRFields[i], fieldEqn);
    if (!hasField) ERReturn(-1);

    int fieldSize = problemStructure_->getFieldSize(CRFields[i]);

    nodeNumbers.append(node->getNodeNumber());
    dofsPerNode.append(fieldSize);

    for(int f=0; f<fieldSize; f++) {
      double weight = CRWeights[offset++];
      weights.append(weight);
    }
  }

  feiArray<double*> matrixCoefs(weights.length());
  feiArray<double> rhsCoefs(weights.length());
  offset = 0;
  for(i=0; i<weights.length(); ++i) {
    double* coefPtr = new double[weights.length()];
    for(j=0; j<weights.length(); ++j) {
      coefPtr[j] = weights[i]*weights[j]*penValue;
    }
    matrixCoefs[i] = coefPtr;
    rhsCoefs[i] = weights[i]*penValue*CRValue;
  }

  int crIndex = snl_fei::binarySearch(CRID, penCRIDs_);

  int index = snl_fei::binarySearch(numCRNodes, constraintBlocks_);

  int blockNum = problemStructure_->getNumElemBlocks() + index;
  int elemNum = numRegularElems_ + crIndex;

  CHK_ERR( feData_->setElemMatrix(blockNum, elemNum,
				  nodeNumbers.length(),
				  nodeNumbers.dataPtr(),
				  dofsPerNode.dataPtr(),
				  matrixCoefs.dataPtr()) );

  CHK_ERR( feData_->setElemVector(blockNum, elemNum,
				  nodeNumbers.length(),
				  nodeNumbers.dataPtr(),
				  dofsPerNode.dataPtr(),
				  rhsCoefs.dataPtr()) );

  newData_ = true;

  for(i=0; i<weights.length(); ++i) {
    delete [] matrixCoefs[i];
  }

  return(0);
}

//------------------------------------------------------------------------------
int FEDataFilter::loadCRMult(int CRID, 
                         int numCRNodes,
                         const GlobalID* CRNodes, 
                         const int* CRFields,
                         const double* CRWeights,
                         double CRValue)
{
//
// Load Lagrange multiplier constraint relation data
//
//   Question: do we really need to pass CRNodes again?  Here, I'm going
//            to ignore it for now (i.e., not store it, but just check it), 
//            as it got passed during the initialization phase, so all we'll 
//            do here is check for errors...
//

  //If we're using the FiniteElementData interface, we give the constraint
  //data to the underlying solver using this special function...
  CHK_ERR( loadFEDataMultCR(CRID, numCRNodes, CRNodes, CRFields, CRWeights,
			    CRValue) );

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::loadCRPen(int CRID, 
                        int numCRNodes, 
                        const GlobalID* CRNodes,
                        const int* CRFields,
                        const double* CRWeights,
                        double CRValue,
                        double penValue)
{
//
// Load penalty constraint relation data
//

   debugOutput("FEI: loadCRPen");

   //If we're using the FiniteElementData interface, we give the constraint
   //data to the underlying solver using this special function...
   CHK_ERR( loadFEDataPenCR(CRID, numCRNodes, CRNodes, CRFields, CRWeights,
			    CRValue, penValue) );

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::parameters(int numParams, const char *const* paramStrings)
{
//
// this function takes parameters for setting internal things like solver
// and preconditioner choice, etc.
//
   if (numParams == 0 || paramStrings == NULL) {
      debugOutput("#FEDataFilter::parameters --- no parameters.");
   }
   else {

      snl_fei::getIntParamValue("outputLevel",numParams, paramStrings,
                                outputLevel_);

      snl_fei::getIntParamValue("internalFei",numParams,paramStrings,
                                internalFei_);

      if (Filter::logStream() != NULL) {
	(*logStream())<<"#FEDataFilter::parameters"<<FEI_ENDL
			 <<"# --- numParams: "<< numParams<<FEI_ENDL;
         for(int i=0; i<numParams; i++){
	   (*logStream())<<"#------ paramStrings["<<i<<"]: "
			    <<paramStrings[i]<<FEI_ENDL;
         }
      }
   }

   CHK_ERR( Filter::parameters(numParams, paramStrings) );

   debugOutput("#FEDataFilter leaving parameters function");

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::loadComplete()
{
//  int flag = newData_ ? 1 : 0;
//  int globalFlag;
//  CHK_ERR( commUtils_->GlobalMax(flag, globalFlag) );
//
//  newData_ = globalFlag > 0 ? true : false;
//
//  if (!newData_) return(0);
  debugOutput("FEI: loadComplete");

//  CHK_ERR( exchangeRemoteEquations() );

  debugOutput("#FEDataFilter calling FEData matrixLoadComplete");

  CHK_ERR( feData_->loadComplete() );

  newData_ = false;

  return(0);
}

//------------------------------------------------------------------------------
int FEDataFilter::residualNorm(int whichNorm, int numFields,
                           int* fieldIDs, double* norms, double& residTime)
{
//
//This function can do 3 kinds of norms: infinity-norm (denoted
//by whichNorm==0), 1-norm and 2-norm.
//
   debugOutput("FEI: residualNorm");

   CHK_ERR( loadComplete() );

   //for now, FiniteElementData doesn't do residual calculations.

   int fdbNumFields = problemStructure_->getNumFields();
   const int* fdbFieldIDs = problemStructure_->getFieldIDsPtr();

   int i;

   //Since we don't calculate actual residual norms, we'll fill the user's
   //array with norm data that is obviously not real norm data.
   int offset = 0;
   i = 0;
   while(offset < numFields && i < fdbNumFields) {
     if (fdbFieldIDs[i] >= 0) {
       fieldIDs[offset++] = fdbFieldIDs[i];
     }
     ++i;
   }
   for(i=0; i<numFields; ++i) {
      norms[i] = -99.9;
   }

   //fill out the end of the array with garbage in case the user-provided
   //array is longer than the list of fields we have in fieldDB.
   for(i=offset; i<numFields; ++i) {
      fieldIDs[i] = -99;
   }

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::formResidual(double* residValues, int numLocalEqns)
{
  //FiniteElementData implementations can't currently do residuals.
  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::solve(int& status, double& sTime) {

   debugOutput("FEI: solve");

   CHK_ERR( loadComplete() );

   debugOutput("#FEDataFilter in solve, calling launchSolver...");
 
   double start = MPI_Wtime();

   CHK_ERR( feData_->launchSolver(status, iterations_) );

   sTime = MPI_Wtime() - start;

   debugOutput("#FEDataFilter... back from solver");
 
   //now unpack the locally-owned shared entries of the solution vector into
   //the eqn-comm-mgr data structures.
   CHK_ERR( unpackSolution() );

   debugOutput("#FEDataFilter leaving solve");

   if (status != 0) return(1);
   else return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::setNumRHSVectors(int numRHSs, int* rhsIDs){

   if (numRHSs < 0) {
      FEI_CERR << "FEDataFilter::setNumRHSVectors: ERROR, numRHSs < 0." << FEI_ENDL;
      ERReturn(-1);
   }

   numRHSs_ = numRHSs;

   rhsIDs_.resize(numRHSs_);
   for(int i=0; i<numRHSs_; i++) rhsIDs_[i] = rhsIDs[i];

  //(we need to set the number of RHSs in the eqn comm manager)
  eqnCommMgr_->setNumRHSs(numRHSs_);

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::setCurrentRHS(int rhsID)
{
   int index = rhsIDs_.find(rhsID);

   if (index < 0) ERReturn(-1)
   
   currentRHS_ = index;

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::giveToMatrix(int numPtRows, const int* ptRows,
			   int numPtCols, const int* ptCols,
			   const double* const* values, int mode)
{
  //This isn't going to be fast... I need to optimize the whole structure
  //of code that's associated with passing data to FiniteElementData.

  feiArray<int> rowNodeNumbers, rowDofOffsets, colNodeNumbers, colDofOffsets;
  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();
  int i;

  //First, we have to get nodeNumbers and dofOffsets for each of the
  //row-numbers and col-numbers.

  for(i=0; i<numPtRows; i++) {
    int nodeNumber = problemStructure_->getAssociatedNodeNumber(ptRows[i]);
    if (nodeNumber < 0) ERReturn(-1);
    int fieldID = problemStructure_->getAssociatedFieldID(ptRows[i]);
    if (fieldID < 0) ERReturn(-1);
    NodeDescriptor* node = NULL;
    CHK_ERR( nodeDB.getNodeWithNumber(nodeNumber, node) );
    int firstEqn = node->getFieldEqnNumbers()[0];

    rowNodeNumbers.append(nodeNumber);
    rowDofOffsets.append(ptRows[i] - firstEqn);
  }

  for(i=0; i<numPtCols; i++) {
    int nodeNumber = problemStructure_->getAssociatedNodeNumber(ptCols[i]);
    if (nodeNumber < 0) ERReturn(-1);
    int fieldID = problemStructure_->getAssociatedFieldID(ptCols[i]);
    if (fieldID < 0) ERReturn(-1);
    NodeDescriptor* node = NULL;
    CHK_ERR( nodeDB.getNodeWithNumber(nodeNumber, node) );
    int firstEqn = node->getFieldEqnNumbers()[0];

    colNodeNumbers.append(nodeNumber);
    colDofOffsets.append(ptCols[i] - firstEqn);
  }

  //now we have to flatten the colNodeNumbers and colDofOffsets out into
  //an array of length numPtRows*numPtCols, where the nodeNumbers and
  //dofOffsets are repeated 'numPtRows' times.

  int len = numPtRows*numPtCols;
  feiArray<int> allColNodeNumbers(len), allColDofOffsets(len);
  feiArray<double> allValues(len);

  int offset = 0;
  for(i=0; i<numPtRows; i++) {
    for(int j=0; j<numPtCols; j++) {
      allColNodeNumbers[offset] = colNodeNumbers[j];
      allColDofOffsets[offset] = colDofOffsets[j];
      allValues[offset++] = values[i][j];
    }
  }

  //while we're at it, let's make an array with numPtCols replicated in it
  //'numPtRows' times.
  feiArray<int> numColsPerRow(numPtRows);
  numColsPerRow = numPtCols; //feiArray::operator=

  //now we're ready to hand this stuff off to the FiniteElementData
  //instantiation.

  CHK_ERR( feData_->sumIntoMatrix(numPtRows,
				  rowNodeNumbers.dataPtr(),
				  rowDofOffsets.dataPtr(),
				  numColsPerRow.dataPtr(),
				  allColNodeNumbers.dataPtr(),
				  allColDofOffsets.dataPtr(),
				  allValues.dataPtr()) );

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::giveToLocalReducedMatrix(int numPtRows, const int* ptRows,
				       int numPtCols, const int* ptCols,
				       const double* const* values, int mode)
{
  //This isn't going to be fast... I need to optimize the whole structure
  //of code that's associated with passing data to FiniteElementData.

  feiArray<int> rowNodeNumbers, rowDofOffsets, colNodeNumbers, colDofOffsets;
  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();
  int i;

  //First, we have to get nodeNumbers and dofOffsets for each of the
  //row-numbers and col-numbers.

  for(i=0; i<numPtRows; i++) {
    int nodeNumber = problemStructure_->getAssociatedNodeNumber(ptRows[i]);
    if (nodeNumber < 0) ERReturn(-1);
    int fieldID = problemStructure_->getAssociatedFieldID(ptRows[i]);
    if (fieldID < 0) ERReturn(-1);
    NodeDescriptor* node = NULL;
    CHK_ERR( nodeDB.getNodeWithNumber(nodeNumber, node) );
    int firstEqn = node->getFieldEqnNumbers()[0];

    rowNodeNumbers.append(nodeNumber);
    rowDofOffsets.append(ptRows[i] - firstEqn);
  }

  for(i=0; i<numPtCols; i++) {
    int nodeNumber = problemStructure_->getAssociatedNodeNumber(ptCols[i]);
    if (nodeNumber < 0) ERReturn(-1);
    int fieldID = problemStructure_->getAssociatedFieldID(ptCols[i]);
    if (fieldID < 0) ERReturn(-1);
    NodeDescriptor* node = NULL;
    CHK_ERR( nodeDB.getNodeWithNumber(nodeNumber, node) );
    int firstEqn = node->getFieldEqnNumbers()[0];

    colNodeNumbers.append(nodeNumber);
    colDofOffsets.append(ptCols[i] - firstEqn);
  }

  //now we have to flatten the colNodeNumbers and colDofOffsets out into
  //an array of length numPtRows*numPtCols, where the nodeNumbers and
  //dofOffsets are repeated 'numPtRows' times.

  int len = numPtRows*numPtCols;
  feiArray<int> allColNodeNumbers(len), allColDofOffsets(len);
  feiArray<double> allValues(len);

  int offset = 0;
  for(i=0; i<numPtRows; i++) {
    for(int j=0; j<numPtCols; j++) {
      allColNodeNumbers[offset] = colNodeNumbers[j];
      allColDofOffsets[offset] = colDofOffsets[j];
      allValues[offset++] = values[i][j];
    }
  }

  //while we're at it, let's make an array with numPtCols replicated in it
  //'numPtRows' times.
  feiArray<int> numColsPerRow(numPtRows);
  numColsPerRow = numPtCols; //feiArray::operator=

  //now we're ready to hand this stuff off to the FiniteElementData
  //instantiation.

  CHK_ERR( feData_->sumIntoMatrix(numPtRows,
				  rowNodeNumbers.dataPtr(),
				  rowDofOffsets.dataPtr(),
				  numColsPerRow.dataPtr(),
				  allColNodeNumbers.dataPtr(),
				  allColDofOffsets.dataPtr(),
				  allValues.dataPtr()) );

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::sumIntoMatrix(SSMat& mat)
{
  int numRows = mat.getRowNumbers().length();
  int* rowNumbers = mat.getRowNumbers().dataPtr();
  if (numRows == 0) return(FEI_SUCCESS);

  feiArray<SSVec*>& rows = mat.getRows();

  for(int i=0; i<numRows; i++) {
    SSVec& row = *(rows[i]);
    double* coefPtr = row.coefs().dataPtr();

    CHK_ERR( giveToMatrix(1, &(rowNumbers[i]),
			  row.indices().length(), row.indices().dataPtr(),
			  &coefPtr, ASSEMBLE_SUM) );
  }

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::getFromMatrix(int numPtRows, const int* ptRows,
			    const int* rowColOffsets, const int* ptCols,
			    int numColsPerRow, double** values)
{
  return(-1);

}

//------------------------------------------------------------------------------
int FEDataFilter::getEqnsFromMatrix(ProcEqns& procEqns, EqnBuffer& eqnData)
{
  ERReturn(-1);
}

//------------------------------------------------------------------------------
int FEDataFilter::getEqnsFromRHS(ProcEqns& procEqns, EqnBuffer& eqnData)
{
  ERReturn(-1);
}

//------------------------------------------------------------------------------
int FEDataFilter::giveToRHS(int num, const double* values,
			const int* indices, int mode)
{
  feiArray<int> workspace(num*2);
  int* rowNodeNumbers = workspace.dataPtr();
  int* rowDofOffsets  = rowNodeNumbers+num;
  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

  for(int i=0; i<num; ++i) {
    NodeDescriptor* nodeptr = 0;
    int err = nodeDB.getNodeWithEqn(indices[i], nodeptr);
    if (err < 0) { 
	rowNodeNumbers[i] = -1;
	rowDofOffsets[i] = -1;
	continue;
    }

    rowNodeNumbers[i] = nodeptr->getNodeNumber();

    int firstEqn = nodeptr->getFieldEqnNumbers()[0];

    rowDofOffsets[i] = indices[i] - firstEqn;
  }

  if (mode == ASSEMBLE_SUM) {
    CHK_ERR( feData_->sumIntoRHSVector(num,
				       rowNodeNumbers,
				       rowDofOffsets,
				       values) );
  }
  else {
    CHK_ERR( feData_->putIntoRHSVector(num,
				       rowNodeNumbers,
				       rowDofOffsets,
				       values) );
  }

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::giveToLocalReducedRHS(int num, const double* values,
				    const int* indices, int mode)
{
  feiArray<int> rowNodeNumbers, rowDofOffsets;
  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();
  int i;

  for(i=0; i<num; i++) {
    int nodeNumber = problemStructure_->getAssociatedNodeNumber(indices[i]);
    if (nodeNumber < 0) ERReturn(-1);

    int fieldID = problemStructure_->getAssociatedFieldID(indices[i]);
    if (fieldID < 0) ERReturn(-1);

    NodeDescriptor* node = NULL;
    CHK_ERR( nodeDB.getNodeWithNumber(nodeNumber, node) );

    int firstEqn = node->getFieldEqnNumbers()[0];

    rowNodeNumbers.append(nodeNumber);
    rowDofOffsets.append(indices[i] - firstEqn);
  }

  if (mode == ASSEMBLE_SUM) {
    CHK_ERR( feData_->sumIntoRHSVector(rowNodeNumbers.length(),
				       rowNodeNumbers.dataPtr(),
				       rowDofOffsets.dataPtr(),
				       values) );
  }
  else {
    CHK_ERR( feData_->putIntoRHSVector(rowNodeNumbers.length(),
				       rowNodeNumbers.dataPtr(),
				       rowDofOffsets.dataPtr(),
				       values) );
  }

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::sumIntoRHS(SSVec& vec)
{
  feiArray<int>& indices = vec.indices();
  feiArray<double>& coefs = vec.coefs();

  CHK_ERR( giveToRHS(indices.length(), coefs.dataPtr(), indices.dataPtr(),
		     ASSEMBLE_SUM) );

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::putIntoRHS(SSVec& vec)
{
  feiArray<int>& indices = vec.indices();
  feiArray<double>& coefs = vec.coefs();

  CHK_ERR( giveToRHS(indices.length(), coefs.dataPtr(), indices.dataPtr(),
		     ASSEMBLE_PUT) );

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::getFromRHS(int num, double* values, const int* indices)
{

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::getEqnSolnEntry(int eqnNumber, double& solnValue)
{
  //This function's task is to retrieve the solution-value for a global
  //equation-number. eqnNumber may or may not be a slave-equation, and may or
  //may not be locally owned. If it is not locally owned, it should at least
  //be shared.
  //return 0 if the solution is successfully retrieved, otherwise return 1.
  //

  if (localStartRow_ > eqnNumber || eqnNumber > localEndRow_) {
    //Dig into the eqn-comm-mgr for the shared-remote solution value.
    CHK_ERR( getSharedRemoteSolnEntry(eqnNumber, solnValue) );
  }
  else {
    //It's local, simply get the solution from the assembled linear system.
    CHK_ERR( getReducedSolnEntry( eqnNumber, solnValue ) );
  }
  return(0);
}

//------------------------------------------------------------------------------
int FEDataFilter::getSharedRemoteSolnEntry(int eqnNumber, double& solnValue)
{
  feiArray<int>& remoteEqnNumbers = eqnCommMgr_->sendEqnNumbersPtr();
  double* remoteSoln = eqnCommMgr_->sendEqnSolnPtr();

  int index = snl_fei::binarySearch(eqnNumber, remoteEqnNumbers);
  if (index < 0) {
    FEI_CERR << "FEDataFilter::getSharedRemoteSolnEntry: ERROR, eqn "
	 << eqnNumber << " not found." << FEI_ENDL;
    ERReturn(-1);
  }
  solnValue = remoteSoln[index];
  return(0);
}

//------------------------------------------------------------------------------
int FEDataFilter::getReducedSolnEntry(int eqnNumber, double& solnValue)
{
  //We may safely assume that this function is called with 'eqnNumber' that is
  //local in the underlying assembled linear system. i.e., it isn't a slave-
  //equation, it isn't remotely owned, etc.
  //

  int nodeNumber = problemStructure_->getAssociatedNodeNumber(eqnNumber);

  //if nodeNumber < 0, it probably means we're trying to look up the
  //node for a lagrange-multiplier (which doesn't exist). In that
  //case, we're just going to ignore the request and return for now...
  if (nodeNumber < 0) {solnValue = -999.99; return(FEI_SUCCESS);}

  NodeDescriptor* node = NULL;
  CHK_ERR( problemStructure_->getNodeDatabase().
	   getNodeWithNumber(nodeNumber, node));

  int eqn = problemStructure_->translateFromReducedEqn(eqnNumber);
  int firstEqn = node->getFieldEqnNumbers()[0];
  int dofOffset = eqn - firstEqn;

  bool fetiHasNode = true;
  GlobalID nodeID = node->getGlobalNodeID();
  NodeCommMgr& nodeCommMgr = problemStructure_->getNodeCommMgr();
  std::vector<GlobalID>& shNodeIDs = nodeCommMgr.getSharedNodeIDs();
  int shIndex = snl_fei::binarySearch(nodeID, &shNodeIDs[0], shNodeIDs.size());
  if (shIndex >= 0) {
    if (!(problemStructure_->isInLocalElement(nodeNumber)) ) fetiHasNode = false;
  }

  if (fetiHasNode) {
    int err = feData_->getSolnEntry(nodeNumber, dofOffset, solnValue);
    if (err != 0) {
      FEI_CERR << "FEDataFilter::getReducedSolnEntry: nodeNumber " << nodeNumber
	   << " (nodeID " << node->getGlobalNodeID() << "), dofOffset "<<dofOffset
	   << " couldn't be obtained from FETI on proc " << localRank_ << FEI_ENDL;
      ERReturn(-1);
    }
  }

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::unpackSolution()
{
  //
  //This function should be called after the solver has returned,
  //and we know that there is a solution in the underlying vector.
  //This function ensures that any locally-owned shared solution values are
  //available on the sharing processors.
  //
  if (Filter::logStream() != NULL) {
    (*logStream())<< "#  entering unpackSolution, outputLevel: "
		     <<outputLevel_<<FEI_ENDL;
  }

  //what we need to do is as follows.
  //The eqn comm mgr has a list of what it calls 'recv eqns'. These are
  //equations that we own, for which we received contributions from other
  //processors. The solution values corresponding to these equations need
  //to be made available to those remote contributing processors.

   int numRecvEqns = eqnCommMgr_->getNumLocalEqns();
   feiArray<int>& recvEqnNumbers = eqnCommMgr_->localEqnNumbersPtr();

   for(int i=0; i<numRecvEqns; i++) {
     int eqn = recvEqnNumbers[i];

     if ((reducedStartRow_ > eqn) || (reducedEndRow_ < eqn)) {
       FEI_CERR << "FEDataFilter::unpackSolution: ERROR, 'recv' eqn (" << eqn
  	   << ") out of local range." << FEI_ENDL;
       MPI_Abort(comm_, -1);
     }

     double solnValue = 0.0;

     CHK_ERR( getReducedSolnEntry(eqn, solnValue) );

     eqnCommMgr_->addSolnValues(&eqn, &solnValue, 1);
   }

   eqnCommMgr_->exchangeSoln();

  debugOutput("#FEDataFilter leaving unpackSolution");
  return(FEI_SUCCESS);
}
             
//------------------------------------------------------------------------------
void FEDataFilter::  setEqnCommMgr(EqnCommMgr* eqnCommMgr)
{
  delete eqnCommMgr_;
  eqnCommMgr_ = eqnCommMgr;
}

//------------------------------------------------------------------------------
int FEDataFilter::getBlockNodeSolution(GlobalID elemBlockID,  
				       int numNodes, 
				       const GlobalID *nodeIDs, 
				       int *offsets,
				       double *results)
{        
   debugOutput("FEI: getBlockNodeSolution");

   int numActiveNodes = problemStructure_->getNumActiveNodes();
   NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

   if (numActiveNodes <= 0) return(0);

   int numSolnParams = 0;

   BlockDescriptor* block = NULL;
   CHK_ERR( problemStructure_->getBlockDescriptor(elemBlockID, block) );

   //Traverse the node list, checking if nodes are associated with this block.
   //If so, put its 'answers' in the results list.

   int offset = 0;
   for(int i=0; i<numActiveNodes; i++) {
     NodeDescriptor* node_i = NULL;
     CHK_ERR( nodeDB.getNodeAtIndex(i, node_i) );

      if (offset == numNodes) break;

      GlobalID nodeID = nodeIDs[offset];

      //first let's set the offset at which this node's solution coefs start.
      offsets[offset++] = numSolnParams;

      NodeDescriptor* node = NULL;
      int err = 0;
      //Obtain the NodeDescriptor of nodeID in the activeNodes list...
      //Don't call the getActiveNodeDesc_ID function unless we have to.

      if (nodeID == node_i->getGlobalNodeID()) {
	node = node_i;
      }
      else {
         err = nodeDB.getNodeWithID(nodeID, node);
      }

      //ok. If err is not 0, meaning nodeID is NOT in the
      //activeNodes list, then skip to the next loop iteration.

      if (err != 0) {
	continue;
      }

      int numFields = node->getNumFields();
      const int* fieldIDs = node->getFieldIDList();

      for(int j=0; j<numFields; j++) {
	if (block->containsField(fieldIDs[j])) {
	  int size = problemStructure_->getFieldSize(fieldIDs[j]);
	  if (size < 1) {
	    continue;
	  }

	  int thisEqn = -1;
	  node->getFieldEqnNumber(fieldIDs[j], thisEqn);

	  double answer;
	  for(int k=0; k<size; k++) {
	    CHK_ERR( getEqnSolnEntry(thisEqn+k, answer) )
	      results[numSolnParams++] = answer;
	  }
	}
      }//for(j<numFields)loop
   }

   offsets[numNodes] = numSolnParams;

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::getNodalSolution(int numNodes, 
				   const GlobalID *nodeIDs, 
				   int *offsets,
				   double *results)
{        
  debugOutput("FEI: getNodalSolution");

  int numActiveNodes = problemStructure_->getNumActiveNodes();
  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

  if (numActiveNodes <= 0) return(0);

  int numSolnParams = 0;

  //Traverse the node list, checking if nodes are local.
  //If so, put 'answers' in the results list.

  int offset = 0;
  for(int i=0; i<numActiveNodes; i++) {
    NodeDescriptor* node_i = NULL;
    CHK_ERR( nodeDB.getNodeAtIndex(i, node_i) );

    if (offset == numNodes) break;

    GlobalID nodeID = nodeIDs[offset];

    //first let's set the offset at which this node's solution coefs start.
    offsets[offset++] = numSolnParams;

    NodeDescriptor* node = NULL;
    int err = 0;
    //Obtain the NodeDescriptor of nodeID in the activeNodes list...
    //Don't call the getNodeWithID function unless we have to.

    if (nodeID == node_i->getGlobalNodeID()) {
      node = node_i;
    }
    else {
      err = nodeDB.getNodeWithID(nodeID, node);
    }

    //ok. If err is not 0, meaning nodeID is NOT in the
    //activeNodes list, then skip to the next loop iteration.

    if (err != 0) {
      continue;
    }

    int numFields = node->getNumFields();
    const int* fieldIDs = node->getFieldIDList();

    for(int j=0; j<numFields; j++) {
      int size = problemStructure_->getFieldSize(fieldIDs[j]);
      if (size < 1) {
	continue;
      }

      int thisEqn = -1;
      node->getFieldEqnNumber(fieldIDs[j], thisEqn);

      double answer;
      for(int k=0; k<size; k++) {
	CHK_ERR( getEqnSolnEntry(thisEqn+k, answer) )
	  results[numSolnParams++] = answer;
      }
    }//for(j<numFields)loop
  }

  offsets[numNodes] = numSolnParams;

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::getBlockFieldNodeSolution(GlobalID elemBlockID,
                                        int fieldID,
                                        int numNodes, 
                                        const GlobalID *nodeIDs, 
                                        double *results)
{
  debugOutput("FEI: getBlockFieldNodeSolution");

  int numActiveNodes = problemStructure_->getNumActiveNodes();
  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

  if (numActiveNodes <= 0) return(0);

  BlockDescriptor* block = NULL;
  CHK_ERR( problemStructure_->getBlockDescriptor(elemBlockID, block) );

  int fieldSize = problemStructure_->getFieldSize(fieldID);
  if (fieldSize <= 0) ERReturn(-1);

  if (!block->containsField(fieldID)) {
    FEI_CERR << "FEDataFilter::getBlockFieldNodeSolution WARNING: fieldID " << fieldID
	 << " not contained in element-block " << static_cast<int>(elemBlockID) << FEI_ENDL;
    return(1);
  }

   //Traverse the node list, checking if nodes are associated with this block.
   //If so, put the answers in the results list.

   for(int i=0; i<numNodes; i++) {
     NodeDescriptor* node_i = NULL;
     CHK_ERR( nodeDB.getNodeAtIndex(i, node_i) );

     GlobalID nodeID = nodeIDs[i];

     NodeDescriptor* node = NULL;
     int err = 0;
     //Obtain the NodeDescriptor of nodeID in the activeNodes list...
     //Don't call the getActiveNodeDesc_ID function unless we have to.

     if (nodeID == node_i->getGlobalNodeID()) {
       node = node_i;
     }
     else {
       err = nodeDB.getNodeWithID(nodeID, node);
     }

     //ok. If err is not 0, meaning nodeID is NOT in the
     //activeNodes list, then skip to the next loop iteration.

     if (err != 0) {
       continue;
     }

     int eqnNumber = -1;
     bool hasField = node->getFieldEqnNumber(fieldID, eqnNumber);
     if (!hasField) continue;

     int offset = fieldSize*i;
     for(int j=0; j<fieldSize; j++) {
       double answer = 0.0;
       CHK_ERR( getEqnSolnEntry(eqnNumber+j, answer) );
       results[offset+j] = answer;
     }
   }

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::getNodalFieldSolution(int fieldID,
					int numNodes, 
					const GlobalID *nodeIDs, 
					double *results)
{
  debugOutput("FEI: getNodalFieldSolution");

  int numActiveNodes = problemStructure_->getNumActiveNodes();
  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

  if (numActiveNodes <= 0) return(0);

  if (problemStructure_->numSlaveEquations() != 0) {
    FEI_CERR << "FEDataFilter::getEqnSolnEntry ERROR FETI-support is not currently"
         << " compatible with the FEI's constraint reduction." << FEI_ENDL;
    ERReturn(-1);
  }

  int fieldSize = problemStructure_->getFieldSize(fieldID);
  if (fieldSize <= 0) {
    ERReturn(-1);
  }

  NodeCommMgr& nodeCommMgr = problemStructure_->getNodeCommMgr();

  //Traverse the node list, checking if nodes have the specified field.
  //If so, put the answers in the results list.

  for(int i=0; i<numNodes; i++) {
    NodeDescriptor* node_i = NULL;
    CHK_ERR( nodeDB.getNodeAtIndex(i, node_i) );

    GlobalID nodeID = nodeIDs[i];

    NodeDescriptor* node = NULL;
    int err = 0;
    //Obtain the NodeDescriptor of nodeID in the activeNodes list...
    //Don't call the getNodeWithID function unless we have to.

    if (nodeID == node_i->getGlobalNodeID()) {
      node = node_i;
    }
    else {
      err = nodeDB.getNodeWithID(nodeID, node);
    }

    //ok. If err is not 0, meaning nodeID is NOT in the
    //activeNodes list, then skip to the next loop iteration.

    if (err != 0) {
      continue;
    }

    int nodeNumber = node->getNodeNumber();

    int eqnNumber = -1;
    bool hasField = node->getFieldEqnNumber(fieldID, eqnNumber);

    //If this node doesn't have the specified field, then skip to the
    //next loop iteration.
    if (!hasField) continue;

    std::vector<GlobalID>& shNodeIDs = nodeCommMgr.getSharedNodeIDs();
    int shIndex = snl_fei::binarySearch(nodeID, &shNodeIDs[0], shNodeIDs.size());
    if (shIndex > -1) {
      if (!(problemStructure_->isInLocalElement(nodeNumber))) continue;
    }

    int firstEqn = node->getFieldEqnNumbers()[0];
    int dofOffset = eqnNumber-firstEqn;

    int offset = fieldSize*i;

    for(int j=0; j<fieldSize; j++) {
      if (localStartRow_ > eqnNumber || eqnNumber > localEndRow_) {
	CHK_ERR( getSharedRemoteSolnEntry(eqnNumber+j, results[offset+j]) );
	continue;
      }

      int err = feData_->getSolnEntry(nodeNumber, dofOffset+j, results[offset+j]);
      if (err != 0) {
	FEI_CERR << "FEDataFilter::getReducedSolnEntry: nodeNumber " << nodeNumber
	     << " (nodeID " << nodeID << "), dofOffset "<<dofOffset
	     << " couldn't be obtained from FETI on proc " << localRank_ << FEI_ENDL;
	ERReturn(-1);
      }
    }
  }

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::putBlockNodeSolution(GlobalID elemBlockID,
                                   int numNodes, 
                                   const GlobalID *nodeIDs, 
                                   const int *offsets,
                                   const double *estimates) {
        
   debugOutput("FEI: putBlockNodeSolution");

   int numActiveNodes = problemStructure_->getNumActiveNodes();

   if (numActiveNodes <= 0) return(0);

   BlockDescriptor* block = NULL;
   CHK_ERR( problemStructure_->getBlockDescriptor(elemBlockID, block) );

   NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

   //traverse the node list, checking for nodes associated with this block
   //when an associated node is found, put its 'answers' into the linear system.

   for(int i=0; i<numNodes; i++) {
     NodeDescriptor* node = NULL;
     int err = nodeDB.getNodeWithID(nodeIDs[i], node);

      if (err != 0) continue;
   
      if (!node->containedInBlock(elemBlockID)) continue;

      if (node->getOwnerProc() != localRank_) continue;

      int numFields = node->getNumFields();
      const int* fieldIDs = node->getFieldIDList();
      const int* fieldEqnNumbers = node->getFieldEqnNumbers();

      if (fieldEqnNumbers[0] < localStartRow_ ||
          fieldEqnNumbers[0] > localEndRow_) continue;

      int offs = offsets[i];

      for(int j=0; j<numFields; j++) {
         int size = problemStructure_->getFieldSize(fieldIDs[j]);

         if (block->containsField(fieldIDs[j])) {
            for(int k=0; k<size; k++) {
               int reducedEqn;
	       problemStructure_->
		 translateToReducedEqn(fieldEqnNumbers[j]+k, reducedEqn);

//                if (useLinSysCore_) {
//                   CHK_ERR( lsc_->putInitialGuess(&reducedEqn,
//                                             &estimates[offs+k], 1) );
// 	       }
            }
         }
         offs += size;
      }//for(j<numFields)loop
   }

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::putBlockFieldNodeSolution(GlobalID elemBlockID, 
                                        int fieldID, 
                                        int numNodes, 
                                        const GlobalID *nodeIDs, 
                                        const double *estimates)
{
   debugOutput("FEI: putBlockFieldNodeSolution");

   BlockDescriptor* block = NULL;
   CHK_ERR( problemStructure_->getBlockDescriptor(elemBlockID, block) );
   if (!block->containsField(fieldID)) return(1);

   int fieldSize = problemStructure_->getFieldSize(fieldID);
   NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

   //if we have a negative fieldID, we'll need a list of length numNodes,
   //in which to put nodeNumbers for passing to the solver... 

   feiArray<int> numbers(numNodes);

   //if we have a fieldID >= 0, then our numbers list will hold equation numbers
   //and we'll need fieldSize*numNodes of them.

   feiArray<double> data;

   if (fieldID >= 0) {
     if (fieldSize < 1) {
       FEI_CERR << "FEI Warning, putBlockFieldNodeSolution called for field "
	    << fieldID<<", which has size "<<fieldSize<<FEI_ENDL;
       return(0);
     }
     try {
     numbers.resize(numNodes*fieldSize);
     data.resize(numNodes*fieldSize);
     }
     catch(fei::Exception& exc) {
       FEI_CERR << exc.what()<<FEI_ENDL;
       ERReturn(-1);
     }
   }

   int count = 0;

   for(int i=0; i<numNodes; i++) {
     NodeDescriptor* node = NULL;
     CHK_ERR( nodeDB.getNodeWithID(nodeIDs[i], node) );

      if (fieldID < 0) numbers[count++] = node->getNodeNumber();
      else {
         int eqn = -1;
	 if (node->getFieldEqnNumber(fieldID, eqn)) {
           if (eqn >= localStartRow_ && eqn <= localEndRow_) {
             for(int j=0; j<fieldSize; j++) { 
               data[count] = estimates[i*fieldSize + j];
               problemStructure_->translateToReducedEqn(eqn+j, numbers[count++]);
             }
	   }
	 }
      }
   }
 
   if (fieldID < 0) {
     CHK_ERR( feData_->putNodalFieldData(fieldID, fieldSize, 
					 numNodes, numbers.dataPtr(),
					 estimates));
   }

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::getBlockElemSolution(GlobalID elemBlockID,
                                   int numElems, 
                                   const GlobalID *elemIDs,
                                   int& numElemDOFPerElement,
                                   double *results)
{
//
//  return the elemental solution parameters associated with a
//  particular block of elements
//
   debugOutput("FEI: getBlockElemSolution");

   BlockDescriptor* block = NULL;
   CHK_ERR( problemStructure_->getBlockDescriptor(elemBlockID, block) )

   std::map<GlobalID,int>& elemIDList = problemStructure_->
                          getBlockConnectivity(elemBlockID).elemIDs;

   int len = block->getNumElements();

   //if the user is only asking for a subset of element-solutions, shrink len.
   if (len > numElems) len = numElems;

   numElemDOFPerElement = block->getNumElemDOFPerElement();
   std::vector<int>& elemDOFEqnNumbers = block->elemDOFEqnNumbers();
   double answer;


   if (numElemDOFPerElement <= 0) return(0);

   std::map<GlobalID,int>::const_iterator
     elemid_end = elemIDList.end(),
     elemid_itr = elemIDList.begin();

   for(int i=0; i<len; i++) {
      int index = i;

      //if the user-supplied elemIDs are out of order, we need the index of
      //the location of this element.
      if (elemid_itr->first != elemIDs[i]) {
         index = elemid_itr->second;
      }

      if (index < 0) continue;

      int offset = i*numElemDOFPerElement;

      for(int j=0; j<numElemDOFPerElement; j++) {
         int eqn = elemDOFEqnNumbers[index] + j;

         CHK_ERR( getEqnSolnEntry(eqn, answer) )

         results[offset+j] = answer;
      }

      ++elemid_itr;
   }

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::putBlockElemSolution(GlobalID elemBlockID,
                                   int numElems,
                                   const GlobalID *elemIDs,
                                   int dofPerElem,
                                   const double *estimates)
{
   debugOutput("FEI: putBlockElemSolution");

   BlockDescriptor* block = NULL;
   CHK_ERR( problemStructure_->getBlockDescriptor(elemBlockID, block) )

   std::map<GlobalID,int>& elemIDList = problemStructure_->
                          getBlockConnectivity(elemBlockID).elemIDs;

   int len = block->getNumElements();
   if (len > numElems) len = numElems;

   int DOFPerElement = block->getNumElemDOFPerElement();
   if (DOFPerElement != dofPerElem) {
     FEI_CERR << "FEI ERROR, putBlockElemSolution called with bad 'dofPerElem' ("
	  <<dofPerElem<<"), block "<<elemBlockID<<" should have dofPerElem=="
	  <<DOFPerElement<<FEI_ENDL;
     ERReturn(-1);
   }

   std::vector<int>& elemDOFEqnNumbers = block->elemDOFEqnNumbers();

   if (DOFPerElement <= 0) return(0);

   std::map<GlobalID,int>::const_iterator
     elemid_end = elemIDList.end(),
     elemid_itr = elemIDList.begin();

   for(int i=0; i<len; i++) {
      int index = i;
      if (elemid_itr->first != elemIDs[i]) {
         index = elemid_itr->second;
      }

      if (index < 0) continue;

      for(int j=0; j<DOFPerElement; j++) {
         int reducedEqn;
	 problemStructure_->
	   translateToReducedEqn(elemDOFEqnNumbers[i] + j, reducedEqn);
//         double soln = estimates[i*DOFPerElement + j];

//       if (useLinSysCore_) {
// 	   CHK_ERR( lsc_->putInitialGuess(&reducedEqn, &soln, 1) );
// 	 }
      }

      ++elemid_itr;
   }

   return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::getCRMultipliers(int numCRs,
				   const int* CRIDs,
				   double* multipliers)
{
  for(int i=0; i<numCRs; i++) {
    //temporarily, FETI's getMultiplierSoln method isn't implemented.
    //CHK_ERR( feData_->getMultiplierSoln(CRIDs[i], multipliers[i]) );
    multipliers[i] = -999.99;
  }

  return(-1);
}

//------------------------------------------------------------------------------
int FEDataFilter::putCRMultipliers(int numMultCRs,
                               const int* CRIDs,
                               const double *multEstimates)
{
  debugOutput("FEI: putCRMultipliers");

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::putNodalFieldData(int fieldID,
				int numNodes,
				const GlobalID* nodeIDs,
				const double* nodeData)
{
  debugOutput("FEI: putNodalFieldData");

  int fieldSize = problemStructure_->getFieldSize(fieldID);
  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

  feiArray<int> nodeNumbers(numNodes);

  for(int i=0; i<numNodes; i++) {
    NodeDescriptor* node = NULL;
    CHK_ERR( nodeDB.getNodeWithID(nodeIDs[i], node) );

    int nodeNumber = node->getNodeNumber();
    if (nodeNumber < 0) {
      GlobalID nodeID = nodeIDs[i];
      FEI_CERR << "FEDataFilter::putNodalFieldData ERROR, node with ID " 
	   << static_cast<int>(nodeID) << " doesn't have an associated nodeNumber "
	   << "assigned. putNodalFieldData shouldn't be called until after the "
	   << "initComplete method has been called." << FEI_ENDL;
      ERReturn(-1);
    }

    nodeNumbers[i] = nodeNumber;
  }

  CHK_ERR( feData_->putNodalFieldData(fieldID, fieldSize,
				      numNodes, nodeNumbers.dataPtr(),
				      nodeData) );

  return(0);
}

//------------------------------------------------------------------------------
int FEDataFilter::putNodalFieldSolution(int fieldID,
				int numNodes,
				const GlobalID* nodeIDs,
				const double* nodeData)
{
  debugOutput("FEI: putNodalFieldSolution");

  if (fieldID < 0) {
    return(putNodalFieldData(fieldID, numNodes, nodeIDs, nodeData));
  }

  int fieldSize = problemStructure_->getFieldSize(fieldID);
  NodeDatabase& nodeDB = problemStructure_->getNodeDatabase();

  feiArray<int> eqnNumbers(fieldSize);

  for(int i=0; i<numNodes; i++) {
    NodeDescriptor* node = NULL;
    CHK_ERR( nodeDB.getNodeWithID(nodeIDs[i], node) );

    int eqn = -1;
    bool hasField = node->getFieldEqnNumber(fieldID, eqn);
    if (!hasField) continue;

  }

  return(0);
}

//------------------------------------------------------------------------------
int FEDataFilter::assembleEqns(SSMat& mat, int mode)
{
  int numRows = mat.getRows().length();
  int* rowNumbers = mat.getRowNumbers().dataPtr();

  if (numRows == 0) return(FEI_SUCCESS);

  feiArray<SSVec*>& rows = mat.getRows();

  for(int i=0; i<numRows; i++) {
    int row = rowNumbers[i];

    int numCols = rows[i]->length();
    const int* indPtr = rows[i]->indices().dataPtr();
    const double* coefPtr = rows[i]->coefs().dataPtr();

    CHK_ERR(giveToMatrix(1, &row,numCols, indPtr, &coefPtr, mode));
  }

  return(FEI_SUCCESS);
}

//------------------------------------------------------------------------------
int FEDataFilter::assembleRHS(SSVec& vec, int mode) {
//
//This function hands the data off to the routine that finally
//sticks it into the RHS vector.
//
  int len = vec.length();
  feiArray<int>& indices = vec.indices();
  feiArray<double>& coefs = vec.coefs();

  if (problemStructure_->numSlaveEquations() == 0) {
    CHK_ERR( giveToRHS(len, coefs.dataPtr(), indices.dataPtr(), mode) );
    return(FEI_SUCCESS);
  }

  for(int i = 0; i < len; i++) {
    int eqn = indices[i];

    CHK_ERR( giveToRHS(1, &(coefs[i]), &eqn, mode ) );
  }

  return(FEI_SUCCESS);
}

//==============================================================================
void FEDataFilter::debugOutput(const char* mesg)
{
  if (Filter::logStream() != NULL) {
    (*logStream()) << mesg << FEI_ENDL;
   }
}
