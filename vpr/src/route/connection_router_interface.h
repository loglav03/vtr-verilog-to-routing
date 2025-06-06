#ifndef _CONNECTION_ROUTER_INTERFACE_H
#define _CONNECTION_ROUTER_INTERFACE_H

#include <utility>

#include "heap_type.h"
#include "route_tree_fwd.h"
#include "rr_graph_fwd.h"
#include "vpr_types.h"
#include "router_stats.h"
#include "spatial_route_tree_lookup.h"

//Delay budget information for a specific connection
struct t_conn_delay_budget {
    float short_path_criticality; //Hold criticality

    float min_delay;    //Minimum legal connection delay
    float target_delay; //Target/goal connection delay
    float max_delay;    //Maximum legal connection delay
    e_routing_budgets_algorithm routing_budgets_algorithm;
};

struct t_conn_cost_params {
    float criticality = 1.;
    float astar_fac = 1.2;
    float astar_offset = 0.f;
    float post_target_prune_fac = 1.2f;
    float post_target_prune_offset = 0.f;
    float bend_cost = 1.;
    float pres_fac = 1.;
    const t_conn_delay_budget* delay_budget = nullptr;

    //TODO: Eventually once delay budgets are working, t_conn_delay_budget
    //should be factoured out, and the delay budget parameters integrated
    //into this struct instead. For now left as a pointer to control whether
    //budgets are enabled.
};

class ConnectionRouterInterface {
  public:
    virtual ~ConnectionRouterInterface() {}

    // Clears the modified list.  Should be called after reset_path_costs
    // have been called.
    virtual void clear_modified_rr_node_info() = 0;

    // Reset modified data in rr_node_route_inf based on modified_rr_node_inf.
    virtual void reset_path_costs() = 0;

    /** Finds a path from the route tree rooted at rt_root to sink_node.
     * This is used when you want to allow previous routing of the same net to
     * serve as valid start locations for the current connection.
     *
     * Returns a tuple of:
     * bool: path exists? (hard failure, rr graph disconnected)
     * bool: should retry with full bounding box?
     * RTExploredNode: the explored sink node, from which the cheapest path can be found via back-tracing */
    virtual std::tuple<bool, bool, RTExploredNode> timing_driven_route_connection_from_route_tree(
        const RouteTreeNode& rt_root,
        RRNodeId sink_node,
        const t_conn_cost_params& cost_params,
        const t_bb& bounding_box,
        RouterStats& router_stats,
        const ConnectionParameters& conn_params) = 0;

    /** Finds a path from the route tree rooted at rt_root to sink_node for a
     * high fanout net.
     *
     * Unlike timing_driven_route_connection_from_route_tree(), only part of
     * the route tree which is spatially close to the sink is added to the heap.
     *
     * Returns a tuple of:
     * bool: path exists? (hard failure, rr graph disconnected)
     * bool: should retry with full bounding box?
     * RTExploredNode: the explored sink node, from which the cheapest path can be found via back-tracing */
    virtual std::tuple<bool, bool, RTExploredNode> timing_driven_route_connection_from_route_tree_high_fanout(
        const RouteTreeNode& rt_root,
        RRNodeId sink_node,
        const t_conn_cost_params& cost_params,
        const t_bb& bounding_box,
        const SpatialRouteTreeLookup& spatial_rt_lookup,
        RouterStats& router_stats,
        const ConnectionParameters& conn_params) = 0;

    // Finds a path from the route tree rooted at rt_root to all sinks
    // available.
    //
    // Each element of the returned vector is a reachable sink.
    //
    // If cost_params.astar_fac is set to 0, this effectively becomes
    // Dijkstra's algorithm with a modified exit condition (runs until heap is
    // empty).  When using cost_params.astar_fac = 0, for efficiency the
    // RouterLookahead used should be the NoOpLookahead.
    //
    // Note: This routine is currently used only to generate information that
    // may be helpful in debugging an architecture.
    virtual vtr::vector<RRNodeId, RTExploredNode> timing_driven_find_all_shortest_paths_from_route_tree(
        const RouteTreeNode& rt_root,
        const t_conn_cost_params& cost_params,
        const t_bb& bounding_box,
        RouterStats& router_stats,
        const ConnectionParameters& conn_params) = 0;

    // Sets whether router debug information should be on.
    virtual void set_router_debug(bool router_debug) = 0;

    // Empty the route tree set used for RCV node detection
    // Will return if RCV is disabled
    // Called after each net is finished routing to flush the set
    virtual void empty_rcv_route_tree_set() = 0;

    // Enable or disable RCV in connection router
    // Enabling this will utilize extra path structures, as well as the RCV cost function
    //
    // Ensure route budgets have been calculated before enabling this
    virtual void set_rcv_enabled(bool enable) = 0;
};

#endif /* _CONNECTION_ROUTER_INTERFACE_H */
