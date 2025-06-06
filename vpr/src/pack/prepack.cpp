/*
 * Prepacking: Group together technology-mapped netlist blocks before packing.
 * This gives hints to the packer on what groups of blocks to keep together during packing.
 * Primary purpose:
 *    1) "Forced" packs (eg LUT+FF pair)
 *    2) Carry-chains
 * Duties: Find pack patterns in architecture, find pack patterns in netlist.
 *
 * Author: Jason Luu
 * March 12, 2012
 */

#include "prepack.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <queue>
#include <utility>
#include <vector>

#include "atom_netlist.h"
#include "echo_files.h"
#include "logic_types.h"
#include "physical_types.h"
#include "vpr_error.h"
#include "vpr_types.h"
#include "vpr_utils.h"
#include "vtr_assert.h"
#include "vtr_range.h"
#include "vtr_time.h"
#include "vtr_util.h"
#include "vtr_vector.h"

/*****************************************/
/*Local Function Declaration			 */
/*****************************************/
static std::vector<t_pack_patterns> alloc_and_load_pack_patterns(const std::vector<t_logical_block_type>& logical_block_types);

static void free_list_of_pack_patterns(std::vector<t_pack_patterns>& list_of_pack_patterns);

static void free_pack_pattern(t_pack_patterns* pack_pattern);

static void discover_pattern_names_in_pb_graph_node(t_pb_graph_node* pb_graph_node,
                                                    std::unordered_map<std::string, int>& pattern_names);

static void forward_infer_pattern(t_pb_graph_pin* pb_graph_pin);

static void backward_infer_pattern(t_pb_graph_pin* pb_graph_pin);

static std::vector<t_pack_patterns> alloc_and_init_pattern_list_from_hash(const std::unordered_map<std::string, int>& pattern_names);

static t_pb_graph_edge* find_expansion_edge_of_pattern(const int pattern_index,
                                                       const t_pb_graph_node* pb_graph_node);

static void forward_expand_pack_pattern_from_edge(const t_pb_graph_edge* expansion_edge,
                                                  t_pack_patterns& packing_pattern,
                                                  int* L_num_blocks,
                                                  const bool make_root_of_chain);

static void backward_expand_pack_pattern_from_edge(const t_pb_graph_edge* expansion_edge,
                                                   t_pack_patterns& packing_pattern,
                                                   t_pb_graph_pin* destination_pin,
                                                   t_pack_pattern_block* destination_block,
                                                   int* L_num_blocks);

static int compare_pack_pattern(const t_pack_patterns* pattern_a, const t_pack_patterns* pattern_b);

static void free_pack_pattern_block(t_pack_pattern_block* pattern_block, t_pack_pattern_block** pattern_block_list);

static bool try_expand_molecule(t_pack_molecule& molecule,
                                const AtomBlockId blk_id,
                                const std::multimap<AtomBlockId, PackMoleculeId>& atom_molecules,
                                const AtomNetlist& atom_nlist,
                                const LogicalModels& models);

static void print_pack_molecules(const char* fname,
                                 const std::vector<t_pack_patterns>& list_of_pack_patterns,
                                 const int num_pack_patterns,
                                 const vtr::vector_map<PackMoleculeId, t_pack_molecule>& pack_molecules,
                                 const AtomNetlist& atom_nlist);

static t_pb_graph_node* get_expected_lowest_cost_primitive_for_atom_block(const AtomBlockId blk_id,
                                                                          const std::vector<t_logical_block_type>& logical_block_types);

static t_pb_graph_node* get_expected_lowest_cost_primitive_for_atom_block_in_pb_graph_node(const AtomBlockId blk_id, t_pb_graph_node* curr_pb_graph_node, float* cost);

static AtomBlockId find_new_root_atom_for_chain(const AtomBlockId blk_id,
                                                const t_pack_patterns* list_of_pack_patterns,
                                                const std::multimap<AtomBlockId, PackMoleculeId>& atom_molecules,
                                                const AtomNetlist& atom_nlist);

static std::vector<t_pb_graph_pin*> find_end_of_path(t_pb_graph_pin* input_pin, int pattern_index);

static void expand_search(const t_pb_graph_pin* input_pin, std::queue<t_pb_graph_pin*>& pins_queue, const int pattern_index);

static void find_all_equivalent_chains(t_pack_patterns* chain_pattern, const t_pb_graph_node* root_block);

static void update_chain_root_pins(t_pack_patterns* chain_pattern,
                                   const std::vector<t_pb_graph_pin*>& chain_input_pins);

/**
 * @brief Get all primitive pins connected to the given cluster input pin
 * 
 * @param cluster_input_pin Cluster input pin to get connected primitive pins from
 * @param pattern_blocks Set of pb_types in the pack pattern. Pins on the blocks in this set will 
 * be added to the connected_primitive_pins vector
 * @param connected_primitive_pins Vector to store connected primitive pins
 */
static void get_all_connected_primitive_pins(const t_pb_graph_pin* cluster_input_pin,
                                             const std::unordered_set<t_pb_type*>& pattern_blocks,
                                             std::vector<t_pb_graph_pin*>& connected_primitive_pins);

static void init_molecule_chain_info(const AtomBlockId blk_id,
                                     t_pack_molecule& molecule,
                                     const vtr::vector_map<PackMoleculeId, t_pack_molecule>& pack_molecules,
                                     const std::multimap<AtomBlockId, PackMoleculeId>& atom_molecules,
                                     vtr::vector<MoleculeChainId, t_chain_info>& chain_info,
                                     const AtomNetlist& atom_nlist);

static AtomBlockId get_sink_block(const AtomBlockId block_id,
                                  const t_pack_pattern_connections& connections,
                                  const AtomNetlist& atom_nlist,
                                  const LogicalModels& models);

static AtomBlockId get_driving_block(const AtomBlockId block_id,
                                     const t_pack_pattern_connections& connections,
                                     const AtomNetlist& atom_nlist);

/**
 * @brief Get an unordered set of all pb_types in the given pack pattern
 * 
 * @param pack_pattern Pack pattern to get pb_types from
 * @return std::unordered_set<t_pb_type*> Set of pb_types in the pack pattern
 */
static std::unordered_set<t_pb_type*> get_pattern_blocks(const t_pack_patterns& pack_pattern);

static void print_chain_starting_points(t_pack_patterns* chain_pattern);

/*****************************************/
/*Function Definitions					 */
/*****************************************/

/**
 * Find all packing patterns in architecture
 * [0..num_packing_patterns-1]
 *
 * Limitations: Currently assumes that forced pack nets must be single-fanout
 * as this covers all the reasonable architectures we wanted.
 * More complicated structures should probably be handled either downstream
 * (general packing) or upstream (in tech mapping).
 * If this limitation is too constraining, code is designed so that this limitation can be removed.
 */
static std::vector<t_pack_patterns> alloc_and_load_pack_patterns(const std::vector<t_logical_block_type>& logical_block_types) {
    int L_num_blocks;
    t_pb_graph_edge* expansion_edge;

    /* alloc and initialize array of packing patterns based on architecture complex blocks */
    std::unordered_map<std::string, int> pattern_names;
    for (const t_logical_block_type& type : logical_block_types) {
        discover_pattern_names_in_pb_graph_node(type.pb_graph_head, pattern_names);
    }

    std::vector<t_pack_patterns> packing_patterns = alloc_and_init_pattern_list_from_hash(pattern_names);

    /* load packing patterns by traversing the edges to find edges belonging to pattern */
    for (size_t i = 0; i < pattern_names.size(); i++) {
        for (const t_logical_block_type& type : logical_block_types) {
            // find an edge that belongs to this pattern
            expansion_edge = find_expansion_edge_of_pattern(i, type.pb_graph_head);
            if (!expansion_edge) {
                continue;
            }

            L_num_blocks = 0;
            packing_patterns[i].base_cost = 0;
            // use the found expansion edge to build the pack pattern
            backward_expand_pack_pattern_from_edge(expansion_edge,
                                                   packing_patterns[i], nullptr, nullptr, &L_num_blocks);
            packing_patterns[i].num_blocks = L_num_blocks;

            /* Default settings: A section of a netlist must match all blocks in a pack
             * pattern before it can be made a molecule except for carry-chains.
             * For carry-chains, since carry-chains are typically quite flexible in terms
             * of size, it is optional whether or not an atom in a netlist matches any
             * particular block inside the chain */
            packing_patterns[i].is_block_optional = new bool[L_num_blocks];
            for (int k = 0; k < L_num_blocks; k++) {
                packing_patterns[i].is_block_optional[k] = false;
                if (packing_patterns[i].is_chain && packing_patterns[i].root_block->block_id != k) {
                    packing_patterns[i].is_block_optional[k] = true;
                }
            }

            // if this is a chain pattern (extends between complex blocks), check if there
            // are multiple equivalent chains with different starting and ending points
            if (packing_patterns[i].is_chain) {
                find_all_equivalent_chains(&packing_patterns[i], type.pb_graph_head);
                print_chain_starting_points(&packing_patterns[i]);
            }

            // if pack pattern i is found to belong to current block type, go to next pack pattern
            break;
        }
    }

    //Sanity check, every pattern should have a root block
    for (size_t i = 0; i < pattern_names.size(); ++i) {
        if (packing_patterns[i].root_block == nullptr) {
            VPR_FATAL_ERROR(VPR_ERROR_ARCH, "Failed to find root block for pack pattern %s", packing_patterns[i].name);
        }
    }

    return packing_patterns;
}

/**
 * Locate all pattern names
 * Side-effect: set all pb_graph_node temp_scratch_pad field to NULL
 *				For cases where a pattern inference is "obvious", mark it as obvious.
 */
