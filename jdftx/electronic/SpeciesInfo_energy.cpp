/*-------------------------------------------------------------------
Copyright 2013 Ravishankar Sundararaman

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#include <electronic/SpeciesInfo.h>
#include <electronic/SpeciesInfo_internal.h>
#include <electronic/Everything.h>
#include <electronic/ColumnBundle.h>
#include <core/matrix.h>

//------- primary SpeciesInfo functions involved in simple energy and gradient calculations (with norm-conserving pseudopotentials) -------


//Return non-local energy and optionally accumulate its electronic and/or ionic gradients for a given quantum number
double SpeciesInfo::EnlAndGrad(const QuantumNumber& qnum, const diagMatrix& Fq, const matrix& VdagCq, matrix& HVdagCq) const
{	static StopWatch watch("EnlAndGrad"); watch.start();
	if(!atpos.size()) return 0.; //unused species
	if(!MnlAll) return 0.; //purely local psp
	int nProj = MnlAll.nRows();
	
	matrix MVdagC = zeroes(VdagCq.nRows(), VdagCq.nCols());
	double Enlq = 0.0;
	for(unsigned atom=0; atom<atpos.size(); atom++)
	{	matrix atomVdagC = VdagCq(atom*nProj,(atom+1)*nProj, 0,VdagCq.nCols());
		matrix MatomVdagC = MnlAll * atomVdagC;
		MVdagC.set(atom*nProj,(atom+1)*nProj, 0,VdagCq.nCols(), MatomVdagC);
		Enlq += trace(Fq * dagger(atomVdagC) * MatomVdagC).real();
	}
	HVdagCq += MVdagC;
	watch.stop();
	return Enlq;
}

//-------------- DFT + U --------------------

size_t SpeciesInfo::rhoAtom_nMatrices() const
{	return plusU.size() * e->eVars.n.size() * atpos.size(); //one per atom per spin channel per Uparam
}

#define rhoAtom_COMMONinit \
	int nSpins = e->eInfo.nSpins(); \
	int spinorLength = e->eInfo.spinorLength();

#define UparamLOOP(code) \
	for(auto Uparams: plusU) \
	{	int orbCount = (2*Uparams.l+1) * spinorLength; /* number of orbitals at given n,l */ \
		code \
	}

void SpeciesInfo::rhoAtom_initZero(matrix* rhoAtomPtr) const
{	rhoAtom_COMMONinit
	UparamLOOP
	(	for(int s=0; s<nSpins; s++)
			for(unsigned a=0; a<atpos.size(); a++)
				*(rhoAtomPtr++) = zeroes(orbCount,orbCount);
	)
}

void SpeciesInfo::rhoAtom_calc(const std::vector<diagMatrix>& F, const std::vector<ColumnBundle>& C, matrix* rhoAtomPtr) const
{	static StopWatch watch("rhoAtom_calc"); watch.start();
	rhoAtom_COMMONinit
	UparamLOOP
	(	int matSize = orbCount * atpos.size();
		std::vector<matrix> rho(nSpins);
		for(int q=e->eInfo.qStart; q<e->eInfo.qStop; q++)
		{	const QuantumNumber& qnum = e->eInfo.qnums[q];
			int s = qnum.index();
			ColumnBundle Opsi(C[q].similar(matSize));
			setAtomicOrbitals(Opsi, true, Uparams.n, Uparams.l);
			matrix psiOCdag = Opsi ^ C[q];
			rho[s] += (qnum.weight/e->eInfo.spinWeight) * psiOCdag * F[q] * dagger(psiOCdag);
		}
		for(int s=0; s<nSpins; s++)
		{	//Collect contributions from all processes:
			if(!rho[s]) rho[s] = zeroes(matSize, matSize);
			mpiWorld->allReduceData(rho[s], MPIUtil::ReduceSum);
			//Symmetrize:
			e->symm.symmetrizeSpherical(rho[s], this);
			//Collect density matrices per atom:
			for(unsigned a=0; a<atpos.size(); a++)
				*(rhoAtomPtr++) = rho[s](a*orbCount,(a+1)*orbCount, a*orbCount,(a+1)*orbCount);
		}
	)
	watch.stop();
}

