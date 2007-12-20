/*--------------------------------------------------------------------*/
/*    Copyright 2005 Sandia Corporation.                              */
/*    Under the terms of Contract DE-AC04-94AL85000, there is a       */
/*    non-exclusive license for use of this work by or on behalf      */
/*    of the U.S. Government.  Export of this program may require     */
/*    a license from the United States Government.                    */
/*--------------------------------------------------------------------*/

#include <fei_macros.hpp>
#include <fei_SharedPtr.hpp>
#include <fei_mpi.h>

#include <cfei_petsc.h>

#include <fei_Data.hpp>
#include <fei_LinearSystemCore.hpp>
#include <fei_PETSc_LinSysCore.hpp>

/*============================================================================*/
/* Create function for a PETSc_LinSysCore object.
*/
extern "C" int PETSc_LinSysCore_create(LinSysCore** lsc, 
				       MPI_Comm comm) {

   fei::SharedPtr<LinearSystemCore>* linSys =
     new fei::SharedPtr<LinearSystemCore>(new PETSc_LinSysCore(comm));

   if (linSys->get() == NULL) return(1);

   *lsc = new LinSysCore;

   (*lsc)->lsc_ = (void*)linSys;

   return(0);
}

/*============================================================================*/
/* Destroy function, to de-allocate a PETSc_LinSysCore object.
*/
extern "C" int PETSc_LinSysCore_destroy(LinSysCore** lsc)
{
   if (*lsc == NULL) return(1);

   fei::SharedPtr<LinearSystemCore>* linSys =
     (fei::SharedPtr<LinearSystemCore>*)((*lsc)->lsc_);

   if (linSys->get() == NULL) return(1);

   delete linSys;

   delete *lsc;
   *lsc = NULL;

   return(0);
}