static void discover_pattern_names_in_pb_graph_node(t_pb_graph_node* pb_graph_node,
                                                    std::unordered_map<std::string, int>& pattern_names) {
    /* Iterate over all edges to discover if an edge in current physical block belongs to a pattern
     * If edge does, then record the name of the pattern in a hash table */

    if (pb_graph_node == nullptr) {
        return;
    }

    pb_graph_node->temp_scratch_pad = nullptr;

    for (int i = 0; i < pb_graph_node->num_input_ports; i++) {
        for (int j = 0; j < pb_graph_node->num_input_pins[i]; j++) {
            bool hasPattern = false;
            for (int k = 0; k < pb_graph_node->input_pins[i][j].num_output_edges; k++) {
                auto output_edge = pb_graph_node->input_pins[i][j].output_edges[k];
                for (int m = 0; m < output_edge->num_pack_patterns; m++) {
                    hasPattern = true;
                    // insert the found pattern name to the hash table. If this pattern is inserted
                    // for the first time, then its index is the current size of the hash table
                    // otherwise the insert function will return an iterator of the previously
                    // inserted element with the index given to that pattern
                    std::string pattern_name(output_edge->pack_pattern_names[m]);
                    int index = (pattern_names.insert({pattern_name, pattern_names.size()}).first)->second;
                    if (!output_edge->pack_pattern_indices) {
                        output_edge->pack_pattern_indices = new int[output_edge->num_pack_patterns];
                    }
                    output_edge->pack_pattern_indices[m] = index;
                    // if this output edges belongs to a pack pattern. Expand forward starting from
                    // all its output pins to check if you need to infer pattern for direct connections
                    for (int ipin = 0; ipin < output_edge->num_output_pins; ipin++) {
                        forward_infer_pattern(output_edge->output_pins[ipin]);
                    }
                }
            }
            // if the output edge to this pin is annotated with a pack pattern
            // trace the inputs to this pin and mark them to infer pattern
            // if they are direct connections (num_input_edges == 1)
            if (hasPattern) {
                backward_infer_pattern(&pb_graph_node->input_pins[i][j]);
            }
        }
    }

    for (int i = 0; i < pb_graph_node->num_output_ports; i++) {
        for (int j = 0; j < pb_graph_node->num_output_pins[i]; j++) {
            bool hasPattern = false;
            for (int k = 0; k < pb_graph_node->output_pins[i][j].num_output_edges; k++) {
                auto output_edge = pb_graph_node->output_pins[i][j].output_edges[k];
                for (int m = 0; m < output_edge->num_pack_patterns; m++) {
                    hasPattern = true;
                    // insert the found pattern name to the hash table. If this pattern is inserted
                    // for the first time, then its index is the current size of the hash table
                    // otherwise the insert function will return an iterator of the previously
                    // inserted element with the index given to that pattern
                    std::string pattern_name(output_edge->pack_pattern_names[m]);
                    int index = (pattern_names.insert({pattern_name, pattern_names.size()}).first)->second;
                    if (!output_edge->pack_pattern_indices) {
                        output_edge->pack_pattern_indices = new int[output_edge->num_pack_patterns];
                    }
                    output_edge->pack_pattern_indices[m] = index;
                    // if this output edges belongs to a pack pattern. Expand forward starting from
                    // all its output pins to check if you need to infer pattern for direct connections
                    for (int ipin = 0; ipin < output_edge->num_output_pins; ipin++) {
                        forward_infer_pattern(output_edge->output_pins[ipin]);
                    }
                }
            }
            // if the output edge to this pin is annotated with a pack pattern
            // trace the inputs to this pin and mark them to infer pattern
            // if they are direct connections (num_input_edges == 1)
            if (hasPattern) {
                backward_infer_pattern(&pb_graph_node->output_pins[i][j]);
            }
        }
    }

    for (int i = 0; i < pb_graph_node->num_clock_ports; i++) {
        for (int j = 0; j < pb_graph_node->num_clock_pins[i]; j++) {
            bool hasPattern = false;
            for (int k = 0; k < pb_graph_node->clock_pins[i][j].num_output_edges; k++) {
                auto& output_edge = pb_graph_node->clock_pins[i][j].output_edges[k];
                for (int m = 0; m < output_edge->num_pack_patterns; m++) {
                    hasPattern = true;
                    // insert the found pattern name to the hash table. If this pattern is inserted
                    // for the first time, then its index is the current size of the hash table
                    // otherwise the insert function will return an iterator of the previously
                    // inserted element with the index given to that pattern
                    std::string pattern_name(output_edge->pack_pattern_names[m]);
                    int index = (pattern_names.insert({pattern_name, pattern_names.size()}).first)->second;
                    if (output_edge->pack_pattern_indices == nullptr) {
                        output_edge->pack_pattern_indices = new int[output_edge->num_pack_patterns];
                    }
                    output_edge->pack_pattern_indices[m] = index;
                    // if this output edges belongs to a pack pattern. Expand forward starting from
                    // all its output pins to check if you need to infer pattern for direct connections
                    for (int ipin = 0; ipin < output_edge->num_output_pins; ipin++) {
                        forward_infer_pattern(output_edge->output_pins[ipin]);
                    }
                }
            }
            // if the output edge to this pin is annotated with a pack pattern
            // trace the inputs to this pin and mark them to infer pattern
            // if they are direct connections (num_input_edges == 1)
            if (hasPattern) {
                backward_infer_pattern(&pb_graph_node->clock_pins[i][j]);
            }
        }
    }

    for (int i = 0; i < pb_graph_node->pb_type->num_modes; i++) {
        for (int j = 0; j < pb_graph_node->pb_type->modes[i].num_pb_type_children; j++) {
            for (int k = 0; k < pb_graph_node->pb_type->modes[i].pb_type_children[j].num_pb; k++) {
                discover_pattern_names_in_pb_graph_node(&pb_graph_node->child_pb_graph_nodes[i][j][k], pattern_names);
            }
        }
    }
}

/**
 * In obvious cases where a pattern edge has only one path to go, set that path to be inferred
 */
static void forward_infer_pattern(t_pb_graph_pin* pb_graph_pin) {
    if (pb_graph_pin->num_output_edges == 1 && pb_graph_pin->output_edges[0]->num_pack_patterns == 0 && pb_graph_pin->output_edges[0]->infer_pattern == false) {
        pb_graph_pin->output_edges[0]->infer_pattern = true;
        if (pb_graph_pin->output_edges[0]->num_output_pins == 1) {
            forward_infer_pattern(pb_graph_pin->output_edges[0]->output_pins[0]);
        }
    }
}
static void backward_infer_pattern(t_pb_graph_pin* pb_graph_pin) {
    if (pb_graph_pin->num_input_edges == 1 && pb_graph_pin->input_edges[0]->num_pack_patterns == 0 && pb_graph_pin->input_edges[0]->infer_pattern == false) {
        pb_graph_pin->input_edges[0]->infer_pattern = true;
        if (pb_graph_pin->input_edges[0]->num_input_pins == 1) {
            backward_infer_pattern(pb_graph_pin->input_edges[0]->input_pins[0]);
        }
    }
}

/**
 * Allocates memory for models and loads the name of the packing pattern
 * so that it can be identified and loaded with more complete information later
 */
static std::vector<t_pack_patterns> alloc_and_init_pattern_list_from_hash(const std::unordered_map<std::string, int>& pattern_names) {
    std::vector<t_pack_patterns> nlist(pattern_names.size());

    for (const auto& curr_pattern : pattern_names) {
        VTR_ASSERT(nlist[curr_pattern.second].name == nullptr);
        nlist[curr_pattern.second].name = vtr::strdup(curr_pattern.first.c_str());
        nlist[curr_pattern.second].root_block = nullptr;
        nlist[curr_pattern.second].is_chain = false;
        nlist[curr_pattern.second].index = curr_pattern.second;
    }

    return nlist;
}

static void free_list_of_pack_patterns(std::vector<t_pack_patterns>& list_of_pack_patterns) {
    for (size_t i = 0; i < list_of_pack_patterns.size(); i++) {
        free_pack_pattern(&list_of_pack_patterns[i]);
    }
}

static void free_pack_pattern(t_pack_patterns* pack_pattern) {
    if (pack_pattern) {
        int num_pack_pattern_blocks = pack_pattern->num_blocks;
        t_pack_pattern_block** pattern_block_list = new t_pack_pattern_block*[num_pack_pattern_blocks];
        for (int i = 0; i < num_pack_pattern_blocks; i++)
            pattern_block_list[i] = nullptr;

        free(pack_pattern->name);
        delete[] pack_pattern->is_block_optional;
        free_pack_pattern_block(pack_pattern->root_block, pattern_block_list);
        for (int j = 0; j < num_pack_pattern_blocks; j++) {
            delete pattern_block_list[j];
        }
        delete[] pattern_block_list;
    }
}

/**
 * Locate first edge that belongs to pattern index
 */
static t_pb_graph_edge* find_expansion_edge_of_pattern(const int pattern_index,
                                                       const t_pb_graph_node* pb_graph_node) {
    int i, j, k, m;
    t_pb_graph_edge* edge;
    /* Iterate over all edges to discover if an edge in current physical block belongs to a pattern
     * If edge does, then return that edge
     */

    if (pb_graph_node == nullptr) {
        return nullptr;
    }

    for (i = 0; i < pb_graph_node->num_input_ports; i++) {
        for (j = 0; j < pb_graph_node->num_input_pins[i]; j++) {
            auto& input_pin = pb_graph_node->input_pins[i][j];
            for (k = 0; k < input_pin.num_output_edges; k++) {
                for (m = 0; m < input_pin.output_edges[k]->num_pack_patterns; m++) {
                    if (input_pin.output_edges[k]->pack_pattern_indices[m] == pattern_index) {
                        return input_pin.output_edges[k];
                    }
                }
            }
        }
    }

    for (i = 0; i < pb_graph_node->num_output_ports; i++) {
        for (j = 0; j < pb_graph_node->num_output_pins[i]; j++) {
            auto& output_pin = pb_graph_node->output_pins[i][j];
            for (k = 0; k < output_pin.num_output_edges; k++) {
                for (m = 0; m < output_pin.output_edges[k]->num_pack_patterns; m++) {
                    if (output_pin.output_edges[k]->pack_pattern_indices[m] == pattern_index) {
                        return output_pin.output_edges[k];
                    }
                }
            }
        }
    }

    for (i = 0; i < pb_graph_node->num_clock_ports; i++) {
        for (j = 0; j < pb_graph_node->num_clock_pins[i]; j++) {
            auto& clock_pin = pb_graph_node->clock_pins[i][j];
            for (k = 0; k < clock_pin.num_output_edges; k++) {
                for (m = 0; m < clock_pin.output_edges[k]->num_pack_patterns; m++) {
                    if (clock_pin.output_edges[k]->pack_pattern_indices[m] == pattern_index) {
                        return clock_pin.output_edges[k];
                    }
                }
            }
        }
    }

    for (i = 0; i < pb_graph_node->pb_type->num_modes; i++) {
        auto& pb_mode = pb_graph_node->pb_type->modes[i];
        for (j = 0; j < pb_mode.num_pb_type_children; j++) {
            for (k = 0; k < pb_mode.pb_type_children[j].num_pb; k++) {
                edge = find_expansion_edge_of_pattern(pattern_index, &pb_graph_node->child_pb_graph_nodes[i][j][k]);
                if (edge != nullptr) {
                    return edge;
                }
            }
        }
    }
    return nullptr;
}

