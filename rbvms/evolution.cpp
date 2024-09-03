// This file is part of the RBVMS application. For more information and source
// code availability visit https://idoakkerman.github.io/
//
// RBVMS is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license.
//------------------------------------------------------------------------------

#include "evolution.hpp"

using namespace mfem;
using namespace RBVMS;

Evolution::Evolution(ParTimeDepBlockNonlinForm &form,
                     Solver &solver)
   : TimeDependentOperator(form.Width(), 0.0, IMPLICIT),
     form(form), solver(solver), dudt(form.Width())
{
   solver.SetOperator(form);
   dudt = 0.0; 
}

void Evolution::ImplicitSolve(const real_t dt,
                              const Vector &u0, Vector &dudt_)
{
   form.SetSolution(dt, u0);
   Vector zero;
   solver.Mult(zero, dudt);
   dudt_ = dudt;
}


ParTimeDepBlockNonlinForm::
   ParTimeDepBlockNonlinForm(Array<ParFiniteElementSpace *> &pfes,
                             RBVMS::IncNavStoIntegrator &integrator)
   : ParBlockNonlinearForm(pfes), integrator(integrator)
{
}

void ParTimeDepBlockNonlinForm::SetSolution(const real_t dt_,
                                            const Vector &x0_)
{
   dt = dt_;
   x0 = x0_;
   x.SetSize(x0.Size());
   integrator.SetTimeStep(dt);
}

void ParTimeDepBlockNonlinForm::Mult(const Vector &dx, Vector &y) const
{
   // Get current solution
   add(x0,dt,dx,x);   // x = x0 + dt*dx

   // xs_true is not modified, so const_cast is okay
   xs_true.Update(const_cast<Vector &>(x), block_trueOffsets);
   dxs_true.Update(const_cast<Vector &>(dx), block_trueOffsets);
   ys_true.Update(y, block_trueOffsets);
   xs.Update(block_offsets);
   dxs.Update(block_offsets);
   ys.Update(block_offsets);

   for (int s=0; s<fes.Size(); ++s)
   {
      fes[s]->GetProlongationMatrix()->Mult(
         xs_true.GetBlock(s), xs.GetBlock(s));
      fes[s]->GetProlongationMatrix()->Mult(
         dxs_true.GetBlock(s), dxs.GetBlock(s));
   }
   MultBlocked(xs, dxs, ys);

   for (int s=0; s<fes.Size(); ++s)
   {
      fes[s]->GetProlongationMatrix()->MultTranspose(
         ys.GetBlock(s), ys_true.GetBlock(s));

      ys_true.GetBlock(s).SetSubVector(*ess_tdofs[s], 0.0);
   }

   ys_true.SyncFromBlocks();
   y.SyncMemory(ys_true);
}

void ParTimeDepBlockNonlinForm::MultBlocked(const BlockVector &bx,
                                            const BlockVector &bdx,
                                            BlockVector &by) const
{
   Array<Array<int> *>vdofs(fes.Size());
   Array<Array<int> *>vdofs2(fes.Size());
   Array<Vector *> el_x(fes.Size());
   Array<const Vector *> el_x_const(fes.Size());
   Array<Vector *> el_dx(fes.Size());
   Array<const Vector *> el_dx_const(fes.Size());
   Array<Vector *> el_y(fes.Size());
   Array<const FiniteElement *> fe(fes.Size());
   Array<const FiniteElement *> fe2(fes.Size());
   ElementTransformation *T;
   Array<DofTransformation *> doftrans(fes.Size()); doftrans = nullptr;
   Mesh *mesh = fes[0]->GetMesh();

   by.UseDevice(true);
   by = 0.0;
   by.SyncToBlocks();
   for (int s=0; s<fes.Size(); ++s)
   {
      el_x_const[s] = el_x[s] = new Vector();
      el_dx_const[s] = el_dx[s] = new Vector();
      el_y[s] = new Vector();
      vdofs[s] = new Array<int>;
      vdofs2[s] = new Array<int>;
   }

   for (int i = 0; i < fes[0]->GetNE(); ++i)
   {
      T = fes[0]->GetElementTransformation(i);
      for (int s = 0; s < fes.Size(); ++s)
      {
         doftrans[s] = fes[s]->GetElementVDofs(i, *(vdofs[s]));
         fe[s] = fes[s]->GetFE(i);
         bx.GetBlock(s).GetSubVector(*(vdofs[s]), *el_x[s]);
         bdx.GetBlock(s).GetSubVector(*(vdofs[s]), *el_dx[s]);
         if (doftrans[s])
         {
            doftrans[s]->InvTransformPrimal(*el_x[s]);
            doftrans[s]->InvTransformPrimal(*el_dx[s]);
         }
      }

      integrator.AssembleElementVector(fe, *T,
                                       el_x_const,
                                       el_dx_const,
                                       el_y);

      for (int s=0; s<fes.Size(); ++s)
      {
         if (el_y[s]->Size() == 0) { continue; }
         if (doftrans[s]) {doftrans[s]->TransformDual(*el_y[s]); }
         by.GetBlock(s).AddElementVector(*(vdofs[s]), *el_y[s]);
      }
   }

   for (int s=0; s<fes.Size(); ++s)
   {
      delete vdofs2[s];
      delete vdofs[s];
      delete el_y[s];
      delete el_x[s];
   }

   by.SyncFromBlocks();
}

