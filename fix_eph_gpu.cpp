/*
 * Authors of the extension Artur Tamm, Alfredo Correa
 * e-mail: artur.tamm.work@gmail.com
 */

#ifdef FIX_EPH_GPU

// external headers
#include <mpi.h>
#include <iostream>
#include <cstdio>
#include <algorithm>
#include <cstring>

// lammps headers
#include "error.h"
#include "domain.h"
#include "neighbor.h"
#include "neigh_request.h"
#include "neigh_list.h"
#include "atom.h"
#include "memory.h"
#include "random_mars.h"
#include "force.h"
#include "update.h"
#include "comm.h"

// internal headers
#include "fix_eph.h"
#include "fix_eph_gpu.h"
#include "eph_gpu.h"

using namespace LAMMPS_NS;
using namespace FixConst;

// constructor
FixEPHGPU::FixEPHGPU(LAMMPS *lmp, int narg, char **arg) :
  FixEPH(lmp, narg, arg) 
{
  eph_gpu = allocate_EPH_GPU(beta, types, type_map);
  eph_gpu.groupbit = groupbit;
  
  eph_gpu.neighmask = NEIGHMASK;
  eph_gpu.eta_factor = 0;
  
  nmax = 0;
  number_neigh = nullptr;
  index_neigh = nullptr;
  
  n_neighs = 0;
  neighs = nullptr;
  
  grow_arrays(atom->nmax);
}

// destructor
FixEPHGPU::~FixEPHGPU() 
{
  deallocate_EPH_GPU(eph_gpu);
  
  if(number_neigh != nullptr) delete[] number_neigh;
  if(index_neigh != nullptr) delete[] index_neigh;
  if(neighs != nullptr) delete[] neighs;
}

void FixEPHGPU::grow_arrays(int ngrow) 
{
  FixEPH::grow_arrays(ngrow);
  
  eph_gpu.grow(ngrow);
  
  if(ngrow > nmax) 
  {
    if(number_neigh != nullptr) delete[] number_neigh;
    if(index_neigh != nullptr) delete[] index_neigh;
    
    number_neigh = new int[ngrow];
    index_neigh = new int[ngrow];
    
    nmax = ngrow;
  }
}

void FixEPHGPU::reset_dt()
{
  FixEPH::reset_dt();
  
  eph_gpu.eta_factor = eta_factor;
}

void FixEPHGPU::post_force(int)
{
  double **x = atom->x;
  double **v = atom->v;
  double **f = atom->f;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;
  int nghost = atom->nghost;
  
  // update numbers
  eph_gpu.nlocal = nlocal;
  eph_gpu.nghost = nghost;
  
  int ntotal = nlocal + nghost;
  
  eph_gpu.eta_factor = eta_factor;
  
  // push coordinates to GPU this is ugly
  cpu_to_device_EPH_GPU((void*) eph_gpu.x_gpu, (void*) x[0], 3*ntotal*sizeof(double));
  cpu_to_device_EPH_GPU((void*) eph_gpu.type_gpu, (void*) type, ntotal*sizeof(int));
  cpu_to_device_EPH_GPU((void*) eph_gpu.mask_gpu, (void*) mask, ntotal*sizeof(int));
  
  transfer_neighbour_list(); // send neighbour list to gpu
  
  //zero all arrays
  // TODO: remove these in the future
  std::fill_n(&(w_i[0][0]), 3 * nlocal, 0);
  std::fill_n(&(xi_i[0][0]), 3 * nlocal, 0);
  
  std::fill_n(&(f_EPH[0][0]), 3 * nlocal, 0);
  std::fill_n(&(f_RNG[0][0]), 3 * nlocal, 0);
  
  zero_data_gpu(eph_gpu);
  
  // generate random forces and distribute them
  // push this into gpu
  if(eph_flag & Flag::RANDOM) {
    for(size_t i = 0; i < nlocal; ++i) {
      if(mask[i] & groupbit) {
        xi_i[i][0] = random->gaussian();
        xi_i[i][1] = random->gaussian();
        xi_i[i][2] = random->gaussian();
      }
    }
    
    state = FixState::XIX;
    comm->forward_comm_fix(this);
    state = FixState::XIY;
    comm->forward_comm_fix(this);
    state = FixState::XIZ;
    comm->forward_comm_fix(this);
  }
  
  cpu_to_device_EPH_GPU((void*) eph_gpu.xi_i_gpu, (void*) xi_i[0], 3*ntotal*sizeof(double));
  
  // calculate site densities
  calculate_environment();
  
  device_to_cpu_EPH_GPU((void*) rho_i, (void*) eph_gpu.rho_i_gpu, nlocal*sizeof(double));
  
  state = FixState::RHO;
  comm->forward_comm_fix(this);
  
  // TODO: transfer only necessary parts
  cpu_to_device_EPH_GPU((void*) (eph_gpu.rho_i_gpu+nlocal), (void*) (rho_i+nlocal), nghost*sizeof(double));
  
  /* 
   * we have separated the model specific codes to make it more readable 
   * at the expense of code duplication 
   */
  
  // get temperatures this will be pushed to gpu
  for(int i = 0; i != ntotal; ++i)
  {
    T_e_i[i] = fdm.get_T(x[i][0], x[i][1], x[i][2]);
  }
  
  cpu_to_device_EPH_GPU((void*) eph_gpu.v_gpu, (void*) v[0], 3*ntotal*sizeof(double));
  cpu_to_device_EPH_GPU((void*) eph_gpu.T_e_i_gpu, (void*) T_e_i, nlocal*sizeof(double));
  
  switch(eph_model) {
    case Model::TTM: force_ttm();
      break;
    case Model::PRB: force_prb();
      break;
    case Model::PRLCM: force_prlcm();
      break;
    case Model::PRL: force_prl();
      break;
    case Model::TESTING: force_testing();
      break;
    default: throw;
  }
  
  device_to_cpu_EPH_GPU((void*) f_EPH[0], (void*) eph_gpu.f_EPH_gpu, 3*nlocal*sizeof(double));
  device_to_cpu_EPH_GPU((void*) f_RNG[0], (void*) eph_gpu.f_RNG_gpu, 3*nlocal*sizeof(double));
  
  // second loop over atoms if needed
  if((eph_flag & Flag::FRICTION) && !(eph_flag & Flag::NOFRICTION)) {
    for(int i = 0; i < nlocal; i++) {
      f[i][0] += f_EPH[i][0];
      f[i][1] += f_EPH[i][1];
      f[i][2] += f_EPH[i][2];
    }
  }
  
  if((eph_flag & Flag::RANDOM) && !(eph_flag & Flag::NORANDOM)) {
    for(int i = 0; i < nlocal; i++) {
      f[i][0] += f_RNG[i][0];
      f[i][1] += f_RNG[i][1];
      f[i][2] += f_RNG[i][2];
    }
  }
}