/**
 *  This function expands forward from the given expansion_edge. If a primitive is found that
 *  belongs to the pack pattern we are searching for, create a pack pattern block of using
 *  this primitive to be added later to the pack pattern when creating the pack pattern
 *  connections in the backward_expand_pack_pattern_from_edge function.
 *
 *  expansion_edge: starting edge to expand forward from
 *  list_of_packing_patterns: list of packing patterns in the architecture
 *  curr_pattern_index: current packing pattern that we are building
 *  L_num_blocks: number of primitives found to belong to this pattern so far
 *  make_root_of_chain: flag indicating that the given expansion_edge is connected
 *                      to a primitive that is the root of this packing pattern
 *
 *  Convention: Pack pattern block connections are made on backward expansion only (to make
 *              future multi-fanout support easier) so this function will not update connections
 */
static void forward_expand_pack_pattern_from_edge(const t_pb_graph_edge* expansion_edge,
                                                  t_pack_patterns& packing_pattern,
                                                  int* L_num_blocks,
                                                  bool make_root_of_chain) {
    int i, j, k;
    int iport, ipin, iedge;
    bool found; /* Error checking, ensure only one fan-out for each pattern net */
    t_pack_pattern_block* destination_block = nullptr;
    t_pb_graph_node* destination_pb_graph_node = nullptr;
    int curr_pattern_index = packing_pattern.index;

    found = expansion_edge->infer_pattern;
    // if the pack pattern shouldn't be inferred check if the expansion
    // edge is annotated with the current pack pattern we are expanding
    for (i = 0; !found && i < expansion_edge->num_pack_patterns; i++) {
        if (expansion_edge->pack_pattern_indices[i] == curr_pattern_index) {
            found = true;
        }
    }
    // if this edge isn't annotated with the current pack pattern
    // no need to explore it
    if (!found) {
        return;
    }

    found = false;
    // iterate over the expansion edge output pins
    for (i = 0; i < expansion_edge->num_output_pins; i++) {
        // check if expansion_edge parent node is a primitive (i.e num_nodes = 0)
        if (expansion_edge->output_pins[i]->is_primitive_pin()) {
            destination_pb_graph_node = expansion_edge->output_pins[i]->parent_node;
            VTR_ASSERT(found == false);
            /* Check assumption that each forced net has only one fan-out */
            /* This is the destination node */
            found = true;

            // the temp_scratch_pad points to the last primitive from this pb_graph_node that was added to a packing pattern.
            const auto& destination_pb_temp = (t_pack_pattern_block*)destination_pb_graph_node->temp_scratch_pad;
            // if this pb_graph_node (primitive) is not added to the packing pattern already, add it and expand all its edges
            if (destination_pb_temp == nullptr || destination_pb_temp->pattern_index != curr_pattern_index) {
                // a primitive that belongs to this pack pattern is found: 1) create a new pattern block,
                // 2) assign an id to this pattern block, 3) increment the number of found blocks belonging to this
                // pattern and 4) expand all its edges to find the other primitives that belong to this pattern
                destination_block = new t_pack_pattern_block();
                packing_pattern.base_cost += compute_primitive_base_cost(destination_pb_graph_node);
                destination_block->block_id = *L_num_blocks;
                (*L_num_blocks)++;
                destination_pb_graph_node->temp_scratch_pad = (void*)destination_block;
                destination_block->pattern_index = curr_pattern_index;
                destination_block->pb_type = destination_pb_graph_node->pb_type;

                // explore the inputs to this primitive
                for (iport = 0; iport < destination_pb_graph_node->num_input_ports; iport++) {
                    for (ipin = 0; ipin < destination_pb_graph_node->num_input_pins[iport]; ipin++) {
                        for (iedge = 0; iedge < destination_pb_graph_node->input_pins[iport][ipin].num_input_edges; iedge++) {
                            backward_expand_pack_pattern_from_edge(destination_pb_graph_node->input_pins[iport][ipin].input_edges[iedge],
                                                                   packing_pattern,
                                                                   &destination_pb_graph_node->input_pins[iport][ipin],
                                                                   destination_block, L_num_blocks);
                        }
                    }
                }

                // explore the outputs of this primitive
                for (iport = 0; iport < destination_pb_graph_node->num_output_ports; iport++) {
                    for (ipin = 0; ipin < destination_pb_graph_node->num_output_pins[iport]; ipin++) {
                        for (iedge = 0; iedge < destination_pb_graph_node->output_pins[iport][ipin].num_output_edges; iedge++) {
                            forward_expand_pack_pattern_from_edge(destination_pb_graph_node->output_pins[iport][ipin].output_edges[iedge],
                                                                  packing_pattern, L_num_blocks, false);
                        }
                    }
                }

                // explore the clock pins of this primitive
                for (iport = 0; iport < destination_pb_graph_node->num_clock_ports; iport++) {
                    for (ipin = 0; ipin < destination_pb_graph_node->num_clock_pins[iport]; ipin++) {
                        for (iedge = 0; iedge < destination_pb_graph_node->clock_pins[iport][ipin].num_input_edges; iedge++) {
                            backward_expand_pack_pattern_from_edge(destination_pb_graph_node->clock_pins[iport][ipin].input_edges[iedge],
                                                                   packing_pattern,
                                                                   &destination_pb_graph_node->clock_pins[iport][ipin],
                                                                   destination_block, L_num_blocks);
                        }
                    }
                }
            }

            // if this pb_graph_node (primitive) should be added to the pack pattern blocks
            if (((t_pack_pattern_block*)destination_pb_graph_node->temp_scratch_pad)->pattern_index == curr_pattern_index) {
                // if this pb_graph_node is known to be the root of the chain, update the root block and root pin
                if (make_root_of_chain == true) {
                    packing_pattern.chain_root_pins = {{expansion_edge->output_pins[i]}};
                    packing_pattern.root_block = destination_block;
                }
            }

            // the expansion_edge parent node is not a primitive
        } else {
            // continue expanding forward
            for (j = 0; j < expansion_edge->output_pins[i]->num_output_edges; j++) {
                if (expansion_edge->output_pins[i]->output_edges[j]->infer_pattern == true) {
                    forward_expand_pack_pattern_from_edge(expansion_edge->output_pins[i]->output_edges[j],
                                                          packing_pattern,
                                                          L_num_blocks,
                                                          make_root_of_chain);
                } else {
                    for (k = 0; k < expansion_edge->output_pins[i]->output_edges[j]->num_pack_patterns; k++) {
                        if (expansion_edge->output_pins[i]->output_edges[j]->pack_pattern_indices[k] == curr_pattern_index) {
                            if (found == true) {
                                /* Check assumption that each forced net has only one fan-out */
                                VPR_FATAL_ERROR(VPR_ERROR_PACK,
                                                "Invalid packing pattern defined.  Multi-fanout nets not supported when specifying pack patterns.\n"
                                                "Problem on %s[%d].%s[%d] for pattern %s\n",
                                                expansion_edge->output_pins[i]->parent_node->pb_type->name,
                                                expansion_edge->output_pins[i]->parent_node->placement_index,
                                                expansion_edge->output_pins[i]->port->name,
                                                expansion_edge->output_pins[i]->pin_number,
                                                packing_pattern.name);
                            }
                            found = true;
                            forward_expand_pack_pattern_from_edge(expansion_edge->output_pins[i]->output_edges[j],
                                                                  packing_pattern,
                                                                  L_num_blocks,
                                                                  make_root_of_chain);
                        }
                    } // End for pack patterns of output edge
                }
            } // End for number of output edges
        }
    } // End for output pins of expansion edge
}

/**
 * Find if driver of edge is in the same pattern, if yes, add to pattern
 *  Convention: Connections are made on backward expansion only (to make future multi-
 *               fanout support easier) so this function must update both source and
 *               destination blocks
 */
