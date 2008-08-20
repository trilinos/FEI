/*--------------------------------------------------------------------*/
/*    Copyright 2005 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/


#include <fei_macros.hpp>

#include <test_utils/test_MatrixGraph.hpp>

#include <test_utils/test_VectorSpace.hpp>

#include <snl_fei_Factory.hpp>
#include <fei_Pattern.hpp>
#include <fei_MatrixGraph_Impl2.hpp>
#include <fei_SparseRowGraph.hpp>
#include <fei_LibraryWrapper.hpp>

#undef fei_file
#define fei_file "test_MatrixGraph.cpp"
#include <fei_ErrMacros.hpp>

test_MatrixGraph::test_MatrixGraph(MPI_Comm comm)
 : tester(comm)
{
}

test_MatrixGraph::~test_MatrixGraph()
{
}

int test_MatrixGraph_test6(MPI_Comm comm, int numProcs, int localProc,
                           const std::string& path)
{
  testData* testdata = new testData(localProc, numProcs);
  std::vector<int>& ids = testdata->ids;

  fei::SharedPtr<LibraryWrapper> wrapper;
  fei::SharedPtr<fei::Factory> factory(new snl_fei::Factory(comm, wrapper));

  fei::SharedPtr<fei::VectorSpace> vectorSpacePtr =
    test_VectorSpace::create_VectorSpace(comm,
					 testdata, localProc, numProcs,
					 false, //defineBothFields
					 false, //initSolnBothFields
					 "U_MatGrph", factory);

  fei::SharedPtr<fei::MatrixGraph> matrixGraphPtr =
    test_MatrixGraph::create_MatrixGraph(testdata, localProc, numProcs,
					 false, false, "U_MatGrph",
					 vectorSpacePtr, factory, path);

  CHK_ERR( matrixGraphPtr->initComplete() );

  fei::SharedPtr<fei::MatrixGraph> matrixGraph2Ptr =
    test_MatrixGraph::create_MatrixGraph(testdata, localProc, numProcs,
					 false, false, "U_MatGrph2",
					 vectorSpacePtr, factory, path);

  CHK_ERR( matrixGraph2Ptr->initComplete() );

  bool equivalent = false;
  CHK_ERR( matrixGraphPtr->compareStructure(*matrixGraph2Ptr, equivalent) );

  if (!equivalent) {
    ERReturn(-1);
  }

  fei::SharedPtr<fei::MatrixGraph> matrixGraph3Ptr =
    test_MatrixGraph::create_MatrixGraph(testdata, localProc, numProcs,
					 false, false, "U_MatGrph3",
					 vectorSpacePtr, factory, path);

  if (localProc == 0) {
    std::vector<int>& fieldIDs = testdata->fieldIDs;
    std::vector<int>& idTypes = testdata->idTypes;
    int offsetOfSlave = 0;
    int offsetIntoSlaveField = 0;
    feiArray<double> weights(2);
    weights = 1.0;
    double rhsValue = 0.0;
    feiArray<int> cr_idTypes(2);
    cr_idTypes = idTypes[0];
    feiArray<int> cr_fieldIDs(2);
    cr_fieldIDs = fieldIDs[0];

    CHK_ERR( matrixGraph3Ptr->initSlaveConstraint(2,
					       cr_idTypes.dataPtr(),
					       &ids[2],
					       cr_fieldIDs.dataPtr(),
					       offsetOfSlave,
					       offsetIntoSlaveField,
					       weights.dataPtr(),
					       rhsValue) );
  }

  CHK_ERR( matrixGraph3Ptr->initComplete() );

  CHK_ERR( matrixGraphPtr->compareStructure(*matrixGraph3Ptr, equivalent) );

  if (equivalent) {
    ERReturn(-1);
  }

  delete testdata;

  return(0);
}

void test_MatrixGraph_test7(MPI_Comm comm, int numProcs, int localProc)
{
  fei::SharedPtr<fei::VectorSpace> rowspace(new fei::VectorSpace(comm));
  fei::SharedPtr<fei::VectorSpace> colspace(new fei::VectorSpace(comm));

  int rowfield = 0, rowfieldsize = 1;
  int colfield = 1, colfieldsize = 3;
  rowspace->defineFields(1, &rowfield, &rowfieldsize);
  colspace->defineFields(1, &colfield, &colfieldsize);

  fei::MatrixGraph_Impl2 mgraph(rowspace, colspace);

  mgraph.definePattern(0, 4, 0, colfield);
  fei::Pattern* pattern = mgraph.getPattern(0);

  if (pattern->getNumIndices() != 4*colfieldsize) {
    FEI_COUT << "getNumIndices: " << pattern->getNumIndices()<<", colfieldsize: " << colfieldsize<<FEI_ENDL;
    FEI_OSTRINGSTREAM osstr;
    osstr << "test_MatrixGraph_test7, line "<<__LINE__<<FEI_ENDL;
    throw std::runtime_error(osstr.str());
  }
}

void test_MatrixGraph_test8(MPI_Comm comm, int numProcs, int localProc)
{
  FEI_COUT << "testing matrix-graph with 'diagonal' connectivity block...";

  try {

  fei::SharedPtr<fei::VectorSpace> rowspace(new fei::VectorSpace(comm));
  fei::SharedPtr<fei::VectorSpace> colspace;

  int rowfield = 0, rowfieldsize = 1;
  rowspace->defineFields(1, &rowfield, &rowfieldsize);
  int idType = 0;
  rowspace->defineIDTypes(1, &idType);

  fei::MatrixGraph_Impl2 mgraph(rowspace, colspace);

  int numIDs = 4;
  int patternID = 0;
  mgraph.definePattern(patternID, numIDs, idType, rowfield);
  fei::Pattern* pattern = mgraph.getPattern(patternID);

  if (pattern->getNumIndices() != 4*rowfieldsize) {
    FEI_OSTRINGSTREAM osstr;
    osstr << "test_MatrixGraph_test8, line "<<__LINE__<<FEI_ENDL;
    throw std::runtime_error(osstr.str());
  }

  int blockID = 0;
  int numConnLists = 1;
  bool diagonal = true;
  mgraph.initConnectivityBlock(blockID, numConnLists, patternID, diagonal);

  feiArray<int> ids(numIDs);
  for(int i=0; i<numIDs; ++i) {
    ids[i] = i;
  }

  mgraph.initConnectivity(blockID, 0, ids.dataPtr());

  mgraph.initComplete();

  fei::SharedPtr<fei::SparseRowGraph> localSRGraph =
    mgraph.createGraph(false);

  if ((int)localSRGraph->packedColumnIndices.size() != numIDs) {
    FEI_OSTRINGSTREAM osstr;
    osstr << "test_MatrixGraph_test8, line "<<__LINE__<<FEI_ENDL;
    throw std::runtime_error(osstr.str());
  }

  }
  catch(std::runtime_error& exc) {
    FEI_OSTRINGSTREAM osstr;
    osstr << "test_MatrixGraph_test8, caught exception: " << exc.what();
    throw std::runtime_error(osstr.str());
  }

  FEI_COUT << "ok" << FEI_ENDL;
}

int test_MatrixGraph::runtests()
{
  if (numProcs_ < 2) {
    CHK_ERR( serialtest1() );
  }

  CHK_ERR( test1() );
  CHK_ERR( test2() );
  CHK_ERR( test3() );
  CHK_ERR( test4() );
  CHK_ERR( test5() );

  CHK_ERR( test_MatrixGraph_test6(comm_, numProcs_, localProc_, path_) );

  test_MatrixGraph_test7(comm_, numProcs_, localProc_);
  test_MatrixGraph_test8(comm_, numProcs_, localProc_);

  return(0);
}

int test_MatrixGraph::serialtest1()
{
  int i, numIDs = 2;
  feiArray<int> idTypes(numIDs);
  feiArray<int> numFieldsPerID(numIDs);
  feiArray<int> fieldIDs(numIDs);
  feiArray<int> fieldSizes(numIDs);

  for(i=0; i<numIDs; ++i) {
    idTypes[i] = 1;
    numFieldsPerID[i] = 1;
    fieldIDs[i] = 0;
    fieldSizes[i] = 1;
  }

  fei::Pattern pattern(0, numIDs, idTypes.dataPtr(),
			   numFieldsPerID.dataPtr(), fieldIDs.dataPtr(),
			   fieldSizes.dataPtr());

  fei::Pattern::PatternType pType = pattern.getPatternType();

  if (pType != fei::Pattern::SIMPLE) {
    ERReturn(-1);
  }

  return(0);
}

int test_MatrixGraph::test1()
{
  testData* testdata = new testData(localProc_, numProcs_);
  std::vector<int>& ids = testdata->ids;

  fei::SharedPtr<LibraryWrapper> wrapper;
  fei::SharedPtr<fei::Factory> factory(new snl_fei::Factory(comm_, wrapper));

  fei::SharedPtr<fei::VectorSpace> vectorSpacePtr =
    test_VectorSpace::create_VectorSpace(comm_,
					 testdata, localProc_, numProcs_,
					 false, //defineBothFields
					 false, //initSolnBothFields
					 "U_MatGrph", factory);

  int dofPerID = 1;

  fei::SharedPtr<fei::MatrixGraph> matrixGraphPtr =
    create_MatrixGraph(testdata, localProc_, numProcs_,
		       false, false, "U_MatGrph", vectorSpacePtr, factory, path_);

  fei::VectorSpace& vectorSpace = *vectorSpacePtr;

  CHK_ERR( matrixGraphPtr->initComplete() );

  feiArray<int> globalIndexOffsets(numProcs_+1);

  CHK_ERR( vectorSpace.getGlobalIndexOffsets(numProcs_+1,
					   globalIndexOffsets.dataPtr()));

  int numRowLengths = globalIndexOffsets[localProc_+1] -
     globalIndexOffsets[localProc_];

  int numLocalRows;

  fei::SharedPtr<fei::SparseRowGraph> localgraph =
    matrixGraphPtr->createGraph(false);

  std::vector<int>& rowOffsets = localgraph->rowOffsets;
  numLocalRows = rowOffsets.size()-1;

  if (numLocalRows != numRowLengths) ERReturn(-1);

  int correctNumLocalRows = localProc_==0 ? ids.size() : ids.size()-2;
  correctNumLocalRows *= dofPerID;

  if (numLocalRows != correctNumLocalRows) {
    ERReturn(-1);
  }

  int numNonzeros = localgraph->packedColumnIndices.size();

  int correctNumNonzeros = numLocalRows*ids.size()*dofPerID;
  if (localProc_ < numProcs_-1) correctNumNonzeros += 4*dofPerID;

  if (numNonzeros != correctNumNonzeros) {
    ERReturn(-1);
  }

  std::vector<int>& nonzeros = localgraph->packedColumnIndices;

  int offset = 0;
  for(int i=0; i<numLocalRows; ++i) {
    int globalRow = globalIndexOffsets[localProc_]+i;
    int globalEndRow = globalIndexOffsets[numProcs_]-1;

    int correctRowLength = 4*dofPerID;
    if (globalRow > 2*dofPerID-1 && globalRow < globalEndRow-(2*dofPerID-1)) {
      correctRowLength += 2*dofPerID;
    }

    if (rowOffsets[i+1]-rowOffsets[i] != correctRowLength) {
      FEI_CERR << "localProc " << localProc_ << ", i: " << i
	   << ", correctRowLength: " << correctRowLength << ", "
	   << "rowOffsets[i+1]-rowOffsets[i]: " << rowOffsets[i+1]-rowOffsets[i] << FEI_ENDL;
      ERReturn(-1);
    }

    for(int j=0; j<rowOffsets[i+1]-rowOffsets[i]; ++j) {
      if (nonzeros[offset++] != ids[0]+j) {
	ERReturn(-1);
      }
    }
  }

  delete testdata;

  return(0);
}

int test_MatrixGraph::test2()
{
  return(0);
}

int init_nonsymmetric_block(testData* testdata,
			    fei::MatrixGraph* matrixGraph)
{
  int rowPatternID = 5, colPatternID = 6;
  matrixGraph->definePattern(rowPatternID, 1, 0,
			     testdata->fieldIDs[0]);
  matrixGraph->definePattern(colPatternID, 1, 0,
			     testdata->fieldIDs[1]);

  CHK_ERR( matrixGraph->initConnectivityBlock(2, 1, rowPatternID, colPatternID) );

  CHK_ERR( matrixGraph->initConnectivity(2, 0,
					 &(testdata->ids[0]),
					 &(testdata->ids[0])) );
  return(0);
}

int test_MatrixGraph::test3()
{
  testData* testdata = new testData(localProc_, numProcs_);
  std::vector<int>& ids = testdata->ids;

  fei::SharedPtr<LibraryWrapper> wrapper;
  fei::SharedPtr<fei::Factory> factory(new snl_fei::Factory(comm_, wrapper));

  fei::SharedPtr<fei::VectorSpace> vectorSpacePtr =
    test_VectorSpace::create_VectorSpace(comm_,
					 testdata, localProc_, numProcs_,
					 true, //defineBothFields
					 true, //initSolnBothFields
					 "U_MatGrph3", factory);

  int dofPerID = 4;

  fei::SharedPtr<fei::MatrixGraph> matrixGraphPtr =
    create_MatrixGraph(testdata, localProc_, numProcs_,
		       true, true, //non-symmetric
		       "U_MatGrph3", vectorSpacePtr,
		       factory, path_);

  fei::VectorSpace& vectorSpace = *vectorSpacePtr;

  CHK_ERR( init_nonsymmetric_block(testdata, matrixGraphPtr.get()) );

  CHK_ERR( matrixGraphPtr->initComplete() );

  feiArray<int> globalIndexOffsets(numProcs_+1);

  CHK_ERR( vectorSpace.getGlobalIndexOffsets(numProcs_+1,
					   globalIndexOffsets.dataPtr()));

  int numRowLengths = globalIndexOffsets[localProc_+1] -
     globalIndexOffsets[localProc_];

  int numLocalRows;

  fei::SharedPtr<fei::SparseRowGraph> localgraph =
    matrixGraphPtr->createGraph(false);

  int numGrphLocalRows = localgraph->rowNumbers.size();

  std::vector<int>& rowOffsets = localgraph->rowOffsets;
  numLocalRows = rowOffsets.size()-1;

  int correctNumLocalRows = localProc_==0 ? ids.size() : ids.size()-2;
  correctNumLocalRows *= dofPerID;

  if (numLocalRows != correctNumLocalRows) {
    ERReturn(-1);
  }

  if (numLocalRows != numRowLengths) ERReturn(-1);
  if (numLocalRows != numGrphLocalRows) ERReturn(-1);

  int numNonzeros = localgraph->packedColumnIndices.size();

  int correctNumNonzeros = numLocalRows*ids.size()*dofPerID;
  if (localProc_ < numProcs_-1) correctNumNonzeros += 4*dofPerID*dofPerID;

  if (numNonzeros != correctNumNonzeros) {
    ERReturn(-1);
  }

  std::vector<int>& nonzeros = localgraph->packedColumnIndices;

  feiArray<int> rowindices;
  int offset = 0;
  for(int i=0; i<numLocalRows; ++i) {
    int globalRow = globalIndexOffsets[localProc_]+i;
    int globalEndRow = globalIndexOffsets[numProcs_]-1;

    int correctRowLength = 4*dofPerID;
    if (globalRow > 2*dofPerID-1 && globalRow < globalEndRow-(2*dofPerID-1)) {
      correctRowLength += 2*dofPerID;
    }

    if (rowOffsets[i+1]-rowOffsets[i] != correctRowLength) {
      FEI_CERR << "localProc " << localProc_ << ", i: " << i
	   << ", correctRowLength: " << correctRowLength << ", "
	   << "rowOffsets[i+1]-rowOffsets[i]: " << rowOffsets[i+1]-rowOffsets[i] << FEI_ENDL;
      ERReturn(-1);
    }

    for(int j=0; j<rowOffsets[i+1]-rowOffsets[i]; ++j) {
      if (nonzeros[offset++] != ids[0]*dofPerID+j) {
	ERReturn(-1);
      }
    }
  }

  delete testdata;

  return(0);
}

int test_MatrixGraph::test4()
{
  testData* testdata = new testData(localProc_, numProcs_);
  std::vector<int>& ids = testdata->ids;

  fei::SharedPtr<LibraryWrapper> wrapper;
  fei::SharedPtr<fei::Factory> factory(new snl_fei::Factory(comm_, wrapper));

  fei::SharedPtr<fei::VectorSpace> vectorSpacePtr =
    test_VectorSpace::create_VectorSpace(comm_,
                                         testdata, localProc_, numProcs_,
					 false, //defineBothFields
					 false, //initSolnBothFields
					 "U_MatGrph4", factory);

  int dofPerID = 1;

  fei::SharedPtr<fei::MatrixGraph> matrixGraphPtr =
    create_MatrixGraph(testdata, localProc_, numProcs_,
		       false, true, //non-symmetric
		       "U_MatGrph4", vectorSpacePtr, factory, path_);

  fei::VectorSpace& vectorSpace = *vectorSpacePtr;

  CHK_ERR( matrixGraphPtr->initComplete() );

  feiArray<int> globalIndexOffsets(numProcs_+1);

  CHK_ERR( vectorSpace.getGlobalIndexOffsets(numProcs_+1,
					   globalIndexOffsets.dataPtr()));

  int numRowLengths = globalIndexOffsets[localProc_+1] -
     globalIndexOffsets[localProc_];

  int numLocalRows;

  fei::SharedPtr<fei::SparseRowGraph> localgraph =
    matrixGraphPtr->createGraph(false);

  std::vector<int>& rowOffsets = localgraph->rowOffsets;
  numLocalRows = rowOffsets.size()-1;

  int correctNumLocalRows = localProc_==0 ? ids.size() : ids.size()-2;
  correctNumLocalRows *= dofPerID;

  if (numLocalRows != correctNumLocalRows) {
    ERReturn(-1);
  }

  if (numLocalRows != numRowLengths) ERReturn(-1);

  int numNonzeros = localgraph->packedColumnIndices.size();

  int correctNumNonzeros = numLocalRows*ids.size()*dofPerID;
  if (localProc_ < numProcs_-1) correctNumNonzeros += 4*dofPerID*dofPerID;

  if (numNonzeros != correctNumNonzeros) {
    ERReturn(-1);
  }

  std::vector<int>& nonzeros = localgraph->packedColumnIndices;

  int offset = 0;
  for(int i=0; i<numLocalRows; ++i) {
    int globalRow = globalIndexOffsets[localProc_]+i;
    int globalEndRow = globalIndexOffsets[numProcs_]-1;

    int correctRowLength = 4*dofPerID;
    if (globalRow > 2*dofPerID-1 && globalRow < globalEndRow-(2*dofPerID-1)) {
      correctRowLength += 2*dofPerID;
    }

    if (rowOffsets[i+1]-rowOffsets[i] != correctRowLength) {
      FEI_CERR << "localProc " << localProc_ << ", i: " << i
	   << ", correctRowLength: " << correctRowLength << ", "
	   << "rowOffsets[i+1]-rowOffsets[i]: " << rowOffsets[i+1]-rowOffsets[i] << FEI_ENDL;
      ERReturn(-1);
    }

    for(int j=0; j<rowOffsets[i+1]-rowOffsets[i]; ++j) {
      if (nonzeros[offset++] != ids[0]*dofPerID+j) {
	ERReturn(-1);
      }
    }
  }

  delete testdata;

  return(0);
}

int test_MatrixGraph::test5()
{
  testData* testdata = new testData(localProc_, numProcs_);
  std::vector<int>& fieldIDs = testdata->fieldIDs;
  std::vector<int>& idTypes = testdata->idTypes;
  std::vector<int>& ids = testdata->ids;

  fei::SharedPtr<LibraryWrapper> wrapper;
  fei::SharedPtr<fei::Factory> factory(new snl_fei::Factory(comm_, wrapper));

  fei::SharedPtr<fei::VectorSpace> vectorSpacePtr =
    test_VectorSpace::create_VectorSpace(comm_,
                                         testdata, localProc_, numProcs_,
					 true, //defineBothFields
					 true, //initSolnBothFields
					 "U_MatGrph5",
					 factory,true);

  fei::SharedPtr<fei::MatrixGraph> matrixGraphPtr =
    create_MatrixGraph(testdata, localProc_, numProcs_,
		       true, false, "U_MatGrph5", vectorSpacePtr,
		       factory, path_, true);

  if (localProc_ == 0) {
    int offsetOfSlave = 0;
    int offsetIntoSlaveField = 0;
    feiArray<double> weights(6);
    weights = 0.0;
    weights[3] = 1.0;
    double rhsValue = 0.0;
    feiArray<int> cr_idTypes(2);
    cr_idTypes = idTypes[0];
    feiArray<int> cr_fieldIDs(2);
    cr_fieldIDs = fieldIDs[1];

    CHK_ERR( matrixGraphPtr->initSlaveConstraint(2,
					       cr_idTypes.dataPtr(),
					       &ids[2],
					       cr_fieldIDs.dataPtr(),
					       offsetOfSlave,
					       offsetIntoSlaveField,
					       weights.dataPtr(),
					       rhsValue) );

    weights[3] = 0.0;
    weights[4] = 1.0;
    offsetIntoSlaveField = 1;
    CHK_ERR( matrixGraphPtr->initSlaveConstraint(2,
					       cr_idTypes.dataPtr(),
					       &ids[2],
					       cr_fieldIDs.dataPtr(),
					       offsetOfSlave,
					       offsetIntoSlaveField,
					       weights.dataPtr(),
					       rhsValue) );
  }

  CHK_ERR( matrixGraphPtr->initComplete() );

  fei::SharedPtr<fei::VectorSpace> reducedSolnSpacePtr =
    matrixGraphPtr->getRowSpace();

  feiArray<int> globalIndexOffsets(numProcs_+1);

  CHK_ERR( reducedSolnSpacePtr->getGlobalIndexOffsets(numProcs_+1,
					   globalIndexOffsets.dataPtr()));

  int numRows_unreduced = globalIndexOffsets[localProc_+1] -
     globalIndexOffsets[localProc_];

  fei::SharedPtr<fei::SparseRowGraph> localgraph =
    matrixGraphPtr->createGraph(false);

  std::vector<int>& rowOffsets = localgraph->rowOffsets;
  int numReducedRows = rowOffsets.size()-1;

  if (localProc_ == 0) {
    if (numReducedRows != numRows_unreduced-2) ERReturn(-1);
  }
  else {
    if (numReducedRows != numRows_unreduced) ERReturn(-1);
  }

//  int numNonzeros = localgraph->packedColumnIndices.length();

//  feiArray<int>& nonzeros = localgraph->packedColumnIndices;
//
//  int offset = 0;
//  for(int i=0; i<numLocalRows; ++i) {
//    FEI_COUT << localProc_ << " test_MatrixGraph5: row "
//	 << i+globalIndexOffsets[localProc_] << ", cols: ";
//    for(int j=0; j<rowOffsets[i+1]-rowOffsets[i]; ++j) {
//      FEI_COUT << nonzeros[offset++] << " ";
//    }
//    FEI_COUT << FEI_ENDL;
//  }

  delete testdata;

  return(0);
}

fei::SharedPtr<fei::MatrixGraph> test_MatrixGraph::create_MatrixGraph(testData* testdata,
					 int localProc, int numProcs,
					 bool bothFields, bool nonSymmetric,
					 const char* name,
					 fei::SharedPtr<fei::VectorSpace> vectorSpacePtr,
					 fei::SharedPtr<fei::Factory> factory,
                                         const std::string& path,
					 bool turnOnDebugOutput)
{
  //
  //This function creates a MatrixGraph object, and initializes it as follows:
  //
  //setRowSpace(vectorSpacePtr)
  //
  //definePattern patternID=0, numIDs=4, idType=testdata->idTypes[0]
  //      fieldID=testdata->fieldIDs[0] if !bothFields, else
  //      fieldIDs=testdata->fieldIDs
  //
  //initConnectivityBlock blockID=0, numConnectivityLists=1
  //
  //initConnectivity blockID, 0, testdata->ids
  //
  //If nonSymmetric==true, then also do the following:
  //  definePattern patternID=1, numIDs=1, idType=testdata->idTypes[0]
  //     fieldID=testdata->fieldIDs[0] if !bothFields, else
  //      fieldIDs=testdata->fieldIDs
  //  definePattern patternID=2, numIDs=4, idType=testdata->idTypes[0]
  //     fieldID=testdata->fieldIDs[0] if !bothFields, else
  //      fieldIDs=testdata->fieldIDs
  //
  //initConnectivityBlock blockID=1, patterns 1 and 2
  //
  //initConnectivity blockID, 0, testdata->ids
  //
  fei::SharedPtr<fei::MatrixGraph> mgptr;
  if (factory.get() == NULL) {
    fei::SharedPtr<fei::MatrixGraph> tmp(new fei::MatrixGraph_Impl2(vectorSpacePtr,
							      vectorSpacePtr, name));
    mgptr = tmp;
  }
  else {
    mgptr = factory->createMatrixGraph(vectorSpacePtr, vectorSpacePtr, name);
  }

  fei::ParameterSet paramset;
  fei::Param param1("name", name);
  paramset.add(param1);
  if (turnOnDebugOutput) {
    if (path.empty()) {
      fei::Param param2("debugOutput", ".");
      paramset.add(param2);
    }
    else {
      fei::Param param2("debugOutput", path.c_str());
      paramset.add(param2);
    }
  }

  fei::MatrixGraph* matrixGraphPtr = mgptr.get();

  matrixGraphPtr->setParameters(paramset);

  matrixGraphPtr->setRowSpace(vectorSpacePtr);

  int patternID = 0;
  int numIDs = 4;
  int idType = testdata->idTypes[0];
  int fieldID = testdata->fieldIDs[0];

  if (bothFields) {
    feiArray<int> numFieldsPerID(numIDs);
    numFieldsPerID = 2;
    feiArray<int> fieldIDsArray(numIDs*2);
    for(int i=0; i<numIDs; ++i) {
      fieldIDsArray[i*2] = testdata->fieldIDs[0];
      fieldIDsArray[i*2+1] = testdata->fieldIDs[1];
    }

    matrixGraphPtr->definePattern(patternID, numIDs, idType,
					 numFieldsPerID.dataPtr(),
					 fieldIDsArray.dataPtr());
  }
  else {
    matrixGraphPtr->definePattern(patternID, numIDs, idType, fieldID);
  }

  int blockID = 0;
  int numConnectivityLists = 1;

  matrixGraphPtr->initConnectivityBlock(blockID,
					   numConnectivityLists,
					   patternID);

  matrixGraphPtr->initConnectivity(blockID, 0, &(testdata->ids[0]));

  if (!nonSymmetric) {
    return(mgptr);
  }

  int patternID1 = 1, patternID2 = 2;
  int numRowIDs = 1, numColIDs = 4;

  if (bothFields) {
    feiArray<int> numFieldsPerID(numIDs);
    numFieldsPerID = 2;
    feiArray<int> fieldIDsArray(numIDs*2);
    for(int i=0; i<numIDs; ++i) {
      fieldIDsArray[i*2] = testdata->fieldIDs[0];
      fieldIDsArray[i*2+1] = testdata->fieldIDs[1];
    }

    matrixGraphPtr->definePattern(patternID1, numRowIDs, idType,
					 numFieldsPerID.dataPtr(),
					 fieldIDsArray.dataPtr());
    matrixGraphPtr->definePattern(patternID2, numColIDs, idType,
					 numFieldsPerID.dataPtr(),
					 fieldIDsArray.dataPtr());
  }
  else {
    matrixGraphPtr->definePattern(patternID1, numRowIDs,
					   idType, fieldID);
    matrixGraphPtr->definePattern(patternID2, numColIDs,
					   idType, fieldID);
  }

  blockID = 1;

  matrixGraphPtr->initConnectivityBlock(blockID,
					   numConnectivityLists,
					   patternID1, patternID2);

  matrixGraphPtr->initConnectivity(blockID, 0,
					  &(testdata->ids[0]),
					  &(testdata->ids[0]));

  return(mgptr);
}
