// This file is part of the RBVMS application. For more information and source
// code availability visit https://idoakkerman.github.io/
//
// RBVMS is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license.
//------------------------------------------------------------------------------

#include "weakform.hpp"
#include <iostream>

using namespace mfem;
using namespace RBVMS;

// Constructor
IncNavStoIntegrator::IncNavStoIntegrator(Coefficient &mu,
                                         VectorCoefficient &force,
                                         VectorCoefficient &sol)
   : c_mu(mu), c_force(force), c_sol(sol)
{
   dim = force.GetVDim();
   u.SetSize(dim);
   dudt.SetSize(dim);
   f.SetSize(dim);
   res_m.SetSize(dim);
   up.SetSize(dim);
   traction.SetSize(dim);
   grad_u.SetSize(dim);
   hess_u.SetSize(dim, (dim*(dim+1))/2);
   grad_p.SetSize(dim);
   Gij.SetSize(dim);
   nor.SetSize(dim);
   hn.SetSize(dim);
   hmap.SetSize(dim,dim);

   if (dim == 2)
   {
      hmap(0,0) = 0;
      hmap(0,1) =  hmap(1,0) =  1;
      hmap(1,1) = 2;
   }
   else if (dim == 2)
   {
      hmap(0,0) = 0;
      hmap(0,1) = hmap(1,0) = 1;
      hmap(0,2) = hmap(2,0) = 2;
      hmap(1,1) = 3;
      hmap(1,2) = hmap(2,1) = 4;
      hmap(2,2) = 5;
   }
   else
   {
      mfem_error("Only implemented for 2D and 3D");
   }
}

// Compute RBVMS stabilisation parameters
void IncNavStoIntegrator::GetTau(real_t &tau_m, real_t &tau_c, real_t &cfl2,
                                 real_t &mu, Vector &u,
                                 ElementTransformation &T)
{
   real_t Cd = 24.0;
   real_t Ct = 1.0;

   // Metric tensor
   MultAtB(T.InverseJacobian(),T.InverseJacobian(),Gij);

   // Temporal part
   tau_m = Ct/(dt*dt);

   // Convective part
   tau_c = 0.0;
   for (int j = 0; j < dim; j++)
   {
      for (int i = 0; i < dim; i++)
      {
         tau_c += Gij(i,j)*u[i]*u[j];
      }
   }
   cfl2 = tau_c/tau_m;
   tau_m += tau_c;

   // Diffusive part
   for (int j = 0; j < dim; j++)
   {
      for (int i = 0; i < dim; i++)
      {
         tau_m  += Cd*Cd*Gij(i,j)*Gij(i,j)*mu*mu;
      }
   }

   // Momentum stabilisation parameter
   tau_m = 1.0/sqrt(tau_m);

   // Continuity stabilisation parameter
   tau_c = 1.0/(tau_m*Gij.Trace());
}

// Compute Weak Dirichlet stabilisation parameters
void IncNavStoIntegrator::GetTauB(real_t &tau_b, real_t &tau_n,
                                  real_t &mu, Vector &u,
                                  Vector &nor,
                                  FaceElementTransformations &Tr)
{
   real_t Cb = 12.0;

   Tr.Elem1->InverseJacobian().Mult(nor,hn);
   tau_b = Cb*mu*hn.Norml2();
   tau_n = 0.0;
}

// Get energy
real_t IncNavStoIntegrator::GetElementEnergy(
   const Array<const FiniteElement *>&el,
   ElementTransformation &Tr,
   const Array<const Vector *> &elsol,
   const Array<const Vector *> &elrate)
{
   if (el.Size() != 2)
   {
      mfem_error("IncNavStoIntegrator::GetElementEnergy"
                 " has incorrect block finite element space size!");
   }
   int dof_u = el[0]->GetDof();
   Vector tau(3);

   sh_u.SetSize(dof_u);
   elf_u.UseExternalData(elsol[0]->GetData(), dof_u, dim);

   int intorder = 2*el[0]->GetOrder();
   const IntegrationRule &ir = IntRules.Get(el[0]->GetGeomType(), intorder);

   real_t energy = 0.0;

   for (int i = 0; i < ir.GetNPoints(); ++i)
   {
      const IntegrationPoint &ip = ir.IntPoint(i);
      Tr.SetIntPoint(&ip);

      real_t w = ip.weight * Tr.Weight();

      el[0]->CalcPhysShape(Tr, sh_u);
      elf_u.MultTranspose(sh_u, u);

      energy += w*(u*u)/2;
   }

   return energy;
}