static void backward_expand_pack_pattern_from_edge(const t_pb_graph_edge* expansion_edge,
                                                   t_pack_patterns& packing_pattern,
                                                   t_pb_graph_pin* destination_pin,
                                                   t_pack_pattern_block* destination_block,
                                                   int* L_num_blocks) {
    int i, j, k;
    int iport, ipin, iedge;
    bool found; /* Error checking, ensure only one fan-out for each pattern net */
    t_pack_pattern_block* source_block = nullptr;
    t_pb_graph_node* source_pb_graph_node = nullptr;
    t_pack_pattern_connections* pack_pattern_connection = nullptr;
    int curr_pattern_index = packing_pattern.index;

    found = expansion_edge->infer_pattern;
    // if the pack pattern shouldn't be inferred check if the expansion
    // edge is annotated with the current pack pattern we are expanding
    for (i = 0; !found && i < expansion_edge->num_pack_patterns; i++) {
        if (expansion_edge->pack_pattern_indices[i] == curr_pattern_index) {
            found = true;
        }
    }

    // if this edge isn't annotated with the current pack pattern
    // no need to explore it
    if (!found) {
        return;
    }

    found = false;
    // iterate over all the drivers of this edge
    for (i = 0; i < expansion_edge->num_input_pins; i++) {
        // check if the expansion_edge parent node is a primitive
        if (expansion_edge->input_pins[i]->is_primitive_pin()) {
            source_pb_graph_node = expansion_edge->input_pins[i]->parent_node;
            VTR_ASSERT(found == false);
            /* Check assumption that each forced net has only one fan-out */
            /* This is the source node for destination */
            found = true;

            /* If this pb_graph_node is part not of the current pattern index, put it in and expand all its edges */
            source_block = (t_pack_pattern_block*)source_pb_graph_node->temp_scratch_pad;
            if (source_block == nullptr || source_block->pattern_index != curr_pattern_index) {
                source_block = new t_pack_pattern_block();
                source_block->block_id = *L_num_blocks;
                (*L_num_blocks)++;
                packing_pattern.base_cost += compute_primitive_base_cost(source_pb_graph_node);
                source_pb_graph_node->temp_scratch_pad = (void*)source_block;
                source_block->pattern_index = curr_pattern_index;
                source_block->pb_type = source_pb_graph_node->pb_type;

                if (packing_pattern.root_block == nullptr) {
                    packing_pattern.root_block = source_block;
                }

                // explore the inputs of this primitive
                for (iport = 0; iport < source_pb_graph_node->num_input_ports; iport++) {
                    for (ipin = 0; ipin < source_pb_graph_node->num_input_pins[iport]; ipin++) {
                        for (iedge = 0; iedge < source_pb_graph_node->input_pins[iport][ipin].num_input_edges; iedge++) {
                            backward_expand_pack_pattern_from_edge(source_pb_graph_node->input_pins[iport][ipin].input_edges[iedge],
                                                                   packing_pattern,
                                                                   &source_pb_graph_node->input_pins[iport][ipin],
                                                                   source_block,
                                                                   L_num_blocks);
                        }
                    }
                }

                // explore the outputs of this primitive
                for (iport = 0; iport < source_pb_graph_node->num_output_ports; iport++) {
                    for (ipin = 0; ipin < source_pb_graph_node->num_output_pins[iport]; ipin++) {
                        for (iedge = 0; iedge < source_pb_graph_node->output_pins[iport][ipin].num_output_edges; iedge++) {
                            forward_expand_pack_pattern_from_edge(source_pb_graph_node->output_pins[iport][ipin].output_edges[iedge],
                                                                  packing_pattern,
                                                                  L_num_blocks,
                                                                  false);
                        }
                    }
                }

                // explore the clock pins of this primitive
                for (iport = 0; iport < source_pb_graph_node->num_clock_ports; iport++) {
                    for (ipin = 0; ipin < source_pb_graph_node->num_clock_pins[iport]; ipin++) {
                        for (iedge = 0; iedge < source_pb_graph_node->clock_pins[iport][ipin].num_input_edges; iedge++) {
                            backward_expand_pack_pattern_from_edge(source_pb_graph_node->clock_pins[iport][ipin].input_edges[iedge],
                                                                   packing_pattern,
                                                                   &source_pb_graph_node->clock_pins[iport][ipin],
                                                                   source_block,
                                                                   L_num_blocks);
                        }
                    }
                }
            }

            if (destination_pin != nullptr) {
                VTR_ASSERT(((t_pack_pattern_block*)source_pb_graph_node->temp_scratch_pad)->pattern_index == curr_pattern_index);
                source_block = (t_pack_pattern_block*)source_pb_graph_node->temp_scratch_pad;
                pack_pattern_connection = new t_pack_pattern_connections();
                pack_pattern_connection->from_block = source_block;
                pack_pattern_connection->from_pin = expansion_edge->input_pins[i];
                pack_pattern_connection->to_block = destination_block;
                pack_pattern_connection->to_pin = destination_pin;
                pack_pattern_connection->next = source_block->connections;
                source_block->connections = pack_pattern_connection;

                pack_pattern_connection = new t_pack_pattern_connections();
                pack_pattern_connection->from_block = source_block;
                pack_pattern_connection->from_pin = expansion_edge->input_pins[i];
                pack_pattern_connection->to_block = destination_block;
                pack_pattern_connection->to_pin = destination_pin;
                pack_pattern_connection->next = destination_block->connections;
                destination_block->connections = pack_pattern_connection;

                if (source_block == destination_block) {
                    VPR_FATAL_ERROR(VPR_ERROR_PACK,
                                    "Invalid packing pattern defined. Source and destination block are the same (%s).\n",
                                    source_block->pb_type->name);
                }
            }

            // expansion edge parent is not a primitive
        } else {
            // check if this input pin of the expansion edge has no driving pin
            if (expansion_edge->input_pins[i]->num_input_edges == 0) {
                // check if this input pin of the expansion edge belongs to a root block (i.e doesn't have a parent block)
                if (expansion_edge->input_pins[i]->parent_node->pb_type->is_root()) {
                    // This pack pattern extends to CLB (root pb block) input pin,
                    // thus it extends across multiple logic blocks, treat as a chain
                    packing_pattern.is_chain = true;
                    // since this input pin has not driving nets, expand in the forward direction instead
                    forward_expand_pack_pattern_from_edge(expansion_edge,
                                                          packing_pattern,
                                                          L_num_blocks,
                                                          true);
                }
                // this input pin of the expansion edge has a driving pin
            } else {
                // iterate over all the driving edges of this input pin
                for (j = 0; j < expansion_edge->input_pins[i]->num_input_edges; j++) {
                    // if pattern should be inferred for this edge continue the expansion backwards
                    if (expansion_edge->input_pins[i]->input_edges[j]->infer_pattern == true) {
                        backward_expand_pack_pattern_from_edge(expansion_edge->input_pins[i]->input_edges[j],
                                                               packing_pattern,
                                                               destination_pin,
                                                               destination_block,
                                                               L_num_blocks);
                        // if pattern shouldn't be inferred
                    } else {
                        // check if this input pin edge is annotated with the current pattern
                        for (k = 0; k < expansion_edge->input_pins[i]->input_edges[j]->num_pack_patterns; k++) {
                            if (expansion_edge->input_pins[i]->input_edges[j]->pack_pattern_indices[k] == curr_pattern_index) {
                                VTR_ASSERT(found == false);
                                /* Check assumption that each forced net has only one fan-out */
                                found = true;
                                backward_expand_pack_pattern_from_edge(expansion_edge->input_pins[i]->input_edges[j],
                                                                       packing_pattern,
                                                                       destination_pin,
                                                                       destination_block,
                                                                       L_num_blocks);
                            }
                        }
                    }
                }
            }
        }
    }
}

/**
 * Pre-pack atoms in netlist to molecules
 * 1.  Single atoms are by definition a molecule.
 * 2.  Forced pack molecules are groupings of atoms that matches a t_pack_pattern definition.
 * 3.  Chained molecules are molecules that follow a carry-chain style pattern,
 *     ie. a single linear chain that can be split across multiple complex blocks
 */
void Prepacker::alloc_and_load_pack_molecules(std::multimap<AtomBlockId, PackMoleculeId>& atom_molecules_multimap,
                                              const AtomNetlist& atom_nlist,
                                              const LogicalModels& models,
                                              const std::vector<t_logical_block_type>& logical_block_types) {
    std::vector<bool> is_used(list_of_pack_patterns.size(), false);

    /* Find forced pack patterns
     * Simplifying assumptions: Each atom can map to at most one molecule,
     *                          use first-fit mapping based on priority of pattern
     * TODO: Need to investigate better mapping strategies than first-fit
     */
    size_t num_packing_patterns = list_of_pack_patterns.size();
    for (size_t i = 0; i < num_packing_patterns; i++) {
        /* Skip pack patterns for modes that are disabled for packing,
         * Ensure no resources in unpackable modes will be mapped during pre-packing stage 
         */
        if ((nullptr != list_of_pack_patterns[i].root_block->pb_type->parent_mode)
            && (true == list_of_pack_patterns[i].root_block->pb_type->parent_mode->disable_packing)) {
            continue;
        }

        size_t best_pattern = 0;
        for (size_t j = 1; j < num_packing_patterns; j++) {
            if (is_used[best_pattern]) {
                best_pattern = j;
            } else if (is_used[j] == false && compare_pack_pattern(&list_of_pack_patterns[j], &list_of_pack_patterns[best_pattern]) == 1) {
                best_pattern = j;
            }
        }
        VTR_ASSERT(is_used[best_pattern] == false);
        is_used[best_pattern] = true;

        auto blocks = atom_nlist.blocks();
        for (auto blk_iter = blocks.begin(); blk_iter != blocks.end(); ++blk_iter) {
            auto blk_id = *blk_iter;

            PackMoleculeId cur_molecule_id = try_create_molecule(best_pattern,
                                                                 blk_id,
                                                                 atom_molecules_multimap,
                                                                 atom_nlist,
                                                                 models);

            // If the molecule could not be created, move to the next block.
            if (!cur_molecule_id.is_valid())
                continue;

            /* In the event of multiple molecules with the same atom block pattern,
             * bias to use the molecule with less costly physical resources first */
            /* TODO: Need to normalize magical number 100 */
            t_pack_molecule& cur_molecule = pack_molecules_[cur_molecule_id];
            cur_molecule.base_gain = cur_molecule.atom_block_ids.size() - (cur_molecule.pack_pattern->base_cost / 100);

            //Note: atom_molecules is an (ordered) multimap so the last molecule
            //      inserted for a given blk_id will be the last valid element
            //      in the equal_range
            auto rng = atom_molecules_multimap.equal_range(blk_id); //The range of molecules matching this block
            bool range_empty = (rng.first == rng.second);
            bool cur_was_last_inserted = false;
            if (!range_empty) {
                auto last_valid_iter = --rng.second; //Iterator to last element (only valid if range is not empty)
                cur_was_last_inserted = (last_valid_iter->second == cur_molecule_id);
            }
            if (range_empty || !cur_was_last_inserted) {
                /* molecule did not cover current atom (possibly because molecule created is
                 * part of a long chain that extends past multiple logic blocks), try again */
                --blk_iter;
            }
        }
    }

    /* List all atom blocks as a molecule for blocks that do not belong to any molecules.
     * This allows the packer to be consistent as it now packs molecules only instead of atoms and molecules
     *
     * If a block belongs to a molecule, then carrying the single atoms around can make the packing problem
     * more difficult because now it needs to consider splitting molecules.
     */
    for (auto blk_id : atom_nlist.blocks()) {
        t_pb_graph_node* best = get_expected_lowest_cost_primitive_for_atom_block(blk_id, logical_block_types);
        if (!best) {
            VPR_FATAL_ERROR(VPR_ERROR_PACK, "Failed to find any location to pack primitive of type '%s' in architecture",
                            models.get_model(atom_nlist.block_model(blk_id)).name);
        }

        VTR_ASSERT_SAFE(nullptr != best);

        expected_lowest_cost_pb_gnode[blk_id] = best;

        auto rng = atom_molecules_multimap.equal_range(blk_id);
        bool rng_empty = (rng.first == rng.second);
        if (rng_empty) {
            PackMoleculeId new_molecule_id = PackMoleculeId(pack_molecules_.size());
            t_pack_molecule new_molecule;
            new_molecule.type = e_pack_pattern_molecule_type::MOLECULE_SINGLE_ATOM;
            new_molecule.root = 0;
            new_molecule.pack_pattern = nullptr;

            new_molecule.atom_block_ids = {blk_id};

            new_molecule.base_gain = 1;
            new_molecule.chain_id = MoleculeChainId::INVALID();

            atom_molecules_multimap.insert({blk_id, new_molecule_id});
            pack_molecules_.push_back(std::move(new_molecule));
            pack_molecule_ids_.push_back(new_molecule_id);
        }
    }

    if (getEchoEnabled() && isEchoFileEnabled(E_ECHO_PRE_PACKING_MOLECULES_AND_PATTERNS)) {
        print_pack_molecules(getEchoFileName(E_ECHO_PRE_PACKING_MOLECULES_AND_PATTERNS),
                             list_of_pack_patterns, num_packing_patterns,
                             pack_molecules_,
                             atom_nlist);
    }
}