double SpeciesInfo::rhoAtom_computeU(const matrix* rhoAtomPtr, matrix* U_rhoAtomPtr) const
{	rhoAtom_COMMONinit
	double Utot = 0.;
	UparamLOOP
	(	double Uprefac = 0.5 * Uparams.UminusJ * e->eInfo.spinWeight;
		for(int s=0; s<nSpins; s++)
			for(unsigned a=0; a<atpos.size(); a++)
			{	const matrix& rhoAtom = *(rhoAtomPtr++);
				const double VextPrefac = Uparams.Vext[a] * e->eInfo.spinWeight;
				Utot += trace((VextPrefac+Uprefac)*rhoAtom - Uprefac*(rhoAtom*rhoAtom)).real();
				*(U_rhoAtomPtr++) = ((VextPrefac+Uprefac)*eye(orbCount) - (2.*Uprefac)*rhoAtom);
			}
	)
	return Utot;
}

//Collect atomic contributions into a larger matrix (by atom and then atomic orbital):
#define U_rho_PACK \
	int matSize = orbCount * atpos.size(); \
	std::vector<matrix> U_rho(nSpins); \
	for(int s=0; s<nSpins; s++) \
	{	U_rho[s] = zeroes(matSize,matSize); \
		for(unsigned a=0; a<atpos.size(); a++) \
			U_rho[s].set(a*orbCount,(a+1)*orbCount, a*orbCount,(a+1)*orbCount, *(U_rhoAtomPtr++)); \
	}

void SpeciesInfo::rhoAtom_grad(const ColumnBundle& Cq, const matrix* U_rhoAtomPtr, ColumnBundle& HCq) const
{	static StopWatch watch("rhoAtom_grad"); watch.start();
	rhoAtom_COMMONinit
	UparamLOOP
	(	U_rho_PACK
		int s = Cq.qnum->index();
		ColumnBundle Opsi(Cq.similar(matSize));
		setAtomicOrbitals(Opsi, true, Uparams.n, Uparams.l);
		HCq += (1./e->eInfo.spinWeight) * Opsi * (U_rho[s] * (Opsi ^ Cq)); //gradient upto state weight and fillings
	)
	watch.stop();
}

//Symmetric matrix indexing used for stress calculations
static const int RRTindex[6][2] = {{0,0}, {1,1}, {2,2}, {1,2}, {2,0}, {0,1}};

void SpeciesInfo::rhoAtom_forces(const std::vector<diagMatrix>& F, const std::vector<ColumnBundle>& C, const matrix* U_rhoAtomPtr,
	std::vector<vector3<> >& forces, matrix3<>* EU_RRT) const
{	rhoAtom_COMMONinit
	UparamLOOP
	(	U_rho_PACK
		for(int q=e->eInfo.qStart; q<e->eInfo.qStop; q++)
		{	const QuantumNumber& qnum = e->eInfo.qnums[q];
			int s = qnum.index();
			ColumnBundle Opsi(C[q].similar(matSize));
			setAtomicOrbitals(Opsi, true, Uparams.n, Uparams.l);
			matrix psiOCdag = Opsi ^ C[q];
			diagMatrix fCartMat[3];
			for(int k=0; k<3; k++)
				fCartMat[k] = (1./e->eInfo.spinWeight) * diag(U_rho[s] * psiOCdag * F[q] * (C[q]^D(Opsi,k)));
			for(unsigned a=0; a<atpos.size(); a++)
			{	vector3<> fCart; //proportional to Cartesian force
				for(int k=0; k<3; k++) fCart[k] = trace(fCartMat[k](a*orbCount,(a+1)*orbCount));
				forces[a] += 2.*qnum.weight * (e->gInfo.RT * fCart);
			}
			if(EU_RRT)
				for(int ij=0; ij<6; ij++) //loop over stress components
				{	int iDir = RRTindex[ij][0];
					int jDir = RRTindex[ij][1];
					int stressDir = 3*iDir+jDir; //combined index for stress projector functions
					ColumnBundle& Opsi_RRT_ij = Opsi; //replace Opsi with Opsi_RRT_ij in-place below
					setAtomicOrbitals(Opsi_RRT_ij, true, Uparams.n, Uparams.l, 0, 0, 0, stressDir);
					double EU_RRT_ij = (2.*C[q].qnum->weight/e->eInfo.spinWeight)
						*  trace(U_rho[s] * psiOCdag * F[q] * (C[q]^Opsi_RRT_ij)).real();
					(*EU_RRT)(iDir,jDir) += EU_RRT_ij;
					if(iDir != jDir)
						(*EU_RRT)(jDir,iDir) += EU_RRT_ij;
				}
		}
	)
}

