/*
 * @BEGIN LICENSE
 *
 * ivocalc by Psi4 Developer, a plugin to:
 *
 * Psi4: an open-source quantum chemistry software package
 *
 * Copyright (c) 2007-2018 The Psi4 Developers.
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This file is part of Psi4.
 *
 * Psi4 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * Psi4 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along
 * with Psi4; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * @END LICENSE
 */
#include "psi4/libmints/vector.h" 
#include "psi4/libmints/matrix.h" 
#include "psi4/libmints/molecule.h"
#include "psi4/libmints/basisset.h" 
#include "psi4/libmints/wavefunction.h"
#include "psi4/liboptions/liboptions.h"
#include "psi4/libpsi4util/PsiOutStream.h"
#include "psi4/libpsi4util/process.h"
#include "psi4/libpsio/psio.hpp"
#include "psi4/psi4-dec.h"
#include "psi4/psifiles.h"
#include "embedding.h"
#include "forte_options.h"

namespace psi{ namespace forte {

void set_EMBEDDING_options(ForteOptions& foptions) {
	foptions.add_str("CUTOFF_BY", "THRESHOLD", "dummy");
	foptions.add_int("NUM_OCC", 0, "dummy");
	foptions.add_int("NUM_VIR", 0, "dummy");
	foptions.add_double("THRESHOLD", 0.5, "dummy");
	foptions.add_str("REFERENCE", "HF", "HF, UHF, ROHF, MCSCF, CASSCF, DFT");
	foptions.add_bool("WRITE_FREEZE_MO", true, "dummy");
	foptions.add_bool("SEMICANON", true, "dummy");
}

embedding::embedding(SharedWavefunction ref_wfn, Options& options,
                     std::shared_ptr<MOSpaceInfo> mo_space_info)
    : Wavefunction(options), mo_space_info_(mo_space_info) {
	outfile->Printf("\n ------ Orbital Localization and Embedding ------ \n");
	shallow_copy(ref_wfn);
	ref_wfn_ = ref_wfn;

	//1. Get necessary information
	print = options.get_int("PRINT");
	thresh = options.get_double("THRESHOLD");
	num_occ = options.get_int("NUM_OCC");
	num_vir = options.get_int("NUM_VIR");

	std::shared_ptr<PSIO> psio(_default_psio_lib_);
	if (!ref_wfn)
		throw PSIEXCEPTION("SCF has not been run yet!");
}

embedding::~embedding() {}

double embedding::compute_energy() {
	std::shared_ptr<Molecule> mol = ref_wfn_->molecule();
	int nfrag = mol->nfragments();

	if (nfrag == 1) {
		outfile->Printf("Warning! A input molecule with fragments (--- in atom list) is required "
			"for embedding!");
	}

	outfile->Printf(
		"\n The input molecule have %d fragments, assigning the first fragment as system! \n",
		nfrag);

	std::vector<int> none_list = {};
	std::vector<int> sys_list = { 0 };
	std::vector<int> env_list = { 1 }; // change to 1-nfrag in the future!

	std::shared_ptr<Molecule> mol_sys = mol->extract_subsets(sys_list, none_list);
	std::shared_ptr<Molecule> mol_env = mol->extract_subsets(env_list, none_list);
	outfile->Printf("\n System Fragment \n");
	mol_sys->print();
	outfile->Printf("\n Environment Fragment(s) \n");
	mol_env->print();

	std::shared_ptr<BasisSet> basis = ref_wfn_->basisset();
	Dimension nmopi = ref_wfn_->nmopi();
	int nirrep = ref_wfn_->nirrep();
	int nbf = basis->nbf();
	outfile->Printf("\n number of basis on all atoms: %d", nbf);

	int natom_sys = mol_sys->natom();
	int count_basis = 0;
	for (int mu = 0; mu < nbf; mu++) {
		int A = basisset_->function_to_center(mu);
		outfile->Printf("\n  Function %d is on atom %d", mu, A);
		if (A < natom_sys) {
			count_basis += 1;
		}
	}
	outfile->Printf("\n number of basis on \"system\" atoms: %d", count_basis);


	//Naive partition
	SharedMatrix S_ao = ref_wfn_->S();

	outfile->Printf("\n Orbital test 1: \n");
	SharedMatrix S_test_1(S_ao->clone());
	S_test_1->transform(Ca_);
	S_test_1->print();

	Dimension noccpi = ref_wfn_->doccpi();
	Dimension zeropi = nmopi - nmopi;
	Dimension nvirpi = nmopi - noccpi;
	Dimension sys_mo = nmopi;
	int num_actv = 0;
	int num_rdocc = 0;
	int num_docc = 0;
	Dimension actv_a = zeropi;
	Dimension res_docc_ori = zeropi;
	Dimension docc_ori = zeropi;

	if (options_.get_str("REFERENCE") == "CASSCF") {

		num_actv = options_["ACTIVE"][0].to_integer();
		num_rdocc = options_["RESTRICTED_DOCC"][0].to_integer();
		num_docc = options_["DOCC"][0].to_integer();

		actv_a[0] = num_actv;
		res_docc_ori[0] = num_rdocc;
		docc_ori[0] = num_docc;

		//Change the following part when symmetry is applied
	}

	int num_actv_docc = num_docc - num_rdocc;
	int num_actv_vir = num_actv - num_actv_docc;
	outfile->Printf("The reference has %d active occupied, %d active virtual, will be assigned to A (without any change)", num_actv_docc, num_actv_vir);
	sys_mo[0] = count_basis;

	Dimension nroccpi = noccpi;
	nroccpi[0] = noccpi[0] - num_actv_docc;
	Dimension nrvirpi = noccpi;
	nrvirpi[0] = nvirpi[0] - num_actv_vir;

	Slice sys(zeropi, sys_mo);
	Slice env(sys_mo, nmopi);
	Slice allmo(zeropi, nmopi);
	Slice occ(zeropi, nroccpi);
	Slice vir(nroccpi + actv_a, nmopi);
	Slice actv(nroccpi, nroccpi + actv_a);

	SharedMatrix S_sys = S_ao->get_block(sys, sys);

	//Construct S_sys^-1 in full size
	S_sys->general_invert();
	SharedMatrix S_sys_in_all(new Matrix("S system in fullsize", nirrep, nmopi, nmopi));
	S_sys_in_all->set_block(sys, sys, S_sys);

	//Build P_pq
	SharedMatrix Ca_t = ref_wfn_->Ca();
	S_sys_in_all->transform(S_ao);
	S_sys_in_all->transform(Ca_t);

	//Diagonalize P_pq for occ and vir part, respectively.
	SharedMatrix P_oo = S_sys_in_all->get_block(occ, occ);
	SharedMatrix Uo(new Matrix("Uo", nirrep, nroccpi, nroccpi));
	SharedVector lo(new Vector("lo", nirrep, nroccpi));
	P_oo->diagonalize(Uo, lo, descending);
	lo->print();

	SharedMatrix P_vv = S_sys_in_all->get_block(vir, vir);
	SharedMatrix Uv(new Matrix("Uv", nirrep, nrvirpi, nrvirpi));
	SharedVector lv(new Vector("lv", nirrep, nrvirpi));
	P_vv->diagonalize(Uv, lv, descending);
	lv->print();

	SharedMatrix U_all(new Matrix("U with Pab", nirrep, nmopi, nmopi));
	U_all->set_block(occ, occ, Uo);
	U_all->set_block(vir, vir, Uv);

	//Based on threshold or num_occ/num_vir, decide the mo_space_info
	std::vector<int> index_trace_occ = {};
	std::vector<int> index_trace_vir = {};
	if (options_.get_str("CUTOFF_BY") == "THRESHOLD") {
		for (int i = 0; i < noccpi[0]; i++) {
			if (lo->get(0, i) > thresh) {
				index_trace_occ.push_back(i);
				outfile->Printf("\n Occupied orbital %d is partitioned to A with eigenvalue %8.8f", i, lo->get(0, i));
			}
		}
		for (int i = 0; i < nvirpi[0]; i++) {
			if (lv->get(0, i) > thresh) {
				index_trace_vir.push_back(i);
				outfile->Printf("\n Virtual orbital %d is partitioned to A with eigenvalue %8.8f", i, lv->get(0, i));
			}
		}
	}
	
	if (options_.get_str("CUTOFF_BY") == "NUMBER") {
		for (int i = 0; i < num_occ; i++) {
			index_trace_occ.push_back(i);
			outfile->Printf("\n Occupied orbital %d is partitioned to A with eigenvalue %8.8f", i, lo->get(0, i));
		}
		for (int i = 0; i < num_vir; i++) {
			index_trace_vir.push_back(i);
			outfile->Printf("\n Occupied orbital %d is partitioned to A with eigenvalue %8.8f", i, lv->get(0, i));
		}
	}

	//Rotate Bocc to first block, rotate Bvir to last block, in order to freeze them
	//Change to block operation soon
	SharedMatrix Ca_Rt(Ca_t->clone());

	//Write Bocc
	int sizeBO = noccpi[0] - index_trace_occ.size() - num_actv_docc;
	int sizeBV = nvirpi[0] - index_trace_vir.size() - num_actv_vir;
	int sizeAO = index_trace_occ.size(); //AO and AV will not include AA
	int sizeAV = index_trace_vir.size();
	outfile->Printf("\n sizeBO: %d, sizeAO: %d, sizeAV: %d, sizeBV: %d \n", sizeBO, sizeAO, sizeAV, sizeBV);

	Dimension AO = nmopi;
	AO[0] = sizeAO;
	Dimension AV = nmopi;
	AV[0] = sizeAV;
	Dimension BO = nmopi;
	BO[0] = sizeBO;
	Dimension BV = nmopi;
	BV[0] = sizeBV;
	Dimension AO_BO = nmopi;
	AO_BO[0] = sizeAO + sizeBO;
	Dimension AO_BO_A = nmopi;
	AO_BO_A[0] = sizeAO + sizeBO + num_actv;
	Dimension AO_BO_A_AV = nmopi;
	AO_BO_A_AV[0] = sizeAO + sizeBO + num_actv + sizeAV;

	outfile->Printf("\n Original coeffs before localization \n");
	Ca_->print(); //Structure is O-A-V

	outfile->Printf("\n Orbital test 2: \n");
	SharedMatrix S_test_2(S_ao->clone());
	S_test_2->transform(Ca_);
	S_test_2->print();

	outfile->Printf("\n Localization rotation matrix: \n");
	U_all->print();

	//Rotate MO coeffs
	Ca_->copy(Matrix::doublet(Ca_t, U_all, false, false)); //Structure becomes AO-BO-0-AV-BV

	if (options_.get_str("REFERENCE") == "CASSCF") {
		//SharedMatrix Ua(new Matrix("Uv", nirrep, actv_a, actv_a));
		//Ua->identity();
		for (int i = 0; i < num_actv; ++i) {
			Ca_->set_column(0, nroccpi[0] + i, Ca_Rt->get_column(0, nroccpi[0] + i)); //Structure becomes AO-BO-A-AV-BV
		}
	}

	outfile->Printf("\n Coeffs after localization \n");
	Ca_->print();

	outfile->Printf("\n Orbital test 3: \n");
	SharedMatrix S_test_3(S_ao->clone());
	S_test_3->transform(Ca_);
	S_test_3->print();

	Ca_Rt->zero();
	//Write Bocc
	for (int i = 0; i < sizeBO; ++i) {
		Ca_Rt->set_column(0, i, Ca_->get_column(0, sizeAO + i));
	}

	//Write Aocc
	for (int i = 0; i < sizeAO; ++i) {
		Ca_Rt->set_column(0, i + sizeBO, Ca_->get_column(0, i));
	}

	//Write Avir
	for (int i = 0; i < sizeAV; ++i) {
		Ca_Rt->set_column(0, i + AO_BO_A[0], Ca_->get_column(0, i + AO_BO_A[0]));
	}

	//Write Bvir
	for (int i = 0; i < sizeBV; ++i) {
		Ca_Rt->set_column(0, i + AO_BO_A_AV[0], Ca_->get_column(0, AO_BO_A_AV[0] + i));
	}

	//outfile->Printf("\n  The MO coefficients after localization and rotation: \n", sizeBO);
	//Ca_Rt->print();

	//Update Ca_
	if (options_.get_str("REFERENCE") == "CASSCF") { //Ca_: AO-BO-A-AV-BV
		//copy original columns in active space from Ca_
		for (int i = 0; i < num_actv; ++i) {
			Ca_Rt->set_column(0, i + num_rdocc, Ca_->get_column(0, num_rdocc + i));
		}
	}
	Ca_->copy(Ca_Rt); //Ca_: BO-AO-A-AV-BV

	outfile->Printf("\n Original coeffs before semicon \n");
	Ca_->print();

	outfile->Printf("\n Orbital test 4: \n");
	SharedMatrix S_test_4(S_ao->clone());
	S_test_4->transform(Ca_);
	S_test_4->print();

	if (options_.get_bool("SEMICANON") == true) {
		outfile->Printf("\n *** Semi-canonicalization *** \n");

		Slice BOs(zeropi, BO);
		Slice AOs(BO, AO_BO);
		Slice AVs(AO_BO_A, AO_BO_A_AV);
		Slice BVs(AO_BO_A_AV, nmopi);

		//Build Fock in localized basis
		SharedMatrix Fa_loc = Matrix::triplet(Ca_Rt, Fa_, Ca_Rt, true, false, false);
		//outfile->Printf("\n Fock matrix in localized basis: \n");
		//Fa_loc->print();
		SharedMatrix Fa_AOAO = Fa_loc->get_block(AOs, AOs);
		SharedMatrix Fa_AVAV = Fa_loc->get_block(AVs, AVs);
		SharedMatrix Fa_AAAA = Fa_loc->get_block(actv, actv);

		//AO[0] -= num_actv_docc;
		SharedMatrix Uao(new Matrix("Uoo", nirrep, AO, AO));
		SharedVector lao(new Vector("loo", nirrep, AO));
		Fa_AOAO->diagonalize(Uao, lao, ascending);
		Fa_AOAO->zero();
		for (int i = 0; i < AO[0]; ++i) {
			Fa_AOAO->set(0, i, i, lao->get(0, i));
		}
		//Fa_AOAO->print();

		//AV[0] -= num_actv_vir;
		SharedMatrix Uav(new Matrix("Uvv", nirrep, AV, AV));
		SharedVector lav(new Vector("lvv", nirrep, AV));
		Fa_AVAV->diagonalize(Uav, lav, ascending);
		Fa_AVAV->zero();
		for (int i = 0; i < AV[0]; ++i) {
			Fa_AVAV->set(0, i, i, lav->get(0, i));
		}
		//Fa_AVAV->print();

		//Build transformation matrix
		SharedMatrix U_all_2(new Matrix("U with Pab", nirrep, nmopi, nmopi));
		SharedMatrix Ubo(new Matrix("Ubo", nirrep, BO, BO));
		SharedMatrix Ubv(new Matrix("Ubv", nirrep, BV, BV));
		Ubo->identity();
		Ubv->identity();
		U_all_2->set_block(AOs, AOs, Uao);
		U_all_2->set_block(AVs, AVs, Uav);
		U_all_2->set_block(BOs, BOs, Ubo);
		U_all_2->set_block(BVs, BVs, Ubv);

		//outfile->Printf("\n Semi-canonicalization rotation matrix \n");
		//U_all_2->print();

		//if (options_.get_str("REFERENCE") == "CASSCF") {
		//	SharedMatrix Ua(new Matrix("Uv", nirrep, actv_a, actv_a));
		//	Ua->identity();
		//	for (int i = 0; i < num_actv; ++i) {
		//		U_all_2->zero_row(0, num_rdocc + i);
		//		U_all_2->zero_column(0, num_rdocc + i);
		//		U_all_2->set(0, num_rdocc + i, num_rdocc + i, 1.0);
		//	}
		//}
		//U_all_2->print();
		
		//Build new Fock
		outfile->Printf("\n Original Fock matrix in localized basis: \n");
		Fa_loc->print();

		//SharedMatrix Fa_loc_previous(Fa_loc->clone());

		//SharedMatrix Fa_actv = Fa_->get_block(actv, actv);
		Fa_loc->set_block(AOs, AOs, Fa_AOAO);
		Fa_loc->set_block(AVs, AVs, Fa_AVAV);

		if (options_.get_str("REFERENCE") == "CASSCF") {
			SharedMatrix Uaa(new Matrix("Uaa", nirrep, actv_a, actv_a));
			SharedVector laa(new Vector("laa", nirrep, actv_a));
			Fa_AAAA->diagonalize(Uaa, laa, ascending);
			Fa_AAAA->zero();
			for (int i = 0; i < actv_a[0]; ++i) {
				Fa_AAAA->set(0, i, i, laa->get(0, i));
			}
			//Fa_AAAA->print();

			U_all_2->set_block(actv, actv, Uaa);
			Fa_loc->set_block(actv, actv, Fa_AAAA);
		}

		Fa_->copy(Fa_loc);

		outfile->Printf("\n Fock matrix in localized basis afer canonicalization: \n");
		Fa_->print();

		//Rotate Coeffs
		//outfile->Printf("\n Coefficients after canonicalization \n");
		Ca_->copy(Matrix::doublet(Ca_Rt, U_all_2, false, false));
		//Ca_->print();

		outfile->Printf("\n Orbital test 5: \n");
		SharedMatrix S_test(S_ao->clone());
		S_test->transform(Ca_);
		S_test->print();
	}

	outfile->Printf("\n Coeffs after semicon \n");
	Ca_->print();

	//Write MO space info and print
	outfile->Printf("\n  FROZEN_DOCC     = %d", sizeBO);
	outfile->Printf("\n  RESTRICTED_DOCC     = %d", sizeAO);
	outfile->Printf("\n  ACTIVE     = %d", num_actv);
	outfile->Printf("\n  RESTRICTED_UOCC     = %d", sizeAV);
	outfile->Printf("\n  FROZEN_UOCC	 = %d", sizeBV);
	outfile->Printf("\n");

	if (options_.get_bool("WRITE_FREEZE_MO") == true) {
		options_["FROZEN_DOCC"].add(0);
		options_["FROZEN_UOCC"].add(0);
		options_["FROZEN_DOCC"][0].assign(sizeBO);
		options_["FROZEN_UOCC"][0].assign(sizeBV);

		if (options_.get_str("REFERENCE") == "CASSCF") {
			options_["RESTRICTED_DOCC"].add(0);
			options_["ACTIVE"].add(0);
			options_["RESTRICTED_UOCC"].add(0);
			options_["RESTRICTED_DOCC"][0].assign(sizeAO - num_actv_docc);
			options_["ACTIVE"][0].assign(num_actv);
			options_["RESTRICTED_UOCC"][0].assign(sizeAV - num_actv_vir);
		}

		/*
		for (int h = 0; h < nirrep_; h++) {
			options_["FROZEN_DOCC"].add(h);
			options_["FROZEN_UOCC"].add(h);
			options_["FROZEN_DOCC"][h].assign(sizeBO);
			options_["FROZEN_UOCC"][h].assign(sizeBV);
		}
		*/
	}

	return 0.0;
}

}} // End namespaces