// Assemble the element interior residual vectors
void IncNavStoIntegrator::AssembleElementVector(
   const Array<const FiniteElement *> &el,
   ElementTransformation &Tr,
   const Array<const Vector *> &elsol,
   const Array<const Vector *> &elrate,
   const Array<Vector *> &elvec,
   real_t &elem_cfl)
{
   if (el.Size() != 2)
   {
      mfem_error("IncNavStoIntegrator::AssembleElementVector"
                 " has finite element space of incorrect block number");
   }

   int dof_u = el[0]->GetDof();
   int dof_p = el[1]->GetDof();

   int spaceDim = Tr.GetSpaceDim();
   bool hess = false;//(el[0]->GetDerivType() == (int) FiniteElement::HESS);
   if (dim != spaceDim)
   {
      mfem_error("IncNavStoIntegrator::AssembleElementVector"
                 " is not defined on manifold meshes");
   }
   elvec[0]->SetSize(dof_u*dim);
   elvec[1]->SetSize(dof_p);

   *elvec[0] = 0.0;
   *elvec[1] = 0.0;

   elf_u.UseExternalData(elsol[0]->GetData(), dof_u, dim);
   elf_du.UseExternalData(elrate[0]->GetData(), dof_u, dim);
   elv_u.UseExternalData(elvec[0]->GetData(), dof_u, dim);

   sh_u.SetSize(dof_u);
   shg_u.SetSize(dof_u, dim);
   ushg_u.SetSize(dof_u);
   shh_u.SetSize(dof_u, (dim*(dim+1))/2);
   sh_p.SetSize(dof_p);
   shg_p.SetSize(dof_p, dim);

   int intorder = 2*el[0]->GetOrder();
   const IntegrationRule &ir = IntRules.Get(el[0]->GetGeomType(), intorder);
   real_t tau_m, tau_c, cfl2;
   elem_cfl = 0.0;
   for (int i = 0; i < ir.GetNPoints(); ++i)
   {
      const IntegrationPoint &ip = ir.IntPoint(i);
      Tr.SetIntPoint(&ip);
      real_t w = ip.weight * Tr.Weight();
      real_t mu = c_mu.Eval(Tr, ip);
      c_force.Eval(f, Tr, ip);

      // Compute shape and interpolate
      el[0]->CalcPhysShape(Tr, sh_u);
      elf_u.MultTranspose(sh_u, u);
      elf_du.MultTranspose(sh_u, dudt);

      el[0]->CalcPhysDShape(Tr, shg_u);
      shg_u.Mult(u, ushg_u);

      el[1]->CalcPhysShape(Tr, sh_p);
      real_t p = sh_p*(*elsol[1]);

      el[1]->CalcPhysDShape(Tr, shg_p);
      shg_p.MultTranspose(*elsol[1], grad_p);

      // Compute strong residual
      MultAtB(elf_u, shg_u, grad_u);
      grad_u.Mult(u,res_m);   // Add convection
      res_m += dudt;          // Add acceleration
      res_m += grad_p;        // Add pressure
      res_m -= f;             // Subtract force

      if (hess)               // Add diffusion
      {
         el[0]->CalcPhysHessian(Tr,shh_u);
         MultAtB(elf_u, shh_u, hess_u);
         for (int i = 0; i < dim; ++i)
         {
            for (int j = 0; j < dim; ++j)
            {
               res_m[j] -= mu*(hess_u(j,hmap(i,i)) +
                               hess_u(i,hmap(j,i)));
            }
         }
      }
      else                   // No diffusion in strong residual
      {
         shh_u = 0.0;
         hess_u = 0.0;
      }
      real_t res_c = grad_u.Trace();

      // Compute stability params
      GetTau(tau_m, tau_c, cfl2, mu, u, Tr);
      elem_cfl = fmax(elem_cfl, cfl2);

      // Small scale reconstruction
      up.Set(-tau_m,res_m);
      u += up;
      p -= tau_c*res_c;

      // Compute momentum weak residual
      flux.Diag(-p, dim);                         // Add pressure
      grad_u.Symmetrize();                        // Grad to strain
      flux.Add(2*mu,grad_u);                      // Add stress to flux
      AddMult_a_VVt(-1.0, u, flux);               // Add convection to flux
      AddMult_a_ABt(w, shg_u, flux, elv_u);       // Add flux term to rhs
      f -= dudt;                                  // Add Acceleration to force
      AddMult_a_VWt(-w, sh_u, f, elv_u);          // Add force + acc term to rhs

      // Compute continuity weak residual
      elvec[1]->Add(-w*res_c, sh_p);              // Add Galerkin term
      shg_p.Mult(up, sh_p);                       // PSPG help term
      elvec[1]->Add(w, sh_p);                     // Add PSPG term
   }

   elem_cfl = sqrt(elem_cfl);
}

