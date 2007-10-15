/*--------------------------------------------------------------------*/
/*    Copyright 2005 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#ifndef _fei_ReverseMapper_hpp_
#define _fei_ReverseMapper_hpp_

#include <fei_macros.hpp>
#include <fei_fwd.hpp>
#include <fei_EqnRecord.hpp>

namespace fei {
/** Allows mapping from equation-numbers to IDs, fields, etc. */
class ReverseMapper {
 public:
  /** constructor */
  ReverseMapper();

  /** destructor */
  virtual ~ReverseMapper();

  EqnRecord getEqnRecord(int global_eqn);

 private:
  ReverseMapper(const ReverseMapper& src);
  ReverseMapper& operator=(const ReverseMapper& src);
};//class ReverseMapper
}//namespace fei
#endif