static void free_pack_pattern_block(t_pack_pattern_block* pattern_block, t_pack_pattern_block** pattern_block_list) {
    t_pack_pattern_connections *connection, *next;
    if (pattern_block == nullptr || pattern_block->block_id == OPEN) {
        /* already traversed, return */
        return;
    }
    pattern_block_list[pattern_block->block_id] = pattern_block;
    pattern_block->block_id = OPEN;
    connection = pattern_block->connections;
    while (connection) {
        free_pack_pattern_block(connection->from_block, pattern_block_list);
        free_pack_pattern_block(connection->to_block, pattern_block_list);
        next = connection->next;
        delete connection;
        connection = next;
    }
}

/**
 * Given a pattern and an atom block to serve as the root block, determine if
 * the candidate atom block serving as the root node matches the pattern.
 * If yes, return the molecule with this atom block as the root, if not, return NULL
 *
 * Limitations: Currently assumes that forced pack nets must be single-fanout as
 *              this covers all the reasonable architectures we wanted. More complicated
 *              structures should probably be handled either downstream (general packing)
 *              or upstream (in tech mapping).
 *              If this limitation is too constraining, code is designed so that this limitation can be removed
 *
 * Side Effect: If successful, link atom to molecule
 */
PackMoleculeId Prepacker::try_create_molecule(const int pack_pattern_index,
                                              AtomBlockId blk_id,
                                              std::multimap<AtomBlockId, PackMoleculeId>& atom_molecules_multimap,
                                              const AtomNetlist& atom_nlist,
                                              const LogicalModels& models) {
    auto pack_pattern = &list_of_pack_patterns[pack_pattern_index];

    // Check pack pattern validity
    if (pack_pattern == nullptr || pack_pattern->num_blocks == 0 || pack_pattern->root_block == nullptr) {
        return PackMoleculeId::INVALID();
    }

    // If a chain pattern extends beyond a single logic block, we must find
    // the furthest blk_id up the chain that is not mapped to a molecule yet.
    if (pack_pattern->is_chain) {
        blk_id = find_new_root_atom_for_chain(blk_id, pack_pattern, atom_molecules_multimap, atom_nlist);
        if (!blk_id) return PackMoleculeId::INVALID();
    }

    PackMoleculeId new_molecule_id = PackMoleculeId(pack_molecules_.size());
    t_pack_molecule molecule;
    molecule.base_gain = 0.f;
    molecule.type = e_pack_pattern_molecule_type::MOLECULE_FORCED_PACK;
    molecule.pack_pattern = pack_pattern;
    molecule.atom_block_ids = std::vector<AtomBlockId>(pack_pattern->num_blocks); //Initializes invalid
    molecule.root = pack_pattern->root_block->block_id;
    molecule.chain_id = MoleculeChainId::INVALID();

    if (!try_expand_molecule(molecule, blk_id, atom_molecules_multimap, atom_nlist, models)) {
        // Failed to create molecule
        return PackMoleculeId::INVALID();
    }

    // Success! commit molecule

    // update chain info for chain molecules
    if (molecule.pack_pattern->is_chain) {
        init_molecule_chain_info(blk_id, molecule, pack_molecules_, atom_molecules_multimap, chain_info_, atom_nlist);
    }

    // update the atom_molcules with the atoms that are mapped to this molecule
    for (int i = 0; i < molecule.pack_pattern->num_blocks; i++) {
        auto blk_id2 = molecule.atom_block_ids[i];
        if (!blk_id2) {
            VTR_ASSERT(molecule.pack_pattern->is_block_optional[i]);
            continue;
        }

        atom_molecules_multimap.insert({blk_id2, new_molecule_id});
    }

    pack_molecules_.push_back(std::move(molecule));
    pack_molecule_ids_.push_back(new_molecule_id);
    return new_molecule_id;
}

/**
 * Determine if an atom block can match with the pattern to from a molecule.
 *
 * This function takes a molecule that represents a packing pattern. It also
 * takes a (netlist) atom block represented by blk_id which matches the
 * root primitive of this packing pattern. Using this atom block and the structure
 * of the packing pattern, this function tries to fill all the available positions
 * in the packing pattern. If all the non-optional primitive positions in the
 * pattern are filled return true, return false otherwise.
 *      molecule       : the molecule we are trying to expand
 *      atom_molecules : map of atom block ids that are assigned a molecule and a pointer to this molecule
 *      blk_id         : chosen to be the root of this molecule and the code is expanding from
 */
static bool try_expand_molecule(t_pack_molecule& molecule,
                                const AtomBlockId blk_id,
                                const std::multimap<AtomBlockId, PackMoleculeId>& atom_molecules,
                                const AtomNetlist& atom_nlist,
                                const LogicalModels& models) {
    // root block of the pack pattern, which is the starting point of this pattern
    const auto pattern_root_block = molecule.pack_pattern->root_block;
    // bool array indicating whether a position in a pack pattern is optional or should
    // be filled with an atom for legality
    const auto is_block_optional = molecule.pack_pattern->is_block_optional;

    // create a queue of pattern block and atom block id suggested for this block
    std::queue<std::pair<t_pack_pattern_block*, AtomBlockId>> pattern_block_queue;
    // initialize the queue with the pattern root block and the matching atom block
    pattern_block_queue.push(std::make_pair(pattern_root_block, blk_id));

    // do breadth first search by walking through the pack pattern structure along
    // with the atom netlist structure
    while (!pattern_block_queue.empty()) {
        // get the front pattern block, atom block id pair from the queue
        const auto pattern_block_atom_pair = pattern_block_queue.front();
        const auto pattern_block = pattern_block_atom_pair.first;
        const auto block_id = pattern_block_atom_pair.second;

        // remove the front of the queue
        pattern_block_queue.pop();

        // get the atom block id of the atom occupying this primitive position in this molecule
        auto molecule_atom_block_id = molecule.atom_block_ids[pattern_block->block_id];

        // if this primitive position in this molecule is already visited and
        // matches block in the atom netlist go to the next node in the queue
        if (molecule_atom_block_id && molecule_atom_block_id == block_id) {
            continue;
        }

        if (!block_id || !primitive_type_feasible(block_id, pattern_block->pb_type) || (molecule_atom_block_id && molecule_atom_block_id != block_id) || atom_molecules.find(block_id) != atom_molecules.end()) {
            // Stopping conditions, if:
            // 1) this is an invalid atom block (nothing)
            // 2) this atom block cannot fit in this primitive type
            // 3) this primitive is occupied by another block
            // 4) this atom block is already used by another molecule
            // then if the molecule cannot be formed without placing an atom
            // at that primitive position, then creating this molecule has failed
            // otherwise go to the next atom block and its corresponding pattern block
            if (!is_block_optional[pattern_block->block_id]) {
                return false;
            }
            continue;
        }

        // set this node in the molecule as visited
        molecule.atom_block_ids[pattern_block->block_id] = block_id;

        // starting from the first connections, add all the connections of this block to the queue
        auto block_connection = pattern_block->connections;

        while (block_connection != nullptr) {
            // this block is the driver of this connection
            if (block_connection->from_block == pattern_block) {
                // find the block this connection is driving and add it to the queue
                auto sink_blk_id = get_sink_block(block_id, *block_connection, atom_nlist, models);
                // add this sink block id with its corresponding pattern block to the queue
                pattern_block_queue.push(std::make_pair(block_connection->to_block, sink_blk_id));
                // this block is being driven by this connection
            } else if (block_connection->to_block == pattern_block) {
                // find the block that is driving this connection and it to the queue
                auto driver_blk_id = get_driving_block(block_id, *block_connection, atom_nlist);
                // add this driver block id with its corresponding pattern block to the queue
                pattern_block_queue.push(std::make_pair(block_connection->from_block, driver_blk_id));
            }

            // this block should be either driving or driven by the connection
            VTR_ASSERT(block_connection->from_block == pattern_block || block_connection->to_block == pattern_block);
            // go to the next connection of this pattern block
            block_connection = block_connection->next;
        }
    }
    // if all non-optional positions in the pack pattern have atoms
    // mapped to them, then this molecule is valid
    return true;
}

/**
 * Find the atom block in the netlist driven by this pin of the input atom block
 * If doesn't exist return AtomBlockId::INVALID()
 *      TODO: Limitation — For pack patterns other than chains, 
 *            the block should be driven by only one block
 *      block_id   : id of the atom block that is driving the net connected to the sink block
 *      connections : pack pattern connections from the given block
 */
static AtomBlockId get_sink_block(const AtomBlockId block_id,
                                  const t_pack_pattern_connections& connections,
                                  const AtomNetlist& atom_nlist,
                                  const LogicalModels& models) {
    const t_model_ports* from_port_model = connections.from_pin->port->model_port;
    const int from_pin_number = connections.from_pin->pin_number;
    auto from_port_id = atom_nlist.find_atom_port(block_id, from_port_model);

    const t_model_ports* to_port_model = connections.to_pin->port->model_port;
    const int to_pin_number = connections.to_pin->pin_number;
    const auto& to_pb_type = connections.to_block->pb_type;

    if (!from_port_id.is_valid()) {
        return AtomBlockId::INVALID();
    }

    auto net_id = atom_nlist.port_net(from_port_id, from_pin_number);
    if (!net_id.is_valid()) {
        return AtomBlockId::INVALID();
    }

    const auto& net_sinks = atom_nlist.net_sinks(net_id);
    // Iterate through all sink blocks and check whether any of them
    // is compatible with the block specified in the pack pattern.
    bool connected_to_latch = false;
    LogicalModelId latch_model_id = models.get_model_by_name(LogicalModels::MODEL_LATCH);
    AtomBlockId pattern_sink_block_id = AtomBlockId::INVALID();
    for (const auto& sink_pin_id : net_sinks) {
        auto sink_block_id = atom_nlist.pin_block(sink_pin_id);
        if (atom_nlist.block_model(sink_block_id) == latch_model_id) {
            connected_to_latch = true;
        }
        if (primitive_type_feasible(sink_block_id, to_pb_type)) {
            auto to_port_id = atom_nlist.find_atom_port(sink_block_id, to_port_model);
            auto to_pin_id = atom_nlist.find_pin(to_port_id, BitIndex(to_pin_number));
            if (to_pin_id == sink_pin_id) {
                pattern_sink_block_id = sink_block_id;
            }
        }
    }
    // If the number of sinks is greater than 1, and one of the connected blocks is a latch,
    // then we drop the block to avoid a situation where only registers or unregistered output
    // of the block can use the output pin.
    // TODO: This is a conservative assumption, and ideally we need to do analysis of the architecture
    // before to determine which pattern is supported by the architecture.
    if (connected_to_latch && net_sinks.size() > 1) {
        pattern_sink_block_id = AtomBlockId::INVALID();
    }
    return pattern_sink_block_id;
}