void IncNavStoIntegrator::AssembleElementGrad(
   const Array<const FiniteElement*> &el,
   ElementTransformation &Tr,
   const Array<const Vector *> &elsol,
   const Array<const Vector *> &elrate,
   const Array2D<DenseMatrix *> &elmats)
{
   
   int dof_u = el[0]->GetDof();
   int dof_p = el[1]->GetDof();

   bool hess = false;// = (el[0]->GetDerivType() == (int) FiniteElement::HESS);

   elf_u.UseExternalData(elsol[0]->GetData(), dof_u, dim);
   elf_du.UseExternalData(elrate[0]->GetData(), dof_u, dim);

   elmats(0,0)->SetSize(dof_u*dim, dof_u*dim);
   elmats(0,1)->SetSize(dof_u*dim, dof_p);
   elmats(1,0)->SetSize(dof_p, dof_u*dim);
   elmats(1,1)->SetSize(dof_p, dof_p);

   *elmats(0,0) = 0.0;
   *elmats(0,1) = 0.0;
   *elmats(1,0) = 0.0;
   *elmats(1,1) = 0.0;

   sh_u.SetSize(dof_u);
   shg_u.SetSize(dof_u, dim);
   ushg_u.SetSize(dof_u);
   dupdu.SetSize(dof_u);
   sh_p.SetSize(dof_p);
   shg_p.SetSize(dof_p, dim);

   //R - initialize matrices and vectors with correct size
   DenseMatrix mat_matrix, mu_mat, tau_mat, wp_mat, qu_mat;
   Vector shg_u_1_vec, shg_u_2_vec, shg_p_vec;

   mat_matrix.SetSize(dof_u,dof_u);
   mu_mat.SetSize(dof_u,dof_u);
   tau_mat.SetSize(dof_u,dof_u);
   wp_mat.SetSize(dof_u,dof_u);
   qu_mat.SetSize(dof_u,dof_u);

   int intorder = 2*el[0]->GetOrder();
   const IntegrationRule &ir = IntRules.Get(el[0]->GetGeomType(), intorder);
   real_t tau_m, tau_c, cfl2;
   for (int i = 0; i < ir.GetNPoints(); ++i)
   {
      const IntegrationPoint &ip = ir.IntPoint(i);
      Tr.SetIntPoint(&ip);
      real_t w = ip.weight * Tr.Weight();
      real_t mu = c_mu.Eval(Tr, ip);

      //R - cache const variables re-used in inner loops
      const real_t w_dt = w*dt;
      const real_t mu_dt = mu*dt;

      el[0]->CalcPhysShape(Tr, sh_u);
      elf_u.MultTranspose(sh_u, u);
      elf_du.MultTranspose(sh_u, dudt);

      el[0]->CalcPhysDShape(Tr, shg_u);
      MultAtB(elf_u, shg_u, grad_u);

      shg_u.Mult(u, ushg_u);

      el[1]->CalcPhysShape(Tr, sh_p);
      real_t p = sh_p*(*elsol[1]);

      el[1]->CalcPhysDShape(Tr, shg_p);
      shg_p.MultTranspose(*elsol[1], grad_p);

      // Compute strong residual
      MultAtB(elf_u, shg_u, grad_u);
      grad_u.Mult(u,res_m);   // Add convection
      res_m += dudt;          // Add acceleration
      res_m += grad_p;        // Add pressure
      res_m -= f;             // Subtract force

      if (hess)               // Add diffusion
      {
         el[0]->CalcPhysHessian(Tr,shh_u);
         MultAtB(elf_u, shh_u, hess_u);
         for (int i = 0; i < dim; ++i)
         {
            for (int j = 0; j < dim; ++j)
            {
               res_m[j] -= mu*(hess_u(j,hmap(i,i)) +
                               hess_u(i,hmap(j,i)));
            }
         }
      }
      else                   // No diffusion in strong residual
      {
         shh_u = 0.0;
         hess_u = 0.0;
      }

      // Compute stability params
      GetTau(tau_m, tau_c, cfl2, mu, u, Tr);

      // Small scale reconstruction
      up.Set(-tau_m,res_m);
      u += up;

      // Compute small scale jacobian
      for (int j_u = 0; j_u < dof_u; ++j_u)
      {
         dupdu(j_u) = -tau_m*(sh_u(j_u) + ushg_u(j_u)*dt);
      }

      // Recompute convective gradient
      MultAtB(elf_u, shg_u, grad_u);

      ///////////////////
      // Vectorization //
      ///////////////////
      
      // R -   Reset matrix. Sets all values of mat_matrix to 0. 
      mat_matrix = 0.0; 

      // Diffusion term. mat_matrix = shg_u * shg_u^T
      MultAAt(shg_u, mat_matrix);
      mat_matrix *= mu_dt;

      // Acceleration term. mat_matrix += sh_u * sh_u^T
      AddMultVVt(sh_u, mat_matrix);
      
      // Convection terms   /// VWt += a * v w^t void AddMult_a_VWt(const real_t a, const Vector &v, const Vector &w, DenseMatrix &VWt);
      AddMult_a_VWt(-dt, ushg_u, sh_u, mat_matrix);
      const real_t tempy =  -1.0;   //Temporary constant to simulate -= when using AddMult_a_VWt()
      AddMult_a_VWt(tempy, ushg_u, dupdu, mat_matrix);
      mat_matrix *= w;

      // Adds blocks of (dof_u, dof_u) to diagonals for every dimension. The matrix is the same for all dimensions.
      for (int dim_u = 0; dim_u < dim; ++dim_u)
      {
         elmats(0,0)->AddSubMatrix(dim_u * dof_u, mat_matrix);
      }
      
      // Momentum - Velocity block (w,u)
      for (int i_dim = 0; i_dim < dim; ++i_dim)
         {
            // Getting columns for outer product
            shg_u.GetColumn(i_dim, shg_u_2_vec);
            for (int j_dim = 0; j_dim < dim; ++j_dim)
            {
               shg_u.GetColumn(j_dim, shg_u_1_vec);

               mu_mat = 0.0;
               // R -   Outer product times scalar, store in mu_mat
               AddMult_a_VWt(w_dt*mu, shg_u_2_vec, shg_u_1_vec, mu_mat);
               // R -   Add mu_mat as a block to elmats
               elmats(0,0)->AddSubMatrix(i_dim * dof_u, j_dim * dof_u, mu_mat);
               
               tau_mat = 0.0;
               // R -   Outer product times scalar, store in tau_mat
               AddMult_a_VWt(w_dt*tau_c,  shg_u_2_vec, shg_u_1_vec, tau_mat);
               // R -   Add tau_mat as a block to elmats
               elmats(0,0)->AddSubMatrix(i_dim * dof_u, j_dim * dof_u, tau_mat);
            }
         }

      // Momentum - Pressure block (w,p)
      for (int dim_u = 0; dim_u < dim; ++dim_u)
      {
         wp_mat = 0.0;

         // Getting columns for outer product
         shg_p.GetColumn(dim_u, shg_p_vec);
         shg_u.GetColumn(dim_u, shg_u_1_vec);

         // R -   Outer product times scalar, store in wp_mat
         AddMult_a_VWt(tau_m * w_dt, ushg_u, shg_p_vec, wp_mat);
         AddMult_a_VWt(-w_dt, shg_u_1_vec, sh_p, wp_mat);

         // R -   Add wp_mat as a block to elmats
         elmats(0,1)->AddSubMatrix(dim_u * dof_u, 0, wp_mat);
      }

      // Continuity - Velocity block (q,u)
      for (int dim_u = 0; dim_u < dim; ++dim_u)
      {
         qu_mat = 0.0;
         shg_p.GetColumn(dim_u, shg_p_vec);
         shg_u.GetColumn(dim_u, shg_u_1_vec);

         AddMult_a_VWt(-w_dt, sh_p, shg_u_1_vec, qu_mat);
         AddMult_a_VWt(w, shg_p_vec, dupdu, qu_mat);

         elmats(1,0)->AddSubMatrix(0, dim_u * dof_u, qu_mat);
      }
      // Continuity - Pressure block (w,p)
      AddMult_a_AAt(-w_dt*tau_m, shg_p, *elmats(1,1));
   }
}   