void SpeciesInfo::rhoAtom_getV(const ColumnBundle& Cq, const matrix* U_rhoAtomPtr, ColumnBundle& Opsi, matrix& M, const vector3<>* derivDir, const int stressDir) const
{	rhoAtom_COMMONinit
	int matSizeTot = 0; UparamLOOP( matSizeTot += orbCount * atpos.size(); )
	if(!matSizeTot) return;
	Opsi = Cq.similar(matSizeTot);
	M = zeroes(matSizeTot, matSizeTot);
	int matSizePrev = 0;
	UparamLOOP
	(	U_rho_PACK
		int s = Cq.qnum->index();
		setAtomicOrbitals(Opsi, true, Uparams.n, Uparams.l, matSizePrev, 0, derivDir, stressDir);
		M.set(matSizePrev,matSizePrev+matSize, matSizePrev,matSizePrev+matSize, (1./e->eInfo.spinWeight) * U_rho[s]);
		matSizePrev += matSize;
	)
}
#undef rhoAtom_COMMONinit
#undef UparamLOOP
#undef U_rho_PACK

void SpeciesInfo::updateLocal(ScalarFieldTilde& Vlocps, ScalarFieldTilde& rhoIon, ScalarFieldTilde& nChargeball,
	ScalarFieldTilde& nCore, ScalarFieldTilde& tauCore) const
{	if(!atpos.size()) return; //unused species
	((SpeciesInfo*)this)->updateLatticeDependent(); //update lattice dependent quantities (if lattice vectors have changed)
	const GridInfo& gInfo = e->gInfo;

	//Prepare optional outputs:
	complex *nChargeballData=0, *nCoreData=0, *tauCoreData=0;
	if(Z_chargeball) { nullToZero(nChargeball, gInfo); nChargeballData = nChargeball->dataPref(); }
	if(nCoreRadial) { nullToZero(nCore, gInfo); nCoreData = nCore->dataPref(); }
	if(tauCoreRadial) { nullToZero(tauCore, gInfo); tauCoreData = tauCore->dataPref(); }
	
	//Calculate in half G-space:
	double invVol = 1.0/gInfo.detR;
	callPref(::updateLocal)(gInfo.S, gInfo.GGT,
		Vlocps->dataPref(), rhoIon->dataPref(), nChargeballData, nCoreData, tauCoreData,
		atpos.size(), atposManaged.dataPref(), invVol, VlocRadial,
		Z, nCoreRadial, tauCoreRadial, Z_chargeball, std::pow(width_chargeball,2));
}