/**
 * Find the atom block in the netlist driving this pin of the input atom block
 * If doesn't exist return AtomBlockId::INVALID()
 * Limitation: This driving block should be driving only the input block
 *      block_id   : id of the atom block that is connected to a net driven by the driving block
 *      connections : pack pattern connections from the given block
 */
static AtomBlockId get_driving_block(const AtomBlockId block_id,
                                     const t_pack_pattern_connections& connections,
                                     const AtomNetlist& atom_nlist) {
    auto to_port_model = connections.to_pin->port->model_port;
    auto to_pin_number = connections.to_pin->pin_number;
    auto to_port_id = atom_nlist.find_atom_port(block_id, to_port_model);

    if (!to_port_id.is_valid()) {
        return AtomBlockId::INVALID();
    }

    auto net_id = atom_nlist.port_net(to_port_id, to_pin_number);
    if (net_id && atom_nlist.net_sinks(net_id).size() == 1) { /* Single fanout assumption */
        auto driver_blk_id = atom_nlist.net_driver_block(net_id);

        if (to_port_model->is_clock) {
            auto driver_blk_type = atom_nlist.block_type(driver_blk_id);

            // TODO: support multi-clock primitives.
            //       If the driver block is a .input block, this assertion should not
            //       be triggered as the sink block might have only one input pin, which
            //       would be a clock pin in case the sink block primitive is a clock generator,
            //       resulting in a pin_number == 0.
            VTR_ASSERT(to_pin_number == 1 || (to_pin_number == 0 && driver_blk_type == AtomBlockType::INPAD));
        }

        return driver_blk_id;
    }

    return AtomBlockId::INVALID();
}

static std::unordered_set<t_pb_type*> get_pattern_blocks(const t_pack_patterns& pack_pattern) {
    std::unordered_set<t_pb_type*> pattern_blocks;

    t_pack_pattern_connections* connections = pack_pattern.root_block->connections;
    if (connections == nullptr) {
        return pattern_blocks;
    }
    std::unordered_set<t_pb_graph_pin*> visited_from_pins;
    std::unordered_set<t_pb_graph_pin*> visited_to_pins;
    std::queue<t_pack_pattern_block*> pack_pattern_blocks;
    pack_pattern_blocks.push(connections->from_block);

    /** Start from the root block of the pack pattern and add the connected block to the queue */
    while (!pack_pattern_blocks.empty()) {
        t_pack_pattern_block* current_pattern_block = pack_pattern_blocks.front();
        pack_pattern_blocks.pop();
        t_pack_pattern_connections* current_connenction = current_pattern_block->connections;
        /*
         * Iterate through all the connections of the current pattern block to
         * add the connected block to the queue
         */
        while (current_connenction != nullptr) {
            if (visited_from_pins.find(current_connenction->from_pin) != visited_from_pins.end()) {
                if (visited_to_pins.find(current_connenction->to_pin) != visited_to_pins.end()) {
                    /* We've already seen this connection */
                    current_connenction = current_connenction->next;
                    continue;
                }
            }
            /*
             * To avoid visiting the same connection twice, since it is both stored in from_pin and to_pin,
             * add the from_pin and to_pin to the visited sets
             */
            visited_from_pins.insert(current_connenction->from_pin);
            visited_to_pins.insert(current_connenction->to_pin);

            /* The from_pin block belongs to the pattern block */
            pattern_blocks.insert(current_connenction->from_pin->port->parent_pb_type);
            pack_pattern_blocks.push(current_connenction->to_block);
            current_connenction = current_connenction->next;
        }
    }
    return pattern_blocks;
}

static void print_pack_molecules(const char* fname,
                                 const std::vector<t_pack_patterns>& list_of_pack_patterns,
                                 const int num_pack_patterns,
                                 const vtr::vector_map<PackMoleculeId, t_pack_molecule>& pack_molecules,
                                 const AtomNetlist& atom_nlist) {
    int i;
    FILE* fp;

    fp = std::fopen(fname, "w");
    fprintf(fp, "# of pack patterns %d\n", num_pack_patterns);

    for (i = 0; i < num_pack_patterns; i++) {
        VTR_ASSERT(list_of_pack_patterns[i].root_block);
        fprintf(fp, "pack pattern index %d block count %d name %s root %s\n",
                list_of_pack_patterns[i].index,
                list_of_pack_patterns[i].num_blocks,
                list_of_pack_patterns[i].name,
                list_of_pack_patterns[i].root_block->pb_type->name);
    }

    for (const t_pack_molecule& molecule : pack_molecules) {
        if (molecule.type == e_pack_pattern_molecule_type::MOLECULE_SINGLE_ATOM) {
            fprintf(fp, "\nmolecule type: atom\n");
            fprintf(fp, "\tpattern index %d: atom block %s\n", i,
                    atom_nlist.block_name(molecule.atom_block_ids[0]).c_str());
        } else if (molecule.type == e_pack_pattern_molecule_type::MOLECULE_FORCED_PACK) {
            fprintf(fp, "\nmolecule type: %s\n",
                    molecule.pack_pattern->name);
            for (i = 0; i < molecule.pack_pattern->num_blocks;
                 i++) {
                if (!molecule.atom_block_ids[i]) {
                    fprintf(fp, "\tpattern index %d: empty \n", i);
                } else {
                    fprintf(fp, "\tpattern index %d: atom block %s",
                            i,
                            atom_nlist.block_name(molecule.atom_block_ids[i]).c_str());
                    if (molecule.pack_pattern->root_block->block_id == i) {
                        fprintf(fp, " root node\n");
                    } else {
                        fprintf(fp, "\n");
                    }
                }
            }
        } else {
            VTR_ASSERT(0);
        }
    }

    fclose(fp);
}

/* Search through all primitives and return the lowest cost primitive that fits this atom block */
static t_pb_graph_node* get_expected_lowest_cost_primitive_for_atom_block(const AtomBlockId blk_id,
                                                                          const std::vector<t_logical_block_type>& logical_block_types) {
    float cost, best_cost;
    t_pb_graph_node *current, *best;

    best_cost = UNDEFINED;
    best = nullptr;
    current = nullptr;
    for (const t_logical_block_type& type : logical_block_types) {
        cost = UNDEFINED;
        current = get_expected_lowest_cost_primitive_for_atom_block_in_pb_graph_node(blk_id, type.pb_graph_head, &cost);
        if (cost != UNDEFINED) {
            if (best_cost == UNDEFINED || best_cost > cost) {
                best_cost = cost;
                best = current;
            }
        }
    }

    return best;
}

static t_pb_graph_node* get_expected_lowest_cost_primitive_for_atom_block_in_pb_graph_node(const AtomBlockId blk_id, t_pb_graph_node* curr_pb_graph_node, float* cost) {
    t_pb_graph_node *best, *cur;
    float cur_cost, best_cost;
    int i, j;

    best = nullptr;
    best_cost = UNDEFINED;
    if (curr_pb_graph_node == nullptr) {
        return nullptr;
    }

    if (curr_pb_graph_node->pb_type->blif_model != nullptr) {
        if (primitive_type_feasible(blk_id, curr_pb_graph_node->pb_type)) {
            cur_cost = compute_primitive_base_cost(curr_pb_graph_node);
            if (best_cost == UNDEFINED || best_cost > cur_cost) {
                best_cost = cur_cost;
                best = curr_pb_graph_node;
            }
        }
    } else {
        for (i = 0; i < curr_pb_graph_node->pb_type->num_modes; i++) {
            /* Early fail if this primitive for a mode that is disabled for packing */
            if (true == curr_pb_graph_node->pb_type->modes[i].disable_packing) {
                continue;
            }

            for (j = 0; j < curr_pb_graph_node->pb_type->modes[i].num_pb_type_children; j++) {
                *cost = UNDEFINED;
                cur = get_expected_lowest_cost_primitive_for_atom_block_in_pb_graph_node(blk_id, &curr_pb_graph_node->child_pb_graph_nodes[i][j][0], cost);
                if (cur != nullptr) {
                    if (best == nullptr || best_cost > *cost) {
                        best = cur;
                        best_cost = *cost;
                    }
                }
            }
        }
    }

    *cost = best_cost;
    return best;
}

/* Determine which of two pack pattern should take priority */
static int compare_pack_pattern(const t_pack_patterns* pattern_a, const t_pack_patterns* pattern_b) {
    float base_gain_a, base_gain_b, diff;

    /* Bigger patterns should take higher priority than smaller patterns because they are harder to fit */
    if (pattern_a->num_blocks > pattern_b->num_blocks) {
        return 1;
    } else if (pattern_a->num_blocks < pattern_b->num_blocks) {
        return -1;
    }

    base_gain_a = pattern_a->base_cost;
    base_gain_b = pattern_b->base_cost;
    diff = base_gain_a - base_gain_b;

    /* Less costly patterns should be used before more costly patterns */
    if (diff < 0) {
        return 1;
    }
    if (diff > 0) {
        return -1;
    }
    return 0;
}

/* A chain can extend across multiple atom blocks.  Must segment the chain to fit in an atom
 * block by identifying the actual atom that forms the root of the new chain.
 * Returns AtomBlockId::INVALID() if this block_index doesn't match up with any chain
 *
 * Assumes that the root of a chain is the primitive that starts the chain or is driven from outside the logic block
 * block_index: index of current atom
 * list_of_pack_pattern: ptr to current chain pattern
 */