// Assemble the outflow boundary residual vectors
void IncNavStoIntegrator
::AssembleOutflowVector(const Array<const FiniteElement *> &el1,
                        const Array<const FiniteElement *> &el2,
                        FaceElementTransformations &Tr,
                        const Array<const Vector *> &elsol,
                        const Array<Vector *> &elvec)
{
   int dof_u = el1[0]->GetDof();

   elvec[0]->SetSize(dof_u*dim);
   *elvec[0] = 0.0;

   elf_u.UseExternalData(elsol[0]->GetData(), dof_u, dim);
   elv_u.UseExternalData(elvec[0]->GetData(), dof_u, dim);

   sh_u.SetSize(dof_u);

   int intorder = 2*el1[0]->GetOrder();
   const IntegrationRule &ir = IntRules.Get(Tr.GetGeometryType(), intorder);
   for (int i = 0; i < ir.GetNPoints(); i++)
   {
      const IntegrationPoint &ip = ir.IntPoint(i);

      // Set the integration point in the face and the neighboring element
      Tr.SetAllIntPoints(&ip);

      // Access the neighboring element's integration point
      const IntegrationPoint &eip = Tr.GetElement1IntPoint();

      CalcOrtho(Tr.Jacobian(), nor);

      real_t w = ip.weight * Tr.Weight();// instead???* 0.5;//

      el1[0]->CalcPhysShape(*Tr.Elem1, sh_u);
      elf_u.MultTranspose(sh_u, u);

      real_t un = u*nor;
      AddMult_a_VWt(w*un, sh_u, u, elv_u);
   }
}