std::vector< vector3<double> > SpeciesInfo::getLocalForces(const ScalarFieldTilde& ccgrad_Vlocps,
	const ScalarFieldTilde& ccgrad_rhoIon, const ScalarFieldTilde& ccgrad_nChargeball,
	const ScalarFieldTilde& ccgrad_nCore, const ScalarFieldTilde& ccgrad_tauCore) const
{	
	if(!atpos.size()) return std::vector< vector3<double> >(); //unused species, return empty forces
	
	const GridInfo& gInfo = e->gInfo;
	complex* ccgrad_rhoIonData = ccgrad_rhoIon ? ccgrad_rhoIon->dataPref() : 0;
	complex* ccgrad_nChargeballData = (Z_chargeball && ccgrad_nChargeball) ? ccgrad_nChargeball->dataPref() : 0;
	complex* ccgrad_nCoreData = nCoreRadial ? ccgrad_nCore->dataPref() : 0;
	complex* ccgrad_tauCoreData = (tauCoreRadial && ccgrad_tauCore) ? ccgrad_tauCore->dataPref() : 0;
	
	//Propagate ccgrad* to gradient w.r.t structure factor:
	ScalarFieldTilde ccgrad_SG(ScalarFieldTildeData::alloc(gInfo, isGpuEnabled())); //complex conjugate gradient w.r.t structure factor
	callPref(gradLocalToSG)(gInfo.S, gInfo.GGT,
		ccgrad_Vlocps->dataPref(), ccgrad_rhoIonData, ccgrad_nChargeballData,
		ccgrad_nCoreData, ccgrad_tauCoreData, ccgrad_SG->dataPref(), VlocRadial,
		Z, nCoreRadial, tauCoreRadial, Z_chargeball, std::pow(width_chargeball,2));
	
	//Now propagate that gradient to each atom of this species:
	VectorFieldTilde gradAtpos; nullToZero(gradAtpos, gInfo);
	vector3<complex*> gradAtposData; for(int k=0; k<3; k++) gradAtposData[k] = gradAtpos[k]->dataPref();
	std::vector< vector3<> > forces(atpos.size());
	for(unsigned at=0; at<atpos.size(); at++)
	{	callPref(gradSGtoAtpos)(gInfo.S, atpos[at], ccgrad_SG->dataPref(), gradAtposData);
		for(int k=0; k<3; k++)
			forces[at][k] = -sum(gradAtpos[k]); //negative gradient
	}
	return forces;
}

matrix3<> SpeciesInfo::getLocalStress(const ScalarFieldTilde& ccgrad_Vlocps,
	const ScalarFieldTilde& ccgrad_rhoIon, const ScalarFieldTilde& ccgrad_nChargeball,
	const ScalarFieldTilde& ccgrad_nCore, const ScalarFieldTilde& ccgrad_tauCore) const
{
	if(!atpos.size()) return matrix3<>(); //unused species, no stress
	
	const GridInfo& gInfo = e->gInfo;
	complex* ccgrad_rhoIonData = ccgrad_rhoIon ? ccgrad_rhoIon->dataPref() : 0;
	complex* ccgrad_nChargeballData = (Z_chargeball && ccgrad_nChargeball) ? ccgrad_nChargeball->dataPref() : 0;
	complex* ccgrad_nCoreData = nCoreRadial ? ccgrad_nCore->dataPref() : 0;
	complex* ccgrad_tauCoreData = (tauCoreRadial && ccgrad_tauCore) ? ccgrad_tauCore->dataPref() : 0;
	
	//Propagate ccgrad* to lattice derivative:
	ManagedArray<symmetricMatrix3<>> result; result.init(gInfo.nG, isGpuEnabled());
	callPref(gradLocalToStress)(gInfo.S, gInfo.GGT,
		ccgrad_Vlocps->dataPref(), ccgrad_rhoIonData, ccgrad_nChargeballData,
		ccgrad_nCoreData, ccgrad_tauCoreData, result.dataPref(), atpos.size(), atposManaged.dataPref(),
		VlocRadial, Z, nCoreRadial, tauCoreRadial, Z_chargeball, std::pow(width_chargeball,2));
	matrix3<> resultSum = callPref(eblas_sum)(gInfo.nG, result.dataPref());
	return gInfo.GT * resultSum * gInfo.G;
}

