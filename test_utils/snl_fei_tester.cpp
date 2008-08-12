/*--------------------------------------------------------------------*/
/*    Copyright 2005 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#include <fei_macros.hpp>

#include <test_utils/snl_fei_tester.hpp>

#include <fei_LinearSystemCore.hpp>
#include <feiArray.hpp>
#include <snl_fei_ArrayUtils.hpp>
#include <test_utils/LibraryFactory.hpp>

#include <fei_base.hpp>

#ifdef FEI_HAVE_FETI
#include <FETI_DP_FiniteElementData.h>
#endif

#include <test_utils/DataReader.hpp>
#include <test_utils/SolnCheck.hpp>

#undef fei_file
#define fei_file "snl_fei_tester.cpp"
#include <fei_ErrMacros.hpp>

//----------------------------------------------------------------------------
snl_fei_tester::snl_fei_tester(fei::SharedPtr<DataReader> data_reader,
			       MPI_Comm comm, int localProc, int numProcs)
  : comm_(comm),
    factory_(),
    vecSpace_(),
    matrixGraph_(),
    A_(),
    x_(),
    b_(),
    linSys_(NULL),
    linSysCore_(NULL),
    feData_(NULL),
    data_(data_reader),
    idTypes_(0, 4),
    numPatterns_(0),
    localProc_(localProc),
    numProcs_(numProcs)
{
}

//----------------------------------------------------------------------------
snl_fei_tester::~snl_fei_tester()
{
  delete linSysCore_;
  delete feData_;
}

//----------------------------------------------------------------------------
int snl_fei_tester::testInitialization()
{
  if (factory_.get() == NULL) {
    try {
      factory_ = fei::create_fei_Factory(comm_, data_->solverLibraryName_.c_str());
    }
    catch (fei::Exception& exc) {
      FEI_CERR << exc.what()<<FEI_ENDL;
      return(1);
    }
    if (factory_.get() == NULL) {
      ERReturn(-1);
    }
  }

  std::vector<std::string> stdstrings;
  fei::utils::char_ptrs_to_strings(data_->numParams_, data_->paramStrings_,
                                   stdstrings);
  fei::ParameterSet paramset;
  fei::utils::parse_strings(stdstrings, " ", paramset);

  if (!path_.empty()) {
    paramset.add(fei::Param("debugOutput", path_.c_str()));
  }

  factory_->parameters(paramset);

  vecSpace_ = factory_->createVectorSpace(comm_, NULL);

  vecSpace_->setParameters(paramset);

  defineFieldsAndIDTypes();

  fei::SharedPtr<fei::VectorSpace> dummy;
  matrixGraph_ = factory_->createMatrixGraph(vecSpace_, dummy, NULL);

  matrixGraph_->setParameters(paramset);

  CHK_ERR( initElemBlocks() );

  CHK_ERR( initConstraints() );

  int i;
  for(i=0; i<data_->numSharedNodeSets_; ++i) {
    CommNodeSet& nodeSet = data_->sharedNodeSets_[i];

    CHK_ERR( vecSpace_->initSharedIDs(nodeSet.numNodes_,
				       idTypes_[nodeTypeOffset_],
				       nodeSet.nodeIDs_,
				       nodeSet.procsPerNode_,
				       nodeSet.procs_) );
  }

  CHK_ERR( matrixGraph_->initComplete() );

  return(0);
}

//----------------------------------------------------------------------------
void snl_fei_tester::dumpMatrixFiles()
{
  FEI_OSTRINGSTREAM osstr;
  osstr << "A_" << A_->typeName() << ".np"<<numProcs_;
  std::string str = osstr.str();
  A_->writeToFile(str.c_str());
}

//----------------------------------------------------------------------------
void snl_fei_tester::setParameter(const char* param)
{
  std::vector<std::string> stdstrings;
  fei::utils::char_ptrs_to_strings(1, &param, stdstrings);
  fei::ParameterSet paramset;
  fei::utils::parse_strings(stdstrings, " ", paramset);
  factory_->parameters(paramset);
  vecSpace_->setParameters(paramset);
  matrixGraph_->setParameters(paramset);

  linSys_->parameters(1, &param);
  A_->parameters(1, &param);
}

//----------------------------------------------------------------------------
int snl_fei_tester::testLoading()
{
  linSys_ = factory_->createLinearSystem(matrixGraph_);

  A_ = factory_->createMatrix(matrixGraph_);
  x_ = factory_->createVector(matrixGraph_, true);
  b_ = factory_->createVector(matrixGraph_);

  CHK_ERR( linSys_->parameters(data_->numParams_, data_->paramStrings_) );
  CHK_ERR( A_->parameters(data_->numParams_, data_->paramStrings_) );

  linSys_->setMatrix(A_);
  linSys_->setRHS(b_);
  linSys_->setSolutionVector(x_);

  CHK_ERR( A_->putScalar(0.0) );
  CHK_ERR( b_->putScalar(0.0) );

  matrixGraph_->createSlaveMatrices();

  CHK_ERR( loadElemBlocks() );

  CHK_ERR( loadConstraints() );

  int i;
  for(i=0; i<data_->numBCNodeSets_; ++i) {
    BCNodeSet& bcSet = data_->bcNodeSets_[i];
    int fieldSize = data_->getFieldSize(bcSet.fieldID_);
    if (fieldSize < 1) {
      continue;
    }

    CHK_ERR( linSys_->loadEssentialBCs(bcSet.numNodes_,
				       bcSet.nodeIDs_,
				       idTypes_[nodeTypeOffset_],
				       bcSet.fieldID_,
				       bcSet.offsetsIntoField_,
				       bcSet.prescribed_values_) );
  }

  CHK_ERR( linSys_->loadComplete() );

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei_tester::testSolve()
{
  fei::SharedPtr<fei::Solver> solver = factory_->createSolver();

  std::vector<std::string> stdstrings;
  fei::utils::char_ptrs_to_strings(data_->numParams_, data_->paramStrings_, stdstrings);
  fei::ParameterSet paramset;
  fei::utils::parse_strings(stdstrings," ",paramset);

  int status, itersTaken = 0;
  CHK_ERR( solver->solve(linSys_.get(),
			 NULL, //preconditioningMatrix
			 paramset, itersTaken, status) );

  CHK_ERR( x_->scatterToOverlap() );

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei_tester::testCheckResult()
{
  CHK_ERR( save_block_node_soln(*data_, x_.get(), data_->solnFileName_.c_str(),
				numProcs_, localProc_, 1));

  CHK_ERR( save_block_elem_soln(*data_, x_.get(), data_->solnFileName_.c_str(),
				numProcs_, localProc_, 1));

  CHK_ERR( save_multiplier_soln(*data_, x_.get(), data_->solnFileName_.c_str(),
				numProcs_, localProc_, 1));

  int err = SolnCheck::checkSolution(localProc_, numProcs_, data_->solnFileName_.c_str(),
				     data_->checkFileName_.c_str(), "node", 1);

  err += SolnCheck::checkSolution(localProc_, numProcs_, data_->solnFileName_.c_str(),
				  data_->checkFileName_.c_str(), "elem", 1);

  err += SolnCheck::checkSolution(localProc_, numProcs_, data_->solnFileName_.c_str(),
				  data_->checkFileName_.c_str(), "mult", 1);
  int globalErr = err;
#ifndef FEI_SER
  if (MPI_SUCCESS != MPI_Allreduce(&err, &globalErr, 1, MPI_INT, MPI_SUM,
				   comm_)) return(-1);
#endif
  if (globalErr != 0) return(-1);
  return(0);
}

//----------------------------------------------------------------------------
void snl_fei_tester::defineFieldsAndIDTypes()
{
  vecSpace_->defineFields(data_->numFields_, data_->fieldIDs_, data_->fieldSizes_);

  //nodeIDType == 0
  idTypes_.append(0);

  //constraintIDType == 1
  idTypes_.append(1);

  //elemDofIDType == 2
  idTypes_.append(2);

  vecSpace_->defineIDTypes(idTypes_.length(), idTypes_.dataPtr() );

  nodeTypeOffset_ = 0;
  constraintTypeOffset_ = 1;
  elemTypeOffset_ = 2;
}

//----------------------------------------------------------------------------
int snl_fei_tester::initElemBlocks()
{
  for(int i=0; i<data_->numElemBlocks_; ++i) {
    ElemBlock& eb = data_->elemBlocks_[i];

    int patternID;
    definePattern(eb, patternID);

    CHK_ERR( matrixGraph_->initConnectivityBlock(eb.blockID_,
					       eb.numElements_,
					       patternID) );

    for(int j=0; j<eb.numElements_; ++j) {
      feiArray<int> conn(eb.numNodesPerElement_+1);
      for(int ii=0; ii<eb.numNodesPerElement_; ++ii) {
	conn[ii] = eb.elemConn_[j][ii];
      }
      conn[eb.numNodesPerElement_] = eb.elemIDs_[j];

      CHK_ERR( matrixGraph_->initConnectivity(eb.blockID_,
					      eb.elemIDs_[j],
					      conn.dataPtr()) );
    }
  }

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei_tester::loadElemBlocks()
{
  int i;
  for(i=0; i<data_->numElemBlocks_; ++i) {
    ElemBlock& eb = data_->elemBlocks_[i];

    if (eb.numElements_ < 1) {
      continue;
    }

    int numIndices = matrixGraph_->getConnectivityNumIndices(eb.blockID_);

    feiArray<int> indices(numIndices);

    for(int j=0; j<eb.numElements_; ++j) {
      int checkNum;
      CHK_ERR( matrixGraph_->getConnectivityIndices(eb.blockID_,
						    eb.elemIDs_[j],
						    numIndices,
						    indices.dataPtr(),
						    checkNum) );
      if (numIndices != checkNum) {
	ERReturn(-1);
      }

      CHK_ERR( A_->sumIn(eb.blockID_, eb.elemIDs_[j],
			 eb.elemStiff_[j]) );

      CHK_ERR( b_->sumIn(numIndices, indices.dataPtr(),
			 eb.elemLoad_[j], 0) );
    }
  }

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei_tester::initConstraints()
{
  feiArray<int> idTypes;
  int constraintID = localProc_*100000;
  int i;
  for(i=0; i<data_->numCRMultSets_; ++i) {
    CRSet& crSet = data_->crMultSets_[i];

    for(int j=0; j<1; ++j) {
      idTypes.resize(crSet.numNodes_);

      idTypes = idTypes_[nodeTypeOffset_]; //feiArray::operator=

      crSet.crID_ = constraintID++;
      int constraintIDType = idTypes_[constraintTypeOffset_];
      CHK_ERR( matrixGraph_->initLagrangeConstraint(crSet.crID_,
						  constraintIDType,
						  crSet.numNodes_,
						  idTypes.dataPtr(),
						  crSet.nodeIDs_[j],
						  crSet.fieldIDs_) );
    }
  }

  for(i=0; i<data_->numCRPenSets_; ++i) {
    CRSet& crSet = data_->crPenSets_[i];

    for(int j=0; j<1; ++j) {
      idTypes.resize(crSet.numNodes_);

      idTypes = idTypes_[nodeTypeOffset_]; //feiArray::operator=

      crSet.crID_ = constraintID++;
      int constraintIDType = idTypes_[constraintTypeOffset_];
      CHK_ERR( matrixGraph_->initPenaltyConstraint(crSet.crID_,
						  constraintIDType,
						  crSet.numNodes_,
						  idTypes.dataPtr(),
						  crSet.nodeIDs_[j],
						  crSet.fieldIDs_) );
    }
  }

  std::map<int,int> fieldDB;
  for(i=0; i<data_->numFields_; ++i) {
    fieldDB.insert(std::pair<int,int>(data_->fieldIDs_[i], data_->fieldSizes_[i]));
  }

  feiArray<int> nodeIDs;
  feiArray<int> fieldIDs;
  feiArray<double> weights;

  for(i=0; i<data_->numSlaveVars_; i++) {
    int ii;
    CRSet& crSet = data_->slaveVars_[i];

    nodeIDs.resize(crSet.numNodes_+1);
    nodeIDs[0] = crSet.slaveNodeID_;
    fieldIDs.resize(0);
    fieldIDs.append(crSet.slaveFieldID_);

    for(ii=0; ii<crSet.numNodes_; ++ii) {
      nodeIDs[ii+1] = crSet.nodeIDs_[0][ii];
      fieldIDs.append(crSet.fieldIDs_[ii]);
    }

    idTypes.resize(crSet.numNodes_+1);
    idTypes = idTypes_[nodeTypeOffset_];

    int fieldSize = fieldDB[crSet.slaveFieldID_];
    weights.resize(0);
    for(ii=0; ii<fieldSize; ++ii) weights.append(0.0);
    weights[crSet.slaveOffset_] = -1.0;
    int offset = 0;
    for(ii=0; ii<crSet.numNodes_; ++ii) {
      fieldSize = fieldDB[crSet.fieldIDs_[ii]];
      for(int jj=0; jj<fieldSize; ++jj) {
	weights.append(crSet.weights_[offset++]);
      }
    }

    CHK_ERR( matrixGraph_->initSlaveConstraint(crSet.numNodes_+1,
					       idTypes.dataPtr(),
					       nodeIDs.dataPtr(),
					       fieldIDs.dataPtr(),
					       0,
					       crSet.slaveOffset_,
					       weights.dataPtr(),
					       crSet.values_[0]));
  }

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei_tester::loadConstraints()
{
  int i;
  for(i=0; i<data_->numCRMultSets_; ++i) {
    CRSet& crSet = data_->crMultSets_[i];

    for(int j=0; j<1; ++j) {
      CHK_ERR( linSys_->loadLagrangeConstraint(crSet.crID_,
					       crSet.weights_,
					       crSet.values_[j]) );
    }
  }

  for(i=0; i<data_->numCRPenSets_; ++i) {
    CRSet& crSet = data_->crPenSets_[i];

    for(int j=0; j<1; ++j) {
      CHK_ERR( linSys_->loadPenaltyConstraint(crSet.crID_,
					      crSet.weights_,
					      crSet.penValues_[j],
					      crSet.values_[j]) );
    }
  }

  return(0);
}

//----------------------------------------------------------------------------
void snl_fei_tester::definePattern(ElemBlock& eb, int& patternID)
{
  int i, j, numIDTypes = 1;
  numIDTypes += eb.numElemDOF_>0 ? 1 : 0;

  //find out how many nodal fields there are, total.
  feiArray<int> nodalFieldIDs;
  feiArray<int> flatFieldIDsArray;
  for(i=0; i<eb.numNodesPerElement_; ++i) {
    for(j=0; j<eb.numFieldsPerNode_[i]; ++j) {
      snl_fei::sortedListInsert(eb.nodalFieldIDs_[i][j], nodalFieldIDs);
      flatFieldIDsArray.append(eb.nodalFieldIDs_[i][j]);
    }
  }

  patternID = numPatterns_++;

  if (numIDTypes == 1 && nodalFieldIDs.length() == 1) {
    //This is a very simple pattern
    matrixGraph_->definePattern(patternID,
				eb.numNodesPerElement_,
				idTypes_[nodeTypeOffset_],
				nodalFieldIDs[0]);
  }
  else if (numIDTypes == 1) {
    feiArray<int> numFieldsPerID(eb.numNodesPerElement_);

    matrixGraph_->definePattern(patternID,
				eb.numNodesPerElement_,
				idTypes_[nodeTypeOffset_],
				eb.numFieldsPerNode_,
				flatFieldIDsArray.dataPtr());
  }
  else {
    feiArray<int> idTypes(eb.numNodesPerElement_+1);
    idTypes = idTypes_[nodeTypeOffset_];
    idTypes[idTypes.length()-1] = idTypes_[elemTypeOffset_];
    feiArray<int> numFieldsPerID(idTypes.length());
    feiArray<int> fieldIDs;
    for(i=0; i<eb.numNodesPerElement_; ++i) {
      numFieldsPerID[i] = eb.numFieldsPerNode_[i];
      for(j=0; j<eb.numFieldsPerNode_[i]; ++j) {
	fieldIDs.append(eb.nodalFieldIDs_[i][j]);
      }
    }
    numFieldsPerID[idTypes.length()-1] = eb.numElemDOF_;
    for(i=0; i<eb.numElemDOF_; ++i) {
      fieldIDs.append(eb.elemDOFFieldIDs_[i]);
    }

    matrixGraph_->definePattern(patternID,
				idTypes.length(),
				idTypes.dataPtr(),
				numFieldsPerID.dataPtr(),
				fieldIDs.dataPtr());
  }
}

//----------------------------------------------------------------------------
int snl_fei_tester::save_block_node_soln(DataReader& data, fei::Vector* vec,
				     const char* solnFileName, int numProcs,
				     int localProc, int solveCounter)
{
  (void)solveCounter;

  int numLocalNodes = vecSpace_->getNumOwnedAndSharedIDs(idTypes_[nodeTypeOffset_]);

  int* nodeList = new int[numLocalNodes];

  int checkNum = 0;
  int err = vecSpace_->getOwnedAndSharedIDs(idTypes_[nodeTypeOffset_],
				    numLocalNodes, nodeList, checkNum);
  if (err != 0) {
    ERReturn(-1);
  }

  FEI_OSTRINGSTREAM fileName;
  fileName<< solnFileName<<".node."<<solveCounter<<"."<<numProcs<<"."<<localProc;
  FEI_OFSTREAM outfile(fileName.str().c_str());

  if (!outfile || outfile.bad()) {
    FEI_CERR << "ERROR opening solution output file " << fileName << FEI_ENDL;
    return(-1);
  }

  feiArray<double> solnData;
  std::vector<int> fieldList;

  for(int i=0; i<numLocalNodes; i++) {
    int idType = idTypes_[nodeTypeOffset_];
    int ID = nodeList[i];

    int numDOF = vecSpace_->getNumDegreesOfFreedom(idType, ID);
    int numFields = vecSpace_->getNumFields(idType, ID);
    solnData.resize(numDOF);
    vecSpace_->getFields(idType, ID, fieldList);

    outfile << ID << " " << numDOF << FEI_ENDL;
    for(int j=0; j<numFields; ++j) {
      int fieldSize = vecSpace_->getFieldSize(fieldList[j]);

      CHK_ERR( vec->copyOutFieldData(fieldList[j], idType,
				     1, &ID, solnData.dataPtr()) );

      for(int k=0; k<fieldSize; ++k) {
	outfile << solnData[k] << " ";
      }
    }
    outfile << FEI_ENDL;
  }

  delete [] nodeList;

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei_tester::save_block_elem_soln(DataReader& data, fei::Vector* vec,
					 const char* solnFileName,
					 int numProcs,
					 int localProc, int solveCounter)
{
  (void)solveCounter;

  int numLocalElems = vecSpace_->getNumOwnedAndSharedIDs(idTypes_[elemTypeOffset_]);

  int* elemList = new int[numLocalElems];

  int checkNum = 0;
  int err = vecSpace_->getOwnedAndSharedIDs(idTypes_[elemTypeOffset_],
				    numLocalElems, elemList, checkNum);
  if (err != 0) {
    ERReturn(-1);
  }

  FEI_OSTRINGSTREAM fileName;
  fileName<< solnFileName<<".elem."<<solveCounter<<"."<<numProcs<<"."<<localProc;
  FEI_OFSTREAM outfile(fileName.str().c_str());

  if (!outfile || outfile.bad()) {
    FEI_CERR << "ERROR opening solution output file " << fileName << FEI_ENDL;
    return(-1);
  }

  feiArray<double> solnData;
  std::vector<int> fieldList;

  for(int i=0; i<numLocalElems; i++) {
    int idType = idTypes_[elemTypeOffset_];
    int ID = elemList[i];

    int numDOF = vecSpace_->getNumDegreesOfFreedom(idType, ID);
    int numFields = vecSpace_->getNumFields(idType, ID);
    solnData.resize(numDOF);
    vecSpace_->getFields(idType, ID, fieldList);

    outfile << ID << " " << numDOF << FEI_ENDL;
    for(int j=0; j<numFields; ++j) {
      int fieldSize = vecSpace_->getFieldSize(fieldList[j]);

      CHK_ERR( vec->copyOutFieldData(fieldList[j], idType,
				     1, &ID, solnData.dataPtr()) );

      for(int k=0; k<fieldSize; ++k) {
	outfile << solnData[k] << " ";
      }
    }
    outfile << FEI_ENDL;
  }

  delete [] elemList;

  return(0);
}

//----------------------------------------------------------------------------
int snl_fei_tester::save_multiplier_soln(DataReader& data, fei::Vector* vec,
					 const char* solnFileName,
					 int numProcs, int localProc,
					 int solveCounter)
{
  (void)solveCounter;

  int numLocalCRs = vecSpace_->getNumOwnedAndSharedIDs(idTypes_[constraintTypeOffset_]);

   int* globalNumCRs = new int[numProcs];
#ifndef FEI_SER
   if (MPI_Allgather(&numLocalCRs, 1, MPI_INT, globalNumCRs, 1, MPI_INT,
		 comm_) != MPI_SUCCESS) {
     ERReturn(-1);
   }
#endif

   int localCRStart = 0;
#ifndef FEI_SER
   for(int p=0; p<localProc; p++) localCRStart += globalNumCRs[p];
#endif

   delete [] globalNumCRs;

  std::vector<int> crList(numLocalCRs);

  int checkNum = 0;
  int err = vecSpace_->getOwnedAndSharedIDs(idTypes_[constraintTypeOffset_],
				    numLocalCRs, &crList[0], checkNum);
  if (err != 0) {
    ERReturn(-1);
  }

  FEI_OSTRINGSTREAM fileName;
  fileName<< solnFileName<<".mult."<<solveCounter<<"."<<numProcs<<"."<<localProc;
  FEI_OFSTREAM outfile(fileName.str().c_str());

  if (!outfile || outfile.bad()) {
    FEI_CERR << "ERROR opening solution output file " << fileName << FEI_ENDL;
    return(-1);
  }

  feiArray<double> solnData;
  feiArray<int> fieldList;

  for(int i=0; i<numLocalCRs; i++) {
    int idType = idTypes_[constraintTypeOffset_];
    int ID = crList[i];

    solnData.resize(1);

    outfile << localCRStart++ << " " << 1 << FEI_ENDL;
    for(int j=0; j<1; ++j) {
      int globalIndex = -1;
      CHK_ERR( vecSpace_->getGlobalIndex(idType, ID, globalIndex) );

      CHK_ERR( vec->copyOut(1, &globalIndex, solnData.dataPtr()) );

      for(int k=0; k<1; ++k) {
	outfile << solnData[k] << " ";
      }
    }
    outfile << FEI_ENDL;
  }

  return(0);
}