// Assemble the outflow boundary gradient matrices
void IncNavStoIntegrator
::AssembleOutflowGrad(const Array<const FiniteElement *>&el1,
                      const Array<const FiniteElement *>&el2,
                      FaceElementTransformations &Tr,
                      const Array<const Vector *> &elsol,
                      const Array2D<DenseMatrix *> &elmats)
{
   int dof_u = el1[0]->GetDof();

   elf_u.UseExternalData(elsol[0]->GetData(), dof_u, dim);

   elmats(0,0)->SetSize(dof_u*dim, dof_u*dim);
   *elmats(0,0) = 0.0;

   sh_u.SetSize(dof_u);

   int intorder = 2*el1[0]->GetOrder();
   const IntegrationRule &ir = IntRules.Get(Tr.GetGeometryType(), intorder);

   for (int i = 0; i < ir.GetNPoints(); i++)
   {
      const IntegrationPoint &ip = ir.IntPoint(i);

      // Set the integration point in the face and the neighboring element
      Tr.SetAllIntPoints(&ip);

      // Access the neighboring element's integration point
      const IntegrationPoint &eip = Tr.GetElement1IntPoint();

      CalcOrtho(Tr.Jacobian(), nor);

      real_t w = ip.weight * Tr.Weight(); //
      //real_t w = ip.weight * 0.5;// instead???

      el1[0]->CalcPhysShape(*Tr.Elem1, sh_u);
      elf_u.MultTranspose(sh_u, u);

      real_t un = u*nor;

      // Momentum - Velocity block (w,u)
      for (int i_u = 0; i_u < dof_u; ++i_u)
      {
         for (int j_u = 0; j_u < dof_u; ++j_u)
         {
            real_t mat = sh_u(i_u)*sh_u(j_u)*un*w*dt;

            for (int dim_u = 0; dim_u < dim; ++dim_u)
            {
               (*elmats(0,0))(i_u + dim_u*dof_u, j_u + dim_u*dof_u) += mat;
            }
         }
      }
   }
}