static AtomBlockId find_new_root_atom_for_chain(const AtomBlockId blk_id,
                                                const t_pack_patterns* list_of_pack_patterns,
                                                const std::multimap<AtomBlockId, PackMoleculeId>& atom_molecules,
                                                const AtomNetlist& atom_nlist) {
    AtomBlockId new_root_blk_id;
    t_pb_graph_pin* root_ipin;
    t_pb_graph_node* root_pb_graph_node;
    t_model_ports* model_port;

    VTR_ASSERT(list_of_pack_patterns->is_chain == true);
    VTR_ASSERT(list_of_pack_patterns->chain_root_pins.size());
    root_ipin = list_of_pack_patterns->chain_root_pins[0][0];
    root_pb_graph_node = root_ipin->parent_node;

    if (primitive_type_feasible(blk_id, root_pb_graph_node->pb_type) == false) {
        return AtomBlockId::INVALID();
    }

    /* Assign driver furthest up the chain that matches the root node and is unassigned to a molecule as the root */
    model_port = root_ipin->port->model_port;

    // find the block id of the atom block driving the input of this block
    AtomBlockId driver_blk_id = atom_nlist.find_atom_pin_driver(blk_id, model_port, root_ipin->pin_number);

    // if there is no driver block for this net
    // then it is the furthest up the chain
    if (!driver_blk_id) {
        return blk_id;
    }
    // check if driver atom is already packed
    auto rng = atom_molecules.equal_range(driver_blk_id);
    bool rng_empty = (rng.first == rng.second);
    if (!rng_empty) {
        /* Driver is used/invalid, so current block is the furthest up the chain, return it */
        return blk_id;
    }

    // didn't find furthest atom up the chain, keep searching further up the chain
    new_root_blk_id = find_new_root_atom_for_chain(driver_blk_id, list_of_pack_patterns, atom_molecules, atom_nlist);

    if (!new_root_blk_id) {
        return blk_id;
    } else {
        return new_root_blk_id;
    }
}

/**
 * This function takes an input pin to a root (has no parent block) pb_graph_node
 * an returns a vector of all the output pins that are reachable from this input
 * pin and have the same packing pattern
 */

static std::vector<t_pb_graph_pin*> find_end_of_path(t_pb_graph_pin* input_pin, int pattern_index) {
    // Enforce some constraints on the function

    // 1) the start of the path should be at the input of the root block
    VTR_ASSERT(input_pin->is_root_block_pin());

    // 2) this pin is an input pin to the root block
    VTR_ASSERT(input_pin->num_input_edges == 0);

    // create a queue of pin pointers for the breadth first search
    std::queue<t_pb_graph_pin*> pins_queue;

    // add the input pin to the queue
    pins_queue.push(input_pin);

    // found reachable output pins
    std::vector<t_pb_graph_pin*> reachable_pins;

    // do breadth first search till all connected
    // pins are explored
    while (!pins_queue.empty()) {
        // get the first pin in the queue
        auto current_pin = pins_queue.front();

        // remove pin from queue
        pins_queue.pop();

        // expand search from current pin
        expand_search(current_pin, pins_queue, pattern_index);

        // if this is an output pin of a root block
        // add to reachable output pins
        if (current_pin->is_root_block_pin()
            && current_pin->num_output_edges == 0) {
            reachable_pins.push_back(current_pin);
        }
    }

    return reachable_pins;
}

static void expand_search(const t_pb_graph_pin* input_pin, std::queue<t_pb_graph_pin*>& pins_queue, const int pattern_index) {
    // If not a primitive input pin (has output edges)
    // -----------------------------------------------

    // iterate over all output edges at this pin
    for (int iedge = 0; iedge < input_pin->num_output_edges; iedge++) {
        const auto& pin_edge = input_pin->output_edges[iedge];
        // if this edge is not anotated with this pattern and its pattern cannot be inferred, ignore it.
        if (!pin_edge->annotated_with_pattern(pattern_index) && !pin_edge->infer_pattern) {
            continue;
        }

        // this edge either matched the pack pattern or its pack pattern could be inferred
        // iterate over all the pins of that edge and add them to the pins_queue
        for (int ipin = 0; ipin < pin_edge->num_output_pins; ipin++) {
            pins_queue.push(pin_edge->output_pins[ipin]);
        }

    } // End for pin edges

    // If a primitive input pin
    // ------------------------

    // if this is an input pin to a primitive, it won't have
    // output edges so the previous for loop won't be entered
    if (input_pin->is_primitive_pin() && input_pin->num_output_edges == 0) {
        // iterate over the output ports of the primitive
        const auto& pin_pb_graph_node = input_pin->parent_node;
        for (int iport = 0; iport < pin_pb_graph_node->num_output_ports; iport++) {
            // iterate over the pins of each port
            const auto& port_pins = pin_pb_graph_node->num_output_pins[iport];
            for (int ipin = 0; ipin < port_pins; ipin++) {
                // add primitive output pins to pins_queue to be explored
                pins_queue.push(&pin_pb_graph_node->output_pins[iport][ipin]);
            }
        }
    }

    // If this is a root block output pin
    // ----------------------------------

    // No expansion will happen in this case
}

/**
 *  This function takes a chain pack pattern and a root pb_block
 *  containing this pattern. Then searches for all the input pins of this
 *  pb_block that are annotated with this pattern. The function then
 *  identifies whether those inputs represent different starting point for
 *  this pattern or are all required for building this pattern.
 *
 *  If this inputs represent different starting point for this pattern, it
 *  means that in this pb_block there exist multiple chains that are exactly
 *  the same. For example an architecture that has two separate adder chains
 *  behaving exactly the same but are totally separate from each other.
 *
 *                        cin[0] cin[1]
 *                       ---|------|---
 *                       | ---    --- |
 *                       | | |    | |<|---- Full Adder
 *                       | ---    --- |
 *   Pb_block ---------> |  |      |<-|---- Second Adder chain
 *                       |  .      .  |
 *                       |  .      .  |
 *                       |  |<-----|--|---- First Adder chain
 *                       | ---    --- |
 *                       | | |    | | |
 *                       | ---    --- |
 *                       ---|------|---
 *                       cout[0] cout[1]
 *
 *  In this case, the chain_root_pin array of the pack pattern is updated
 *  with all the pin that represent a starting point for this pattern.
 */
static void find_all_equivalent_chains(t_pack_patterns* chain_pattern, const t_pb_graph_node* root_block) {
    // this vector will be updated with all root_block input
    // pins that are annotated with this chain pattern
    std::vector<t_pb_graph_pin*> chain_input_pins;

    // iterate over all the input pins of the root_block and populate
    // the chain_input_pins vector
    for (int iports = 0; iports < root_block->num_input_ports; iports++) {
        for (int ipins = 0; ipins < root_block->num_input_pins[iports]; ipins++) {
            auto& input_pin = root_block->input_pins[iports][ipins];
            for (int iedge = 0; iedge < input_pin.num_output_edges; iedge++) {
                if (input_pin.output_edges[iedge]->belongs_to_pattern(chain_pattern->index)) {
                    chain_input_pins.push_back(&input_pin);
                }
            }
        }
    }

    // if this chain has only one cluster input, then
    // there is no need to proceed with the search
    if (chain_input_pins.size() == 1) {
        update_chain_root_pins(chain_pattern, chain_input_pins);
        return;
    }

    // find the root block output pins reachable when starting from the chain_input_pins
    // found before following the edges that are annotated with the given pack_pattern
    std::vector<std::vector<t_pb_graph_pin*>> reachable_pins;

    for (const auto& pin_ptr : chain_input_pins) {
        auto reachable_output_pins = find_end_of_path(pin_ptr, chain_pattern->index);
        // sort the reachable output pins to compare them later using set_intersection
        std::stable_sort(reachable_output_pins.begin(), reachable_output_pins.end());
        reachable_pins.push_back(reachable_output_pins);
    }

    // Search for intersections between reachable pins. Intersection
    // between reachable indicates that found chain_input_pins
    // represent a single chain pattern and not multiple similar
    // chain patterns with multiple starting locations.
    std::vector<t_pb_graph_pin*> intersection;
    for (size_t i = 0; i < reachable_pins.size() - 1; i++) {
        for (size_t j = 1; j < reachable_pins.size(); j++) {
            std::set_intersection(reachable_pins[i].begin(), reachable_pins[i].end(),
                                  reachable_pins[j].begin(), reachable_pins[j].end(),
                                  std::back_inserter(intersection));
            if (intersection.size()) break;
        }
        if (intersection.size()) break;
    }

    // if there are no intersections between the reachable pins,
    // this each input pin represents a separate chain of type
    // chain_pattern. Else, they are all representing the same
    // chain.
    if (intersection.empty()) {
        // update the chain_root_pin array of the chain_pattern
        // with all the possible starting points of the chain.
        update_chain_root_pins(chain_pattern, chain_input_pins);
    }
}

/**
 *  This function takes a chain pack pattern and a vector of pin
 *  pointers that represent the root pb block input pins that can connect
 *  a chain to the previous pb block. The function uses the pin pointers
 *  to find the primitive input pin connected to them and updates
 *  the chain_root_pin array with this those pointers
 *  Side Effect: Updates the chain_root_pins array of the input chain_pattern
 */
static void update_chain_root_pins(t_pack_patterns* chain_pattern,
                                   const std::vector<t_pb_graph_pin*>& chain_input_pins) {
    std::vector<std::vector<t_pb_graph_pin*>> primitive_input_pins;

    std::unordered_set<t_pb_type*> pattern_blocks = get_pattern_blocks(*chain_pattern);
    for (const auto pin_ptr : chain_input_pins) {
        std::vector<t_pb_graph_pin*> connected_primitive_pins;
        get_all_connected_primitive_pins(pin_ptr, pattern_blocks, connected_primitive_pins);

        /**
         * It is required that the chain pins are connected inside a complex
         * block. Although it is allowed to have them disconnected in some
         * modes of the block provided that there is always at least one mode
         * that has them connected inside. The following assert checks for
         * that.
         */
        VTR_ASSERT(connected_primitive_pins.size());

        primitive_input_pins.push_back(connected_primitive_pins);
    }

    chain_pattern->chain_root_pins = primitive_input_pins;
}

/**
 *  This function takes a pin as an input an does a depth first search on all the output edges
 *  of this pin till it finds all the primitive input pins connected to this pin. For example,
 *  if the input pin given to this function is the Cin pin of the cluster. This pin will return
 *  the Cin pin of all the adder primitives connected to this pin. Which is for typical architectures
 *  will be only one pin connected to the very first adder in the cluster.
 */
