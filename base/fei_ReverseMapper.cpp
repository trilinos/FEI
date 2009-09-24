/*--------------------------------------------------------------------*/
/*    Copyright 2009 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#include <fei_ReverseMapper.hpp>
#include <stdexcept>
#include <sstream>
#include <vector>
#include <fei_VectorSpace.hpp>
#include <snl_fei_RecordCollection.hpp>

namespace fei {

ReverseMapper::ReverseMapper(const VectorSpace& vspace)
 : eqnmap_()
{
  std::vector<int> idTypes;
  vspace.getIDTypes(idTypes);

  const std::vector<int>& eqnNumbers = vspace.getEqnNumbers();

  for(size_t idt=0; idt<idTypes.size(); ++idt) {
    const snl_fei::RecordCollection* recordcollection = NULL;
    int err = vspace.getRecordCollection(idTypes[idt], recordcollection);
    if (err != 0) {
      throw std::runtime_error("fei::ReverseMapper ERROR, failed to retrieve record-collection.");
    }

    const snl_fei::RecordCollection::map_type&
        recordmap = recordcollection->getRecords();

    snl_fei::RecordCollection::map_type::const_iterator
      r_iter = recordmap.begin(),
      r_end  = recordmap.end();

    for(; r_iter != r_end; ++r_iter) {
      const fei::Record* record = r_iter->second;

      const fei::FieldMask* fm = record->getFieldMask();
      const std::vector<int>& fieldIDs = fm->getFieldIDs();
      const std::vector<int>& fieldSizes = fm->getFieldSizes();

      int offsetIntoEqnNumbers = record->getOffsetIntoEqnNumbers();

      for(size_t i=0; i<fieldIDs.size(); ++i) {
        int offset2 = 0, numInstances = 1;
        fm->getFieldEqnOffset(fieldIDs[i], offset2, numInstances);

        EqnRecord erec;
        erec.IDType = idTypes[idt];
        erec.ID = record->getID();
        erec.fieldID = fieldIDs[i];

        for(int j=0; j<fieldSizes[i]; ++j) {
          erec.offset = j;
          erec.global_eqn = eqnNumbers[offsetIntoEqnNumbers+offset2+j];
          eqnmap_.insert(std::make_pair(erec.global_eqn, erec));
        }
      }
    }
  }
}

ReverseMapper::~ReverseMapper()
{
}

EqnRecord ReverseMapper::getEqnRecord(int global_eqn) const
{
  std::map<int,EqnRecord>::const_iterator
    iter = eqnmap_.find(global_eqn);

  if (iter == eqnmap_.end()) {
    std::ostringstream osstr;
    osstr << "fei::ReverseMapper::getEqnRecord ERROR, global_eqn="<<global_eqn
       << " not found.";
    throw std::runtime_error(osstr.str());
  }

  return iter->second;
}

}//namespace fei