void FixEPHGPU::calculate_environment()
{
  calculate_environment_gpu(eph_gpu);
}

void FixEPHGPU::force_prl()
{
  int nlocal = atom->nlocal;
  int nghost = atom->nghost;
  
  // update numbers
  eph_gpu.nlocal = nlocal;
  eph_gpu.nghost = nghost;
  
  int ntotal = nlocal + nghost;
  
  force_prl_stage1_gpu(eph_gpu);
  
  device_to_cpu_EPH_GPU((void*) w_i[0], (void*) eph_gpu.w_i_gpu, 3*ntotal*sizeof(double));
  
  state = FixState::WX;
  comm->forward_comm_fix(this);
  state = FixState::WY;
  comm->forward_comm_fix(this);
  state = FixState::WZ;
  comm->forward_comm_fix(this);
  
  // TODO: copy only ghost values
  cpu_to_device_EPH_GPU((void*) eph_gpu.w_i_gpu, (void*) w_i[0], 3*ntotal*sizeof(double));
  
  force_prl_stage2_gpu(eph_gpu);
  force_prl_stage3_gpu(eph_gpu);
}

void FixEPHGPU::transfer_neighbour_list()
{
  int nlocal = atom->nlocal;
  int *numneigh = list->numneigh;
  int **firstneigh = list->firstneigh;
  
  size_t n = 0;
  for(int i = 0; i < nlocal; ++i)
  {
    number_neigh[i] = numneigh[i];
    index_neigh[i] = n;
    n += numneigh[i];
  }
  
  if(n > n_neighs)
  {
    if(neighs == nullptr) delete[] neighs;
    n_neighs = n;
    neighs = new int[n];
  }
  
  // maybe this could be avoided
  for(int i = 0; i < nlocal; ++i)
  {
    std::copy_n(firstneigh[i], numneigh[i], neighs + index_neigh[i]);
  }
  
  eph_gpu.grow_neigh(n); // grow array if needed
  cpu_to_device_EPH_GPU(eph_gpu.number_neigh_gpu, number_neigh, nlocal*sizeof(int));
  cpu_to_device_EPH_GPU(eph_gpu.index_neigh_gpu, index_neigh, nlocal*sizeof(int));
  cpu_to_device_EPH_GPU(eph_gpu.neighs_gpu, neighs, n_neighs*sizeof(int));
  
  //create_neighbour_list_gpu(eph_gpu);
  
  
  /*
  double t09 = MPI_Wtime();
  std::cout << "TIMING: " << std::fixed << t08 - t01 << '\n';
  */
}

#endif

