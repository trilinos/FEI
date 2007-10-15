#ifndef _fei_ConnectivityTable_hpp_
#define _fei_ConnectivityTable_hpp_

/*--------------------------------------------------------------------*/
/*    Copyright 2005 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#include "fei_macros.hpp"
#include "feiArray.hpp"
#include "fei_defs.h"

#include <map>

/** ConnectivityTable is basically a struct that holds the nodal connectivity
lists for the elements in an element-block.
*/

class ConnectivityTable {
 public:
   ConnectivityTable() : numRows(0), elemIDs(), elemNumbers(),
                         elem_conn_ids(NULL), elem_conn_ptrs(NULL),
                         connectivities(NULL), numNodesPerElem(0) {}

   virtual ~ConnectivityTable() {
      for(int i=0; i<numRows; i++) delete connectivities[i];
      if (numRows > 0) {
         delete [] connectivities;
      }

      if (elem_conn_ids != NULL) {
	delete elem_conn_ids;
      }

      if (elem_conn_ptrs != NULL) {
	delete elem_conn_ptrs;
      }
   }

   int numRows;
   std::map<GlobalID,int> elemIDs;
   feiArray<int> elemNumbers;
   feiArray<GlobalID>* elem_conn_ids;
   feiArray<NodeDescriptor*>* elem_conn_ptrs;
   feiArray<GlobalID>** connectivities;
   int numNodesPerElem;

 private:
   ConnectivityTable(const ConnectivityTable& /*src*/);

   ConnectivityTable& operator=(const ConnectivityTable& /*src*/);
};


#endif