BlockOperator & ParTimeDepBlockNonlinForm::GetGradient(const Vector &x) const
{
   if (pBlockGrad == NULL)
   {
      pBlockGrad = new BlockOperator(block_trueOffsets);
   }

   Array<const ParFiniteElementSpace *> pfes(fes.Size());

   for (int s1=0; s1<fes.Size(); ++s1)
   {
      pfes[s1] = ParFESpace(s1);

      for (int s2=0; s2<fes.Size(); ++s2)
      {
         phBlockGrad(s1,s2)->Clear();
      }
   }

   GetLocalGradient(x); // gradients are stored in 'Grads'

   if (fnfi.Size() > 0)
   {
      MFEM_ABORT("TODO: assemble contributions from shared face terms");
   }

   for (int s1=0; s1<fes.Size(); ++s1)
   {
      for (int s2=0; s2<fes.Size(); ++s2)
      {
         OperatorHandle dA(phBlockGrad(s1,s2)->Type()),
                        Ph(phBlockGrad(s1,s2)->Type()),
                        Rh(phBlockGrad(s1,s2)->Type());

         if (s1 == s2)
         {
            dA.MakeSquareBlockDiag(pfes[s1]->GetComm(), pfes[s1]->GlobalVSize(),
                                   pfes[s1]->GetDofOffsets(), Grads(s1,s1));
            Ph.ConvertFrom(pfes[s1]->Dof_TrueDof_Matrix());
            phBlockGrad(s1,s1)->MakePtAP(dA, Ph);

            OperatorHandle Ae;
            Ae.EliminateRowsCols(*phBlockGrad(s1,s1), *ess_tdofs[s1]);
         }
         else
         {
            dA.MakeRectangularBlockDiag(pfes[s1]->GetComm(),
                                        pfes[s1]->GlobalVSize(),
                                        pfes[s2]->GlobalVSize(),
                                        pfes[s1]->GetDofOffsets(),
                                        pfes[s2]->GetDofOffsets(),
                                        Grads(s1,s2));
            Rh.ConvertFrom(pfes[s1]->Dof_TrueDof_Matrix());
            Ph.ConvertFrom(pfes[s2]->Dof_TrueDof_Matrix());

            phBlockGrad(s1,s2)->MakeRAP(Rh, dA, Ph);

            phBlockGrad(s1,s2)->EliminateRows(*ess_tdofs[s1]);
            phBlockGrad(s1,s2)->EliminateCols(*ess_tdofs[s2]);
         }

         pBlockGrad->SetBlock(s1, s2, phBlockGrad(s1,s2)->Ptr());
      }
   }

   return *pBlockGrad;
}

