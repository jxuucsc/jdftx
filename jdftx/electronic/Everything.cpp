/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman

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

#include <electronic/Everything.h>
#include <electronic/ExactExchange.h>
#include <electronic/VanDerWaalsD3.h>
#include <electronic/Vibrations.h>
#include <electronic/DOS.h>
#include <electronic/DumpBGW_internal.h>
#include <core/LatticeUtils.h>
#include <fluid/FluidSolver.h>

void Everything::setup()
{
	//Symmetries (phase 1: lattice+basis dependent)
	if(vibrations)
	{	symmUnperturbed = symm;
		symm.mode = SymmetriesNone; //disable symmetries in remainder of calculation
		symmUnperturbed.setup(*this); //calculate symmetries of unperturbed system for optimizing force matrix calculation
	}
	symm.setup(*this);
	
	//Initialize the grid:
	gInfo.Gmax = sqrt(2*cntrl.Ecut); //Ecut = 0.5 Gmax^2
	gInfo.GmaxRho = sqrt(2*cntrl.EcutRho); //Ecut = 0.5 Gmax^2
	gInfo.initialize(false, vibrations ? symmUnperturbed.getMatrices() : symm.getMatrices());
	if(cntrl.EcutRho && cntrl.EcutRho>4*cntrl.Ecut)
	{	gInfoWfns = std::make_shared<GridInfo>();
		gInfoWfns->R = gInfo.R;
		gInfoWfns->Gmax = gInfo.Gmax;
		logPrintf("\n---------- Initializing tighter grid for wavefunction operations ----------\n");
		gInfoWfns->initialize(true, vibrations ? symmUnperturbed.getMatrices() : symm.getMatrices());
		if(gInfoWfns->S == gInfo.S)
		{	logPrintf("Disabling tighter grid as its sample count matches original.\n");
			gInfoWfns = 0;
		}
	}

	//Exchange correlation setup
	logPrintf("\n---------- Exchange Correlation functional ----------\n");
	exCorr.setup(*this); //main functional
	if(exCorrDiff.size())
	{	logPrintf("\n---------- Auxiliary functionals for error estimation ----------\n");
		for(auto ec: exCorrDiff)
			ec->setup(*this); //comparison functionals evaluated at the end
	}

	//Atom positions, pseudopotentials
	iInfo.setup(*this);
	
	//Symmetries (phase 2: fftbox and k-mesh dependent)
	eInfo.kpointsFold();
	symm.setupMesh();
	if(vibrations) symmUnperturbed.setupMesh();
	
	//Set up k-points, bands and fillings
	eInfo.setup(*this, eVars.F, ener);

	//Set up the reduced bases for wavefunctions:
	logPrintf("\n----- Setting up reduced wavefunction bases (%s) -----\n",
		(cntrl.basisKdep==BasisKpointIndep) ? "single at Gamma point" :  "one per k-point");
	basis.resize(eInfo.nStates);
	double avg_nbasis = 0.;
	const GridInfo& gInfoBasis = gInfoWfns ? *gInfoWfns : gInfo;
	if(!cntrl.shouldPrintKpointsBasis) logSuspend();
	for(int q=0; q<eInfo.nStates; q++)
	{	if(cntrl.basisKdep==BasisKpointDep)
			basis[q].setup(gInfoBasis, iInfo, cntrl.Ecut, eInfo.qnums[q].k);
		else
		{	if(q==0) basis[q].setup(gInfoBasis, iInfo, cntrl.Ecut, vector3<>(0,0,0));
			else basis[q] = basis[0];
		}
		avg_nbasis += eInfo.qnums[q].weight * basis[q].nbasis;
	}
	avg_nbasis /= eInfo.qWeightSum;
	if(!cntrl.shouldPrintKpointsBasis) logResume();
	logPrintf("average nbasis = %7.3lf , ideal nbasis = %7.3lf\n", avg_nbasis,
		pow(sqrt(2*cntrl.Ecut),3)*(gInfo.detR/(6*M_PI*M_PI)));
	logFlush();

	//Check if DOS calculator is needed:
	if(!dump.dos)
	{	for(auto dumpPair: dump)
			if(dumpPair.second == DumpDOS)
			{	dump.dos = std::make_shared<DOS>();
				break;
			}
	}
	
	//Check exact exchange requirements:
	if(exCorr.exxFactor()) //Check main functional first
		coulombParams.omegaSet.insert(exCorr.exxRange());
	for(auto ec: exCorrDiff) //Check the comparison functionals next
		if(ec->exxFactor())
			coulombParams.omegaSet.insert(ec->exxRange());
	if(dump.bgwParams and (dump.bgwParams->saveVxx or dump.bgwParams->rpaExx))
		coulombParams.omegaSet.insert(0.); //bare exchange output for BGW
	bool exxPresent = coulombParams.omegaSet.size();
	if(dump.polarizability || dump.electronScattering) coulombParams.omegaSet.insert(0.); //These are not EXX, but they use Coulomb_ExchangeEval
	for(const auto& entry: dump) if(entry.second==DumpFCI) coulombParams.omegaSet.insert(0.); //DumpFCI also uses Coulomb_ExchangeEval
	
	//Coulomb-interaction setup (with knowledge of exact-exchange requirements):
	updateSupercell();
	coulombParams.computeStress = iInfo.computeStress; //some Coulomb modes need extra initialization for stress
	coulomb = coulombParams.createCoulomb(gInfo, gInfoWfns, coulombWfns);
	
	//Exact exchange (if required)
	if(exxPresent)
		exx = std::make_shared<ExactExchange>(*this);

	//Setup VanDerWaals corrections
	//--- for electronic system:
	if(iInfo.vdWenable or iInfo.ljOverride)
	{	switch(iInfo.vdWstyle)
		{	case VDW_D2: { vanDerWaals = std::make_shared<VanDerWaalsD2>(*this); break; }
			case VDW_D3: { vanDerWaals = std::make_shared<VanDerWaalsD3>(*this); break; }
		}
	}
	//--- for fluid (must be D2):
	if(eVars.fluidParams.needsVDW())
	{	if(iInfo.vdWenable and (iInfo.vdWstyle == VDW_D2))
			vanDerWaalsFluid = std::static_pointer_cast<VanDerWaalsD2>(vanDerWaals); //reuse D2 created above
		else
			vanDerWaalsFluid = std::make_shared<VanDerWaalsD2>(*this, "fluid / solvation");
	}
	if(iInfo.ljOverride) eVars.skipWfnsInit = true; //don't need electronic degrees of freedom
	
	//Setup wavefunctions, densities, fluid, output module etc:
	iInfo.update(ener); //needs to happen before eVars setup for LCAO
	eVars.setup(*this);
	dump.setup(*this);

	//Setup vibrations module:
	if(vibrations) vibrations->setup(this);
	
	//Setup electronic minimization parameters:
	elecMinParams.nDim = 0;
	for(int q=eInfo.qStart; q<eInfo.qStop; q++)
	{	elecMinParams.nDim += 2 * basis[q].nbasis * eInfo.nBands;
		if(eInfo.fillingsUpdate==ElecInfo::FillingsHsub)
			elecMinParams.nDim += eInfo.nBands * eInfo.nBands;
	}
	mpiWorld->allReduce(elecMinParams.nDim, MPIUtil::ReduceSum);
	elecMinParams.fpLog = globalLog;
	elecMinParams.linePrefix = "ElecMinimize: ";
	elecMinParams.energyLabel = relevantFreeEnergyName(*this);
	elecMinParams.energyFormat = "%+.15lf";
	
	//Setup ionic minimization parameters:
	ionicMinParams.nDim = 0;
	for(auto sp: iInfo.species)
		for(unsigned at=0; at<sp->atpos.size(); at++)
			ionicMinParams.nDim += sp->constraints[at].getDimension();
	if(!ionicMinParams.nDim) ionicMinParams.nDim = 1;
	ionicMinParams.fpLog = globalLog;
	ionicMinParams.linePrefix = "IonicMinimize: ";
	ionicMinParams.energyLabel = relevantFreeEnergyName(*this);
	ionicMinParams.energyFormat = "%+.15lf";
	
	//Setup fluid minimization parameters:
	switch(eVars.fluidParams.fluidType)
	{	case FluidLinearPCM:
		case FluidSaLSA:
			fluidMinParams.nDim = gInfo.nr; break;
		case FluidNonlinearPCM: fluidMinParams.nDim = 4 * gInfo.nr; break;
		case FluidClassicalDFT:
			fluidMinParams.nDim = 4 * gInfo.nr; 
			break;
		default:
			fluidMinParams.nDim = 0;
	}
	fluidMinParams.fpLog = globalLog;
	fluidMinParams.energyLabel = relevantFreeEnergyName(*this);
	fluidMinParams.energyFormat = "%+.15lf";
	if(eVars.fluidSolver && eVars.fluidSolver->useGummel())
	{	fluidMinParams.linePrefix = "FluidMinimize: ";
		eVars.fluidParams.scfParams.linePrefix = "NonlinearFluidSCF: ";
	}
	else //indent for inner minimization:
	{	fluidMinParams.linePrefix = "\tFluidMinimize: ";
		eVars.fluidParams.scfParams.linePrefix = "\tNonlinearFluidSCF: ";
		//Disable inner iterations for linear solvers:
		if(!eVars.fluidParams.verboseLog
			&& (eVars.fluidParams.fluidType==FluidLinearPCM
			 || eVars.fluidParams.fluidType==FluidSaLSA) )
			fluidMinParams.fpLog = nullLog;
	}
	
	//Setup lattice minimization parameters:
	latticeMinParams.fpLog = globalLog;
	latticeMinParams.linePrefix = "LatticeMinimize: ";
	latticeMinParams.energyLabel = relevantFreeEnergyName(*this);
	latticeMinParams.energyFormat = "%+.15lf";

	logPrintf("\n"); logFlush();
}


void Everything::updateSupercell(bool force)
{	if(force || coulombParams.omegaSet.size() || dump.dos || dump.electronScattering)
	{	//Initialize k-point sampled supercell:
		std::vector<vector3<>> kmeshReduced;
		for(const QuantumNumber& qnum: eInfo.qnums)
			kmeshReduced.push_back(qnum.k);
		coulombParams.supercell = std::make_shared<Supercell>(gInfo, kmeshReduced, symm.getMatrices(), symm.getKpointInvertList());
	}
}