void SpeciesInfo::accumNonlocalForces(const ColumnBundle& Cq, const matrix& VdagC, const matrix& E_VdagC, const matrix& grad_CdagOCq,
	std::vector<vector3<>>& forces, matrix3<>* Enl_RRT) const
{
	//Cartesian gradient of VdagC:
	matrix DVdagC[3]; 
	{	auto V = getV(Cq);
		for(int k=0; k<3; k++)
			DVdagC[k] = D(*V,k)^Cq;
	}
	//Lattice derivative of VdagC
	matrix VdagC_RRT[6]; 
	if(Enl_RRT)
	{	for(int ij=0; ij<6; ij++) //loop over stress components
		{	int iDir = RRTindex[ij][0];
			int jDir = RRTindex[ij][1];
			int stressDir = 3*iDir+jDir; //combined index for stress projector functions
			VdagC_RRT[ij] = (*getV(Cq, 0, stressDir)) ^ Cq;
		}
	}
	int nProj = MnlAll.nRows();
	//Loop over atoms:
	for(unsigned atom=0; atom<atpos.size(); atom++)
	{	matrix atomVdagC = VdagC(atom*nProj,(atom+1)*nProj, 0,E_VdagC.nCols());
		matrix E_atomVdagC = E_VdagC(atom*nProj,(atom+1)*nProj, 0,E_VdagC.nCols());
		if(QintAll) E_atomVdagC += QintAll * atomVdagC * grad_CdagOCq; //Contribution via overlap augmentation
		
		vector3<> fCart; //proportional to cartesian force
		for(int k=0; k<3; k++)
		{	matrix atomDVdagC = DVdagC[k](atom*nProj,(atom+1)*nProj, 0,E_VdagC.nCols());
			fCart[k] = trace(E_atomVdagC * dagger(atomDVdagC)).real();
		}
		forces[atom] += 2.*Cq.qnum->weight * (e->gInfo.RT * fCart);
		
		if(Enl_RRT)
		{	for(int ij=0; ij<6; ij++) //loop over stress components
			{	int iDir = RRTindex[ij][0];
				int jDir = RRTindex[ij][1];
				matrix atomVdagC_RRT = VdagC_RRT[ij](atom*nProj,(atom+1)*nProj, 0,E_VdagC.nCols());
				double Enl_RRT_ij = 2.*Cq.qnum->weight * trace(E_atomVdagC * dagger(atomVdagC_RRT)).real();
				(*Enl_RRT)(iDir,jDir) += Enl_RRT_ij;
				if(iDir != jDir)
					(*Enl_RRT)(jDir,iDir) += Enl_RRT_ij;
			}
		}
	}
}

std::shared_ptr<ColumnBundle> SpeciesInfo::getV(const ColumnBundle& Cq, const vector3<>* derivDir, const int stressDir) const
{	const QuantumNumber& qnum = *(Cq.qnum);
	const Basis& basis = *(Cq.basis);
	std::pair<vector3<>,const Basis*> cacheKey = std::make_pair(qnum.k, &basis);
	int nProj = MnlAll.nRows() / e->eInfo.spinorLength();
	if(!nProj) return 0; //purely local psp
	//First check cache
	if(e->cntrl.cacheProjectors && (!derivDir) && (!stressDir))
	{	auto iter = cachedV.find(cacheKey);
		if(iter != cachedV.end()) //found
			return iter->second; //return cached value
	}
	//No cache / not found in cache; compute:
	std::shared_ptr<ColumnBundle> V = std::make_shared<ColumnBundle>(nProj*atpos.size(), basis.nbasis, &basis, &qnum, isGpuEnabled()); //not a spinor regardless of spin type
	int iProj = 0;
	for(int l=0; l<int(VnlRadial.size()); l++)
		for(unsigned p=0; p<VnlRadial[l].size(); p++)
			for(int m=-l; m<=l; m++)
			{	size_t offs = iProj * basis.nbasis;
				size_t atomStride = nProj * basis.nbasis;
				callPref(Vnl)(basis.nbasis, atomStride, atpos.size(), l, m, qnum.k, basis.iGarr.dataPref(),
					basis.gInfo->G, atposManaged.dataPref(), VnlRadial[l][p], V->dataPref()+offs, derivDir, stressDir);
				iProj++;
			}
	//Add to cache if necessary:
	if(e->cntrl.cacheProjectors && (!derivDir) && (!stressDir))
		((SpeciesInfo*)this)->cachedV[cacheKey] = V;
	return V;
}