// Assemble the weak Dirichlet BC boundary residual vectors
void IncNavStoIntegrator
::AssembleWeakDirBCVector(const Array<const FiniteElement *> &el1,
                          const Array<const FiniteElement *> &el2,
                          FaceElementTransformations &Tr,
                          const Array<const Vector *> &elsol,
                          const Array<Vector *> &elvec)
{
   int dof_u = el1[0]->GetDof();
   int dof_p = el1[1]->GetDof();

   elvec[0]->SetSize(dof_u*dim);
   elvec[1]->SetSize(dof_p);

   *elvec[0] = 0.0;
   *elvec[1] = 0.0;

   elf_u.UseExternalData(elsol[0]->GetData(), dof_u, dim);
   elv_u.UseExternalData(elvec[0]->GetData(), dof_u, dim);

   sh_u.SetSize(dof_u);

   int intorder = 2*el1[0]->GetOrder();
   const IntegrationRule &ir = IntRules.Get(Tr.GetGeometryType(), intorder);
   real_t tau_b, tau_n;
   for (int i = 0; i < ir.GetNPoints(); i++)
   {
      const IntegrationPoint &ip = ir.IntPoint(i);

      // Set the integration point in the face and the neighboring element
      Tr.SetAllIntPoints(&ip);

      // Access the neighboring element's integration point
      const IntegrationPoint &eip = Tr.GetElement1IntPoint();

      real_t mu = c_mu.Eval(*Tr.Elem1, eip);
      c_sol.Eval(up, *Tr.Elem1, eip);

      CalcOrtho(Tr.Jacobian(), nor);
      nor /= nor.Norml2();

      real_t w = ip.weight * Tr.Weight();

      el1[0]->CalcPhysShape(*Tr.Elem1, sh_u);
      elf_u.MultTranspose(sh_u, u);

      up -= u;
      up.Neg();
      real_t un = up*nor;

      el1[0]->CalcPhysDShape(*Tr.Elem1, shg_u);
      MultAtB(elf_u, shg_u, grad_u);
      grad_u.Symmetrize();           // Grad to strain

      el1[1]->CalcPhysShape(*Tr.Elem1, sh_p);
      real_t p = sh_p*(*elsol[1]);

      GetTauB(tau_b, tau_n, mu, u, nor,Tr);

      // Traction
      grad_u.Mult(nor, traction);
      traction *= -2*mu;             // Consistency
      traction.Add(p, nor);          // Pressure
      traction.Add(tau_b,up);        // Penalty
      traction.Add(tau_n*un,nor);    // Penalty -- normal
      AddMult_a_VWt(w, sh_u, traction, elv_u);

      // Dual consistency
      MultVWt(nor,up, flux);
      flux.Symmetrize();
      AddMult_a_ABt(-w*2*mu, shg_u, flux, elv_u);

      // Continuity
      elvec[1]->Add(w*un, sh_p);
   }
}

