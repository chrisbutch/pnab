//
// Created by jbarnett8 on 2/26/18.
//

#include <iomanip>
#include "MonteCarloRotorSearch.h"

using namespace PNAB;
using namespace std;
using namespace OpenBabel;

MonteCarloRotorSearch::MonteCarloRotorSearch(RuntimeParameters &runtime_params, Backbone backbone,
                                             HelicalParameters &helical_params, Bases bases) {
    runtime_params_ = runtime_params;
    strand_ = runtime_params_.strand;
    for (auto &v : strand_)
        transform(v.begin(), v.end(), v.begin(), ::tolower);
    auto name = strand_[0];
    transform(name.begin(), name.end(), name.begin(), ::tolower);
    base_a_ = bases.getBaseFromName(name);
    helical_params_ = helical_params;
    backbone_ = backbone;
    step_rot_ = helical_params_.getStepRotationOBMatrix();
    glbl_rot_ = helical_params_.getGlobalRotationMatrix();
    step_translate_ = helical_params_.getStepTranslationVec();
    glbl_translate_ = helical_params_.getGlobalTranslationVec();
    bases_ = bases;
    rng_.seed(std::random_device()());
    is_double_stranded_ = runtime_params_.is_double_stranded;
    ff_type_ = runtime_params_.type;
}

bool MonteCarloRotorSearch::run() {
    BaseUnit unit(base_a_, backbone_);
    auto range = unit.getBackboneIndexRange();
    backbone_range_ = {static_cast<unsigned >(range[0]), static_cast<unsigned >(range[1])};
    Chain chain(bases_, backbone_,strand_, ff_type_, backbone_range_, false);
    test_chain_ = chain.getChain();
    auto bu_a_mol = unit.getMol();
    auto bu_a_head_tail = unit.getBackboneLinkers();
    unsigned head = static_cast<unsigned>(bu_a_head_tail[0]),
             tail = static_cast<unsigned>(bu_a_head_tail[1]);

    bu_a_mol.Translate(glbl_translate_);
    bu_a_mol.Rotate(glbl_rot_.data());

    double* coords = bu_a_mol.GetCoordinates();
    uniform_real_distribution<double> dist(0, 2 * M_PI);
    uniform_real_distribution<double> one_zero_dist(0, 1);
    double k_effective = 0.59 / 5.15; // Angstroms^2
    monomer_num_coords_ = bu_a_mol.NumAtoms() * 3;

    OBRotorList rl;
    OBBitVec fix_bonds(backbone_.getMolecule().NumAtoms());
    auto base_indices = unit.getBaseIndexRange();
    for (unsigned i = static_cast<unsigned>(base_indices[0]); i <= base_indices[1]; ++i)
        fix_bonds.SetBitOn(i);

    rl.Setup(bu_a_mol);
    rl.SetFixAtoms(fix_bonds);
    rl.SetRotAtomsByFix(bu_a_mol);

    size_t search_size = runtime_params_.num_steps;
    OBRotorIterator ri;

    for (size_t search_index = 0; search_index < search_size; ++search_index) {
        OBRotor *r = rl.BeginRotor(ri);
        double best_dist = std::numeric_limits<double>::infinity(), cur_dist = best_dist;
        while (r) {

            bool accept = false;
            best_dist = std::numeric_limits<double>::infinity();
            while (!accept) {
                auto angle = dist(rng_);
                r->SetToAngle(coords, angle);
                cur_dist = measureDistance(coords, head, tail);
                if (cur_dist < best_dist
                    || exp(-pow(cur_dist - best_dist, 2) / k_effective) > one_zero_dist(rng_)) {
                    accept = true;
                    best_dist = cur_dist;
                }
            }
            r = rl.NextRotor(ri);
        }

        // if accept, add to vector of coord_vec_
        if (cur_dist < runtime_params_.max_distance) {

            auto data = chain.generateConformerData(coords, helical_params_);

            if (!isPassingEFilter(data)) {
                delete data.coords;
            } else {
                data.monomer_coord = new double[monomer_num_coords_];
                data.index = search_index;
                data.distance = cur_dist;
                memcpy(data.monomer_coord, coords, sizeof(double) * monomer_num_coords_);
                print(data);
            }
        }
        if (search_index % 100000 == 0) {
            auto prgrs = conf_data_vec_.size();
            cout << setw(8);
            cout << 100 * static_cast<double>(search_index) / search_size;
            cout << "%\tAccepted: " << setw(8) << prgrs;
            if (prgrs > 0) {
                cout << ", Best Conformer (distance, energy): (" << setw(10) << conf_data_vec_[0].distance
                     << ", " << setw(10) << conf_data_vec_[0].total_energy << ") -- conformer_"
                     << conf_data_vec_[0].index << ".pdb" << endl;
            } else {
                cout << endl;
            }
        }
    }

    for (auto &v : conf_data_vec_)
        delete v.monomer_coord;

    return true;
}

