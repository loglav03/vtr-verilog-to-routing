#include "timing_reports.h"

#include "tatum/TimingReporter.hpp"

#include "vpr_types.h"
#include "globals.h"

#include "timing_info.h"
#include "timing_util.h"

#include "VprTimingGraphResolver.h"

void generate_setup_timing_stats(const std::string& prefix,
                                 const SetupTimingInfo& timing_info,
                                 const AnalysisDelayCalculator& delay_calc,
                                 const t_analysis_opts& analysis_opts,
                                 bool is_flat,
                                 const BlkLocRegistry& blk_loc_registry) {
    auto& timing_ctx = g_vpr_ctx.timing();
    auto& atom_ctx = g_vpr_ctx.atom();
    const LogicalModels& models = g_vpr_ctx.device().arch->models;

    print_setup_timing_summary(*timing_ctx.constraints, *timing_info.setup_analyzer(), "Final ", analysis_opts.write_timing_summary);

    VprTimingGraphResolver resolver(atom_ctx.netlist(), atom_ctx.lookup(), models, *timing_ctx.graph, delay_calc, is_flat, blk_loc_registry);
    resolver.set_detail_level(analysis_opts.timing_report_detail);

    tatum::TimingReporter timing_reporter(resolver, *timing_ctx.graph, *timing_ctx.constraints);

    timing_reporter.report_timing_setup(prefix + "report_timing.setup.rpt", *timing_info.setup_analyzer(), analysis_opts.timing_report_npaths);

    if (analysis_opts.timing_report_skew) {
        timing_reporter.report_skew_setup(prefix + "report_skew.setup.rpt", *timing_info.setup_analyzer(), analysis_opts.timing_report_npaths);
    }

    timing_reporter.report_unconstrained_setup(prefix + "report_unconstrained_timing.setup.rpt", *timing_info.setup_analyzer());
}

void generate_hold_timing_stats(const std::string& prefix,
                                const HoldTimingInfo& timing_info,
                                const AnalysisDelayCalculator& delay_calc,
                                const t_analysis_opts& analysis_opts,
                                bool is_flat,
                                const BlkLocRegistry& blk_loc_registry) {
    auto& timing_ctx = g_vpr_ctx.timing();
    auto& atom_ctx = g_vpr_ctx.atom();
    const LogicalModels& models = g_vpr_ctx.device().arch->models;

    print_hold_timing_summary(*timing_ctx.constraints, *timing_info.hold_analyzer(), "Final ");

    VprTimingGraphResolver resolver(atom_ctx.netlist(), atom_ctx.lookup(), models, *timing_ctx.graph, delay_calc, is_flat, blk_loc_registry);
    resolver.set_detail_level(analysis_opts.timing_report_detail);

    tatum::TimingReporter timing_reporter(resolver, *timing_ctx.graph, *timing_ctx.constraints);

    timing_reporter.report_timing_hold(prefix + "report_timing.hold.rpt", *timing_info.hold_analyzer(), analysis_opts.timing_report_npaths);

    if (analysis_opts.timing_report_skew) {
        timing_reporter.report_skew_hold(prefix + "report_skew.hold.rpt", *timing_info.hold_analyzer(), analysis_opts.timing_report_npaths);
    }

    timing_reporter.report_unconstrained_hold(prefix + "report_unconstrained_timing.hold.rpt", *timing_info.hold_analyzer());
}