static void get_all_connected_primitive_pins(const t_pb_graph_pin* cluster_input_pin,
                                             const std::unordered_set<t_pb_type*>& pattern_blocks,
                                             std::vector<t_pb_graph_pin*>& connected_primitive_pins) {
    /* Skip pins for modes that are disabled for packing*/
    if ((nullptr != cluster_input_pin->parent_node->pb_type->parent_mode)
        && (true == cluster_input_pin->parent_node->pb_type->parent_mode->disable_packing)) {
        return;
    }

    for (int iedge = 0; iedge < cluster_input_pin->num_output_edges; iedge++) {
        const auto& output_edge = cluster_input_pin->output_edges[iedge];
        for (int ipin = 0; ipin < output_edge->num_output_pins; ipin++) {
            if (output_edge->output_pins[ipin]->is_primitive_pin()) {
                /** Add the output pin to the vector only if it belongs to a pb_type registered in the pattern_blocks set */
                if (pattern_blocks.find(output_edge->output_pins[ipin]->parent_node->pb_type) != pattern_blocks.end()) {
                    connected_primitive_pins.push_back(output_edge->output_pins[ipin]);
                }
            } else {
                get_all_connected_primitive_pins(output_edge->output_pins[ipin], pattern_blocks, connected_primitive_pins);
            }
        }
    }
}

/**
 * This function initializes the chain info data structure of the molecule.
 * If this is the furthest molecule up the chain, the chain_info data
 * structure is created. Otherwise, the input pack_molecule is set to
 * point to the same chain_info of the molecule feeding it
 *
 * Limitation: This function assumes that the molecules of a chain are
 * created and fed to this function in order. Meaning the first molecule
 * fed to the function should be the furthest molecule up the chain.
 * The second one should should be the molecule directly after that one
 * and so on.
 */
static void init_molecule_chain_info(const AtomBlockId blk_id,
                                     t_pack_molecule& molecule,
                                     const vtr::vector_map<PackMoleculeId, t_pack_molecule>& pack_molecules,
                                     const std::multimap<AtomBlockId, PackMoleculeId>& atom_molecules,
                                     vtr::vector<MoleculeChainId, t_chain_info>& chain_info,
                                     const AtomNetlist& atom_nlist) {
    // the input molecule to this function should have a pack
    // pattern assigned to it and the input block should be valid
    VTR_ASSERT(molecule.pack_pattern && blk_id);

    auto root_ipin = molecule.pack_pattern->chain_root_pins[0][0];
    auto model_pin = root_ipin->port->model_port;
    auto pin_bit = root_ipin->pin_number;

    // find the atom driving the chain input pin of this atom
    auto driver_atom_id = atom_nlist.find_atom_pin_driver(blk_id, model_pin, pin_bit);

    // find the molecule this driver atom is mapped to
    auto itr = atom_molecules.find(driver_atom_id);

    // if this is the first molecule to be created for this chain
    // initialize the chain info data structure. This is the case
    // if either there is no driver to the block input pin or
    // if the driver is not part of a molecule
    if (!driver_atom_id || itr == atom_molecules.end()) {
        MoleculeChainId new_chain_id = MoleculeChainId(chain_info.size());
        t_chain_info new_chain_info;
        new_chain_info.is_long_chain = false;
        chain_info.push_back(std::move(new_chain_info));
        molecule.chain_id = new_chain_id;
    } else {
        // this is not the first molecule to be created for this chain
        // molecule driving blk_id
        PackMoleculeId prev_molecule_id = itr->second;
        VTR_ASSERT(prev_molecule_id.is_valid());
        const t_pack_molecule& prev_molecule = pack_molecules[prev_molecule_id];
        // molecule should have chain_info associated with it
        VTR_ASSERT(prev_molecule.chain_id.is_valid());
        // this molecule is now known to belong to a long chain
        chain_info[prev_molecule.chain_id].is_long_chain = true;
        // this new molecule should share the same chain
        molecule.chain_id = prev_molecule.chain_id;
    }
}

/**
 * This function prints all the starting points of the carry chains in the architecture
 */
static void print_chain_starting_points(t_pack_patterns* chain_pattern) {
    const auto& chain_root_pins = chain_pattern->chain_root_pins;

    VTR_LOGV(chain_root_pins.size() > 1, "\nThere are %zu independent chains for chain pattern \"%s\":\n",
             chain_pattern->chain_root_pins.size(), chain_pattern->name);
    VTR_LOGV(chain_root_pins.size() == 1, "\nThere is one chain in this architecture called \"%s\" with the following starting points:\n", chain_pattern->name);

    size_t chainId = 0;
    for (const auto& chain : chain_root_pins) {
        VTR_LOGV(chain_root_pins.size() > 1 && chain.size() > 1, "\n There are %zu starting points for chain id #%zu:\n", chain.size(), chainId++);
        VTR_LOGV(chain_root_pins.size() > 1 && chain.size() == 1, "\n There is 1 starting point for chain id #%zu:\n", chainId++);
        for (const auto& pin_ptr : chain) {
            VTR_LOG("\t%s\n", pin_ptr->to_string().c_str());
        }
    }

    VTR_LOG("\n");
}

Prepacker::Prepacker(const AtomNetlist& atom_nlist,
                     const LogicalModels& models,
                     const std::vector<t_logical_block_type>& logical_block_types) {
    vtr::ScopedStartFinishTimer prepacker_timer("Prepacker");

    // Allocate the pack patterns from the logical block types.
    list_of_pack_patterns = alloc_and_load_pack_patterns(logical_block_types);
    // Use the pack patterns to allocate and load the pack molecules.
    std::multimap<AtomBlockId, PackMoleculeId> atom_molecules_multimap;
    expected_lowest_cost_pb_gnode.resize(atom_nlist.blocks().size(), nullptr);
    alloc_and_load_pack_molecules(atom_molecules_multimap,
                                  atom_nlist,
                                  models,
                                  logical_block_types);

    // The multimap is a legacy thing. Since blocks can be part of multiple pack
    // patterns, during prepacking a block may be contained within multiple
    // molecules. However, by the end of prepacking, molecules should be
    // combined such that each block is contained in one and only one molecule.
    atom_molecule_.resize(atom_nlist.blocks().size(), PackMoleculeId::INVALID());
    for (AtomBlockId blk_id : atom_nlist.blocks()) {
        // Every atom block should be packed into a single molecule (no more
        // or less).
        VTR_ASSERT(atom_molecules_multimap.count(blk_id) == 1);
        atom_molecule_[blk_id] = atom_molecules_multimap.find(blk_id)->second;
    }
}

// TODO: Since this is constant per molecule, it may make sense to precompute
//       this information and store it in the prepacker class. This may be
//       expensive to calculate for large molecules.
t_molecule_stats Prepacker::calc_molecule_stats(PackMoleculeId molecule_id,
                                                const AtomNetlist& atom_nlist,
                                                const LogicalModels& models) const {
    VTR_ASSERT(molecule_id.is_valid());
    t_molecule_stats molecule_stats;

    const t_pack_molecule& molecule = pack_molecules_[molecule_id];

    //Calculate the number of available pins on primitives within the molecule
    for (auto blk : molecule.atom_block_ids) {
        if (!blk) continue;

        ++molecule_stats.num_blocks; //Record number of valid blocks in molecule

        LogicalModelId model_id = atom_nlist.block_model(blk);
        const t_model& model = models.get_model(model_id);

        for (const t_model_ports* input_port = model.inputs; input_port != nullptr; input_port = input_port->next) {
            molecule_stats.num_input_pins += input_port->size;
        }

        for (const t_model_ports* output_port = model.outputs; output_port != nullptr; output_port = output_port->next) {
            molecule_stats.num_output_pins += output_port->size;
        }
    }
    molecule_stats.num_pins = molecule_stats.num_input_pins + molecule_stats.num_output_pins;

    //Calculate the number of externally used pins
    std::set<AtomBlockId> molecule_atoms(molecule.atom_block_ids.begin(), molecule.atom_block_ids.end());
    for (auto blk : molecule.atom_block_ids) {
        if (!blk) continue;

        for (auto pin : atom_nlist.block_pins(blk)) {
            auto net = atom_nlist.pin_net(pin);

            auto pin_type = atom_nlist.pin_type(pin);
            if (pin_type == PinType::SINK) {
                auto driver_blk = atom_nlist.net_driver_block(net);

                if (molecule_atoms.count(driver_blk)) {
                    //Pin driven by a block within the molecule
                    //Does not count as an external connection
                } else {
                    //Pin driven by a block outside the molecule
                    ++molecule_stats.num_used_ext_inputs;
                }

            } else {
                VTR_ASSERT(pin_type == PinType::DRIVER);

                bool net_leaves_molecule = false;
                for (auto sink_pin : atom_nlist.net_sinks(net)) {
                    auto sink_blk = atom_nlist.pin_block(sink_pin);

                    if (!molecule_atoms.count(sink_blk)) {
                        //There is at least one sink outside of the current molecule
                        net_leaves_molecule = true;
                        break;
                    }
                }

                //We assume that any fanout occurs outside of the molecule, hence we only
                //count one used output (even if there are multiple sinks outside the molecule)
                if (net_leaves_molecule) {
                    ++molecule_stats.num_used_ext_outputs;
                }
            }
        }
    }
    molecule_stats.num_used_ext_pins = molecule_stats.num_used_ext_inputs + molecule_stats.num_used_ext_outputs;

    return molecule_stats;
}

t_molecule_stats Prepacker::calc_max_molecule_stats(const AtomNetlist& atom_nlist, const LogicalModels& models) const {
    t_molecule_stats max_molecules_stats;
    for (PackMoleculeId molecule_id : molecules()) {
        //Calculate per-molecule statistics
        t_molecule_stats cur_molecule_stats = calc_molecule_stats(molecule_id, atom_nlist, models);

        //Record the maximums (member-wise) over all molecules
        max_molecules_stats.num_blocks = std::max(max_molecules_stats.num_blocks, cur_molecule_stats.num_blocks);

        max_molecules_stats.num_pins = std::max(max_molecules_stats.num_pins, cur_molecule_stats.num_pins);
        max_molecules_stats.num_input_pins = std::max(max_molecules_stats.num_input_pins, cur_molecule_stats.num_input_pins);
        max_molecules_stats.num_output_pins = std::max(max_molecules_stats.num_output_pins, cur_molecule_stats.num_output_pins);

        max_molecules_stats.num_used_ext_pins = std::max(max_molecules_stats.num_used_ext_pins, cur_molecule_stats.num_used_ext_pins);
        max_molecules_stats.num_used_ext_inputs = std::max(max_molecules_stats.num_used_ext_inputs, cur_molecule_stats.num_used_ext_inputs);
        max_molecules_stats.num_used_ext_outputs = std::max(max_molecules_stats.num_used_ext_outputs, cur_molecule_stats.num_used_ext_outputs);
    }

    return max_molecules_stats;
}

Prepacker::~Prepacker() {
    // When the prepacker is reset (or destroyed), clean up the internal data
    // members.
    free_list_of_pack_patterns(list_of_pack_patterns);
}
