/*--------------------------------------------------------------------*/
/*    Copyright 2005 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#include <fei_macros.hpp>

#include <test_utils/test_Utils.hpp>

#include <feiArray.hpp>
#include <snl_fei_ArrayUtils.hpp>
#include <fei_utils.hpp>
#include <fei_CommUtilsBase.hpp>
#include <snl_fei_Utils.hpp>
#include <fei_BCRecord.hpp>
#include <fei_Param.hpp>
#include <fei_ParameterSet.hpp>
#include <fei_Exception.hpp>
#include <fei_SharedPtr.hpp>
#include <fei_SSVec.hpp>
#include <fei_SSMat.hpp>
#include <cmath>

#undef fei_file
#define fei_file "test_Utils.cpp"
#include <fei_ErrMacros.hpp>

test_Utils::test_Utils(MPI_Comm comm)
  : tester(comm)
{
}

test_Utils::~test_Utils()
{
}

void test_Utils_packSSMat()
{
  FEI_COUT << "testing snl_fei::packSSMat/unpackSSMat...";
  SSMat mat0;
  SSMat mat01;

  unsigned nnz = 0;
  feiArray<int> indices;
  feiArray<double> coefs;
  for(unsigned i=0; i<5; ++i) {
    unsigned row = i;
    unsigned rowlen = i+1;
    nnz += rowlen;
    for(unsigned j=0; j<rowlen; ++j) {
      unsigned col = j;
      double coef = 1.0*(i+j+1);
      mat0.putCoef(row, col, coef);
      indices.append(col);
      coefs.append(coef);
    }
    mat01.putRow(row, indices.dataPtr(), coefs.dataPtr(), indices.length());
    indices.resize(0);
    coefs.resize(0);
  }

  if (mat0 != mat01) {
    throw fei::Exception("snl_fei::packSSMat test failed assembling mat...");
  }

  std::vector<int> intdata0;
  std::vector<double> doubledata0;

  snl_fei::packSSMat(mat0, intdata0, doubledata0);

  if (nnz != doubledata0.size()) {
    throw fei::Exception("snl_fei::packSSMat test failed");
  }

  SSMat mat1;
  snl_fei::unpackIntoSSMat(intdata0, doubledata0, mat1);

  if (mat1.getRowNumbers() != mat0.getRowNumbers()) {
    throw fei::Exception("snl_fei::packSSMat test failed");
  }

  std::vector<int> intdata1;
  std::vector<double> doubledata1;
  snl_fei::packSSMat(mat1, intdata1, doubledata1);

  if (intdata1 != intdata0 || doubledata1 != doubledata0) {
    throw fei::Exception("snl_fei::packSSMat test failed");
  }

  FEI_COUT << "ok"<<FEI_ENDL;
}

void test_Utils_globalUnionVec()
{
  FEI_COUT << "testing snl_fei::globalUnion(SSVec)...";

  fei::CommUtilsBase commUtilsBase(MPI_COMM_WORLD);

  int numProcs = commUtilsBase.numProcs();
  int localProc = commUtilsBase.localProc();

  int numlocalrows = 5;

  SSVec globalvec0;
  SSVec localvec;
  int row=0;
  for(int p=0; p<numProcs; ++p) {
    for(int i=0; i<numlocalrows; ++i) {
      globalvec0.putEntry(row, 1.0);
      if (p == localProc) {
	localvec.putEntry(row, 1.0);
      }
    }
    ++row;
  }

  SSVec globalvec;

  snl_fei::globalUnion(commUtilsBase.getCommunicator(), localvec, globalvec);

  if (globalvec.indices() != globalvec0.indices()) {
    throw fei::Exception("globalUnion vec int test failed");
  }

  if (globalvec.coefs() != globalvec0.coefs()) {
    throw fei::Exception("globalUnion vec double test failed");
  }

  FEI_COUT << "ok"<<FEI_ENDL;
}

void test_Utils_globalUnionMat()
{
  FEI_COUT << "testing snl_fei::globalUnion(SSMat)...";

  fei::CommUtilsBase commUtilsBase(MPI_COMM_WORLD);

  int numProcs = commUtilsBase.numProcs();
  int localProc = commUtilsBase.localProc();

  int numlocalrows = 5;
  int rowlen = 5;

  SSMat globalmat0;
  SSMat localmat;
  int row=0;
  for(int p=0; p<numProcs; ++p) {
    for(int i=0; i<numlocalrows; ++i) {
      for(int j=0; j<rowlen; ++j) {
	globalmat0.putCoef(row, j, 1.0);
	if (p == localProc) {
	  localmat.putCoef(row, j, 1.0);
	}
      }
      ++row;
    }
  }

  SSMat globalmat;

  snl_fei::globalUnion(commUtilsBase.getCommunicator(), localmat, globalmat);

  std::vector<int> intdata;
  std::vector<double> doubledata;

  snl_fei::packSSMat(globalmat, intdata, doubledata);

  std::vector<int> intdata0;
  std::vector<double> doubledata0;

  snl_fei::packSSMat(globalmat0, intdata0, doubledata0);

  if (intdata0 != intdata) {
    throw fei::Exception("globalUnion test (int) failed");
  }

  if (doubledata0 != doubledata) {
    throw fei::Exception("globalUnion test (double) failed");
  }

  FEI_COUT << "ok"<<FEI_ENDL;
}

void test_Utils_removeCouplings()
{
  FEI_COUT << "testing snl_fei::removeCouplings...";

  SSMat mat;

  mat.putCoef(2, 0, 0.5);
  mat.putCoef(2, 10, 0.5);
  mat.putCoef(8, 2, 0.5);
  mat.putCoef(8, 10, 0.5);

  int levels = snl_fei::removeCouplings(mat);
  if (levels < 1) {
    throw fei::Exception("removeCouplings test failed");
  }

  //after remove-couplings, the matrix-row for 8 should have
  //2 column-indices, and they should be 0 and 10. Also, the
  //coefficients should be 0.25 and 0.75.
  SSVec* matrow = mat.getRow(8);
  if (matrow==NULL) {
    throw fei::Exception("error getting matrix row 8");
  }

  if (matrow->length() != 2) {
    throw fei::Exception("matrow 8 has wrong length");
  }

  feiArray<int>& indices = matrow->indices();
  feiArray<double>& coefs = matrow->coefs();
  if (indices[0] != 0 || indices[1] != 10 ||
      std::abs(coefs[0] -0.25) > 1.e-49 || std::abs(coefs[1] -0.75) > 1.e-49) {
    throw fei::Exception("matrow 8 has wrong contents after removeCouplings");
  }

  levels = snl_fei::removeCouplings(mat);
  if (levels > 0) {
    throw fei::Exception("removeCouplings test2 failed");
  }

  SSMat D;

  D.putCoef(2, 0, 0.5); D.putCoef(2, 1, 0.5);
  D.putCoef(3, 2, 0.25); D.putCoef(3, 4, 0.25);
  D.putCoef(3, 1, 0.25); D.putCoef(3, 6, 0.25);
  D.putCoef(5, 0, 0.5); D.putCoef(5, 1, 0.5);

  FEI_COUT << "D: " << FEI_ENDL << D << FEI_ENDL;

  levels = snl_fei::removeCouplings(D);

  FEI_COUT << "D after removeCouplings: "<<FEI_ENDL << D << FEI_ENDL;

  FEI_COUT <<"ok"<<FEI_ENDL;
}

void test_Utils_feiArray()
{
  FEI_COUT << "testing feiArray basic functionality...";

  feiArray<int> array;
  array.reAllocate(1000);
  if (array.allocatedLength() != 1000) {
    throw fei::Exception("feiArray::reAllocate test failed.");
  }
  array.reAllocate(0);
  feiArray<int> array2;

  int i, len = 4;
  for(i=0; i<len; ++i) {
    snl_fei::sortedListInsert(i, array);
    snl_fei::sortedListInsert(i+len, array);

    if (i>0) snl_fei::sortedListInsert(-i, array);
  }

  if (array.length() != 3*len-1) {
    throw fei::Exception("feiArray::insert test failed.");
  }

  if (array[0] != -(len-1)) {
    throw fei::Exception("feiArray::insert test 2 failed.");
  }

  if (array[array.length()-1] != 2*len-1) {
    throw fei::Exception("feiArray::insert test 3 failed.");
  }

  feiArray<int> arrayView;
  arrayView.setInternalData(array.length(), array.length(),
                                     array.dataPtr());
  if (arrayView != array) {
    throw fei::Exception("feiArray::setInternalData test failed.");
  }

  int find_index = array.find(-999);
  if (find_index >= 0) {
    throw fei::Exception("feiArray::find test failed.");
  }
  FEI_COUT << "ok"<<FEI_ENDL;
}

void test_Utils_binarySearch()
{
  feiArray<int> intarray;
  intarray.append(1);
  intarray.append(2);
  intarray.append(5);
  intarray.append(6);
  intarray.append(9);

  int offset = 0;
  int insertPoint = -1;

  FEI_COUT << "testing correctness of snl_fei::binarySearch(int,int*,int,int)...";

  offset = snl_fei::binarySearch(0, intarray.dataPtr(), intarray.length(),
				 insertPoint);
  if (offset != -1 || insertPoint != 0) {
    throw fei::Exception("snl_fei::binarySearch test failed 1.");
  }

  offset = snl_fei::binarySearch(2, intarray.dataPtr(), intarray.length(),
				 insertPoint);
  if (offset != 1) {
    throw fei::Exception("snl_fei::binarySearch test failed 2.");
  }

  offset = snl_fei::binarySearch(3, intarray.dataPtr(), intarray.length(),
				 insertPoint);
  if (offset != -1 || insertPoint != 2) {
    throw fei::Exception("snl_fei::binarySearch test failed 3.");
  }

  offset = snl_fei::binarySearch(4, intarray.dataPtr(), intarray.length(),
				 insertPoint);
  if (offset != -1 || insertPoint != 2) {
    throw fei::Exception("snl_fei::binarySearch test failed 4.");
  }

  offset = snl_fei::binarySearch(9, intarray.dataPtr(), intarray.length(),
				 insertPoint);
  if (offset != 4) {
    throw fei::Exception("snl_fei::binarySearch test failed 5.");
  }

  offset = snl_fei::binarySearch(8, intarray.dataPtr(), intarray.length(),
				 insertPoint);
  if (offset != -1 || insertPoint != 4) {
    throw fei::Exception("snl_fei::binarySearch test failed 6.");
  }

  offset = snl_fei::binarySearch(10, intarray.dataPtr(), intarray.length(),
				 insertPoint);
  if (offset != -1 || insertPoint != 5) {
    throw fei::Exception("snl_fei::binarySearch test failed 7.");
  }

  FEI_COUT << "ok"<<FEI_ENDL;
}

int test_Utils::runtests()
{
  if (numProcs_ < 2) {
    test_Utils_feiArray();
    test_Utils_binarySearch();
    test_Utils_packSSMat();
    test_Utils_removeCouplings();

    CHK_ERR( serialtest0() );
    CHK_ERR( serialtest1() );
    CHK_ERR( serialtest2() );
    CHK_ERR( serialtest3() );
  }

  test_Utils_globalUnionVec();
  test_Utils_globalUnionMat();

  CHK_ERR( test1() );
  CHK_ERR( test2() );
  CHK_ERR( test3() );
  CHK_ERR( test4() );
  return(0);
}

int test_Utils::serialtest0()
{
  FEI_COUT << "testing snl_fei::binarySearchPtr...";
  int n = 10;
  feiArray<const BCRecord*> bclist(0, n);

  feiArray<double> coefs(3);
  coefs = 1;

  int fieldID = 1;
  int fieldSize = 3;

  BCRecord* bc0 = new BCRecord;
  bc0->init(4, fieldID, fieldSize, coefs.dataPtr());

  int insertPoint = -1;
  int index = snl_fei::binarySearchPtr(bc0,
				    bclist.dataPtr(), bclist.length(),
				    insertPoint);

  if (index != -1 || insertPoint != 0) {
    ERReturn(-1);
  }

  bclist.insert(bc0, insertPoint);

  BCRecord* bc1 = new BCRecord;
  bc1->init(6, fieldID, fieldSize, coefs.dataPtr());

  index = snl_fei::binarySearchPtr(bc1,
				bclist.dataPtr(), bclist.length(),
				insertPoint);

  if (index != -1 || insertPoint != 1) {
    ERReturn(-1);
  }

  bclist.insert(bc1, insertPoint);

  BCRecord* bc2 = new BCRecord;
  bc1->init(2, fieldID, fieldSize, coefs.dataPtr());

  index = snl_fei::binarySearchPtr(bc2,
				bclist.dataPtr(), bclist.length(),
				insertPoint);

  if (index != -1 || insertPoint != 0) {
    ERReturn(-1);
  }

  bclist.insert(bc2, insertPoint);

  index = snl_fei::binarySearchPtr(bc0,
				bclist.dataPtr(), bclist.length(),
				insertPoint);

  if (index != 1) {
    ERReturn(-1);
  }

  fieldID = 0;
  BCRecord* bc3 = new BCRecord;
  bc3->init(4, fieldID, fieldSize, coefs.dataPtr());

  index = snl_fei::binarySearchPtr(bc3,
				bclist.dataPtr(), bclist.length(),
				insertPoint);

  if (index != -1 || insertPoint != 1) {
    ERReturn(-1);
  }

  delete bc0;
  delete bc1;
  delete bc2;
  delete bc3;

  FEI_COUT << "ok"<<FEI_ENDL;

  return(0);
}

int test_Utils::serialtest1()
{
  FEI_COUT << "testing snl_fei::leading_substring_length...";

  static char string1[] = "test ";
  string1[4] = '\0';
  if (snl_fei::leading_substring_length(string1) != 4) {
    ERReturn(-1);
  }

  static char string2[] = "second test";
  if (snl_fei::leading_substring_length(string2) != 6) {
    ERReturn(-1);
  }

  static char string3[] = "third test";
  string3[5] = '\t';
  if (snl_fei::leading_substring_length(string3) != 5) {
    ERReturn(-1);
  }

  FEI_COUT << "ok"<<FEI_ENDL;

  return(0);
}

int test_Utils::serialtest2()
{
  FEI_COUT << "testing snl_fei::getDoubleParamValue...";

  static char string1[] = "DOUBLE1 1.0";
  static char string2[] = "DOUBLE2 1.0e+0";
  static char string3[] = "DOUBLE3 1.0E+0";
  static char string4[] = "DOUBLE4 1";

  feiArray<char*> params;
  params.append(string1);
  params.append(string2);
  params.append(string3);
  params.append(string4);

  double d1,d2,d3,d4;

  CHK_ERR( snl_fei::getDoubleParamValue("DOUBLE1",
					params.length(), params.dataPtr(),d1));
  CHK_ERR( snl_fei::getDoubleParamValue("DOUBLE2",
					params.length(), params.dataPtr(),d2));
  CHK_ERR( snl_fei::getDoubleParamValue("DOUBLE3",
					params.length(), params.dataPtr(),d3));
  CHK_ERR( snl_fei::getDoubleParamValue("DOUBLE4",
					params.length(), params.dataPtr(),d4));

  if (std::abs(d1 - 1.0) > 1.e-49 || std::abs(d2 - 1.0) > 1.e-49 ||
      std::abs(d3 - 1.0) > 1.e-49 || std::abs(d4 - 1.0) > 1.e-49) {
    ERReturn(-1);
  }

  FEI_COUT <<"ok"<<FEI_ENDL;

  return(0);
}

int test_Utils::serialtest3()
{
  FEI_COUT << "testing fei::Param and fei::ParameterSet...";

  fei::Param param1("string-param", "garbage value");
  fei::Param param2("double-param", 2.5);
  fei::Param param3("int-param", 1);

  if (param1.getType() != fei::Param::STRING) {
    ERReturn(-1);
  }

  if (param2.getType() != fei::Param::DOUBLE) {
    ERReturn(-1);
  }

  if (param3.getType() != fei::Param::INT) {
    ERReturn(-1);
  }

  fei::ParameterSet paramset;
  paramset.add(fei::Param("string-param", "garbage value"));
  paramset.add(param2);
  paramset.add(param3);

  if (paramset.size() != 3) {
    ERReturn(-1);
  }

  fei::ParameterSet::const_iterator
    iter = paramset.begin(),
    iter_end = paramset.end();

  int i=0;
  for(; iter != iter_end; ++iter) {
    if (i==3) {
      ERReturn(-1);
    }
    ++i;
  }
 
  if (paramset.get("int-param") == NULL) {
    ERReturn(-1);
  }

  int dummy;
  int err = paramset.getIntParamValue("int-param", dummy);
  if (err != 0) {
    ERReturn(-1);
  }

  if (dummy != 1) {
    ERReturn(-1);
  }

  std::string dummychars;
  err = paramset.getStringParamValue("string-param", dummychars);
  if (err != 0) {
    ERReturn(-1);
  }

  if ("garbage value" != dummychars) {
    ERReturn(-1);
  }

  //if (!snl_fei::leadingSubstring("garbage-value", "garbage")) {
  //  ERReturn(-1);
  //}

  //if (snl_fei::leadingSubstring("garb-value", "garbage")) {
  //  ERReturn(-1);
  //}

  std::vector<std::string> stdstrings;
  std::string tempstr;

  tempstr = "string-param garbage value";
  stdstrings.push_back(tempstr);

  tempstr = "int-param 58";
  stdstrings.push_back(tempstr);

  tempstr = "real-param 45.e-2";
  stdstrings.push_back(tempstr);

  fei::ParameterSet pset;
  fei::utils::parse_strings(stdstrings, " ", pset);

  err = pset.getStringParamValue("string-param", dummychars);
  if ("garbage value" != dummychars) {
    ERReturn(-1);
  }

  err = pset.getIntParamValue("int-param", dummy);
  if (dummy != 58) {
    ERReturn(-1);
  }

  double ddummy;
  err = pset.getDoubleParamValue("real-param", ddummy);
  if (std::abs(ddummy - 45.e-2) > 1.e-49) {
    ERReturn(-1);
  }

  FEI_COUT << "ok"<<FEI_ENDL;

  return(0);
}

void test_Utils_function_that_throws()
{
  throw fei::Exception("testing...");
}

int test_Utils::test1()
{
  FEI_COUT << "testing fei::Exception...";

  bool exc_thrown_and_caught = false;

  try {
    test_Utils_function_that_throws();
  }
  catch(fei::Exception& exc) {
    std::string str(exc.what());
    if (str == "testing...") {
      exc_thrown_and_caught = true;
    }
  }

  if (!exc_thrown_and_caught) {
    ERReturn(-1);
  }

  FEI_COUT << "ok"<<FEI_ENDL;
  return(0);
}

bool test_Utils_dummy_destroyed = true;

class test_Utils_dummy {
public:
  test_Utils_dummy() {test_Utils_dummy_destroyed = false;}
  ~test_Utils_dummy()
  {
    test_Utils_dummy_destroyed = true;
  }
};

int test_Utils_test_SharedPtr()
{
  //In this function, make sure the global bool is set to true, then create
  //the fei::SharedPtr and make sure that the global bool has been set to false.
  //If so, return 0, otherwise return -1.
  //When we return, the SharedPtr goes out of scope which should destroy the
  //test-dummy and cause the global bool to get set back to true. The code 
  //that's calling this function will verify that.

  test_Utils_dummy_destroyed = true;
  fei::SharedPtr<test_Utils_dummy> ptr(new test_Utils_dummy);
  if (test_Utils_dummy_destroyed == true) return(-1);
  else return(0);
}

int test_Utils::test2()
{
  FEI_COUT << "testing fei::SharedPtr...";
  int err = test_Utils_test_SharedPtr();
  if (err != 0) {
    ERReturn(-1);
  }

  if (test_Utils_dummy_destroyed != true) {
    ERReturn(-1);
  }

  FEI_COUT << "ok"<<FEI_ENDL;
 return(0);
}

int test_Utils::test3()
{
  FEI_COUT << "testing snl_fei::copy2DToColumnContig...";

  int numrows1 = 3;
  int numcols1 = 4;
  int numrows2 = 4;
  int numcols2 = 3;

  int i, j;
  int len1 = numrows1*numcols1;
  int len2 = numrows2*numcols2;

  double** table2d_1 = new double*[numrows1];
  for(i=0; i<numrows1; ++i) {
    table2d_1[i] = new double[numcols1];
    for(j=0; j<numcols1; ++j) {
      table2d_1[i][j] = j*numrows1+i;
    }
  }

  double** table2d_2 = new double*[numcols2];
  for(j=0; j<numcols2; ++j) {
    table2d_2[j] = new double[numrows2];
    for(i=0; i<numrows2; ++i) {
      table2d_2[j][i] = j*numrows2+i;
    }
  }

  double* cc1 = new double[len1];
  double* cc2 = new double[len2];

  snl_fei::copy2DToColumnContig(numrows1, numcols1, table2d_1,
				FEI_DENSE_ROW, cc1);

  snl_fei::copy2DToColumnContig(numrows2, numcols2, table2d_2,
				FEI_DENSE_COL, cc2);

  for(i=0; i<len1; ++i) {
    if (std::abs(cc1[i] - cc2[i]) > 1.e-49) {
      throw fei::Exception("column-contig arrays not equal.");
    }
  }

  for(j=0; j<numrows1; ++j) delete [] table2d_1[j];
  delete [] table2d_1;
  delete [] cc1;
  delete [] cc2;

  FEI_COUT << "ok"<<FEI_ENDL;

  FEI_COUT << "testing snl_fei::copy2DBlockDiagToColumnContig...";

  numrows1 = 12;
  int numBlocks = 3;
  int* blockSizes = new int[numBlocks];
  for(i=0; i<numBlocks; ++i) {
    blockSizes[i] = 4;
  }

  table2d_1 = new double*[numrows1];
  for(i=0; i<numrows1; ++i) {
    table2d_1[i] = new double[4];
    for(j=0; j<4; ++j) {
      table2d_1[i][j] = 1.0*i*4+j;
    }
  }

  len1 = numrows1*4;
  cc1 = new double[len1];

  snl_fei::copy2DBlockDiagToColumnContig(numBlocks, blockSizes, table2d_1,
					 FEI_BLOCK_DIAGONAL_ROW, cc1);

  for(i=0; i<len1; ++i) {
    if (std::abs(1.0*i - cc1[i]) > 1.e-49) {
      throw fei::Exception("copy2DBlockDiagToColumnContig row test failed.");
    }
  }

  for(j=0; j<numrows1; ++j) delete [] table2d_1[j];
  delete [] table2d_1;
  for(j=0; j<numcols2; ++j) delete [] table2d_2[j];
  delete [] table2d_2;

  delete [] cc1;
  delete [] blockSizes;

  FEI_COUT << "ok"<<FEI_ENDL;
  return(0);
}

int test_Utils::test4()
{
  return(0);
}