// Assemble the weak Dirichlet BC boundary gradient matrices
void IncNavStoIntegrator
::AssembleWeakDirBCGrad(const
                        Array<const FiniteElement *>&el1,
                        const Array<const FiniteElement *>&el2,
                        FaceElementTransformations &Tr,
                        const Array<const Vector *> &elsol,
                        const Array2D<DenseMatrix *> &elmats)
{
   int dof_u = el1[0]->GetDof();
   int dof_p = el1[1]->GetDof();

   elf_u.UseExternalData(elsol[0]->GetData(), dof_u, dim);

   elmats(0,0)->SetSize(dof_u*dim, dof_u*dim);
   elmats(0,1)->SetSize(dof_u*dim, dof_p);
   elmats(1,0)->SetSize(dof_p, dof_u*dim);
   elmats(1,1)->SetSize(dof_p, dof_p);

   *elmats(0,0) = 0.0;
   *elmats(0,1) = 0.0;
   *elmats(1,0) = 0.0;
   *elmats(1,1) = 0.0;

   sh_u.SetSize(dof_u);
   shg_u.SetSize(dof_u, dim);
   ushg_u.SetSize(dof_u);
   dupdu.SetSize(dof_u);
   sh_p.SetSize(dof_p);
   shg_p.SetSize(dof_p, dim);

   int intorder = 2*el1[0]->GetOrder();
   const IntegrationRule &ir = IntRules.Get(Tr.GetGeometryType(), intorder);
   real_t tau_b, tau_n;
   for (int i = 0; i < ir.GetNPoints(); i++)
   {
      const IntegrationPoint &ip = ir.IntPoint(i);

      // Set the integration point in the face and the neighboring element
      Tr.SetAllIntPoints(&ip);

      // Access the neighboring element's integration point
      const IntegrationPoint &eip = Tr.GetElement1IntPoint();
      real_t mu = c_mu.Eval(*Tr.Elem1, eip);
      CalcOrtho(Tr.Jacobian(), nor);
      nor /= nor.Norml2();

      real_t w = ip.weight * Tr.Weight(); //
      //real_t w = ip.weight * 0.5;// instead???

      el1[0]->CalcPhysShape(*Tr.Elem1, sh_u);
      elf_u.MultTranspose(sh_u, u);

      //  real_t un = u*nor;

      el1[0]->CalcPhysDShape(*Tr.Elem1, shg_u);
      //  MultAtB(elf_u, shg_u, grad_u);
      //   grad_u.Symmetrize();  // Grad to strain

      el1[1]->CalcPhysShape(*Tr.Elem1, sh_p);
      //  real_t p = sh_p*(*elsol[1]);

      GetTauB(tau_b, tau_n, mu, u, nor,Tr);

      /*
            // Traction
            grad_u.Mult(nor, traction);
            traction *= -2*mu;             // Consistency
            traction.Add(p, nor);          // Pressure
            traction.Add(lambda,up);       // Penalty
            traction.Add(lambda_n*un,nor); // Penalty -- normal
            AddMult_a_VWt(w, sh_u, traction, elv_u);

            // Dual consistency
            MultVWt(nor,up, flux);
            flux.Symmetrize();
            AddMult_a_ABt(-w*2*mu, shg_u, flux, elv_u);

            // Continuity
            elvec[1]->Add(w*un, sh_p);

      */

      // Momentum - Velocity block (w,u)
      for (int i_u = 0; i_u < dof_u; ++i_u)
      {
         for (int j_u = 0; j_u < dof_u; ++j_u)
         {

            for (int i_dim = 0; i_dim < dim; ++i_dim)
            {
               // Consistency 1
               for (int j_dim = 0; j_dim < dim; ++j_dim)
               {
                  (*elmats(0,0))(i_u + i_dim*dof_u, j_u + i_dim*dof_u)
                    -= mu*sh_u(i_u)*nor(j_dim)*shg_u(j_u,j_dim)*w*dt;
               }
               // Consistency 2
               for (int j_dim = 0; j_dim < dim; ++j_dim)
               {
                  (*elmats(0,0))(i_u + i_dim*dof_u, j_u + j_dim*dof_u)
                    -= mu*sh_u(i_u)*nor(j_dim)*shg_u(j_u,i_dim)*w*dt;
               }

               // Dual Consistency 1
               for (int j_dim = 0; j_dim < dim; ++j_dim)
               {
                  (*elmats(0,0))(i_u + i_dim*dof_u, j_u + i_dim*dof_u)
                    -= mu*shg_u(i_u,j_dim)*nor(j_dim)*sh_u(j_u)*w*dt;
               }
               // Dual Consistency 2
               for (int j_dim = 0; j_dim < dim; ++j_dim)
               {
                  (*elmats(0,0))(i_u + i_dim*dof_u, j_u + j_dim*dof_u)
                    -= mu*shg_u(i_u,i_dim)*nor(j_dim)*sh_u(j_u)*w*dt;
               }
            }

            // Penalty
            real_t mat = sh_u(i_u)*sh_u(j_u)*tau_b*w*dt;

            for (int dim_u = 0; dim_u < dim; ++dim_u)
            {
               (*elmats(0,0))(i_u + dim_u*dof_u, j_u + dim_u*dof_u) += mat;
            }
         }
      }

      // Momentum - Pressure block (w,p)
      for (int i_p = 0; i_p < dof_p; ++i_p)
      {
         for (int j_u = 0; j_u < dof_u; ++j_u)
         {
            for (int dim_u = 0; dim_u < dim; ++dim_u)
            {
               (*elmats(0,1))(j_u + dof_u * dim_u, i_p)
               += sh_u(j_u)*nor(dim_u)*sh_p(i_p)*w*dt;
            }
         }
      }

      // Continuity - Velocity block (q,u)
      for (int i_p = 0; i_p < dof_p; ++i_p)
      {
         for (int j_u = 0; j_u < dof_u; ++j_u)
         {
            for (int dim_u = 0; dim_u < dim; ++dim_u)
            {
               (*elmats(1,0))(i_p, j_u + dof_u * dim_u)
               += sh_p(i_p)*sh_u(j_u)*nor(dim_u)*w*dt;
            }
         }
      }
   }
}