/// Return the local gradient matrix for the given true-dof vector x
const BlockOperator& ParTimeDepBlockNonlinForm
   ::GetLocalGradient(const Vector &dx) const
{
   // Get current solution
   add(x0,dt,dx,x);   // x = x0 + dt*dx

   // xs_true is not modified, so const_cast is okay
   xs_true.Update(const_cast<Vector &>(x), block_trueOffsets);
   dxs_true.Update(const_cast<Vector &>(dx), block_trueOffsets);
   xs.Update(block_offsets);
   dxs.Update(block_offsets);

   for (int s=0; s<fes.Size(); ++s)
   {
      fes[s]->GetProlongationMatrix()->Mult(
         xs_true.GetBlock(s), xs.GetBlock(s));
      fes[s]->GetProlongationMatrix()->Mult(
         dxs_true.GetBlock(s), dxs.GetBlock(s));
   }

   // (re)assemble Grad without b.c. into 'Grads'
   ComputeGradientBlocked(xs, dxs);

   delete BlockGrad;
   BlockGrad = new BlockOperator(block_offsets);

   for (int i = 0; i < fes.Size(); ++i)
   {
      for (int j = 0; j < fes.Size(); ++j)
      {
         BlockGrad->SetBlock(i, j, Grads(i, j));
      }
   }
   return *BlockGrad;
}

void ParTimeDepBlockNonlinForm
   ::ComputeGradientBlocked(const BlockVector &bx,
                            const BlockVector &bdx) const
{
   const int skip_zeros = 0;
   Array<Array<int> *> vdofs(fes.Size());
   Array<Array<int> *> vdofs2(fes.Size());
   Array<Vector *> el_x(fes.Size());
   Array<const Vector *> el_x_const(fes.Size());
   Array<Vector *> el_dx(fes.Size());
   Array<const Vector *> el_dx_const(fes.Size());
   Array2D<DenseMatrix *> elmats(fes.Size(), fes.Size());
   Array<const FiniteElement *>fe(fes.Size());
   Array<const FiniteElement *>fe2(fes.Size());
   ElementTransformation * T;
   Array<DofTransformation *> doftrans(fes.Size()); doftrans = nullptr;
   Mesh *mesh = fes[0]->GetMesh();

   for (int i=0; i<fes.Size(); ++i)
   {
      el_x_const[i] = el_x[i] = new Vector();
      el_dx_const[i] = el_dx[i] = new Vector();
      vdofs[i] = new Array<int>;
      vdofs2[i] = new Array<int>;
      for (int j=0; j<fes.Size(); ++j)
      {
         elmats(i,j) = new DenseMatrix();
      }
   }

   for (int i=0; i<fes.Size(); ++i)
   {
      for (int j=0; j<fes.Size(); ++j)
      {
         if (Grads(i,j) != NULL)
         {
            *Grads(i,j) = 0.0;
         }
         else
         {
            Grads(i,j) = new SparseMatrix(fes[i]->GetVSize(),
                                          fes[j]->GetVSize());
         }
      }
   }

   for (int i = 0; i < fes[0]->GetNE(); ++i)
   {
      T = fes[0]->GetElementTransformation(i);
      for (int s = 0; s < fes.Size(); ++s)
      {
         fe[s] = fes[s]->GetFE(i);
         doftrans[s] = fes[s]->GetElementVDofs(i, *vdofs[s]);
         bx.GetBlock(s).GetSubVector(*vdofs[s], *el_x[s]);
         bdx.GetBlock(s).GetSubVector(*vdofs[s], *el_dx[s]);
         if (doftrans[s])
         {
            doftrans[s]->InvTransformPrimal(*el_x[s]);
            doftrans[s]->InvTransformPrimal(*el_dx[s]);
         }
      }

      integrator.AssembleElementGrad(fe, *T, el_x_const,el_dx_const, elmats);

      for (int j=0; j<fes.Size(); ++j)
      {
         for (int l=0; l<fes.Size(); ++l)
         {
            if (elmats(j,l)->Height() == 0) { continue; }
            if (doftrans[j] || doftrans[l])
            {
               TransformDual(doftrans[j], doftrans[l], *elmats(j,l));
            }
            Grads(j,l)->AddSubMatrix(*vdofs[j], *vdofs[l],
                                     *elmats(j,l), skip_zeros);
         }
      }
   }

   if (!Grads(0,0)->Finalized())
   {
      for (int i=0; i<fes.Size(); ++i)
      {
         for (int j=0; j<fes.Size(); ++j)
         {
            Grads(i,j)->Finalize(skip_zeros);
         }
      }
   }

   for (int i=0; i<fes.Size(); ++i)
   {
      for (int j=0; j<fes.Size(); ++j)
      {
         delete elmats(i,j);
      }
      delete vdofs2[i];
      delete vdofs[i];
      delete el_x[i];
   }

}