double MonteCarloRotorSearch::measureDistance(double *coords, unsigned head, unsigned tail) {
    auto hi = 3 * (head - 1), ti = 3 * (tail - 1);
    vector3 head_coord(coords[hi], coords[hi + 1], coords[hi + 2]);
    vector3 tail_coord(coords[ti], coords[ti + 1], coords[ti + 2]);

    tail_coord *= step_rot_;
    tail_coord += step_translate_;
    return sqrt(head_coord.distSq(tail_coord));
}

bool MonteCarloRotorSearch::isPassingEFilter(const PNAB::ConformerData &conf_data) {
    vector<double> cur_vals = {conf_data.total_energy, conf_data.angleE, conf_data.bondE,
                               conf_data.VDWE, conf_data.totTorsionE};
    auto max_vals = runtime_params_.energy_filter;
    for (auto i = 0; i < max_vals.size(); ++i)
        if (max_vals[i] < cur_vals[i])
            return false;
    return true;
}

void MonteCarloRotorSearch::print(PNAB::ConformerData conf_data) {
    if (!conf_data.chain_coords_present) {
        cerr << "Trying to print conformer with no chain_ coordinates. Exiting..." << endl;
        exit(1);
    }
    ostringstream strs;
    filebuf fb;
    ofstream csv;

    // Set output format
    conv_.SetOutFormat("PDB");

    // Clear string stream
    strs.str(std::string());

    // All conformers are named "conformer_" + index
    strs << "conformer_" << conf_data.index << ".pdb";

    // Open file for writing...
    fb.open(strs.str().c_str(), std::ios::out);
    ostream fileStream(&fb);

    // Set the conformer save to file
    test_chain_.SetCoordinates(conf_data.coords);
    conv_.SetOutStream(&fileStream);
    conv_.Write(&test_chain_);
    fb.close();

    delete conf_data.coords;
    conf_data.chain_coords_present = false;
    conf_data_vec_.push_back(conf_data);

    // csv file containing energy headings
    csv.open("energy_data.csv");
    csv << "Conformer Index, Energy (kcal/mol), Distance (A), Bond Energy, Angle Energy,";
    csv << " Torsion Energy, VDW Energy, Total Torsion Energy, RMSD (A)" << endl;
    std::sort(conf_data_vec_.begin(), conf_data_vec_.end());

    double *ref = conf_data_vec_[0].monomer_coord;

    for (auto &v : conf_data_vec_) {
        v.rmsd = calcRMSD(ref, v.monomer_coord, monomer_num_coords_);
        csv << v.index  << ", " << v.total_energy << ", " << v.distance << ", " << v.bondE << ", "
            << v.angleE << ", " << v.torsionE     << ", " << v.VDWE     << ", " << v.totTorsionE << ", "
            << v.rmsd   << endl;
    }
    csv.close();
}