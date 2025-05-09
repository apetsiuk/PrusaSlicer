#include "libslic3r.h"
#include "I18N.hpp"
#include "GCode.hpp"
#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "EdgeGrid.hpp"
#include "Geometry/ConvexHull.hpp"
#include "GCode/PrintExtents.hpp"
#include "GCode/Thumbnails.hpp"
#include "GCode/WipeTower.hpp"
#include "ShortestPath.hpp"
#include "Print.hpp"
#include "Utils.hpp"
#include "ClipperUtils.hpp"
#include "libslic3r.h"
#include "LocalesUtils.hpp"
#include "libslic3r/format.hpp"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <math.h>
#include <string_view>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <boost/nowide/iostream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/cstdlib.hpp>

#include "SVG.hpp"

#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_observer.h>
#include <tbb/enumerable_thread_specific.h>

// Intel redesigned some TBB interface considerably when merging TBB with their oneAPI set of libraries, see GH #7332.
// We are using quite an old TBB 2017 U7. Before we update our build servers, let's use the old API, which is deprecated in up to date TBB.
#if ! defined(TBB_VERSION_MAJOR)
    #include <tbb/version.h>
#endif
#if ! defined(TBB_VERSION_MAJOR)
    static_assert(false, "TBB_VERSION_MAJOR not defined");
#endif
#if TBB_VERSION_MAJOR >= 2021
    #include <tbb/parallel_pipeline.h>
    using slic3r_tbb_filtermode = tbb::filter_mode;
#else
    #include <tbb/pipeline.h>
    using slic3r_tbb_filtermode = tbb::filter;
#endif

#include <Shiny/Shiny.h>

using namespace std::literals::string_view_literals;

#if 0
// Enable debugging and asserts, even in the release build.
#define DEBUG
#define _DEBUG
#undef NDEBUG
#endif

#include <assert.h>

namespace Slic3r {

    //! macro used to mark string used at localization,
    //! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

// Only add a newline in case the current G-code does not end with a newline.
    static inline void check_add_eol(std::string& gcode)
    {
        if (!gcode.empty() && gcode.back() != '\n')
            gcode += '\n';
    }


    // Return true if tch_prefix is found in custom_gcode
    static bool custom_gcode_changes_tool(const std::string& custom_gcode, const std::string& tch_prefix, unsigned next_extruder)
    {
        bool ok = false;
        size_t from_pos = 0;
        size_t pos = 0;
        while ((pos = custom_gcode.find(tch_prefix, from_pos)) != std::string::npos) {
            if (pos + 1 == custom_gcode.size())
                break;
            from_pos = pos + 1;
            // only whitespace is allowed before the command
            while (--pos < custom_gcode.size() && custom_gcode[pos] != '\n') {
                if (!std::isspace(custom_gcode[pos]))
                    goto NEXT;
            }
            {
                // we should also check that the extruder changes to what was expected
                std::istringstream ss(custom_gcode.substr(from_pos, std::string::npos));
                unsigned num = 0;
                if (ss >> num)
                    ok = (num == next_extruder);
            }
        NEXT:;
        }
        return ok;
    }

    std::string OozePrevention::pre_toolchange(GCode& gcodegen)
    {
        std::string gcode;

        // move to the nearest standby point
        if (!this->standby_points.empty()) {
            // get current position in print coordinates
            Vec3d writer_pos = gcodegen.writer().get_position();
            Point pos = Point::new_scale(writer_pos(0), writer_pos(1));

            // find standby point
            Point standby_point;
            pos.nearest_point(this->standby_points, &standby_point);

            /*  We don't call gcodegen.travel_to() because we don't need retraction (it was already
                triggered by the caller) nor avoid_crossing_perimeters and also because the coordinates
                of the destination point must not be transformed by origin nor current extruder offset.  */
            gcode += gcodegen.writer().travel_to_xy(unscale(standby_point),
                "move to standby position");
        }

        if (gcodegen.config().standby_temperature_delta.value != 0) {
            // we assume that heating is always slower than cooling, so no need to block
            gcode += gcodegen.writer().set_temperature
            (this->_get_temp(gcodegen) + gcodegen.config().standby_temperature_delta.value, false, gcodegen.writer().extruder()->id());
        }

        return gcode;
    }

    std::string OozePrevention::post_toolchange(GCode& gcodegen)
    {
        return (gcodegen.config().standby_temperature_delta.value != 0) ?
            gcodegen.writer().set_temperature(this->_get_temp(gcodegen), true, gcodegen.writer().extruder()->id()) :
            std::string();
    }

    int
        OozePrevention::_get_temp(GCode& gcodegen)
    {
        return (gcodegen.layer() != NULL && gcodegen.layer()->id() == 0)
            ? gcodegen.config().first_layer_temperature.get_at(gcodegen.writer().extruder()->id())
            : gcodegen.config().temperature.get_at(gcodegen.writer().extruder()->id());
    }

    std::string Wipe::wipe(GCode& gcodegen, bool toolchange)
    {
        std::string     gcode;
        const Extruder &extruder = *gcodegen.writer().extruder();

        // Remaining quantized retraction length.
        if (double retract_length = extruder.retract_to_go(toolchange ? extruder.retract_length_toolchange() : extruder.retract_length()); 
            retract_length > 0 && this->path.size() >= 2) {
            // Reduce feedrate a bit; travel speed is often too high to move on existing material.
            // Too fast = ripping of existing material; too slow = short wipe path, thus more blob.
            const double wipe_speed = gcodegen.writer().config.travel_speed.value * 0.8;
            // Reduce retraction length a bit to avoid effective retraction speed to be greater than the configured one
            // due to rounding (TODO: test and/or better math for this).
            const double xy_to_e    = 0.95 * extruder.retract_speed() / wipe_speed;
            // Start with the current position, which may be different from the wipe path start in case of loop clipping.
            Vec2d prev = gcodegen.point_to_gcode_quantized(gcodegen.last_pos());
            auto  it   = this->path.points.begin();
            Vec2d p    = gcodegen.point_to_gcode_quantized(*(++ it));
            if (p != prev) {
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start) + "\n";
                auto  end  = this->path.points.end();
                bool  done = false;
                for (; it != end && ! done; ++ it) {
                    p = gcodegen.point_to_gcode_quantized(*it);
                    double segment_length = (p - prev).norm();
                    double dE = GCodeFormatter::quantize_e(xy_to_e * segment_length);
                    if (dE > retract_length - EPSILON) {
                        if (dE > retract_length + EPSILON)
                            // Shorten the segment.
                            p = prev + (p - prev) * (retract_length / dE);
                        dE   = retract_length;
                        done = true;
                    }
                    //FIXME one shall not generate the unnecessary G1 Fxxx commands, here wipe_speed is a constant inside this cycle.
                    // Is it here for the cooling markers? Or should it be outside of the cycle?
                    gcode += gcodegen.writer().set_speed(wipe_speed * 60, {}, gcodegen.enable_cooling_markers() ? ";_WIPE" : "");
                    gcode += gcodegen.writer().extrude_to_xy(p, -dE, "wipe and retract");
                    prev = p;
                    retract_length -= dE;
                }
                // add tag for processor
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End) + "\n";
                gcodegen.set_last_pos(gcodegen.gcode_to_point(prev));
            }
        }

        // Prevent wiping again on the same path.
        this->reset_path();
        return gcode;
    }

    static inline Point wipe_tower_point_to_object_point(GCode& gcodegen, const Vec2f& wipe_tower_pt)
    {
        return Point(scale_(wipe_tower_pt.x() - gcodegen.origin()(0)), scale_(wipe_tower_pt.y() - gcodegen.origin()(1)));
    }

    std::string WipeTowerIntegration::append_tcr(GCode& gcodegen, const WipeTower::ToolChangeResult& tcr, int new_extruder_id, double z) const
    {
        if (new_extruder_id != -1 && new_extruder_id != tcr.new_tool)
            throw Slic3r::InvalidArgument("Error: WipeTowerIntegration::append_tcr was asked to do a toolchange it didn't expect.");

        std::string gcode;

        // Toolchangeresult.gcode assumes the wipe tower corner is at the origin (except for priming lines)
        // We want to rotate and shift all extrusions (gcode postprocessing) and starting and ending position
        float alpha = m_wipe_tower_rotation / 180.f * float(M_PI);

        auto transform_wt_pt = [&alpha, this](const Vec2f& pt) -> Vec2f {
            Vec2f out = Eigen::Rotation2Df(alpha) * pt;
            out += m_wipe_tower_pos;
            return out;
        };

        Vec2f start_pos = tcr.start_pos;
        Vec2f end_pos = tcr.end_pos;
        if (! tcr.priming) {
            start_pos = transform_wt_pt(start_pos);
            end_pos = transform_wt_pt(end_pos);
        }

        Vec2f wipe_tower_offset = tcr.priming ? Vec2f::Zero() : m_wipe_tower_pos;
        float wipe_tower_rotation = tcr.priming ? 0.f : alpha;

        std::string tcr_rotated_gcode = post_process_wipe_tower_moves(tcr, wipe_tower_offset, wipe_tower_rotation);

        if (! tcr.priming) {
            // Move over the wipe tower.
            gcode += gcodegen.retract();
            gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
            gcode += gcodegen.travel_to(
                wipe_tower_point_to_object_point(gcodegen, start_pos),
                erMixed,
                "Travel to a Wipe Tower");
            gcode += gcodegen.unretract();
        }

        double current_z = gcodegen.writer().get_position().z();
        if (z == -1.) // in case no specific z was provided, print at current_z pos
            z = current_z;
        if (! is_approx(z, current_z)) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(z, "Travel down to the last wipe tower layer.");
            gcode += gcodegen.writer().unretract();
        }


        // Process the end filament gcode.
        std::string end_filament_gcode_str;
        if (gcodegen.writer().extruder() != nullptr) {
            // Process the custom end_filament_gcode in case of single_extruder_multi_material.
            unsigned int        old_extruder_id = gcodegen.writer().extruder()->id();
            const std::string& end_filament_gcode = gcodegen.config().end_filament_gcode.get_at(old_extruder_id);
            if (gcodegen.writer().extruder() != nullptr && !end_filament_gcode.empty()) {
                end_filament_gcode_str = gcodegen.placeholder_parser_process("end_filament_gcode", end_filament_gcode, old_extruder_id);
                check_add_eol(end_filament_gcode_str);
            }
        }

        // Process the custom toolchange_gcode. If it is empty, provide a simple Tn command to change the filament.
        // Otherwise, leave control to the user completely.
        std::string toolchange_gcode_str;
        const std::string& toolchange_gcode = gcodegen.config().toolchange_gcode.value;
        if (! toolchange_gcode.empty()) {
            DynamicConfig config;
            int previous_extruder_id = gcodegen.writer().extruder() ? (int)gcodegen.writer().extruder()->id() : -1;
            config.set_key_value("previous_extruder", new ConfigOptionInt(previous_extruder_id));
            config.set_key_value("next_extruder", new ConfigOptionInt((int)new_extruder_id));
            config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(tcr.print_z));
            config.set_key_value("toolchange_z", new ConfigOptionFloat(z));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(gcodegen.m_max_layer_z));
            toolchange_gcode_str = gcodegen.placeholder_parser_process("toolchange_gcode", toolchange_gcode, new_extruder_id, &config);
            check_add_eol(toolchange_gcode_str);
        }

        std::string toolchange_command;
        if (tcr.priming || (new_extruder_id >= 0 && gcodegen.writer().need_toolchange(new_extruder_id)))
            toolchange_command = gcodegen.writer().toolchange(new_extruder_id);
        if (!custom_gcode_changes_tool(toolchange_gcode_str, gcodegen.writer().toolchange_prefix(), new_extruder_id))
            toolchange_gcode_str += toolchange_command;
        else {
            // We have informed the m_writer about the current extruder_id, we can ignore the generated G-code.
        }

        gcodegen.placeholder_parser().set("current_extruder", new_extruder_id);

        // Process the start filament gcode.
        std::string start_filament_gcode_str;
        const std::string& start_filament_gcode = gcodegen.config().start_filament_gcode.get_at(new_extruder_id);
        if (!start_filament_gcode.empty()) {
            // Process the start_filament_gcode for the active filament only.
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
            config.set_key_value("layer_z",   new ConfigOptionFloat(gcodegen.writer().get_position()(2) - gcodegen.m_config.z_offset.value));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(gcodegen.m_max_layer_z));
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(new_extruder_id));
            start_filament_gcode_str = gcodegen.placeholder_parser_process("start_filament_gcode", start_filament_gcode, new_extruder_id, &config);
            check_add_eol(start_filament_gcode_str);
        }

        // Insert the end filament, toolchange, and start filament gcode into the generated gcode.
        DynamicConfig config;
        config.set_key_value("end_filament_gcode", new ConfigOptionString(end_filament_gcode_str));
        config.set_key_value("toolchange_gcode", new ConfigOptionString(toolchange_gcode_str));
        config.set_key_value("start_filament_gcode", new ConfigOptionString(start_filament_gcode_str));
        std::string tcr_gcode, tcr_escaped_gcode = gcodegen.placeholder_parser_process("tcr_rotated_gcode", tcr_rotated_gcode, new_extruder_id, &config);
        unescape_string_cstyle(tcr_escaped_gcode, tcr_gcode);
        gcode += tcr_gcode;
        check_add_eol(toolchange_gcode_str);


        // A phony move to the end position at the wipe tower.
        gcodegen.writer().travel_to_xy(end_pos.cast<double>());
        gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, end_pos));
        if (!is_approx(z, current_z)) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(current_z, "Travel back up to the topmost object layer.");
            gcode += gcodegen.writer().unretract();
        }

        else {
            // Prepare a future wipe.
            gcodegen.m_wipe.reset_path();
            for (const Vec2f& wipe_pt : tcr.wipe_path)
                gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(wipe_pt)));
        }

        // Let the planner know we are traveling between objects.
        gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
        return gcode;
    }

    // This function postprocesses gcode_original, rotates and moves all G1 extrusions and returns resulting gcode
    // Starting position has to be supplied explicitely (otherwise it would fail in case first G1 command only contained one coordinate)
    std::string WipeTowerIntegration::post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr, const Vec2f& translation, float angle) const
    {
        Vec2f extruder_offset = m_extruder_offsets[tcr.initial_tool].cast<float>();

        std::istringstream gcode_str(tcr.gcode);
        std::string gcode_out;
        std::string line;
        Vec2f pos = tcr.start_pos;
        Vec2f transformed_pos = pos;
        Vec2f old_pos(-1000.1f, -1000.1f);

        while (gcode_str) {
            std::getline(gcode_str, line);  // we read the gcode line by line

            // All G1 commands should be translated and rotated. X and Y coords are
            // only pushed to the output when they differ from last time.
            // WT generator can override this by appending the never_skip_tag
            if (boost::starts_with(line, "G1 ")) {
                bool never_skip = false;
                auto it = line.find(WipeTower::never_skip_tag());
                if (it != std::string::npos) {
                    // remove the tag and remember we saw it
                    never_skip = true;
                    line.erase(it, it + WipeTower::never_skip_tag().size());
                }
                std::ostringstream line_out;
                std::istringstream line_str(line);
                line_str >> std::noskipws;  // don't skip whitespace
                char ch = 0;
                line_str >> ch >> ch; // read the "G1"
                while (line_str >> ch) {
                    if (ch == 'X' || ch == 'Y')
                        line_str >> (ch == 'X' ? pos.x() : pos.y());
                    else
                        line_out << ch;
                }

                transformed_pos = Eigen::Rotation2Df(angle) * pos + translation;

                if (transformed_pos != old_pos || never_skip) {
                    line = line_out.str();
                    boost::trim_left(line); // Remove leading spaces
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(3) << "G1";
                    if (transformed_pos.x() != old_pos.x() || never_skip)
                        oss << " X" << transformed_pos.x() - extruder_offset.x();
                    if (transformed_pos.y() != old_pos.y() || never_skip)
                        oss << " Y" << transformed_pos.y() - extruder_offset.y();
                    if (! line.empty())
                        oss << " ";
                    line = oss.str() + line;
                    old_pos = transformed_pos;
                }
            }

            gcode_out += line + "\n";

            // If this was a toolchange command, we should change current extruder offset
            if (line == "[toolchange_gcode]") {
                extruder_offset = m_extruder_offsets[tcr.new_tool].cast<float>();

                // If the extruder offset changed, add an extra move so everything is continuous
                if (extruder_offset != m_extruder_offsets[tcr.initial_tool].cast<float>()) {
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(3)
                        << "G1 X" << transformed_pos.x() - extruder_offset.x()
                        << " Y" << transformed_pos.y() - extruder_offset.y()
                        << "\n";
                    gcode_out += oss.str();
                }
            }
        }
        return gcode_out;
    }


    std::string WipeTowerIntegration::prime(GCode& gcodegen)
    {
        std::string gcode;
        for (const WipeTower::ToolChangeResult& tcr : m_priming) {
            if (! tcr.extrusions.empty())
                gcode += append_tcr(gcodegen, tcr, tcr.new_tool);
        }
        return gcode;
    }

    std::string WipeTowerIntegration::tool_change(GCode& gcodegen, int extruder_id, bool finish_layer)
    {
        std::string gcode;
        assert(m_layer_idx >= 0);
        if (gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
            if (m_layer_idx < (int)m_tool_changes.size()) {
                if (!(size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
                    throw Slic3r::RuntimeError("Wipe tower generation failed, possibly due to empty first layer.");

                // Calculate where the wipe tower layer will be printed. -1 means that print z will not change,
                // resulting in a wipe tower with sparse layers.
                double wipe_tower_z = -1;
                bool ignore_sparse = false;
                if (gcodegen.config().wipe_tower_no_sparse_layers.value) {
                    wipe_tower_z = m_last_wipe_tower_print_z;
                    ignore_sparse = (m_tool_changes[m_layer_idx].size() == 1 && m_tool_changes[m_layer_idx].front().initial_tool == m_tool_changes[m_layer_idx].front().new_tool && m_layer_idx != 0);
                    if (m_tool_change_idx == 0 && !ignore_sparse)
                        wipe_tower_z = m_last_wipe_tower_print_z + m_tool_changes[m_layer_idx].front().layer_height;
                }

                if (!ignore_sparse) {
                    gcode += append_tcr(gcodegen, m_tool_changes[m_layer_idx][m_tool_change_idx++], extruder_id, wipe_tower_z);
                    m_last_wipe_tower_print_z = wipe_tower_z;
                }
            }
        }
        return gcode;
    }

    // Print is finished. Now it remains to unload the filament safely with ramming over the wipe tower.
    std::string WipeTowerIntegration::finalize(GCode& gcodegen)
    {
        std::string gcode;
        if (std::abs(gcodegen.writer().get_position()(2) - m_final_purge.print_z) > EPSILON)
            gcode += gcodegen.change_layer(m_final_purge.print_z);
        gcode += append_tcr(gcodegen, m_final_purge, -1);
        return gcode;
    }

    const std::vector<std::string> ColorPrintColors::Colors = { "#C0392B", "#E67E22", "#F1C40F", "#27AE60", "#1ABC9C", "#2980B9", "#9B59B6" };

#define EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_writer.extruder()->id())

// Collect pairs of object_layer + support_layer sorted by print_z.
// object_layer & support_layer are considered to be on the same print_z, if they are not further than EPSILON.
std::vector<GCode::LayerToPrint> GCode::collect_layers_to_print(const PrintObject& object)
{
    std::vector<GCode::LayerToPrint> layers_to_print;
    layers_to_print.reserve(object.layers().size() + object.support_layers().size());

    /*
    // Calculate a minimum support layer height as a minimum over all extruders, but not smaller than 10um.
    // This is the same logic as in support generator.
    //FIXME should we use the printing extruders instead?
    double gap_over_supports = object.config().support_material_contact_distance;
    // FIXME should we test object.config().support_material_synchronize_layers ? Currently the support layers are synchronized with object layers iff soluble supports.
    assert(!object.has_support() || gap_over_supports != 0. || object.config().support_material_synchronize_layers);
    if (gap_over_supports != 0.) {
        gap_over_supports = std::max(0., gap_over_supports);
        // Not a soluble support,
        double support_layer_height_min = 1000000.;
        for (auto lh : object.print()->config().min_layer_height.values)
            support_layer_height_min = std::min(support_layer_height_min, std::max(0.01, lh));
        gap_over_supports += support_layer_height_min;
    }*/

    std::vector<std::pair<double, double>> warning_ranges;

    // Pair the object layers with the support layers by z.
    size_t idx_object_layer = 0;
    size_t idx_support_layer = 0;
    const LayerToPrint* last_extrusion_layer = nullptr;
    while (idx_object_layer < object.layers().size() || idx_support_layer < object.support_layers().size()) {
        LayerToPrint layer_to_print;
        layer_to_print.object_layer = (idx_object_layer < object.layers().size()) ? object.layers()[idx_object_layer++] : nullptr;
        layer_to_print.support_layer = (idx_support_layer < object.support_layers().size()) ? object.support_layers()[idx_support_layer++] : nullptr;
        if (layer_to_print.object_layer && layer_to_print.support_layer) {
            if (layer_to_print.object_layer->print_z < layer_to_print.support_layer->print_z - EPSILON) {
                layer_to_print.support_layer = nullptr;
                --idx_support_layer;
            }
            else if (layer_to_print.support_layer->print_z < layer_to_print.object_layer->print_z - EPSILON) {
                layer_to_print.object_layer = nullptr;
                --idx_object_layer;
            }
        }

        layers_to_print.emplace_back(layer_to_print);

        bool has_extrusions = (layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
            || (layer_to_print.support_layer && layer_to_print.support_layer->has_extrusions());

        // Check that there are extrusions on the very first layer. The case with empty
        // first layer may result in skirt/brim in the air and maybe other issues.
        if (layers_to_print.size() == 1u) {
            if (!has_extrusions)
                throw Slic3r::SlicingError(_(L("There is an object with no extrusions in the first layer.")) + "\n" +
                                           _(L("Object name")) + ": " + object.model_object()->name);
        }

        // In case there are extrusions on this layer, check there is a layer to lay it on.
        if ((layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
            // Allow empty support layers, as the support generator may produce no extrusions for non-empty support regions.
            || (layer_to_print.support_layer /* && layer_to_print.support_layer->has_extrusions() */)) {
            double top_cd = object.config().support_material_contact_distance;
            double bottom_cd = object.config().support_material_bottom_contact_distance == 0. ? top_cd : object.config().support_material_bottom_contact_distance;

            double extra_gap = (layer_to_print.support_layer ? bottom_cd : top_cd);

            double maximal_print_z = (last_extrusion_layer ? last_extrusion_layer->print_z() : 0.)
                + layer_to_print.layer()->height
                + std::max(0., extra_gap);
            // Negative support_contact_z is not taken into account, it can result in false positives in cases
            // where previous layer has object extrusions too (https://github.com/prusa3d/PrusaSlicer/issues/2752)

            if (has_extrusions && layer_to_print.print_z() > maximal_print_z + 2. * EPSILON)
                warning_ranges.emplace_back(std::make_pair((last_extrusion_layer ? last_extrusion_layer->print_z() : 0.), layers_to_print.back().print_z()));
        }
        // Remember last layer with extrusions.
        if (has_extrusions)
            last_extrusion_layer = &layers_to_print.back();
    }

    if (! warning_ranges.empty()) {
        std::string warning;
        size_t i = 0;
        for (i = 0; i < std::min(warning_ranges.size(), size_t(3)); ++i)
            warning += Slic3r::format(_(L("Empty layer between %1% and %2%.")),
                                      warning_ranges[i].first, warning_ranges[i].second) + "\n";
        if (i < warning_ranges.size())
            warning += _(L("(Some lines not shown)")) + "\n";
        warning += "\n";
        warning += Slic3r::format(_(L("Object name: %1%")), object.model_object()->name) + "\n\n"
            + _(L("Make sure the object is printable. This is usually caused by negligibly small extrusions or by a faulty model. "
                "Try to repair the model or change its orientation on the bed."));

        const_cast<Print*>(object.print())->active_step_add_warning(
            PrintStateBase::WarningLevel::CRITICAL, warning);
    }

    return layers_to_print;
}

// Prepare for non-sequential printing of multiple objects: Support resp. object layers with nearly identical print_z
// will be printed for  all objects at once.
// Return a list of <print_z, per object LayerToPrint> items.
std::vector<std::pair<coordf_t, std::vector<GCode::LayerToPrint>>> GCode::collect_layers_to_print(const Print& print)
{
    struct OrderingItem {
        coordf_t    print_z;
        size_t      object_idx;
        size_t      layer_idx;
    };

    std::vector<std::vector<LayerToPrint>>  per_object(print.objects().size(), std::vector<LayerToPrint>());
    std::vector<OrderingItem>               ordering;
    for (size_t i = 0; i < print.objects().size(); ++i) {
        per_object[i] = collect_layers_to_print(*print.objects()[i]);
        OrderingItem ordering_item;
        ordering_item.object_idx = i;
        ordering.reserve(ordering.size() + per_object[i].size());
        const LayerToPrint& front = per_object[i].front();
        for (const LayerToPrint& ltp : per_object[i]) {
            ordering_item.print_z = ltp.print_z();
            ordering_item.layer_idx = &ltp - &front;
            ordering.emplace_back(ordering_item);
        }
    }

    std::sort(ordering.begin(), ordering.end(), [](const OrderingItem& oi1, const OrderingItem& oi2) { return oi1.print_z < oi2.print_z; });

    std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print;

    // Merge numerically very close Z values.
    for (size_t i = 0; i < ordering.size();) {
        // Find the last layer with roughly the same print_z.
        size_t j = i + 1;
        coordf_t zmax = ordering[i].print_z + EPSILON;
        for (; j < ordering.size() && ordering[j].print_z <= zmax; ++j);
        // Merge into layers_to_print.
        std::pair<coordf_t, std::vector<LayerToPrint>> merged;
        // Assign an average print_z to the set of layers with nearly equal print_z.
        merged.first = 0.5 * (ordering[i].print_z + ordering[j - 1].print_z);
        merged.second.assign(print.objects().size(), LayerToPrint());
        for (; i < j; ++i) {
            const OrderingItem& oi = ordering[i];
            assert(merged.second[oi.object_idx].layer() == nullptr);
            merged.second[oi.object_idx] = std::move(per_object[oi.object_idx][oi.layer_idx]);
        }
        layers_to_print.emplace_back(std::move(merged));
    }

    return layers_to_print;
}

// free functions called by GCode::do_export()
namespace DoExport {
//    static void update_print_estimated_times_stats(const GCodeProcessor& processor, PrintStatistics& print_statistics)
//    {
//        const GCodeProcessorResult& result = processor.get_result();
//        print_statistics.estimated_normal_print_time = get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
//        print_statistics.estimated_silent_print_time = processor.is_stealth_time_estimator_enabled() ?
//            get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time) : "N/A";
//    }

    static void update_print_estimated_stats(const GCodeProcessor& processor, const std::vector<Extruder>& extruders, PrintStatistics& print_statistics)
    {
        const GCodeProcessorResult& result = processor.get_result();
        print_statistics.estimated_normal_print_time = get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
        print_statistics.estimated_silent_print_time = processor.is_stealth_time_estimator_enabled() ?
            get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time) : "N/A";

        // update filament statictics
        double total_extruded_volume = 0.0;
        double total_used_filament   = 0.0;
        double total_weight          = 0.0;
        double total_cost            = 0.0;
        for (auto volume : result.print_statistics.volumes_per_extruder) {
            total_extruded_volume += volume.second;

            size_t extruder_id = volume.first;
            auto extruder = std::find_if(extruders.begin(), extruders.end(), [extruder_id](const Extruder& extr) { return extr.id() == extruder_id; });
            if (extruder == extruders.end())
                continue;

            double s = PI * sqr(0.5* extruder->filament_diameter());
            double weight = volume.second * extruder->filament_density() * 0.001;
            total_used_filament += volume.second/s;
            total_weight        += weight;
            total_cost          += weight * extruder->filament_cost() * 0.001;
        }

        print_statistics.total_extruded_volume = total_extruded_volume;
        print_statistics.total_used_filament   = total_used_filament;
        print_statistics.total_weight          = total_weight;
        print_statistics.total_cost            = total_cost;

        print_statistics.filament_stats        = result.print_statistics.volumes_per_extruder;
    }

    // if any reserved keyword is found, returns a std::vector containing the first MAX_COUNT keywords found
    // into pairs containing:
    // first: source
    // second: keyword
    // to be shown in the warning notification
    // The returned vector is empty if no keyword has been found
    static std::vector<std::pair<std::string, std::string>> validate_custom_gcode(const Print& print) {
        static const unsigned int MAX_TAGS_COUNT = 5;
        std::vector<std::pair<std::string, std::string>> ret;

        auto check = [&ret](const std::string& source, const std::string& gcode) {
            std::vector<std::string> tags;
            if (GCodeProcessor::contains_reserved_tags(gcode, MAX_TAGS_COUNT, tags)) {
                if (!tags.empty()) {
                    size_t i = 0;
                    while (ret.size() < MAX_TAGS_COUNT && i < tags.size()) {
                        ret.push_back({ source, tags[i] });
                        ++i;
                    }
                }
            }
        };

        const GCodeConfig& config = print.config();
        check(_(L("Start G-code")), config.start_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("End G-code")), config.end_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Before layer change G-code")), config.before_layer_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("After layer change G-code")), config.layer_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Tool change G-code")), config.toolchange_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Between objects G-code (for sequential printing)")), config.between_objects_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Color Change G-code")), config.color_change_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Pause Print G-code")), config.pause_print_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Template Custom G-code")), config.template_custom_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : config.start_filament_gcode.values) {
                check(_(L("Filament Start G-code")), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : config.end_filament_gcode.values) {
                check(_(L("Filament End G-code")), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        if (ret.size() < MAX_TAGS_COUNT) {
            const CustomGCode::Info& custom_gcode_per_print_z = print.model().custom_gcode_per_print_z;
            for (const auto& gcode : custom_gcode_per_print_z.gcodes) {
                check(_(L("Custom G-code")), gcode.extra);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }

        return ret;
    }
} // namespace DoExport

void GCode::do_export(Print* print, const char* path, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb)
{
    PROFILE_CLEAR();

    CNumericLocalesSetter locales_setter;

    // Does the file exist? If so, we hope that it is still valid.
    {
        PrintStateBase::StateWithTimeStamp state = print->step_state_with_timestamp(psGCodeExport);
        if (! state.enabled || (state.state == PrintStateBase::DONE && boost::filesystem::exists(boost::filesystem::path(path))))
            return;
    }

    // Enabled and either not done, or marked as done while the output file is missing.
    print->set_started(psGCodeExport);

    // check if any custom gcode contains keywords used by the gcode processor to
    // produce time estimation and gcode toolpaths
    std::vector<std::pair<std::string, std::string>> validation_res = DoExport::validate_custom_gcode(*print);
    if (!validation_res.empty()) {
        std::string reports;
        for (const auto& [source, keyword] : validation_res) {
            reports += source + ": \"" + keyword + "\"\n";
        }
        print->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
            _(L("In the custom G-code were found reserved keywords:")) + "\n" +
            reports +
            _(L("This may cause problems in g-code visualization and printing time estimation.")));
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code..." << log_memory_info();

    // Remove the old g-code if it exists.
    boost::nowide::remove(path);

    std::string path_tmp(path);
    path_tmp += ".tmp";

    m_processor.initialize(path_tmp);
    GCodeOutputStream file(boost::nowide::fopen(path_tmp.c_str(), "wb"), m_processor);
    if (! file.is_open())
        throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed.\nCannot open the file for writing.\n");

    try {
        m_placeholder_parser_failed_templates.clear();
        this->_do_export(*print, file, thumbnail_cb);
        file.flush();
        if (file.is_error()) {
            file.close();
            boost::nowide::remove(path_tmp.c_str());
            throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed\nIs the disk full?\n");
        }
    } catch (std::exception & /* ex */) {
        // Rethrow on any exception. std::runtime_exception and CanceledException are expected to be thrown.
        // Close and remove the file.
        file.close();
        boost::nowide::remove(path_tmp.c_str());
        throw;
    }
    file.close();

    if (! m_placeholder_parser_failed_templates.empty()) {
        // G-code export proceeded, but some of the PlaceholderParser substitutions failed.
        //FIXME localize!
        std::string msg = std::string("G-code export to ") + path + " failed due to invalid custom G-code sections:\n\n";
        for (const auto &name_and_error : m_placeholder_parser_failed_templates)
            msg += name_and_error.first + "\n" + name_and_error.second + "\n";
        msg += "\nPlease inspect the file ";
        msg += path_tmp + " for error messages enclosed between\n";
        msg += "        !!!!! Failed to process the custom G-code template ...\n";
        msg += "and\n";
        msg += "        !!!!! End of an error report for the custom G-code template ...\n";
        msg += "for all macro processing errors.";
        throw Slic3r::PlaceholderParserError(msg);
    }

    BOOST_LOG_TRIVIAL(debug) << "Start processing gcode, " << log_memory_info();
    // Post-process the G-code to update time stamps.
    m_processor.finalize(true);
//    DoExport::update_print_estimated_times_stats(m_processor, print->m_print_statistics);
    DoExport::update_print_estimated_stats(m_processor, m_writer.extruders(), print->m_print_statistics);
    if (result != nullptr) {
        *result = std::move(m_processor.extract_result());
        // set the filename to the correct value
        result->filename = path;
    }
    BOOST_LOG_TRIVIAL(debug) << "Finished processing gcode, " << log_memory_info();

    if (rename_file(path_tmp, path))
        throw Slic3r::RuntimeError(
            std::string("Failed to rename the output G-code file from ") + path_tmp + " to " + path + '\n' +
            "Is " + path_tmp + " locked?" + '\n');

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code finished" << log_memory_info();
    print->set_done(psGCodeExport);

    // Write the profiler measurements to file
    PROFILE_UPDATE();
    PROFILE_OUTPUT(debug_out_path("gcode-export-profile.txt").c_str());
}

void GCode::do_batched_export(Print* print, const char* path, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb)
{
    PROFILE_CLEAR();

    CNumericLocalesSetter locales_setter;

    // Does the file exist? If so, we hope that it is still valid.
    {
        PrintStateBase::StateWithTimeStamp state = print->step_state_with_timestamp(psGCodeExport);
        if (!state.enabled || (state.state == PrintStateBase::DONE && boost::filesystem::exists(boost::filesystem::path(path))))
            return;
    }

    // Enabled and either not done, or marked as done while the output file is missing.
    print->set_started(psGCodeExport);

    // check if any custom gcode contains keywords used by the gcode processor to
    // produce time estimation and gcode toolpaths
    std::vector<std::pair<std::string, std::string>> validation_res = DoExport::validate_custom_gcode(*print);
    if (!validation_res.empty()) {
        std::string reports;
        for (const auto& [source, keyword] : validation_res) {
            reports += source + ": \"" + keyword + "\"\n";
        }
        print->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
            _(L("In the custom G-code were found reserved keywords:")) + "\n" +
            reports +
            _(L("This may cause problems in g-code visualization and printing time estimation.")));
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code..." << log_memory_info();

    // Remove the old g-code if it exists.
    boost::nowide::remove(path);

    std::string path_tmp(path);
    path_tmp += ".tmp";

    m_processor.initialize(path_tmp);
    GCodeOutputStream file(boost::nowide::fopen(path_tmp.c_str(), "wb"), m_processor);
    if (!file.is_open())
        throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed.\nCannot open the file for writing.\n");

    try {
        m_placeholder_parser_failed_templates.clear();
        this->_do_batched_export(*print, file, thumbnail_cb);
        file.flush();
        if (file.is_error()) {
            file.close();
            boost::nowide::remove(path_tmp.c_str());
            throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed\nIs the disk full?\n");
        }
    }
    catch (std::exception& /* ex */) {
        // Rethrow on any exception. std::runtime_exception and CanceledException are expected to be thrown.
        // Close and remove the file.
        file.close();
        boost::nowide::remove(path_tmp.c_str());
        throw;
    }
    file.close();

    if (!m_placeholder_parser_failed_templates.empty()) {
        // G-code export proceeded, but some of the PlaceholderParser substitutions failed.
        //FIXME localize!
        std::string msg = std::string("G-code export to ") + path + " failed due to invalid custom G-code sections:\n\n";
        for (const auto& name_and_error : m_placeholder_parser_failed_templates)
            msg += name_and_error.first + "\n" + name_and_error.second + "\n";
        msg += "\nPlease inspect the file ";
        msg += path_tmp + " for error messages enclosed between\n";
        msg += "        !!!!! Failed to process the custom G-code template ...\n";
        msg += "and\n";
        msg += "        !!!!! End of an error report for the custom G-code template ...\n";
        msg += "for all macro processing errors.";
        throw Slic3r::PlaceholderParserError(msg);
    }

    BOOST_LOG_TRIVIAL(debug) << "Start processing gcode, " << log_memory_info();

    // Assign bath numbers to GCodeProcessor for color visualization
    //m_processor.atc_assign_batch_numbers(gcode);
    //for (GCodeProcessorResult::MoveVertex& move : m_result.moves)
    //this->ATC_printing_map.get_node(0);
    //m_processor.get_result().moves[9].atc_batching = 103;

    /*
    for (GCodeProcessorResult::MoveVertex& move : result->moves)
    {
        if (move.type == EMoveType::Extrude) {
            move.atc_batching = 52;
        }
    }
    result->moves[30].atc_batching = 103;
    result->moves[40].atc_batching = 35;
    */

    // Post-process the G-code to update time stamps.
    m_processor.finalize(true);
    
    //    DoExport::update_print_estimated_times_stats(m_processor, print->m_print_statistics);
    DoExport::update_print_estimated_stats(m_processor, m_writer.extruders(), print->m_print_statistics);
    if (result != nullptr) {
        *result = std::move(m_processor.extract_result());
        // set the filename to the correct value
        result->filename = path;
    }
    BOOST_LOG_TRIVIAL(debug) << "Finished processing gcode, " << log_memory_info();

    if (rename_file(path_tmp, path))
        throw Slic3r::RuntimeError(
            std::string("Failed to rename the output G-code file from ") + path_tmp + " to " + path + '\n' +
            "Is " + path_tmp + " locked?" + '\n');

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code finished" << log_memory_info();
    print->set_done(psGCodeExport);

    // Write the profiler measurements to file
    PROFILE_UPDATE();
    PROFILE_OUTPUT(debug_out_path("gcode-export-profile.txt").c_str());
}


// free functions called by GCode::_do_export()
namespace DoExport {
    static void init_gcode_processor(const PrintConfig& config, GCodeProcessor& processor, bool& silent_time_estimator_enabled)
    {
        silent_time_estimator_enabled = (config.gcode_flavor == gcfMarlinLegacy || config.gcode_flavor == gcfMarlinFirmware)
                                        && config.silent_mode;
        processor.reset();
        processor.apply_config(config);
        processor.enable_stealth_time_estimator(silent_time_estimator_enabled);
    }

	static double autospeed_volumetric_limit(const Print &print)
	{
	    // get the minimum cross-section used in the print
	    std::vector<double> mm3_per_mm;
	    for (auto object : print.objects()) {
	        for (size_t region_id = 0; region_id < object->num_printing_regions(); ++ region_id) {
	            const PrintRegion &region = object->printing_region(region_id);
	            for (auto layer : object->layers()) {
	                const LayerRegion* layerm = layer->regions()[region_id];
	                if (region.config().get_abs_value("perimeter_speed") == 0 ||
	                    region.config().get_abs_value("small_perimeter_speed") == 0 ||
	                    region.config().get_abs_value("external_perimeter_speed") == 0 ||
	                    region.config().get_abs_value("bridge_speed") == 0)
	                    mm3_per_mm.push_back(layerm->perimeters.min_mm3_per_mm());
	                if (region.config().get_abs_value("infill_speed") == 0 ||
	                    region.config().get_abs_value("solid_infill_speed") == 0 ||
	                    region.config().get_abs_value("top_solid_infill_speed") == 0 ||
                        region.config().get_abs_value("bridge_speed") == 0)
                    {
                        // Minimal volumetric flow should not be calculated over ironing extrusions.
                        // Use following lambda instead of the built-it method.
                        // https://github.com/prusa3d/PrusaSlicer/issues/5082
                        auto min_mm3_per_mm_no_ironing = [](const ExtrusionEntityCollection& eec) -> double {
                            double min = std::numeric_limits<double>::max();
                            for (const ExtrusionEntity* ee : eec.entities)
                                if (ee->role() != erIroning)
                                    min = std::min(min, ee->min_mm3_per_mm());
                            return min;
                        };

                        mm3_per_mm.push_back(min_mm3_per_mm_no_ironing(layerm->fills));
                    }
	            }
	        }
	        if (object->config().get_abs_value("support_material_speed") == 0 ||
	            object->config().get_abs_value("support_material_interface_speed") == 0)
	            for (auto layer : object->support_layers())
	                mm3_per_mm.push_back(layer->support_fills.min_mm3_per_mm());
	    }
	    // filter out 0-width segments
	    mm3_per_mm.erase(std::remove_if(mm3_per_mm.begin(), mm3_per_mm.end(), [](double v) { return v < 0.000001; }), mm3_per_mm.end());
	    double volumetric_speed = 0.;
	    if (! mm3_per_mm.empty()) {
	        // In order to honor max_print_speed we need to find a target volumetric
	        // speed that we can use throughout the print. So we define this target 
	        // volumetric speed as the volumetric speed produced by printing the 
	        // smallest cross-section at the maximum speed: any larger cross-section
	        // will need slower feedrates.
	        volumetric_speed = *std::min_element(mm3_per_mm.begin(), mm3_per_mm.end()) * print.config().max_print_speed.value;
	        // limit such volumetric speed with max_volumetric_speed if set
	        if (print.config().max_volumetric_speed.value > 0)
	            volumetric_speed = std::min(volumetric_speed, print.config().max_volumetric_speed.value);
	    }
	    return volumetric_speed;
	}


    static void init_ooze_prevention(const Print &print, OozePrevention &ooze_prevention)
	{
	    // Calculate wiping points if needed
	    if (print.config().ooze_prevention.value && ! print.config().single_extruder_multi_material) {
	        Points skirt_points;
	        for (const ExtrusionEntity *ee : print.skirt().entities)
	            for (const ExtrusionPath &path : dynamic_cast<const ExtrusionLoop*>(ee)->paths)
	                append(skirt_points, path.polyline.points);
	        if (! skirt_points.empty()) {
	            Polygon outer_skirt = Slic3r::Geometry::convex_hull(skirt_points);
	            Polygons skirts;
	            for (unsigned int extruder_id : print.extruders()) {
	                const Vec2d &extruder_offset = print.config().extruder_offset.get_at(extruder_id);
	                Polygon s(outer_skirt);
	                s.translate(Point::new_scale(-extruder_offset(0), -extruder_offset(1)));
	                skirts.emplace_back(std::move(s));
	            }
	            ooze_prevention.enable = true;
	            ooze_prevention.standby_points = offset(Slic3r::Geometry::convex_hull(skirts), float(scale_(3.))).front().equally_spaced_points(float(scale_(10.)));
	#if 0
	                require "Slic3r/SVG.pm";
	                Slic3r::SVG::output(
	                    "ooze_prevention.svg",
	                    red_polygons    => \@skirts,
	                    polygons        => [$outer_skirt],
	                    points          => $gcodegen->ooze_prevention->standby_points,
	                );
	#endif
	        }
	    }
	}

	// Fill in print_statistics and return formatted string containing filament statistics to be inserted into G-code comment section.
    static std::string update_print_stats_and_format_filament_stats(
        const bool                   has_wipe_tower,
	    const WipeTowerData         &wipe_tower_data,
        const FullPrintConfig       &config,
	    const std::vector<Extruder> &extruders,
        unsigned int                 initial_extruder_id,
		PrintStatistics 		    &print_statistics)
    {
		std::string filament_stats_string_out;

	    print_statistics.clear();
        print_statistics.total_toolchanges = std::max(0, wipe_tower_data.number_of_toolchanges);
        print_statistics.initial_extruder_id = initial_extruder_id;
        std::vector<std::string> filament_types;
	    if (! extruders.empty()) {
	        std::pair<std::string, unsigned int> out_filament_used_mm ("; filament used [mm] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_used_cm3("; filament used [cm3] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_used_g  ("; filament used [g] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_cost    ("; filament cost = ", 0);
	        for (const Extruder &extruder : extruders) {
                print_statistics.printing_extruders.emplace_back(extruder.id());
                filament_types.emplace_back(config.filament_type.get_at(extruder.id()));

	            double used_filament   = extruder.used_filament() + (has_wipe_tower ? wipe_tower_data.used_filament[extruder.id()] : 0.f);
	            double extruded_volume = extruder.extruded_volume() + (has_wipe_tower ? wipe_tower_data.used_filament[extruder.id()] * 2.4052f : 0.f); // assumes 1.75mm filament diameter
	            double filament_weight = extruded_volume * extruder.filament_density() * 0.001;
	            double filament_cost   = filament_weight * extruder.filament_cost()    * 0.001;
                auto append = [&extruder](std::pair<std::string, unsigned int> &dst, const char *tmpl, double value) {
                    assert(is_decimal_separator_point());
	                while (dst.second < extruder.id()) {
	                    // Fill in the non-printing extruders with zeros.
	                    dst.first += (dst.second > 0) ? ", 0" : "0";
	                    ++ dst.second;
	                }
	                if (dst.second > 0)
	                    dst.first += ", ";
	                char buf[64];
					sprintf(buf, tmpl, value);
	                dst.first += buf;
	                ++ dst.second;
	            };
	            append(out_filament_used_mm,  "%.2lf", used_filament);
	            append(out_filament_used_cm3, "%.2lf", extruded_volume * 0.001);
	            if (filament_weight > 0.) {
	                print_statistics.total_weight = print_statistics.total_weight + filament_weight;
	                append(out_filament_used_g, "%.2lf", filament_weight);
	                if (filament_cost > 0.) {
	                    print_statistics.total_cost = print_statistics.total_cost + filament_cost;
	                    append(out_filament_cost, "%.2lf", filament_cost);
	                }
	            }
	            print_statistics.total_used_filament += used_filament;
	            print_statistics.total_extruded_volume += extruded_volume;
	            print_statistics.total_wipe_tower_filament += has_wipe_tower ? used_filament - extruder.used_filament() : 0.;
	            print_statistics.total_wipe_tower_cost += has_wipe_tower ? (extruded_volume - extruder.extruded_volume())* extruder.filament_density() * 0.001 * extruder.filament_cost() * 0.001 : 0.;
	        }
	        filament_stats_string_out += out_filament_used_mm.first;
            filament_stats_string_out += "\n" + out_filament_used_cm3.first;
            if (out_filament_used_g.second)
                filament_stats_string_out += "\n" + out_filament_used_g.first;
            if (out_filament_cost.second)
                filament_stats_string_out += "\n" + out_filament_cost.first;
            print_statistics.initial_filament_type = config.filament_type.get_at(initial_extruder_id);
            std::sort(filament_types.begin(), filament_types.end());
            print_statistics.printing_filament_types = filament_types.front();
            for (size_t i = 1; i < filament_types.size(); ++ i) {
                print_statistics.printing_filament_types += ",";
                print_statistics.printing_filament_types += filament_types[i];
            }
        }
        return filament_stats_string_out;
    }
}

#if 0
// Sort the PrintObjects by their increasing Z, likely useful for avoiding colisions on Deltas during sequential prints.
static inline std::vector<const PrintInstance*> sort_object_instances_by_max_z(const Print &print)
{
    std::vector<const PrintObject*> objects(print.objects().begin(), print.objects().end());
    std::sort(objects.begin(), objects.end(), [](const PrintObject *po1, const PrintObject *po2) { return po1->height() < po2->height(); });
    std::vector<const PrintInstance*> instances;
    instances.reserve(objects.size());
    for (const PrintObject *object : objects)
        for (size_t i = 0; i < object->instances().size(); ++ i)
            instances.emplace_back(&object->instances()[i]);
    return instances;
}
#endif

// Produce a vector of PrintObjects in the order of their respective ModelObjects in print.model().
std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print)
{
    // Build up map from ModelInstance* to PrintInstance*
    std::vector<std::pair<const ModelInstance*, const PrintInstance*>> model_instance_to_print_instance;
    model_instance_to_print_instance.reserve(print.num_object_instances());
    for (const PrintObject *print_object : print.objects())
        for (const PrintInstance &print_instance : print_object->instances())
            model_instance_to_print_instance.emplace_back(print_instance.model_instance, &print_instance);
    std::sort(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), [](auto &l, auto &r) { return l.first < r.first; });

    std::vector<const PrintInstance*> instances;
    instances.reserve(model_instance_to_print_instance.size());
    for (const ModelObject *model_object : print.model().objects)
        for (const ModelInstance *model_instance : model_object->instances) {
            auto it = std::lower_bound(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), std::make_pair(model_instance, nullptr), [](auto &l, auto &r) { return l.first < r.first; });
            if (it != model_instance_to_print_instance.end() && it->first == model_instance)
                instances.emplace_back(it->second);
        }
    return instances;
}

void GCode::_do_export(Print& print, GCodeOutputStream &file, ThumbnailsGeneratorCallback thumbnail_cb)
{
    PROFILE_FUNC();

    // modifies m_silent_time_estimator_enabled
    DoExport::init_gcode_processor(print.config(), m_processor, m_silent_time_estimator_enabled);

    if (! print.config().gcode_substitutions.values.empty()) {
        m_find_replace = make_unique<GCodeFindReplace>(print.config());
        file.set_find_replace(m_find_replace.get(), false);
    }

    // resets analyzer's tracking data
    m_last_height  = 0.f;
    m_last_layer_z = 0.f;
    m_max_layer_z  = 0.f;
    m_last_width = 0.f;
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    m_last_mm3_per_mm = 0.;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    // How many times will be change_layer() called?
    // change_layer() in turn increments the progress bar status.
    m_layer_count = 0;
    if (print.config().complete_objects.value) {
        // Add each of the object's layers separately.
        for (auto object : print.objects()) {
            std::vector<coordf_t> zs;
            zs.reserve(object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.push_back(layer->print_z);
            std::sort(zs.begin(), zs.end());
            m_layer_count += (unsigned int)(object->instances().size() * (std::unique(zs.begin(), zs.end()) - zs.begin()));
        }
    } else {
        // Print all objects with the same print_z together.
        std::vector<coordf_t> zs;
        for (auto object : print.objects()) {
            zs.reserve(zs.size() + object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.push_back(layer->print_z);
        }
        std::sort(zs.begin(), zs.end());
        m_layer_count = (unsigned int)(std::unique(zs.begin(), zs.end()) - zs.begin());
    }
    print.throw_if_canceled();

    m_enable_cooling_markers = true;
    this->apply_print_config(print.config());

    m_volumetric_speed = DoExport::autospeed_volumetric_limit(print);
    print.throw_if_canceled();

    if (print.config().spiral_vase.value)
        m_spiral_vase = make_unique<SpiralVase>(print.config());
#ifdef HAS_PRESSURE_EQUALIZER
    if (print.config().max_volumetric_extrusion_rate_slope_positive.value > 0 ||
        print.config().max_volumetric_extrusion_rate_slope_negative.value > 0)
        m_pressure_equalizer = make_unique<PressureEqualizer>(&print.config());
    m_enable_extrusion_role_markers = (bool)m_pressure_equalizer;
#else /* HAS_PRESSURE_EQUALIZER */
    m_enable_extrusion_role_markers = false;
#endif /* HAS_PRESSURE_EQUALIZER */

    // Write information on the generator.
    file.write_format("; %s\n\n", Slic3r::header_slic3r_generated().c_str());

    // Unit tests or command line slicing may not define "thumbnails" or "thumbnails_format".
    // If "thumbnails_format" is not defined, export to PNG.
    if (const auto [thumbnails, thumbnails_format] = std::make_pair(
            print.full_print_config().option<ConfigOptionPoints>("thumbnails"),
            print.full_print_config().option<ConfigOptionEnum<GCodeThumbnailsFormat>>("thumbnails_format"));
        thumbnails)
        GCodeThumbnails::export_thumbnails_to_file(
            thumbnail_cb, thumbnails->values, thumbnails_format ? thumbnails_format->value : GCodeThumbnailsFormat::PNG,
            [&file](const char* sz) { file.write(sz); },
            [&print]() { print.throw_if_canceled(); });

    // Write notes (content of the Print Settings tab -> Notes)
    {
        std::list<std::string> lines;
        boost::split(lines, print.config().notes.value, boost::is_any_of("\n"), boost::token_compress_off);
        for (auto line : lines) {
            // Remove the trailing '\r' from the '\r\n' sequence.
            if (! line.empty() && line.back() == '\r')
                line.pop_back();
            file.write_format("; %s\n", line.c_str());
        }
        if (! lines.empty())
            file.write("\n");
    }
    print.throw_if_canceled();

    // Write some terse information on the slicing parameters.
    const PrintObject *first_object         = print.objects().front();
    const double       layer_height         = first_object->config().layer_height.value;
    assert(! print.config().first_layer_height.percent);
    const double       first_layer_height   = print.config().first_layer_height.value;
    for (size_t region_id = 0; region_id < print.num_print_regions(); ++ region_id) {
        const PrintRegion &region = print.get_print_region(region_id);
        file.write_format("; external perimeters extrusion width = %.2fmm\n", region.flow(*first_object, frExternalPerimeter, layer_height).width());
        file.write_format("; perimeters extrusion width = %.2fmm\n",          region.flow(*first_object, frPerimeter,         layer_height).width());
        file.write_format("; infill extrusion width = %.2fmm\n",              region.flow(*first_object, frInfill,            layer_height).width());
        file.write_format("; solid infill extrusion width = %.2fmm\n",        region.flow(*first_object, frSolidInfill,       layer_height).width());
        file.write_format("; top infill extrusion width = %.2fmm\n",          region.flow(*first_object, frTopSolidInfill,    layer_height).width());
        if (print.has_support_material())
            file.write_format("; support material extrusion width = %.2fmm\n", support_material_flow(first_object).width());
        if (print.config().first_layer_extrusion_width.value > 0)
            file.write_format("; first layer extrusion width = %.2fmm\n",   region.flow(*first_object, frPerimeter, first_layer_height, true).width());
        file.write_format("\n");
    }
    print.throw_if_canceled();

    // adds tags for time estimators
    if (print.config().remaining_times.value)
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::First_Line_M73_Placeholder).c_str());

    // Starting now, the G-code find / replace post-processor will be enabled.
    file.find_replace_enable();

    // Prepare the helper object for replacing placeholders in custom G-code and output filename.
    m_placeholder_parser = print.placeholder_parser();
    m_placeholder_parser.update_timestamp();
    m_placeholder_parser_context.rng = std::mt19937(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    print.update_object_placeholders(m_placeholder_parser.config_writable(), ".gcode");

    // Get optimal tool ordering to minimize tool switches of a multi-exruder print.
    // For a print by objects, find the 1st printing object.
    ToolOrdering tool_ordering;
    unsigned int initial_extruder_id = (unsigned int)-1;
    unsigned int final_extruder_id   = (unsigned int)-1;
    bool         has_wipe_tower      = false;
    std::vector<const PrintInstance*> 					print_object_instances_ordering;
    std::vector<const PrintInstance*>::const_iterator 	print_object_instance_sequential_active;
    if (print.config().complete_objects.value) {
        // Order object instances for sequential print.
        print_object_instances_ordering = sort_object_instances_by_model_order(print);
//        print_object_instances_ordering = sort_object_instances_by_max_z(print);
        // Find the 1st printing object, find its tool ordering and the initial extruder ID.
        print_object_instance_sequential_active = print_object_instances_ordering.begin();
        for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++ print_object_instance_sequential_active) {
            tool_ordering = ToolOrdering(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);
            if ((initial_extruder_id = tool_ordering.first_extruder()) != static_cast<unsigned int>(-1))
                break;
        }
        if (initial_extruder_id == static_cast<unsigned int>(-1))
            // No object to print was found, cancel the G-code export.
            throw Slic3r::SlicingError(_(L("No extrusions were generated for objects.")));
        // We don't allow switching of extruders per layer by Model::custom_gcode_per_print_z in sequential mode.
        // Use the extruder IDs collected from Regions.
        this->set_extruders(print.extruders());
    } else {
        // Find tool ordering for all the objects at once, and the initial extruder ID.
        // If the tool ordering has been pre-calculated by Print class for wipe tower already, reuse it.
        tool_ordering = print.tool_ordering();
        tool_ordering.assign_custom_gcodes(print);
        if (tool_ordering.all_extruders().empty())
            // No object to print was found, cancel the G-code export.
            throw Slic3r::SlicingError(_(L("No extrusions were generated for objects.")));
        has_wipe_tower = print.has_wipe_tower() && tool_ordering.has_wipe_tower();
        initial_extruder_id = (has_wipe_tower && ! print.config().single_extruder_multi_material_priming) ?
            // The priming towers will be skipped.
            tool_ordering.all_extruders().back() :
            // Don't skip the priming towers.
            tool_ordering.first_extruder();
        // In non-sequential print, the printing extruders may have been modified by the extruder switches stored in Model::custom_gcode_per_print_z.
        // Therefore initialize the printing extruders from there.
        this->set_extruders(tool_ordering.all_extruders());
        // Order object instances using a nearest neighbor search.
        print_object_instances_ordering = chain_print_object_instances(print);
    }
    if (initial_extruder_id == (unsigned int)-1) {
        // Nothing to print!
        initial_extruder_id = 0;
        final_extruder_id   = 0;
    } else {
        final_extruder_id = tool_ordering.last_extruder();
        assert(final_extruder_id != (unsigned int)-1);
    }
    print.throw_if_canceled();

    m_cooling_buffer = make_unique<CoolingBuffer>(*this);
    m_cooling_buffer->set_current_extruder(initial_extruder_id);

    // Emit machine envelope limits for the Marlin firmware.
    this->print_machine_envelope(file, print);

    // Disable fan.
    if (! print.config().cooling.get_at(initial_extruder_id) || print.config().disable_fan_first_layers.get_at(initial_extruder_id))
        file.write(m_writer.set_fan(0));

    // Let the start-up script prime the 1st printing tool.
    m_placeholder_parser.set("initial_tool", initial_extruder_id);
    m_placeholder_parser.set("initial_extruder", initial_extruder_id);
    m_placeholder_parser.set("current_extruder", initial_extruder_id);
    //Set variable for total layer count so it can be used in custom gcode.
    m_placeholder_parser.set("total_layer_count", m_layer_count);
    // Useful for sequential prints.
    m_placeholder_parser.set("current_object_idx", 0);
    // For the start / end G-code to do the priming and final filament pull in case there is no wipe tower provided.
    m_placeholder_parser.set("has_wipe_tower", has_wipe_tower);
    m_placeholder_parser.set("has_single_extruder_multi_material_priming", has_wipe_tower && print.config().single_extruder_multi_material_priming);
    m_placeholder_parser.set("total_toolchanges", std::max(0, print.wipe_tower_data().number_of_toolchanges)); // Check for negative toolchanges (single extruder mode) and set to 0 (no tool change).
    {
        BoundingBoxf bbox(print.config().bed_shape.values);
        m_placeholder_parser.set("print_bed_min",  new ConfigOptionFloats({ bbox.min.x(), bbox.min.y() }));
        m_placeholder_parser.set("print_bed_max",  new ConfigOptionFloats({ bbox.max.x(), bbox.max.y() }));
        m_placeholder_parser.set("print_bed_size", new ConfigOptionFloats({ bbox.size().x(), bbox.size().y() }));
    }
    {
        // Convex hull of the 1st layer extrusions, for bed leveling and placing the initial purge line.
        // It encompasses the object extrusions, support extrusions, skirt, brim, wipe tower.
        // It does NOT encompass user extrusions generated by custom G-code,
        // therefore it does NOT encompass the initial purge line.
        // It does NOT encompass MMU/MMU2 starting (wipe) areas.
        auto pts = std::make_unique<ConfigOptionPoints>();
        pts->values.reserve(print.first_layer_convex_hull().size());
        for (const Point &pt : print.first_layer_convex_hull().points)
            pts->values.emplace_back(unscale(pt));
        BoundingBoxf bbox(pts->values);
        m_placeholder_parser.set("first_layer_print_convex_hull", pts.release());
        m_placeholder_parser.set("first_layer_print_min",  new ConfigOptionFloats({ bbox.min.x(), bbox.min.y() }));
        m_placeholder_parser.set("first_layer_print_max",  new ConfigOptionFloats({ bbox.max.x(), bbox.max.y() }));
        m_placeholder_parser.set("first_layer_print_size", new ConfigOptionFloats({ bbox.size().x(), bbox.size().y() }));
    }
    std::string start_gcode = this->placeholder_parser_process("start_gcode", print.config().start_gcode.value, initial_extruder_id);
    // Set bed temperature if the start G-code does not contain any bed temp control G-codes.
    this->_print_first_layer_bed_temperature(file, print, start_gcode, initial_extruder_id, true);
    // Set extruder(s) temperature before and after start G-code.
    this->_print_first_layer_extruder_temperatures(file, print, start_gcode, initial_extruder_id, false);

    // adds tag for processor
    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(erCustom).c_str());

    // Write the custom start G-code
    file.writeln(start_gcode);

    this->_print_first_layer_extruder_temperatures(file, print, start_gcode, initial_extruder_id, true);
    print.throw_if_canceled();

    // Set other general things.
    file.write(this->preamble());

    // Calculate wiping points if needed
    DoExport::init_ooze_prevention(print, m_ooze_prevention);
    print.throw_if_canceled();

    // Collect custom seam data from all objects.
    m_seam_placer.init(print);

    if (! (has_wipe_tower && print.config().single_extruder_multi_material_priming)) {
        // Set initial extruder only after custom start G-code.
        // Ugly hack: Do not set the initial extruder if the extruder is primed using the MMU priming towers at the edge of the print bed.
        file.write(this->set_extruder(initial_extruder_id, 0.));
    }

    // Do all objects for each layer.
    if (print.config().complete_objects.value) {
        size_t finished_objects = 0;
        const PrintObject *prev_object = (*print_object_instance_sequential_active)->print_object;
        for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++ print_object_instance_sequential_active) {
            const PrintObject &object = *(*print_object_instance_sequential_active)->print_object;
            if (&object != prev_object || tool_ordering.first_extruder() != final_extruder_id) {
                tool_ordering = ToolOrdering(object, final_extruder_id);
                unsigned int new_extruder_id = tool_ordering.first_extruder();
                if (new_extruder_id == (unsigned int)-1)
                    // Skip this object.
                    continue;
                initial_extruder_id = new_extruder_id;
                final_extruder_id   = tool_ordering.last_extruder();
                assert(final_extruder_id != (unsigned int)-1);
            }
            print.throw_if_canceled();
            this->set_origin(unscale((*print_object_instance_sequential_active)->shift));
            if (finished_objects > 0) {
                // Move to the origin position for the copy we're going to print.
                // This happens before Z goes down to layer 0 again, so that no collision happens hopefully.
                m_enable_cooling_markers = false; // we're not filtering these moves through CoolingBuffer
                m_avoid_crossing_perimeters.use_external_mp_once();
                file.write(this->retract());
                file.write(this->travel_to(Point(0, 0), erNone, "move to origin position for next object"));
                m_enable_cooling_markers = true;
                // Disable motion planner when traveling to first object point.
                m_avoid_crossing_perimeters.disable_once();
                // Ff we are printing the bottom layer of an object, and we have already finished
                // another one, set first layer temperatures. This happens before the Z move
                // is triggered, so machine has more time to reach such temperatures.
                m_placeholder_parser.set("current_object_idx", int(finished_objects));
                std::string between_objects_gcode = this->placeholder_parser_process("between_objects_gcode", print.config().between_objects_gcode.value, initial_extruder_id);
                // Set first layer bed and extruder temperatures, don't wait for it to reach the temperature.
                this->_print_first_layer_bed_temperature(file, print, between_objects_gcode, initial_extruder_id, false);
                this->_print_first_layer_extruder_temperatures(file, print, between_objects_gcode, initial_extruder_id, false);
                file.writeln(between_objects_gcode);
            }
            // Reset the cooling buffer internal state (the current position, feed rate, accelerations).
            m_cooling_buffer->reset(this->writer().get_position());
            m_cooling_buffer->set_current_extruder(initial_extruder_id);
            // Process all layers of a single object instance (sequential mode) with a parallel pipeline:
            // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
            // and export G-code into file.
            this->process_layers(print, tool_ordering, collect_layers_to_print(object), *print_object_instance_sequential_active - object.instances().data(), file);
#ifdef HAS_PRESSURE_EQUALIZER
            if (m_pressure_equalizer)
                file.write(m_pressure_equalizer->process("", true));
#endif /* HAS_PRESSURE_EQUALIZER */
            ++ finished_objects;
            // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
            // Reset it when starting another object from 1st layer.
            m_second_layer_things_done = false;
            prev_object = &object;
        }
    } else {
        // Sort layers by Z.
        // All extrusion moves with the same top layer height are extruded uninterrupted.
        std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print = collect_layers_to_print(print);
        // Prusa Multi-Material wipe tower.
        if (has_wipe_tower && ! layers_to_print.empty()) {
            m_wipe_tower.reset(new WipeTowerIntegration(print.config(), *print.wipe_tower_data().priming.get(), print.wipe_tower_data().tool_changes, *print.wipe_tower_data().final_purge.get()));
            file.write(m_writer.travel_to_z(first_layer_height + m_config.z_offset.value, "Move to the first layer height"));
            if (print.config().single_extruder_multi_material_priming) {
                file.write(m_wipe_tower->prime(*this));
                // Verify, whether the print overaps the priming extrusions.
                BoundingBoxf bbox_print(get_print_extrusions_extents(print));
                coordf_t twolayers_printz = ((layers_to_print.size() == 1) ? layers_to_print.front() : layers_to_print[1]).first + EPSILON;
                for (const PrintObject *print_object : print.objects())
                    bbox_print.merge(get_print_object_extrusions_extents(*print_object, twolayers_printz));
                bbox_print.merge(get_wipe_tower_extrusions_extents(print, twolayers_printz));
                BoundingBoxf bbox_prime(get_wipe_tower_priming_extrusions_extents(print));
                bbox_prime.offset(0.5f);
                bool overlap = bbox_prime.overlap(bbox_print);

                if (print.config().gcode_flavor == gcfMarlinLegacy || print.config().gcode_flavor == gcfMarlinFirmware) {
                    file.write(this->retract());
                    file.write("M300 S800 P500\n"); // Beep for 500ms, tone 800Hz.
                    if (overlap) {
                        // Wait for the user to remove the priming extrusions.
                        file.write("M1 Remove priming towers and click button.\n");
                    } else {
                        // Just wait for a bit to let the user check, that the priming succeeded.
                        //TODO Add a message explaining what the printer is waiting for. This needs a firmware fix.
                        file.write("M1 S10\n");
                    }
                } else {
                    // This is not Marlin, M1 command is probably not supported.
                    // (See https://github.com/prusa3d/PrusaSlicer/issues/5441.)
                    if (overlap) {
                        print.active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL,
                            _(L("Your print is very close to the priming regions. "
                              "Make sure there is no collision.")));
                    } else {
                        // Just continue printing, no action necessary.
                    }

                }
            }
            print.throw_if_canceled();
        }
        // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
        // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
        // and export G-code into file.
        this->process_layers(print, tool_ordering, print_object_instances_ordering, layers_to_print, file);
#ifdef HAS_PRESSURE_EQUALIZER
        if (m_pressure_equalizer)
            file.write(m_pressure_equalizer->process("", true));
#endif /* HAS_PRESSURE_EQUALIZER */
        if (m_wipe_tower)
            // Purge the extruder, pull out the active filament.
            file.write(m_wipe_tower->finalize(*this));
    }

    // Write end commands to file.
    file.write(this->retract());
    file.write(m_writer.set_fan(0));

    // adds tag for processor
    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(erCustom).c_str());

    // Process filament-specific gcode in extruder order.
    {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z",   new ConfigOptionFloat(m_writer.get_position()(2) - m_config.z_offset.value));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        if (print.config().single_extruder_multi_material) {
            // Process the end_filament_gcode for the active filament only.
            int extruder_id = m_writer.extruder()->id();
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
            file.writeln(this->placeholder_parser_process("end_filament_gcode", print.config().end_filament_gcode.get_at(extruder_id), extruder_id, &config));
        } else {
            for (const std::string &end_gcode : print.config().end_filament_gcode.values) {
                int extruder_id = (unsigned int)(&end_gcode - &print.config().end_filament_gcode.values.front());
                config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
                file.writeln(this->placeholder_parser_process("end_filament_gcode", end_gcode, extruder_id, &config));
            }
        }
        file.writeln(this->placeholder_parser_process("end_gcode", print.config().end_gcode, m_writer.extruder()->id(), &config));
    }
    file.write(m_writer.update_progress(m_layer_count, m_layer_count, true)); // 100%
    file.write(m_writer.postamble());

    // From now to the end of G-code, the G-code find / replace post-processor will be disabled.
    // Thus the PrusaSlicer generated config will NOT be processed by the G-code post-processor, see GH issue #7952.
    file.find_replace_supress();

    // adds tags for time estimators
    if (print.config().remaining_times.value)
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Last_Line_M73_Placeholder).c_str());

    print.throw_if_canceled();

    // Get filament stats.
    file.write(DoExport::update_print_stats_and_format_filament_stats(
    	// Const inputs
        has_wipe_tower, print.wipe_tower_data(),
        this->config(),
        m_writer.extruders(),
        initial_extruder_id,
        // Modifies
        print.m_print_statistics));
    file.write("\n");
    file.write_format("; total filament used [g] = %.2lf\n", print.m_print_statistics.total_weight);
    file.write_format("; total filament cost = %.2lf\n", print.m_print_statistics.total_cost);
    if (print.m_print_statistics.total_toolchanges > 0)
    	file.write_format("; total toolchanges = %i\n", print.m_print_statistics.total_toolchanges);
    file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Estimated_Printing_Time_Placeholder).c_str());

    // Append full config, delimited by two 'phony' configuration keys prusaslicer_config = begin and prusaslicer_config = end.
    // The delimiters are structured as configuration key / value pairs to be parsable by older versions of PrusaSlicer G-code viewer.
    {
        file.write("\n; prusaslicer_config = begin\n");
        std::string full_config;
        append_full_config(print, full_config);
        if (!full_config.empty())
            file.write(full_config);
        file.write("; prusaslicer_config = end\n");
    }
    print.throw_if_canceled();
}

void GCode::_do_batched_export(Print& print, GCodeOutputStream& file, ThumbnailsGeneratorCallback thumbnail_cb)
{
    std::cout << "~~~~~~~~~~~ _do_batched_export() ~~~~~~~~~~~" << std::endl;
    PROFILE_FUNC();

    // modifies m_silent_time_estimator_enabled
    DoExport::init_gcode_processor(print.config(), m_processor, m_silent_time_estimator_enabled);

    if (!print.config().gcode_substitutions.values.empty()) {
        m_find_replace = make_unique<GCodeFindReplace>(print.config());
        file.set_find_replace(m_find_replace.get(), false);
    }

    // resets analyzer's tracking data
    m_last_height = 0.f;
    m_last_layer_z = 0.f;
    m_max_layer_z = 0.f;
    m_last_width = 0.f;
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    m_last_mm3_per_mm = 0.;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    // How many times will be change_layer() called?
    // change_layer() in turn increments the progress bar status.
    m_layer_count = 0;
    if (print.config().complete_objects.value) {
        // Add each of the object's layers separately.
        for (auto object : print.objects()) {
            std::vector<coordf_t> zs;
            zs.reserve(object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.push_back(layer->print_z);
            std::sort(zs.begin(), zs.end());
            m_layer_count += (unsigned int)(object->instances().size() * (std::unique(zs.begin(), zs.end()) - zs.begin()));
        }
    }
    else {
        // Print all objects with the same print_z together.
        std::vector<coordf_t> zs;
        for (auto object : print.objects()) {
            zs.reserve(zs.size() + object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.push_back(layer->print_z);
        }
        std::sort(zs.begin(), zs.end());
        m_layer_count = (unsigned int)(std::unique(zs.begin(), zs.end()) - zs.begin());
    }
    print.throw_if_canceled();

    m_enable_cooling_markers = true;
    this->apply_print_config(print.config());

    m_volumetric_speed = DoExport::autospeed_volumetric_limit(print);
    print.throw_if_canceled();

    if (print.config().spiral_vase.value)
        m_spiral_vase = make_unique<SpiralVase>(print.config());
#ifdef HAS_PRESSURE_EQUALIZER
    if (print.config().max_volumetric_extrusion_rate_slope_positive.value > 0 ||
        print.config().max_volumetric_extrusion_rate_slope_negative.value > 0)
        m_pressure_equalizer = make_unique<PressureEqualizer>(&print.config());
    m_enable_extrusion_role_markers = (bool)m_pressure_equalizer;
#else /* HAS_PRESSURE_EQUALIZER */
    m_enable_extrusion_role_markers = false;
#endif /* HAS_PRESSURE_EQUALIZER */

    // Write information on the generator.
    file.write_format("; %s\n\n", Slic3r::header_slic3r_generated().c_str());

    // Unit tests or command line slicing may not define "thumbnails" or "thumbnails_format".
    // If "thumbnails_format" is not defined, export to PNG.
    if (const auto [thumbnails, thumbnails_format] = std::make_pair(
        print.full_print_config().option<ConfigOptionPoints>("thumbnails"),
        print.full_print_config().option<ConfigOptionEnum<GCodeThumbnailsFormat>>("thumbnails_format"));
        thumbnails)
        GCodeThumbnails::export_thumbnails_to_file(
            thumbnail_cb, thumbnails->values, thumbnails_format ? thumbnails_format->value : GCodeThumbnailsFormat::PNG,
            [&file](const char* sz) { file.write(sz); },
            [&print]() { print.throw_if_canceled(); });

    // Write notes (content of the Print Settings tab -> Notes)
    {
        std::list<std::string> lines;
        boost::split(lines, print.config().notes.value, boost::is_any_of("\n"), boost::token_compress_off);
        for (auto line : lines) {
            // Remove the trailing '\r' from the '\r\n' sequence.
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            file.write_format("; %s\n", line.c_str());
        }
        if (!lines.empty())
            file.write("\n");
    }
    print.throw_if_canceled();

    // Write some terse information on the slicing parameters.
    const PrintObject* first_object = print.objects().front();
    const double       layer_height = first_object->config().layer_height.value;
    assert(!print.config().first_layer_height.percent);
    const double       first_layer_height = print.config().first_layer_height.value;
    for (size_t region_id = 0; region_id < print.num_print_regions(); ++region_id) {
        const PrintRegion& region = print.get_print_region(region_id);
        file.write_format("; external perimeters extrusion width = %.2fmm\n", region.flow(*first_object, frExternalPerimeter, layer_height).width());
        file.write_format("; perimeters extrusion width = %.2fmm\n", region.flow(*first_object, frPerimeter, layer_height).width());
        file.write_format("; infill extrusion width = %.2fmm\n", region.flow(*first_object, frInfill, layer_height).width());
        file.write_format("; solid infill extrusion width = %.2fmm\n", region.flow(*first_object, frSolidInfill, layer_height).width());
        file.write_format("; top infill extrusion width = %.2fmm\n", region.flow(*first_object, frTopSolidInfill, layer_height).width());
        if (print.has_support_material())
            file.write_format("; support material extrusion width = %.2fmm\n", support_material_flow(first_object).width());
        if (print.config().first_layer_extrusion_width.value > 0)
            file.write_format("; first layer extrusion width = %.2fmm\n", region.flow(*first_object, frPerimeter, first_layer_height, true).width());
        file.write_format("\n");
    }
    print.throw_if_canceled();

    // adds tags for time estimators
    if (print.config().remaining_times.value)
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::First_Line_M73_Placeholder).c_str());

    // Starting now, the G-code find / replace post-processor will be enabled.
    file.find_replace_enable();

    // Prepare the helper object for replacing placeholders in custom G-code and output filename.
    m_placeholder_parser = print.placeholder_parser();
    m_placeholder_parser.update_timestamp();
    m_placeholder_parser_context.rng = std::mt19937(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    print.update_object_placeholders(m_placeholder_parser.config_writable(), ".gcode");

    // Get optimal tool ordering to minimize tool switches of a multi-exruder print.
    // For a print by objects, find the 1st printing object.
    ToolOrdering tool_ordering;
    unsigned int initial_extruder_id = (unsigned int)-1;
    unsigned int final_extruder_id = (unsigned int)-1;
    bool         has_wipe_tower = false;
    std::vector<const PrintInstance*> 					print_object_instances_ordering;
    std::vector<const PrintInstance*>::const_iterator 	print_object_instance_sequential_active;
    if (print.config().complete_objects.value) {
        // Order object instances for sequential print.
        print_object_instances_ordering = sort_object_instances_by_model_order(print);
        //        print_object_instances_ordering = sort_object_instances_by_max_z(print);
                // Find the 1st printing object, find its tool ordering and the initial extruder ID.
        print_object_instance_sequential_active = print_object_instances_ordering.begin();
        for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++print_object_instance_sequential_active) {
            tool_ordering = ToolOrdering(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);
            if ((initial_extruder_id = tool_ordering.first_extruder()) != static_cast<unsigned int>(-1))
                break;
        }
        if (initial_extruder_id == static_cast<unsigned int>(-1))
            // No object to print was found, cancel the G-code export.
            throw Slic3r::SlicingError(_(L("No extrusions were generated for objects.")));
        // We don't allow switching of extruders per layer by Model::custom_gcode_per_print_z in sequential mode.
        // Use the extruder IDs collected from Regions.
        this->set_extruders(print.extruders());
    }
    else {
        // Find tool ordering for all the objects at once, and the initial extruder ID.
        // If the tool ordering has been pre-calculated by Print class for wipe tower already, reuse it.
        tool_ordering = print.tool_ordering();
        tool_ordering.assign_custom_gcodes(print);
        if (tool_ordering.all_extruders().empty())
            // No object to print was found, cancel the G-code export.
            throw Slic3r::SlicingError(_(L("No extrusions were generated for objects.")));
        has_wipe_tower = print.has_wipe_tower() && tool_ordering.has_wipe_tower();
        initial_extruder_id = (has_wipe_tower && !print.config().single_extruder_multi_material_priming) ?
            // The priming towers will be skipped.
            tool_ordering.all_extruders().back() :
            // Don't skip the priming towers.
            tool_ordering.first_extruder();
        // In non-sequential print, the printing extruders may have been modified by the extruder switches stored in Model::custom_gcode_per_print_z.
        // Therefore initialize the printing extruders from there.
        this->set_extruders(tool_ordering.all_extruders());
        // Order object instances using a nearest neighbor search.
        print_object_instances_ordering = chain_print_object_instances(print);
    }
    if (initial_extruder_id == (unsigned int)-1) {
        // Nothing to print!
        initial_extruder_id = 0;
        final_extruder_id = 0;
    }
    else {
        final_extruder_id = tool_ordering.last_extruder();
        assert(final_extruder_id != (unsigned int)-1);
    }
    print.throw_if_canceled();

    m_cooling_buffer = make_unique<CoolingBuffer>(*this);
    m_cooling_buffer->set_current_extruder(initial_extruder_id);

    // Emit machine envelope limits for the Marlin firmware.
    this->print_machine_envelope(file, print);

    // Disable fan.
    if (!print.config().cooling.get_at(initial_extruder_id) || print.config().disable_fan_first_layers.get_at(initial_extruder_id))
        file.write(m_writer.set_fan(0));

    // Let the start-up script prime the 1st printing tool.
    m_placeholder_parser.set("initial_tool", initial_extruder_id);
    m_placeholder_parser.set("initial_extruder", initial_extruder_id);
    m_placeholder_parser.set("current_extruder", initial_extruder_id);
    //Set variable for total layer count so it can be used in custom gcode.
    m_placeholder_parser.set("total_layer_count", m_layer_count);
    // Useful for sequential prints.
    m_placeholder_parser.set("current_object_idx", 0);
    // For the start / end G-code to do the priming and final filament pull in case there is no wipe tower provided.
    m_placeholder_parser.set("has_wipe_tower", has_wipe_tower);
    m_placeholder_parser.set("has_single_extruder_multi_material_priming", has_wipe_tower && print.config().single_extruder_multi_material_priming);
    m_placeholder_parser.set("total_toolchanges", std::max(0, print.wipe_tower_data().number_of_toolchanges)); // Check for negative toolchanges (single extruder mode) and set to 0 (no tool change).
    {
        BoundingBoxf bbox(print.config().bed_shape.values);
        m_placeholder_parser.set("print_bed_min", new ConfigOptionFloats({ bbox.min.x(), bbox.min.y() }));
        m_placeholder_parser.set("print_bed_max", new ConfigOptionFloats({ bbox.max.x(), bbox.max.y() }));
        m_placeholder_parser.set("print_bed_size", new ConfigOptionFloats({ bbox.size().x(), bbox.size().y() }));
    }
    {
        // Convex hull of the 1st layer extrusions, for bed leveling and placing the initial purge line.
        // It encompasses the object extrusions, support extrusions, skirt, brim, wipe tower.
        // It does NOT encompass user extrusions generated by custom G-code,
        // therefore it does NOT encompass the initial purge line.
        // It does NOT encompass MMU/MMU2 starting (wipe) areas.
        auto pts = std::make_unique<ConfigOptionPoints>();
        pts->values.reserve(print.first_layer_convex_hull().size());
        for (const Point& pt : print.first_layer_convex_hull().points)
            pts->values.emplace_back(unscale(pt));
        BoundingBoxf bbox(pts->values);
        m_placeholder_parser.set("first_layer_print_convex_hull", pts.release());
        m_placeholder_parser.set("first_layer_print_min", new ConfigOptionFloats({ bbox.min.x(), bbox.min.y() }));
        m_placeholder_parser.set("first_layer_print_max", new ConfigOptionFloats({ bbox.max.x(), bbox.max.y() }));
        m_placeholder_parser.set("first_layer_print_size", new ConfigOptionFloats({ bbox.size().x(), bbox.size().y() }));
    }
    std::string start_gcode = this->placeholder_parser_process("start_gcode", print.config().start_gcode.value, initial_extruder_id);
    // Set bed temperature if the start G-code does not contain any bed temp control G-codes.
    this->_print_first_layer_bed_temperature(file, print, start_gcode, initial_extruder_id, true);
    // Set extruder(s) temperature before and after start G-code.
    this->_print_first_layer_extruder_temperatures(file, print, start_gcode, initial_extruder_id, false);

    // adds tag for processor
    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(erCustom).c_str());

    // Write the custom start G-code
    file.writeln(start_gcode);

    this->_print_first_layer_extruder_temperatures(file, print, start_gcode, initial_extruder_id, true);
    print.throw_if_canceled();

    // Set other general things.
    file.write(this->preamble());

    // Calculate wiping points if needed
    DoExport::init_ooze_prevention(print, m_ooze_prevention);
    print.throw_if_canceled();

    // Collect custom seam data from all objects.
    m_seam_placer.init(print);

    if (!(has_wipe_tower && print.config().single_extruder_multi_material_priming)) {
        // Set initial extruder only after custom start G-code.
        // Ugly hack: Do not set the initial extruder if the extruder is primed using the MMU priming towers at the edge of the print bed.
        file.write(this->set_extruder(initial_extruder_id, 0.));
    }

    // Do all objects for each layer. (SEQUENTIAL)
    //if (print.config().complete_objects.value) {
    if (false) {
        std::cout << "~~~~~~~~~~~ Do all objects for each layer ~~~~~~~~~~~" << std::endl;
        size_t finished_objects = 0;
        const PrintObject* prev_object = (*print_object_instance_sequential_active)->print_object;
        for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++print_object_instance_sequential_active) {
            const PrintObject& object = *(*print_object_instance_sequential_active)->print_object;
            if (&object != prev_object || tool_ordering.first_extruder() != final_extruder_id) {
                tool_ordering = ToolOrdering(object, final_extruder_id);
                unsigned int new_extruder_id = tool_ordering.first_extruder();
                if (new_extruder_id == (unsigned int)-1)
                    // Skip this object.
                    continue;
                initial_extruder_id = new_extruder_id;
                final_extruder_id = tool_ordering.last_extruder();
                assert(final_extruder_id != (unsigned int)-1);
            }
            print.throw_if_canceled();
            this->set_origin(unscale((*print_object_instance_sequential_active)->shift));
            if (finished_objects > 0) {
                // Move to the origin position for the copy we're going to print.
                // This happens before Z goes down to layer 0 again, so that no collision happens hopefully.
                m_enable_cooling_markers = false; // we're not filtering these moves through CoolingBuffer
                m_avoid_crossing_perimeters.use_external_mp_once();
                file.write(this->retract());
                file.write(this->travel_to(Point(0, 0), erNone, "move to origin position for next object"));
                m_enable_cooling_markers = true;
                // Disable motion planner when traveling to first object point.
                m_avoid_crossing_perimeters.disable_once();
                // Ff we are printing the bottom layer of an object, and we have already finished
                // another one, set first layer temperatures. This happens before the Z move
                // is triggered, so machine has more time to reach such temperatures.
                m_placeholder_parser.set("current_object_idx", int(finished_objects));
                std::string between_objects_gcode = this->placeholder_parser_process("between_objects_gcode", print.config().between_objects_gcode.value, initial_extruder_id);
                // Set first layer bed and extruder temperatures, don't wait for it to reach the temperature.
                this->_print_first_layer_bed_temperature(file, print, between_objects_gcode, initial_extruder_id, false);
                this->_print_first_layer_extruder_temperatures(file, print, between_objects_gcode, initial_extruder_id, false);
                file.writeln(between_objects_gcode);
            }
            // Reset the cooling buffer internal state (the current position, feed rate, accelerations).
            m_cooling_buffer->reset(this->writer().get_position());
            m_cooling_buffer->set_current_extruder(initial_extruder_id);
            // Process all layers of a single object instance (sequential mode) with a parallel pipeline:
            // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
            // and export G-code into file.
            std::cout << "~~~~~~~~~~~ process_sequential_batched_layers ~~~~~~~~~~~" << std::endl;
            this->process_sequential_batched_layers(print, tool_ordering, collect_layers_to_print(object), *print_object_instance_sequential_active - object.instances().data(), file);
#ifdef HAS_PRESSURE_EQUALIZER
            if (m_pressure_equalizer)
                file.write(m_pressure_equalizer->process("", true));
#endif /* HAS_PRESSURE_EQUALIZER */
            ++finished_objects;
            // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
            // Reset it when starting another object from 1st layer.
            m_second_layer_things_done = false;
            prev_object = &object;
        }
    }
    // NON-SEQUENTIAL
    //else {
    //if(!print.config().complete_objects.value) {
    if(false) {
        std::cout << "~~~~~~~~~~~ Sort layers by Z ~~~~~~~~~~~" << std::endl;
        // Sort layers by Z.
        // All extrusion moves with the same top layer height are extruded uninterrupted.
        std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print = collect_layers_to_print(print);

        // Prusa Multi-Material wipe tower.
        if (has_wipe_tower && !layers_to_print.empty()) {
            m_wipe_tower.reset(new WipeTowerIntegration(print.config(), *print.wipe_tower_data().priming.get(), print.wipe_tower_data().tool_changes, *print.wipe_tower_data().final_purge.get()));
            file.write(m_writer.travel_to_z(first_layer_height + m_config.z_offset.value, "Move to the first layer height"));
            if (print.config().single_extruder_multi_material_priming) {
                file.write(m_wipe_tower->prime(*this));
                // Verify, whether the print overaps the priming extrusions.
                BoundingBoxf bbox_print(get_print_extrusions_extents(print));
                coordf_t twolayers_printz = ((layers_to_print.size() == 1) ? layers_to_print.front() : layers_to_print[1]).first + EPSILON;
                for (const PrintObject* print_object : print.objects())
                    bbox_print.merge(get_print_object_extrusions_extents(*print_object, twolayers_printz));
                bbox_print.merge(get_wipe_tower_extrusions_extents(print, twolayers_printz));
                BoundingBoxf bbox_prime(get_wipe_tower_priming_extrusions_extents(print));
                bbox_prime.offset(0.5f);
                bool overlap = bbox_prime.overlap(bbox_print);

                if (print.config().gcode_flavor == gcfMarlinLegacy || print.config().gcode_flavor == gcfMarlinFirmware) {
                    file.write(this->retract());
                    file.write("M300 S800 P500\n"); // Beep for 500ms, tone 800Hz.
                    if (overlap) {
                        // Wait for the user to remove the priming extrusions.
                        file.write("M1 Remove priming towers and click button.\n");
                    }
                    else {
                        // Just wait for a bit to let the user check, that the priming succeeded.
                        //TODO Add a message explaining what the printer is waiting for. This needs a firmware fix.
                        file.write("M1 S10\n");
                    }
                }
                else {
                    // This is not Marlin, M1 command is probably not supported.
                    // (See https://github.com/prusa3d/PrusaSlicer/issues/5441.)
                    if (overlap) {
                        print.active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL,
                            _(L("Your print is very close to the priming regions. "
                                "Make sure there is no collision.")));
                    }
                    else {
                        // Just continue printing, no action necessary.
                    }

                }
            }
            print.throw_if_canceled();
        }
        // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
        // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
        // and export G-code into file.
        this->process_layers(print, tool_ordering, print_object_instances_ordering, layers_to_print, file);
        
#ifdef HAS_PRESSURE_EQUALIZER
        if (m_pressure_equalizer)
            file.write(m_pressure_equalizer->process("", true));
#endif /* HAS_PRESSURE_EQUALIZER */
        if (m_wipe_tower)
            // Purge the extruder, pull out the active filament.
            file.write(m_wipe_tower->finalize(*this));
    }



    
    
    
    
    // ATC // put here whatever in the "else" statement above // inherit from the NON-SEQUENTIAL mode
    //if (print.default_object_config().atc_enable_wipe_tower)
    if (true)
    {
        std::cout << "~~~~~~~~~~~ ATC NON-SEQUENTIAL mode ~~~~~~~~~~~" << std::endl;
        // Sort layers by Z.
        // All extrusion moves with the same top layer height are extruded uninterrupted.
        std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print = collect_layers_to_print(print);
        // Prusa Multi-Material wipe tower.
        if (has_wipe_tower && !layers_to_print.empty()) {
            m_wipe_tower.reset(new WipeTowerIntegration(print.config(), *print.wipe_tower_data().priming.get(), print.wipe_tower_data().tool_changes, *print.wipe_tower_data().final_purge.get()));
            file.write(m_writer.travel_to_z(first_layer_height + m_config.z_offset.value, "Move to the first layer height"));
            if (print.config().single_extruder_multi_material_priming) {
                file.write(m_wipe_tower->prime(*this));
                // Verify, whether the print overaps the priming extrusions.
                BoundingBoxf bbox_print(get_print_extrusions_extents(print));
                coordf_t twolayers_printz = ((layers_to_print.size() == 1) ? layers_to_print.front() : layers_to_print[1]).first + EPSILON;
                for (const PrintObject* print_object : print.objects())
                    bbox_print.merge(get_print_object_extrusions_extents(*print_object, twolayers_printz));
                bbox_print.merge(get_wipe_tower_extrusions_extents(print, twolayers_printz));
                BoundingBoxf bbox_prime(get_wipe_tower_priming_extrusions_extents(print));
                bbox_prime.offset(0.5f);
                bool overlap = bbox_prime.overlap(bbox_print);

                if (print.config().gcode_flavor == gcfMarlinLegacy || print.config().gcode_flavor == gcfMarlinFirmware) {
                    file.write(this->retract());
                    file.write("M300 S800 P500\n"); // Beep for 500ms, tone 800Hz.
                    if (overlap) {
                        // Wait for the user to remove the priming extrusions.
                        file.write("M1 Remove priming towers and click button.\n");
                    }
                    else {
                        // Just wait for a bit to let the user check, that the priming succeeded.
                        //TODO Add a message explaining what the printer is waiting for. This needs a firmware fix.
                        file.write("M1 S10\n");
                    }
                }
                else {
                    // This is not Marlin, M1 command is probably not supported.
                    // (See https://github.com/prusa3d/PrusaSlicer/issues/5441.)
                    if (overlap) {
                        print.active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL,
                            _(L("Your print is very close to the priming regions. "
                                "Make sure there is no collision.")));
                    }
                    else {
                        // Just continue printing, no action necessary.
                    }

                }
            }
            print.throw_if_canceled();
        }
        // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
        // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
        // and export G-code into file.
        // ===========================================================
        // ATC should be another function


        //print.layer_batch_labeling();
        // HERE ! or right in the atc_process_layers(print, tool_ordering, file) function





        this->atc_process_layers(print, tool_ordering, file);
        // this->process_layers(print, tool_ordering, print_object_instances_ordering, layers_to_print, file);
        // ============================================================

#ifdef HAS_PRESSURE_EQUALIZER
        if (m_pressure_equalizer)
            file.write(m_pressure_equalizer->process("", true));
#endif /* HAS_PRESSURE_EQUALIZER */
        if (m_wipe_tower)
            // Purge the extruder, pull out the active filament.
            file.write(m_wipe_tower->finalize(*this));
    }
    
    
    
    
    
    
    
    
    
    // Write end commands to file.
    file.write(this->retract());
    file.write(m_writer.set_fan(0));

    // adds tag for processor
    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(erCustom).c_str());

    // Process filament-specific gcode in extruder order.
    {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(m_writer.get_position()(2) - m_config.z_offset.value));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        if (print.config().single_extruder_multi_material) {
            // Process the end_filament_gcode for the active filament only.
            int extruder_id = m_writer.extruder()->id();
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
            file.writeln(this->placeholder_parser_process("end_filament_gcode", print.config().end_filament_gcode.get_at(extruder_id), extruder_id, &config));
        }
        else {
            for (const std::string& end_gcode : print.config().end_filament_gcode.values) {
                int extruder_id = (unsigned int)(&end_gcode - &print.config().end_filament_gcode.values.front());
                config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
                file.writeln(this->placeholder_parser_process("end_filament_gcode", end_gcode, extruder_id, &config));
            }
        }
        file.writeln(this->placeholder_parser_process("end_gcode", print.config().end_gcode, m_writer.extruder()->id(), &config));
    }
    file.write(m_writer.update_progress(m_layer_count, m_layer_count, true)); // 100%
    file.write(m_writer.postamble());

    // From now to the end of G-code, the G-code find / replace post-processor will be disabled.
    // Thus the PrusaSlicer generated config will NOT be processed by the G-code post-processor, see GH issue #7952.
    file.find_replace_supress();

    // adds tags for time estimators
    if (print.config().remaining_times.value)
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Last_Line_M73_Placeholder).c_str());

    print.throw_if_canceled();

    // Get filament stats.
    file.write(DoExport::update_print_stats_and_format_filament_stats(
        // Const inputs
        has_wipe_tower, print.wipe_tower_data(),
        // ---here you can update your ATC stats:
        //has_wipe_tower, print.m_ATC_wipe_tower_data,
        
        this->config(),
        m_writer.extruders(),
        initial_extruder_id,
        // Modifies
        print.m_print_statistics));

    print.m_print_statistics.total_toolchanges = print.m_ATC_wipe_tower_data.number_of_toolchanges;

    file.write("\n");
    file.write_format("; total filament used [g] = %.2lf\n", print.m_print_statistics.total_weight);
    file.write_format("; total filament cost = %.2lf\n", print.m_print_statistics.total_cost);
    if (print.m_print_statistics.total_toolchanges > 0)
        file.write_format("; total toolchanges = %i\n", print.m_print_statistics.total_toolchanges);
    file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Estimated_Printing_Time_Placeholder).c_str());

    // Append full config, delimited by two 'phony' configuration keys prusaslicer_config = begin and prusaslicer_config = end.
    // The delimiters are structured as configuration key / value pairs to be parsable by older versions of PrusaSlicer G-code viewer.
    {
        file.write("\n; prusaslicer_config = begin\n");
        std::string full_config;
        append_full_config(print, full_config);
        if (!full_config.empty())
            file.write(full_config);
        file.write("; prusaslicer_config = end\n");
    }
    print.throw_if_canceled();
}


// For unknown reasons and in sporadic cases when GCode export is processing, some participating thread
// in tbb::parallel_pipeline has not set locales to "C", probably because this thread is newly spawned.
// So in this class method on_scheduler_entry is called for every thread before it starts participating
// in tbb::parallel_pipeline to ensure that locales are set correctly

// For tbb::parallel_pipeline, it seems that on_scheduler_entry is called for every layer and every filter.
// We ensure using thread-local storage that locales will be set to "C" just once for any participating thread.
class TBBLocalesSetter : public tbb::task_scheduler_observer
{
public:
    TBBLocalesSetter() { this->observe(true); }
    ~TBBLocalesSetter() override { this->observe(false); };

    void on_scheduler_entry(bool is_worker) override
    {
        if (bool &is_locales_sets = m_is_locales_sets.local(); !is_locales_sets) {
            // Set locales of the worker thread to "C".
            set_c_locales();
            is_locales_sets = true;
        }
    }

private:
    tbb::enumerable_thread_specific<bool, tbb::cache_aligned_allocator<bool>, tbb::ets_key_usage_type::ets_key_per_instance> m_is_locales_sets{false};
};

// Process all layers of all objects (non-sequential mode) with a parallel pipeline:
// Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
// and export G-code into file.
void GCode::process_layers(
    const Print                                                         &print,
    const ToolOrdering                                                  &tool_ordering,
    const std::vector<const PrintInstance*>                             &print_object_instances_ordering,
    const std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>>   &layers_to_print,
    GCodeOutputStream                                                   &output_stream)
{
    // The pipeline is variable: The vase mode filter is optional.
    size_t layer_to_print_idx = 0;
    const auto generator = tbb::make_filter<void, GCode::LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &tool_ordering, &print_object_instances_ordering, &layers_to_print, &layer_to_print_idx](tbb::flow_control& fc) -> GCode::LayerResult {
            if (layer_to_print_idx == layers_to_print.size()) {
                fc.stop();
                return {};
            } else {
                const std::pair<coordf_t, std::vector<LayerToPrint>>& layer = layers_to_print[layer_to_print_idx++];
                const LayerTools& layer_tools = tool_ordering.tools_for_layer(layer.first);
                if (m_wipe_tower && layer_tools.has_wipe_tower)
                    m_wipe_tower->next_layer();
                print.throw_if_canceled();
                return this->process_layer(print, layer.second, layer_tools, &layer == &layers_to_print.back(), &print_object_instances_ordering, size_t(-1));
            }
        });
    const auto spiral_vase = tbb::make_filter<GCode::LayerResult, GCode::LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [&spiral_vase = *this->m_spiral_vase.get()](GCode::LayerResult in) -> GCode::LayerResult {
            spiral_vase.enable(in.spiral_vase_enable);
            return { spiral_vase.process_layer(std::move(in.gcode)), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush };
        });
    const auto cooling = tbb::make_filter<GCode::LayerResult, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&cooling_buffer = *this->m_cooling_buffer.get()](GCode::LayerResult in) -> std::string {
            return cooling_buffer.process_layer(std::move(in.gcode), in.layer_id, in.cooling_buffer_flush);
        });
    const auto find_replace = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&self = *this->m_find_replace.get()](std::string s) -> std::string {
            return self.process_layer(std::move(s));
        });
    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
        [&output_stream](std::string s) { output_stream.write(s); }
    );

    // It registers a handler that sets locales to "C" before any TBB thread starts participating in tbb::parallel_pipeline.
    // Handler is unregistered when the destructor is called.
    TBBLocalesSetter locales_setter;

    // The pipeline elements are joined using const references, thus no copying is performed.
    output_stream.find_replace_supress();
    if (m_spiral_vase && m_find_replace)
        tbb::parallel_pipeline(12, generator & spiral_vase & cooling & find_replace & output);
    else if (m_spiral_vase)
        tbb::parallel_pipeline(12, generator & spiral_vase & cooling & output);
    else if (m_find_replace)
        tbb::parallel_pipeline(12, generator & cooling & find_replace & output);
    else
        tbb::parallel_pipeline(12, generator & cooling & output);
    output_stream.find_replace_enable();
}






// Process all layers of a single object instance (sequential mode) with a parallel pipeline:
// Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
// and export G-code into file.
void GCode::process_layers(
    const Print                             &print,
    const ToolOrdering                      &tool_ordering,
    std::vector<LayerToPrint>                layers_to_print,
    const size_t                             single_object_idx,
    GCodeOutputStream                       &output_stream)
{
    // The pipeline is variable: The vase mode filter is optional.
    size_t layer_to_print_idx = 0;
    const auto generator = tbb::make_filter<void, GCode::LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &tool_ordering, &layers_to_print, &layer_to_print_idx, single_object_idx](tbb::flow_control& fc) -> GCode::LayerResult {
            if (layer_to_print_idx == layers_to_print.size()) {
                fc.stop();
                return {};
            } else {
                LayerToPrint &layer = layers_to_print[layer_to_print_idx ++];
                print.throw_if_canceled();
                return this->process_layer(print, { std::move(layer) }, tool_ordering.tools_for_layer(layer.print_z()), &layer == &layers_to_print.back(), nullptr, single_object_idx);
            }
        });
    const auto spiral_vase = tbb::make_filter<GCode::LayerResult, GCode::LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [&spiral_vase = *this->m_spiral_vase.get()](GCode::LayerResult in)->GCode::LayerResult {
            spiral_vase.enable(in.spiral_vase_enable);
            return { spiral_vase.process_layer(std::move(in.gcode)), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush };
        });
    const auto cooling = tbb::make_filter<GCode::LayerResult, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&cooling_buffer = *this->m_cooling_buffer.get()](GCode::LayerResult in)->std::string {
            return cooling_buffer.process_layer(std::move(in.gcode), in.layer_id, in.cooling_buffer_flush);
        });
    const auto find_replace = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&self = *this->m_find_replace.get()](std::string s) -> std::string {
            return self.process_layer(std::move(s));
        });
    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
        [&output_stream](std::string s) { output_stream.write(s); }
    );

    // It registers a handler that sets locales to "C" before any TBB thread starts participating in tbb::parallel_pipeline.
    // Handler is unregistered when the destructor is called.
    TBBLocalesSetter locales_setter;

    // The pipeline elements are joined using const references, thus no copying is performed.
    output_stream.find_replace_supress();
    if (m_spiral_vase && m_find_replace)
        tbb::parallel_pipeline(12, generator & spiral_vase & cooling & find_replace & output);
    else if (m_spiral_vase)
        tbb::parallel_pipeline(12, generator & spiral_vase & cooling & output);
    else if (m_find_replace)
        tbb::parallel_pipeline(12, generator & cooling & find_replace & output);
    else
        tbb::parallel_pipeline(12, generator & cooling & output);
    output_stream.find_replace_enable();
}

void GCode::process_sequential_batched_layers(
    const Print& print,
    const ToolOrdering& tool_ordering,
    std::vector<LayerToPrint>                layers_to_print,
    const size_t                             single_object_idx,
    GCodeOutputStream& output_stream)
{
    std::cout << "~~~~~~~~~~~~~~~ process_sequential_batched_layers() ~~~~~~~~~~~~~~~" << std::endl;

    /*
    for (size_t layer_number = 0; layer_number < layers_to_print.size(); layer_number++) {
        LayerToPrint& layer = layers_to_print[0];
        size_t region_in_batch = 3;
        this->process_layer_batched_region(print, { std::move(layer) }, tool_ordering.tools_for_layer(layer.print_z()), &layer == &layers_to_print.back(), nullptr, region_in_batch, single_object_idx);

    }
    */


    ATC_linked_list ATC_printing_map;
    ATC_printing_map = print.m_ATC_printing_map;
    std::cout << "ATC_printing_map.get_count() = " << ATC_printing_map.get_count() << std::endl;
    ATC_printing_map.display(ATC_printing_map.gethead());

    struct ATC_printing_piece* printing_piece;
    int atc_current_tool = 0;
    int atc_statistics_number_of_TC = 0;
    for (int print_step = 0; print_step < ATC_printing_map.get_count(); print_step++)
    {
        printing_piece = ATC_printing_map.get_node(print_step);
        int print_layer_idx = printing_piece->layer;
        int print_region_idx = printing_piece->region;

        GCode::LayerResult printing_piece_gcode;
        GCode::LayerToPrint& layer = layers_to_print[print_layer_idx];
        printing_piece_gcode = this->process_layer_batched_region(print, { std::move(layer) },
            tool_ordering.tools_for_layer(layer.print_z()), &layer == &layers_to_print.back(), nullptr, print_region_idx, single_object_idx);

        /*
        if (print_region_idx != atc_current_tool)
        {
            atc_current_tool = print_region_idx;
            atc_statistics_number_of_TC += 1;
            printing_piece_gcode.gcode += "; --- ATC Tool Change ---\n";
            printing_piece_gcode.gcode += "T" + std::to_string(atc_current_tool) + "\n";
            printing_piece_gcode.gcode += "M104 S215; set temperature\n";
            printing_piece_gcode.gcode += "M900 K0.05; Filament gcode LA 1.5\n";
            printing_piece_gcode.gcode += "M900 K30; Filament gcode LA 1.0\n";
        }
        */

        output_stream.write(printing_piece_gcode.gcode);
    }
    std::cout << "\n\n\n\n\n\n\n\n atc_statistics_number_of_TC:" + std::to_string(atc_statistics_number_of_TC) + "\n\n\n\n\n\n" << std::endl;



    /*
    // The pipeline is variable: The vase mode filter is optional.
    size_t layer_to_print_idx = 0;
    const auto generator = tbb::make_filter<void, GCode::LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &tool_ordering, &layers_to_print, &layer_to_print_idx, single_object_idx](tbb::flow_control& fc) -> GCode::LayerResult {
            if (layer_to_print_idx == layers_to_print.size()) {
                fc.stop();
                return {};
            }
            else {
                LayerToPrint& layer = layers_to_print[layer_to_print_idx++];
                print.throw_if_canceled();
                size_t region_in_batch = 3;
                return this->process_layer_batched_region(print, { std::move(layer) }, tool_ordering.tools_for_layer(layer.print_z()), &layer == &layers_to_print.back(), nullptr, region_in_batch, single_object_idx);
            }
        });
    const auto spiral_vase = tbb::make_filter<GCode::LayerResult, GCode::LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [&spiral_vase = *this->m_spiral_vase.get()](GCode::LayerResult in)->GCode::LayerResult {
        spiral_vase.enable(in.spiral_vase_enable);
        return { spiral_vase.process_layer(std::move(in.gcode)), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush };
    });
    const auto cooling = tbb::make_filter<GCode::LayerResult, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&cooling_buffer = *this->m_cooling_buffer.get()](GCode::LayerResult in)->std::string {
        return cooling_buffer.process_layer(std::move(in.gcode), in.layer_id, in.cooling_buffer_flush);
    });
    const auto find_replace = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&self = *this->m_find_replace.get()](std::string s)->std::string {
        return self.process_layer(std::move(s));
    });
    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
        [&output_stream](std::string s) { output_stream.write(s); }
    );

    // It registers a handler that sets locales to "C" before any TBB thread starts participating in tbb::parallel_pipeline.
    // Handler is unregistered when the destructor is called.
    TBBLocalesSetter locales_setter;

    // The pipeline elements are joined using const references, thus no copying is performed.
    output_stream.find_replace_supress();
    if (m_spiral_vase && m_find_replace)
        tbb::parallel_pipeline(12, generator & spiral_vase & cooling & find_replace & output);
    else if (m_spiral_vase)
        tbb::parallel_pipeline(12, generator & spiral_vase & cooling & output);
    else if (m_find_replace)
        tbb::parallel_pipeline(12, generator & cooling & find_replace & output);
    else
        tbb::parallel_pipeline(12, generator & cooling & output);


    */
    output_stream.find_replace_enable();
}



std::string GCode::placeholder_parser_process(const std::string &name, const std::string &templ, unsigned int current_extruder_id, const DynamicConfig *config_override)
{
    try {
        return m_placeholder_parser.process(templ, current_extruder_id, config_override, &m_placeholder_parser_context);
    } catch (std::runtime_error &err) {
        // Collect the names of failed template substitutions for error reporting.
        auto it = m_placeholder_parser_failed_templates.find(name);
        if (it == m_placeholder_parser_failed_templates.end())
            // Only if there was no error reported for this template, store the first error message into the map to be reported.
            // We don't want to collect error message for each and every occurence of a single custom G-code section.
            m_placeholder_parser_failed_templates.insert(it, std::make_pair(name, std::string(err.what())));
        // Insert the macro error message into the G-code.
        return
            std::string("\n!!!!! Failed to process the custom G-code template ") + name + "\n" +
            err.what() +
            "!!!!! End of an error report for the custom G-code template " + name + "\n\n";
    }
}

// Parse the custom G-code, try to find mcode_set_temp_dont_wait and mcode_set_temp_and_wait or optionally G10 with temperature inside the custom G-code.
// Returns true if one of the temp commands are found, and try to parse the target temperature value into temp_out.
static bool custom_gcode_sets_temperature(const std::string &gcode, const int mcode_set_temp_dont_wait, const int mcode_set_temp_and_wait, const bool include_g10, int &temp_out)
{
    temp_out = -1;
    if (gcode.empty())
        return false;

    const char *ptr = gcode.data();
    bool temp_set_by_gcode = false;
    while (*ptr != 0) {
        // Skip whitespaces.
        for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
        if (*ptr == 'M' || // Line starts with 'M'. It is a machine command.
            (*ptr == 'G' && include_g10)) { // Only check for G10 if requested
            bool is_gcode = *ptr == 'G';
            ++ ptr;
            // Parse the M or G code value.
            char *endptr = nullptr;
            int mgcode = int(strtol(ptr, &endptr, 10));
            if (endptr != nullptr && endptr != ptr && 
                is_gcode ?
                    // G10 found
                    mgcode == 10 :
                    // M104/M109 or M140/M190 found.
                    (mgcode == mcode_set_temp_dont_wait || mgcode == mcode_set_temp_and_wait)) {
                ptr = endptr;
                if (! is_gcode)
                    // Let the caller know that the custom M-code sets the temperature.
                    temp_set_by_gcode = true;
                // Now try to parse the temperature value.
                // While not at the end of the line:
                while (strchr(";\r\n\0", *ptr) == nullptr) {
                    // Skip whitespaces.
                    for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                    if (*ptr == 'S') {
                        // Skip whitespaces.
                        for (++ ptr; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                        // Parse an int.
                        endptr = nullptr;
                        long temp_parsed = strtol(ptr, &endptr, 10);
                        if (endptr > ptr) {
                            ptr = endptr;
                            temp_out = temp_parsed;
                            // Let the caller know that the custom G-code sets the temperature
                            // Only do this after successfully parsing temperature since G10
                            // can be used for other reasons
                            temp_set_by_gcode = true;
                        }
                    } else {
                        // Skip this word.
                        for (; strchr(" \t;\r\n\0", *ptr) == nullptr; ++ ptr);
                    }
                }
            }
        }
        // Skip the rest of the line.
        for (; *ptr != 0 && *ptr != '\r' && *ptr != '\n'; ++ ptr);
        // Skip the end of line indicators.
        for (; *ptr == '\r' || *ptr == '\n'; ++ ptr);
    }
    return temp_set_by_gcode;
}

// Print the machine envelope G-code for the Marlin firmware based on the "machine_max_xxx" parameters.
// Do not process this piece of G-code by the time estimator, it already knows the values through another sources.
void GCode::print_machine_envelope(GCodeOutputStream &file, Print &print)
{
    const GCodeFlavor flavor = print.config().gcode_flavor.value;
    if ( (flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware || flavor == gcfRepRapFirmware)
     && print.config().machine_limits_usage.value == MachineLimitsUsage::EmitToGCode) {
        int factor = flavor == gcfRepRapFirmware ? 60 : 1; // RRF M203 and M566 are in mm/min
        file.write_format("M201 X%d Y%d Z%d E%d ; sets maximum accelerations, mm/sec^2\n",
            int(print.config().machine_max_acceleration_x.values.front() + 0.5),
            int(print.config().machine_max_acceleration_y.values.front() + 0.5),
            int(print.config().machine_max_acceleration_z.values.front() + 0.5),
            int(print.config().machine_max_acceleration_e.values.front() + 0.5));
        file.write_format("M203 X%d Y%d Z%d E%d ; sets maximum feedrates, %s\n",
            int(print.config().machine_max_feedrate_x.values.front() * factor + 0.5),
            int(print.config().machine_max_feedrate_y.values.front() * factor + 0.5),
            int(print.config().machine_max_feedrate_z.values.front() * factor + 0.5),
            int(print.config().machine_max_feedrate_e.values.front() * factor + 0.5),
            factor == 60 ? "mm / min" : "mm / sec");

        // Now M204 - acceleration. This one is quite hairy thanks to how Marlin guys care about
        // backwards compatibility: https://github.com/prusa3d/PrusaSlicer/issues/1089
        // Legacy Marlin should export travel acceleration the same as printing acceleration.
        // MarlinFirmware has the two separated.
        int travel_acc = flavor == gcfMarlinLegacy
                       ? int(print.config().machine_max_acceleration_extruding.values.front() + 0.5)
                       : int(print.config().machine_max_acceleration_travel.values.front() + 0.5);
        // Retract acceleration not accepted in M204 in RRF
        if (flavor == gcfRepRapFirmware)
            file.write_format("M204 P%d T%d ; sets acceleration (P, T), mm/sec^2\n",
                int(print.config().machine_max_acceleration_extruding.values.front() + 0.5),
                travel_acc);
        else
            file.write_format("M204 P%d R%d T%d ; sets acceleration (P, T) and retract acceleration (R), mm/sec^2\n",
                int(print.config().machine_max_acceleration_extruding.values.front() + 0.5),
                int(print.config().machine_max_acceleration_retracting.values.front() + 0.5),
                travel_acc);

        assert(is_decimal_separator_point());
        file.write_format(flavor == gcfRepRapFirmware
            ? "M566 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/min\n"
            : "M205 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/sec\n",
            print.config().machine_max_jerk_x.values.front() * factor,
            print.config().machine_max_jerk_y.values.front() * factor,
            print.config().machine_max_jerk_z.values.front() * factor,
            print.config().machine_max_jerk_e.values.front() * factor);
        if (flavor != gcfRepRapFirmware)
            file.write_format("M205 S%d T%d ; sets the minimum extruding and travel feed rate, mm/sec\n",
                int(print.config().machine_min_extruding_rate.values.front() + 0.5),
                int(print.config().machine_min_travel_rate.values.front() + 0.5));
        else {
            // M205 Sn Tn not supported in RRF. They use M203 Inn to set minimum feedrate for
            // all moves. This is currently not implemented.
        }
    }
}

// Write 1st layer bed temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M140 - Set Extruder Temperature
// M190 - Set Extruder Temperature and Wait
void GCode::_print_first_layer_bed_temperature(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait)
{
    // Initial bed temperature based on the first extruder.
    int  temp = print.config().first_layer_bed_temperature.get_at(first_printing_extruder_id);
    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode     = -1;
    bool temp_set_by_gcode = custom_gcode_sets_temperature(gcode, 140, 190, false, temp_by_gcode);
    if (temp_set_by_gcode && temp_by_gcode >= 0 && temp_by_gcode < 1000)
        temp = temp_by_gcode;
    // Always call m_writer.set_bed_temperature() so it will set the internal "current" state of the bed temp as if
    // the custom start G-code emited these.
    std::string set_temp_gcode = m_writer.set_bed_temperature(temp, wait);
    if (! temp_set_by_gcode)
        file.write(set_temp_gcode);
}

// Write 1st layer extruder temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M104 - Set Extruder Temperature
// M109 - Set Extruder Temperature and Wait
// RepRapFirmware: G10 Sxx
void GCode::_print_first_layer_extruder_temperatures(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait)
{
    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode = -1;
    bool include_g10   = print.config().gcode_flavor == gcfRepRapFirmware;
    if (custom_gcode_sets_temperature(gcode, 104, 109, include_g10, temp_by_gcode)) {
        // Set the extruder temperature at m_writer, but throw away the generated G-code as it will be written with the custom G-code.
        int temp = print.config().first_layer_temperature.get_at(first_printing_extruder_id);
        if (temp_by_gcode >= 0 && temp_by_gcode < 1000)
            temp = temp_by_gcode;
        m_writer.set_temperature(temp, wait, first_printing_extruder_id);
    } else {
        // Custom G-code does not set the extruder temperature. Do it now.
        if (print.config().single_extruder_multi_material.value) {
            // Set temperature of the first printing extruder only.
            int temp = print.config().first_layer_temperature.get_at(first_printing_extruder_id);
            if (temp > 0)
                file.write(m_writer.set_temperature(temp, wait, first_printing_extruder_id));
        } else {
            // Set temperatures of all the printing extruders.
            for (unsigned int tool_id : print.extruders()) {
                int temp = print.config().first_layer_temperature.get_at(tool_id);
                if (print.config().ooze_prevention.value)
                    temp += print.config().standby_temperature_delta.value;
                if (temp > 0)
                    file.write(m_writer.set_temperature(temp, wait, tool_id));
            }
        }
    }
}

inline GCode::ObjectByExtruder& object_by_extruder(
    std::map<unsigned int, std::vector<GCode::ObjectByExtruder>> &by_extruder,
    unsigned int                                                  extruder_id,
    size_t                                                        object_idx,
    size_t                                                        num_objects)
{
    std::vector<GCode::ObjectByExtruder> &objects_by_extruder = by_extruder[extruder_id];
    if (objects_by_extruder.empty())
        objects_by_extruder.assign(num_objects, GCode::ObjectByExtruder());
    return objects_by_extruder[object_idx];
}

inline std::vector<GCode::ObjectByExtruder::Island>& object_islands_by_extruder(
    std::map<unsigned int, std::vector<GCode::ObjectByExtruder>>  &by_extruder,
    unsigned int                                                   extruder_id,
    size_t                                                         object_idx,
    size_t                                                         num_objects,
    size_t                                                         num_islands)
{
    std::vector<GCode::ObjectByExtruder::Island> &islands = object_by_extruder(by_extruder, extruder_id, object_idx, num_objects).islands;
    if (islands.empty())
        islands.assign(num_islands, GCode::ObjectByExtruder::Island());
    return islands;
}

std::vector<GCode::InstanceToPrint> GCode::sort_print_object_instances(
    std::vector<GCode::ObjectByExtruder> 		&objects_by_extruder,
    const std::vector<LayerToPrint> 			&layers,
    // Ordering must be defined for normal (non-sequential print).
    const std::vector<const PrintInstance*> 	*ordering,
    // For sequential print, the instance of the object to be printing has to be defined.
    const size_t                     		 	 single_object_instance_idx)
{
    std::vector<InstanceToPrint> out;

    if (ordering == nullptr) {
        // Sequential print, single object is being printed.
        for (ObjectByExtruder &object_by_extruder : objects_by_extruder) {
            const size_t       layer_id     = &object_by_extruder - objects_by_extruder.data();
            const PrintObject *print_object = layers[layer_id].object();
            if (print_object)
                out.emplace_back(object_by_extruder, layer_id, *print_object, single_object_instance_idx);
        }
    } else {
        // Create mapping from PrintObject* to ObjectByExtruder*.
        std::vector<std::pair<const PrintObject*, ObjectByExtruder*>> sorted;
        sorted.reserve(objects_by_extruder.size());
        for (ObjectByExtruder &object_by_extruder : objects_by_extruder) {
            const size_t       layer_id     = &object_by_extruder - objects_by_extruder.data();
            const PrintObject *print_object = layers[layer_id].object();
            if (print_object)
                sorted.emplace_back(print_object, &object_by_extruder);
        }
        std::sort(sorted.begin(), sorted.end());

        if (! sorted.empty()) {
            out.reserve(sorted.size());
            for (const PrintInstance *instance : *ordering) {
                const PrintObject &print_object = *instance->print_object;
                std::pair<const PrintObject*, ObjectByExtruder*> key(&print_object, nullptr);
                auto it = std::lower_bound(sorted.begin(), sorted.end(), key);
                if (it != sorted.end() && it->first == &print_object)
                    // ObjectByExtruder for this PrintObject was found.
                    out.emplace_back(*it->second, it->second - objects_by_extruder.data(), print_object, instance - print_object.instances().data());
            }
        }
    }
    return out;
}

namespace ProcessLayer
{

    static std::string emit_custom_gcode_per_print_z(
        GCode                                                   &gcodegen,
        const CustomGCode::Item 								*custom_gcode,
        unsigned int                                             current_extruder_id,
        // ID of the first extruder printing this layer.
        unsigned int                                             first_extruder_id,
        const PrintConfig                                       &config)
    {
        std::string gcode;
        bool single_extruder_printer = config.nozzle_diameter.size() == 1;

        if (custom_gcode != nullptr) {
            // Extruder switches are processed by LayerTools, they should be filtered out.
            assert(custom_gcode->type != CustomGCode::ToolChange);

            CustomGCode::Type   gcode_type   = custom_gcode->type;
            bool  				color_change = gcode_type == CustomGCode::ColorChange;
            bool 				tool_change  = gcode_type == CustomGCode::ToolChange;
            // Tool Change is applied as Color Change for a single extruder printer only.
            assert(! tool_change || single_extruder_printer);

            std::string pause_print_msg;
            int m600_extruder_before_layer = -1;
            if (color_change && custom_gcode->extruder > 0)
                m600_extruder_before_layer = custom_gcode->extruder - 1;
            else if (gcode_type == CustomGCode::PausePrint)
                pause_print_msg = custom_gcode->extra;

            // we should add or not colorprint_change in respect to nozzle_diameter count instead of really used extruders count
            if (color_change || tool_change)
            {
                assert(m600_extruder_before_layer >= 0);
		        // Color Change or Tool Change as Color Change.
                // add tag for processor
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Color_Change) + ",T" + std::to_string(m600_extruder_before_layer) + "," + custom_gcode->color + "\n";

                if (!single_extruder_printer && m600_extruder_before_layer >= 0 && first_extruder_id != (unsigned)m600_extruder_before_layer
                    // && !MMU1
                    ) {
                    //! FIXME_in_fw show message during print pause
                    // FIXME: Why is pause_print_gcode here? Why is it supplied "color_change_extruder"? Why is that not 
                    //        passed to color_change_gcode below?
                    DynamicConfig cfg;
                    cfg.set_key_value("color_change_extruder", new ConfigOptionInt(m600_extruder_before_layer));
                    gcode += gcodegen.placeholder_parser_process("pause_print_gcode", config.pause_print_gcode, current_extruder_id, &cfg);
                    gcode += "\n";
                    gcode += "M117 Change filament for Extruder " + std::to_string(m600_extruder_before_layer) + "\n";
                }
                else {
                    gcode += gcodegen.placeholder_parser_process("color_change_gcode", config.color_change_gcode, current_extruder_id);
                    gcode += "\n";
                    //FIXME Tell G-code writer that M600 filled the extruder, thus the G-code writer shall reset the extruder to unretracted state after
                    // return from M600. Thus the G-code generated by the following line is ignored.
                    // see GH issue #6362
                    gcodegen.writer().unretract();
                }
	        } 
	        else {
	            if (gcode_type == CustomGCode::PausePrint) // Pause print
	            {
                    // add tag for processor
                    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Pause_Print) + "\n";
                    //! FIXME_in_fw show message during print pause
	                if (!pause_print_msg.empty())
	                    gcode += "M117 " + pause_print_msg + "\n";
                    gcode += gcodegen.placeholder_parser_process("pause_print_gcode", config.pause_print_gcode, current_extruder_id);
                }
	            else {
                    // add tag for processor
                    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Custom_Code) + "\n";
                    if (gcode_type == CustomGCode::Template)    // Template Custom Gcode
                        gcode += gcodegen.placeholder_parser_process("template_custom_gcode", config.template_custom_gcode, current_extruder_id);
                    else                                        // custom Gcode
                        gcode += custom_gcode->extra;

                }
                gcode += "\n";
            }
        }

        return gcode;
    }
} // namespace ProcessLayer

namespace Skirt {
    static void skirt_loops_per_extruder_all_printing(const Print &print, const LayerTools &layer_tools, std::map<unsigned int, std::pair<size_t, size_t>> &skirt_loops_per_extruder_out)
    {
        // Prime all extruders printing over the 1st layer over the skirt lines.
        size_t n_loops = print.skirt().entities.size();
        size_t n_tools = layer_tools.extruders.size();
        size_t lines_per_extruder = (n_loops + n_tools - 1) / n_tools;
        for (size_t i = 0; i < n_loops; i += lines_per_extruder)
            skirt_loops_per_extruder_out[layer_tools.extruders[i / lines_per_extruder]] = std::pair<size_t, size_t>(i, std::min(i + lines_per_extruder, n_loops));
    }

    static std::map<unsigned int, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_1st_layer(
        const Print             				&print,
        const LayerTools                		&layer_tools,
        // Heights (print_z) at which the skirt has already been extruded.
        std::vector<coordf_t>  			    	&skirt_done)
    {
        // Extrude skirt at the print_z of the raft layers and normal object layers
        // not at the print_z of the interlaced support material layers.
        std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
        //For sequential print, the following test may fail when extruding the 2nd and other objects.
        // assert(skirt_done.empty());
        if (skirt_done.empty() && print.has_skirt() && ! print.skirt().entities.empty() && layer_tools.has_skirt) {
            skirt_loops_per_extruder_all_printing(print, layer_tools, skirt_loops_per_extruder_out);
            skirt_done.emplace_back(layer_tools.print_z);
        }
        return skirt_loops_per_extruder_out;
    }

    static std::map<unsigned int, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_other_layers(
        const Print 							&print,
        const LayerTools                		&layer_tools,
        // Heights (print_z) at which the skirt has already been extruded.
        std::vector<coordf_t>			    	&skirt_done)
    {
        // Extrude skirt at the print_z of the raft layers and normal object layers
        // not at the print_z of the interlaced support material layers.
        std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
        if (print.has_skirt() && ! print.skirt().entities.empty() && layer_tools.has_skirt &&
            // Not enough skirt layers printed yet.
            //FIXME infinite or high skirt does not make sense for sequential print!
            (skirt_done.size() < (size_t)print.config().skirt_height.value || print.has_infinite_skirt())) {
            bool valid = ! skirt_done.empty() && skirt_done.back() < layer_tools.print_z - EPSILON;
            assert(valid);
            // This print_z has not been extruded yet (sequential print)
            // FIXME: The skirt_done should not be empty at this point. The check is a workaround
            // of https://github.com/prusa3d/PrusaSlicer/issues/5652, but it deserves a real fix.
            if (valid) {
#if 0
                // Prime just the first printing extruder. This is original Slic3r's implementation.
                skirt_loops_per_extruder_out[layer_tools.extruders.front()] = std::pair<size_t, size_t>(0, print.config().skirts.value);
#else
                // Prime all extruders planned for this layer, see
                // https://github.com/prusa3d/PrusaSlicer/issues/469#issuecomment-322450619
                skirt_loops_per_extruder_all_printing(print, layer_tools, skirt_loops_per_extruder_out);
#endif
                assert(!skirt_done.empty());
                skirt_done.emplace_back(layer_tools.print_z);
            }
        }
        return skirt_loops_per_extruder_out;
    }

} // namespace Skirt




// --------------------------------------------------------------------
double ATC_check_region_intersection2(LayerRegion& upper, LayerRegion& lower)
{
    ExPolygons A_polygons = to_expolygons(upper.slices.surfaces); // upper
    ExPolygons B_polygons = to_expolygons(lower.slices.surfaces); // lower
    ExPolygons region_intersection = intersection_ex(A_polygons, B_polygons);

    double A_expolygon_area = area(A_polygons);
    //double A_expolygon_perimeter = perimeter

    double B_expolygon_area = area(B_polygons);
    double intersection_area = area(region_intersection);
    return intersection_area;
}
// --------------------------------------------------------------------
double ATC_find_region_area(LayerRegion& region)
{
    ExPolygons region_expolygon = to_expolygons(region.slices.surfaces);
    double region_area = area(region_expolygon);
    return region_area;
}

double ATC_find_region_perimiter(LayerRegion& region)
{
    ExPolygons region_polygon = to_expolygons(region.slices.surfaces);
    double region_perimiter = 0;
    for (int i = 0; i < region_polygon.size(); i++)
    {
        region_perimiter = region_perimiter + region_polygon[i].contour.length();
    }
    return region_perimiter;
}
// --------------------------------------------------------------------

void GCode::ATC_export_CSV2(Print& print)
{
    std::vector<GCode::LayerToPrint> layers_to_print_ATC = collect_layers_to_print(*print.m_objects[0]);
    atc_printing_CSV printing_map_CVS;
    std::cout << "-- GCode:: ATC_export_CSV2() --" << std::endl;
    struct printing_piece_CSV* temp_CSV_piece;

    std::ofstream myfile;
    double region_area = 0;
    double region_perimeter = 0;

    size_t csv_number = 0;
    
    for (size_t RL = 0; RL < layers_to_print_ATC.size(); RL++) {
        if (layers_to_print_ATC[RL].object_layer != NULL)
        {
            Layer* current_layer = print.get_object(0)->layers()[RL];
            for (size_t R = 0; R < layers_to_print_ATC[RL].object_layer->regions().size(); R++)
            {
                float bound_X = 0;
                float bound_Y = 0;
                bool region_exist = 1;

                float R0 = 3;
                float R1 = 3;
                float R2 = 3;
                float R3 = 3;
                float R4 = 3;
                float R5 = 3;
                float R6 = 3;
                float R7 = 3;
                
                LayerRegion& current_region = *current_layer->regions()[R];
                region_area = ATC_find_region_area(current_region);
                csv_number++;

                if (layers_to_print_ATC[RL].object_layer->regions()[R]->perimeters.entities.size() != 0)
                {
                    region_perimeter = ATC_find_region_perimiter(current_region);
                    region_exist = 1; // Exist = 1
                }
                else
                {
                    region_perimeter = 0;
                    region_exist = 0; // Exist = 0
                }

                if (RL == 0 || layers_to_print_ATC[RL].object_layer->regions()[R]->perimeters.entities.size() == 0)
                {
                    // *Rs = zeros if the region does not exist or it is a first layer
                    R0 = 0;
                    R1 = 0;
                    R2 = 0;
                    R3 = 0;
                    R4 = 0;
                    R5 = 0;
                    R6 = 0;
                    R7 = 0;
                }

                printing_map_CVS.append_node(
                    csv_number, // consecutive number
                    RL, // layer
                    R,  // region
                    region_exist,  // exist
                    bound_X,  // X
                    bound_Y,  // Y
                    R0,  // R0
                    R1,  // R1
                    R2,  // R2
                    R3,  // R3
                    R4,  // R4
                    R5,  // R5
                    R6,  // R6
                    R7,  // R7
                    region_area,  // region area
                    region_perimeter  // region perimeter
                );
            }
        }
    }

    // write the obtained linked list to csv file

    myfile.open("NEW_GRAPH_001.csv");
    myfile << "-=to_python=-\n";
    myfile << "No.,Layer,Region,Exist,X,Y,*R0,*R1,*R2,*R3,*R4,*R5,*R6,*R7,Area,Perimeter,\n";

    for (int i = 0; i < printing_map_CVS.get_count(); i++)
    {
        temp_CSV_piece = printing_map_CVS.get_node(i);

        myfile
            << std::to_string(temp_CSV_piece->number) << ","
            << std::to_string(temp_CSV_piece->layer) << ","
            << std::to_string(temp_CSV_piece->region) << ","
            << std::to_string(temp_CSV_piece->X) << ","
            << std::to_string(temp_CSV_piece->Y) << ","
            << std::to_string(temp_CSV_piece->R0) << ","
            << std::to_string(temp_CSV_piece->R1) << ","
            << std::to_string(temp_CSV_piece->R2) << ","
            << std::to_string(temp_CSV_piece->R3) << ","
            << std::to_string(temp_CSV_piece->R4) << ","
            << std::to_string(temp_CSV_piece->R5) << ","
            << std::to_string(temp_CSV_piece->R6) << ","
            << std::to_string(temp_CSV_piece->R7) << ","
            << std::to_string(temp_CSV_piece->area) << ","
            << std::to_string(temp_CSV_piece->perimeter) << ","
            << "\n";
    }

    myfile.close();
    
}


void GCode::ATC_export_CSV(Print& print)
{
    std::vector<GCode::LayerToPrint> layers_to_print_ATC = collect_layers_to_print(*print.m_objects[0]);
    atc_printing_CSV printing_map_CVS;
    std::ofstream myfile;

    double region_area = 0;
    double region_perimeter = 0;

    std::cout << "-- GCode:: ATC_export_CSV() --" << std::endl;
    atc_linked_list_UPD printing_map_initial, printing_map_batched;


    myfile.open("DEPENDENCY_GRAPH_001.csv");
    myfile << "-=to_python=-\n";
    myfile << "No.,Layer,Region,Exist,X,Y,Tool,*R0,*R1,*R2,*R3,*R4,*R5,*R6,*R7,Area,Perimeter,\n";


    size_t number = 0;

    for (size_t RL = 0; RL < layers_to_print_ATC.size(); RL++) {
        if (layers_to_print_ATC[RL].object_layer != NULL)
        {
            Layer* current_layer = print.get_object(0)->layers()[RL];
            for (size_t R = 0; R < layers_to_print_ATC[RL].object_layer->regions().size(); R++)
            {
                LayerRegion& current_region = *current_layer->regions()[R];
                region_area = ATC_find_region_area(current_region);
                number++;
                myfile
                    << std::to_string(number) << ","
                    << std::to_string(RL) << ","
                    << std::to_string(R) << ",";
                    
                if (layers_to_print_ATC[RL].object_layer->regions()[R]->perimeters.entities.size() != 0)
                {
                    myfile
                        << std::to_string(1) << ","; // Exist = 1
                }
                else
                {
                    myfile
                        << std::to_string(0) << ","; // Exist = 0
                }
                myfile
                    << std::to_string(0) << "," // X
                    << std::to_string(0) << "," // Y
                    << std::to_string(R) << ","; // Tool = region (color)

                
                
                if (RL == 0 || layers_to_print_ATC[RL].object_layer->regions()[R]->perimeters.entities.size() == 0)
                {
                    for (int i = 0; i < 8; i++)
                    {
                        myfile << std::to_string(0) << ","; // *Rs = zeros if the region does not exist or it is a first layer
                    }
                }

                if (RL != 0 && layers_to_print_ATC[RL].object_layer->regions()[R]->perimeters.entities.size() != 0)
                {
                    Layer* layer_below = print.get_object(0)->layers()[RL-1];
                    for (int color = 0; color < layers_to_print_ATC[RL].object_layer->regions().size(); color++)
                    {
                        LayerRegion& region_below = *layer_below->regions()[color];
                        double region_intersection = ATC_check_region_intersection2(current_region, region_below);
                        myfile << std::to_string(region_intersection) << ","; // *Rs
                        /*
                        if (region_below.perimeters.entities.size() != 0)
                        {
                            double region_intersection = ATC_check_region_intersection2(current_region, region_below);
                            myfile << std::to_string(region_intersection) << ","; // *Rs
                        }
                        else
                        {
                            myfile << std::to_string(0) << ","; // *Rs
                        }
                        */
                    }
                }


                if (layers_to_print_ATC[RL].object_layer->regions()[R]->perimeters.entities.size() != 0)
                {
                    region_perimeter = ATC_find_region_perimiter(current_region);
                    myfile
                        << std::to_string(region_area) << "," // Area
                        << std::to_string(region_perimeter) << ","; // Perimeter
                }
                else
                {
                    myfile
                        << std::to_string(0) << "," // Area
                        << std::to_string(0) << ","; // Perimeter
                }
                myfile 
                    << "\n";

            }
        }
    }


        /*struct printing_piece_UPD* ATC_CSV_printing_piece;
        for (int i = 0; i < this->ATC_printing_map.get_count(); i++)
        {
            ATC_CSV_printing_piece = this->ATC_printing_map.get_node(i);
            //int print_Rlayer_idx = obj_temp_piece->Rlayer;
            myfile
                << std::to_string(i) << ","
                << std::to_string(ATC_CSV_printing_piece->print_z) << ","
                << std::to_string(ATC_CSV_printing_piece->object) << ","
                << std::to_string(ATC_CSV_printing_piece->support) << ","
                << std::to_string(ATC_CSV_printing_piece->Rlayer) << ","
                << std::to_string(ATC_CSV_printing_piece->Blayer) << ","
                << std::to_string(ATC_CSV_printing_piece->region) << ","
                << std::to_string(ATC_CSV_printing_piece->batch) << ","
                << std::to_string(ATC_CSV_printing_piece->area) << ","
                << std::to_string(ATC_CSV_printing_piece->perimeter) << ","
                //<< std::to_string(ATC_CSV_printing_piece->shared_perimeter) << ","
                //<< std::to_string(ATC_CSV_printing_piece->intersection_self) << ","
                << "\n";
        } */
    myfile.close();
}



void GCode::layer_batch_labeling(Print& print)
{
    std::cout << "-- GCode:: layer_batch_labeling() --" << std::endl;
    
    std::vector<GCode::LayerToPrint> layers_to_print_ATC = collect_layers_to_print(*print.m_objects[0]);

    atc_linked_list_UPD printing_map_initial, printing_map_batched;
    atc_linked_list_UPD support_map;
    bool need_wipe = 0;


    int BL = 0; // here we add additional variable just for batching analysis
    bool state = false; // initially all nodes are not processed
    int batch = 0; // initial batch is zero
    size_t atc_map_number = 0;

    float region_area = 0;
    float region_perimeter = 0;

    float atc_region_order_flip = 1;


    for (size_t RL = 0; RL < layers_to_print_ATC.size(); RL++) {
        if (layers_to_print_ATC[RL].object_layer != NULL)
        {
            for (size_t R = 0; R < layers_to_print_ATC[RL].object_layer->regions().size(); R++)
            {
                //if (layers_to_print_ATC[RL].object_layer->regions()[R]->slices.surfaces.size() != 0)
                if (layers_to_print_ATC[RL].object_layer->regions()[R]->perimeters.entities.size() != 0)
                {
                    printing_map_initial.append_node(
                        atc_map_number, // consecutive number
                        layers_to_print_ATC[RL].object_layer->print_z, // print_z
                        true, // object layer
                        false, // support layer
                        RL, // layer,
                        BL, // batch layer
                        R,  // region
                        region_area,  // area
                        region_perimeter,  // perimeter
                        state, // node processed state
                        batch, // batch
                        need_wipe, // wiping layer
                        0 // region_intersection
                    );
                    atc_map_number += 1;
                }
            }
            /*
            if (atc_region_order_flip > 0)
            {
                for (size_t R = 0; R < layers_to_print_ATC[RL].object_layer->regions().size(); R++)
                {
                    if (layers_to_print_ATC[RL].object_layer->regions()[R]->slices.surfaces.size() != 0)
                    {
                        printing_map_initial.append_node(
                            atc_map_number, // consecutive number
                            layers_to_print_ATC[RL].object_layer->print_z, // print_z
                            true, // object layer
                            false, // support layer
                            RL, // layer,
                            BL, // batch layer
                            R,  // region
                            state, // node processed state
                            batch, // batch
                            need_wipe, // wiping layer
                            0 // region_intersection
                        );
                        atc_map_number += 1;
                    }
                }
            }

            if (atc_region_order_flip < 0)
            {
                for (int R = layers_to_print_ATC[RL].object_layer->regions().size()-1; R > -1 ; R--)
                {
                    if (layers_to_print_ATC[RL].object_layer->regions()[R]->slices.surfaces.size() != 0)
                    {
                        printing_map_initial.append_node(
                            atc_map_number, // consecutive number
                            layers_to_print_ATC[RL].object_layer->print_z, // print_z
                            true, // object layer
                            false, // support layer
                            RL, // layer,
                            BL, // batch layer
                            R,  // region
                            state, // node processed state
                            batch, // batch
                            need_wipe, // wiping layer
                            0 // region_intersection
                        );
                        atc_map_number += 1;
                    }
                }
            }
            */
            
            BL += 1;
            atc_region_order_flip = atc_region_order_flip * (-1);
        }
    }

    atc_map_number = 0;
    for (size_t RL = 0; RL < layers_to_print_ATC.size(); RL++) {
        if (layers_to_print_ATC[RL].support_layer != NULL)
        {
            support_map.append_node(
                atc_map_number, // consecutive number
                -1, // print_z
                false, // object layer
                true, // support layer
                RL, // layer,
                -1, // batch layer
                -1,  // region
                region_area,  // area
                region_perimeter,  // perimeter
                false, // node processed state
                -1, // batch
                need_wipe, // wiping layer
                0 // region_intersection
            );
            atc_map_number += 1;
        }
    }

    std::cout << "PRINTING MAP INITIAL: (count=" << printing_map_initial.get_count() << ")" << std::endl;
    atc_linked_list_UPD::display(printing_map_initial.gethead());
    std::cout << "\n\nSUPPORT MAP: (count=" << support_map.get_count() << ")" << std::endl;
    atc_linked_list_UPD::display(support_map.gethead());




    int printing_pieces_count = printing_map_initial.get_count();
    int atc_appending_node_number = 0;
    float atc_print_z = 0;
    double cum_layer_height = 0;
    //float atc_batch_epsilon = 0.02; // in mm, additional batch height to make sure the batch size is a multiple of a layer height
    double atc_safe_height = print.m_objects[0]->config().atc_safe_batch_height.value; // 0.4 default value in mm
    double atc_running_height = 0; // check it more thoroughly at the later stages
    int batch_new = 0;
    double region_intersection = 0;
    double critical_intersection = print.m_objects[0]->config().atc_critical_intersection_area; // 0.5
    double intersection_self = 0;

    //this->config().bed_temperature;

    int number_of_colors = print.get_object(0)->all_regions().size();
    int max_layers_in_object = print.get_object(0)->layers().size();
    int intersected_node_state;

    //int atc_iterator = 0;
    //int atc_step = 0;
    
    int intersected_region;
    struct printing_piece_UPD* node;
    node = NULL;
    struct printing_piece_UPD* last_node;
    last_node = NULL;
    struct printing_piece_UPD* last_appended_node;
    last_appended_node = NULL;
    struct printing_piece_UPD* candidate_node;
    candidate_node = NULL;
    int overall_intersections_below = 0;
    
    
    int current_Rlayer_idx, current_Blayer_idx, current_region_idx, candidate_Blayer_idx, candidate_region_idx;

    std::cout << "*********************************************" << std::endl;
    std::cout << "************** UPD MAIN ALGORITHM ***************" << std::endl;
    while (printing_map_batched.get_count() <= printing_map_initial.get_count() - 1)
    {
        //atc_step += 1;
        // get the first node in the list with zero-state (which is not done)
        if (last_node != NULL)
            node = last_node;
        if (last_node == NULL)
            node = printing_map_initial.node_search(printing_map_initial.gethead(), 0); // get first node with zero-state

        


        atc_print_z = node->print_z;
        current_Rlayer_idx = node->Rlayer;
        current_Blayer_idx = node->Blayer;
        current_region_idx = node->region;
        candidate_Blayer_idx = node->Blayer + 1;
        candidate_region_idx = node->region; // the same region

        // find area and perimeter
        //Layer* layer_current_area = print.get_object(0)->layers()[current_Rlayer_idx];
        //LayerRegion& region_current_area = *layer_current_area->regions()[current_region_idx];
        //region_area = ATC_find_region_area(region_current_area);
        //region_perimeter = ATC_find_region_perimiter(region_current_area);

        //std::cout << "--STEP-- " << atc_step << ", PROCESSED NODES=" << atc_iterator << std::endl;
        std::cout << "got node {L" << current_Blayer_idx << ", R" << current_region_idx << "}" << " -- candidate {Lc" << candidate_Blayer_idx << ", Rc" << candidate_region_idx << "}" << std::endl;



        if (node->state == 0)
        {
            printing_map_batched.append_node(
                atc_appending_node_number,
                atc_print_z,
                true, // object_layer
                false, // support_layer
                current_Rlayer_idx, // regular layer idx
                current_Blayer_idx,
                current_region_idx,
                region_area,  // area
                region_perimeter,  // perimeter
                1, // state = 1
                batch_new,
                need_wipe,
                intersection_self
            );

            atc_appending_node_number += 1;
            last_node = printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, current_region_idx);
            printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, current_region_idx)->state = 1;
            std::cout << "appended node {L" << current_Blayer_idx << ", R" << current_region_idx << "}" << std::endl;
            //atc_iterator += 1;
            atc_running_height += print.get_object(0)->layers()[current_Blayer_idx]->height; //in mm
            std::cout << "===atc_running_height===" << atc_running_height << "mm" << std::endl;
        }

        if (printing_map_initial.node_search(printing_map_initial.gethead(), candidate_Blayer_idx, candidate_region_idx) && candidate_Blayer_idx < max_layers_in_object)
        {
            Layer* layer_candidate = print.get_object(0)->layers()[candidate_Blayer_idx];
            Layer* layer_current = print.get_object(0)->layers()[current_Blayer_idx];
            LayerRegion& region_candidate = *layer_candidate->regions()[current_region_idx];

            overall_intersections_below = 0;
            for (int color = 0; color < number_of_colors; color++)
            {
                LayerRegion& region_below = *layer_current->regions()[color];

                region_intersection = ATC_check_region_intersection2(region_candidate, region_below) / 1e+10;
                std::cout << "checking intersections for {L" << current_Blayer_idx << ", R" << color << "}: region_intersection=" << region_intersection << std::endl;
                if (printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, color))
                {
                    intersected_node_state = printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, color)->state;
                    if (color != current_region_idx && region_intersection > critical_intersection && intersected_node_state == 0)
                    {
                        overall_intersections_below += 1;
                        std::cout << "overall_intersections_below=" << overall_intersections_below << std::endl;
                    }
                }
            }

            for (int color = 0; color < number_of_colors; color++)
            {
                LayerRegion& region_below = *layer_current->regions()[color];
                region_intersection = ATC_check_region_intersection2(region_candidate, region_below) / 1e+10;
                std::cout << "checking intersections for {L" << current_Blayer_idx << ", R" << color << "}: region_intersection=" << region_intersection << std::endl;
                if (color == current_region_idx)
                {
                    intersection_self = 0;
                }
                if (printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, color))
                {
                    intersected_node_state = printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, color)->state;
                    if (color != current_region_idx && (region_intersection > critical_intersection && intersected_node_state == 0))
                    {
                        // stop, remap
                        std::cout << "detected intersection with {L" << current_Blayer_idx << ", R" << color << "}" << std::endl;
                        std::cout << "breaking" << std::endl;
                        last_node = NULL;
                        break;
                    }
                    if (color != current_region_idx && region_intersection <= critical_intersection && overall_intersections_below == 0)
                    {
                        candidate_node = printing_map_initial.node_search(printing_map_initial.gethead(), candidate_Blayer_idx, candidate_region_idx);
                        if (candidate_node)
                        {
                            region_area = ATC_find_region_area(region_candidate);
                            region_perimeter = ATC_find_region_perimiter(region_candidate);
                            // append new node to the batched map
                            printing_map_batched.append_node(
                                atc_appending_node_number,
                                candidate_node->print_z,
                                true,
                                false,
                                candidate_node->Rlayer,
                                candidate_Blayer_idx,
                                candidate_region_idx,
                                region_area,  // area
                                region_perimeter,  // perimeter
                                1, // state = 1
                                batch_new,
                                need_wipe,
                                region_intersection
                            );

                            atc_appending_node_number += 1;
                            last_node = printing_map_initial.node_search(printing_map_initial.gethead(), candidate_Blayer_idx, candidate_region_idx);
                            atc_running_height += print.get_object(0)->layers()[current_Blayer_idx]->height; //in mm
                            std::cout << "===atc_running_height===" << atc_running_height << "mm" << std::endl;

                            printing_map_initial.node_search(printing_map_initial.gethead(), candidate_Blayer_idx, candidate_region_idx)->state = 1;
                            //atc_iterator += 1;
                            std::cout << "no intersections --> appending node {L" << candidate_Blayer_idx << ", R" << candidate_region_idx << "}" << std::endl;
                            break;
                        }
                        else
                        {
                            std::cout << "no intersections, BUT the candidate node is not found: breaking" << std::endl;
                            last_node = NULL;
                            break;
                        }
                    }
                }
                else
                {
                    std::cout << "before intersection check the candidate node is not found: continuing" << std::endl;
                    last_node = NULL;
                    continue;
                }
            }
        }

        if (candidate_Blayer_idx >= max_layers_in_object || printing_map_initial.node_search(printing_map_initial.gethead(), candidate_Blayer_idx, candidate_region_idx) == NULL)
        {
            std::cout << "candidate_layer_idx >= max_layers_in_object OR printing node==NULL: continuing" << std::endl;
            last_node = NULL;
            continue;
        }

        if (atc_running_height >= atc_safe_height)
        {
            atc_running_height = 0;
            std::cout << "detected critical height: continuing" << std::endl;
            last_node = NULL;
            continue;
        }

        //this->ATC_printing_map = printing_map_batched;
    }
    std::cout << "********** END of UPD MAIN ALGORITHM ************" << std::endl;
    std::cout << "*********************************************" << std::endl;

    std::cout << "::::::::::::::::::::::::::::::::::\n\n\n" << std::endl;
    std::cout << "PRINTING MAP INITIAL (" << printing_map_initial.get_count() << "):" << std::endl;
    atc_linked_list_UPD::display(printing_map_initial.gethead());
    std::cout << "\nPRINTING MAP BATCHED (" << printing_map_batched.get_count() << "):" << std::endl;
    atc_linked_list_UPD::display(printing_map_batched.gethead());
    std::cout << "\nSUPPORT MAP (" << support_map.get_count() << "):" << std::endl;
    atc_linked_list_UPD::display(support_map.gethead());
    std::cout << "\n\n\n::::::::::::::::::::::::::::::::::" << std::endl;



    std::cout << "*************************************************************************" << std::endl;
    std::cout << "********** ANALYZE BATCHES BEFORE COMBINING INTO A FINAL MAP ************" << std::endl;

    struct printing_piece_UPD* temp_batch_piece;
    int running_batch = 0;
    int last_region = 0;
    size_t atc_temp_tool_changes = 0;
    for (int i = 0; i < printing_map_batched.get_count(); i++)
    {
        temp_batch_piece = printing_map_batched.get_node(i);
        int print_region_idx = temp_batch_piece->region;

        if (print_region_idx != last_region)
        {
            running_batch = running_batch + 1;
            temp_batch_piece->batch = running_batch;
            last_region = print_region_idx;
            atc_temp_tool_changes = atc_temp_tool_changes + 1;
        }
        temp_batch_piece->batch = running_batch;
    }

    std::cout << "\n NUMBER OF BATCHED TOOL CHANGES = " << atc_temp_tool_changes << std::endl;
    std::cout << "\n******* END of ANALYZE BATCHES BEFORE COMBINING INTO A FINAL MAP ********" << std::endl;
    std::cout << "*************************************************************************" << std::endl;



    std::cout << "*************************************************************************" << std::endl;
    std::cout << "****** COMBINE OBJECT AND SUPPORT PIECES INTO A FINAL SINGLE MAP ********" << std::endl;
    atc_linked_list_UPD FINAL_MAP;

    struct printing_piece_UPD* obj_temp_piece;
    struct printing_piece_UPD* supp_temp_piece;
    size_t atc_final_map_counter = 0;
    for (int i = 0; i < printing_map_batched.get_count(); i++)
    {
        obj_temp_piece = printing_map_batched.get_node(i);
        int print_Rlayer_idx = obj_temp_piece->Rlayer;
        int print_region_idx = obj_temp_piece->region;
        std::cout << "{RL" << print_Rlayer_idx << ", R" << print_region_idx << "}" << std::endl;
        FINAL_MAP.append_node(
            atc_final_map_counter, // consecutive number
            obj_temp_piece->print_z, // print_z
            true, // object layer
            false, // support layer
            obj_temp_piece->Rlayer, // layer,
            obj_temp_piece->Blayer, // batch layer
            obj_temp_piece->region,  // region
            region_area,  // area
            region_perimeter,  // perimeter
            false, // node processed state
            obj_temp_piece->batch, // batch
            obj_temp_piece->need_wipe, // need_wipe
            obj_temp_piece->region_intersection
        );
        atc_final_map_counter += 1;

        for (int k = 0; k < support_map.get_count(); k++)
        {
            supp_temp_piece = support_map.get_node(k);
            if (!supp_temp_piece->state)
            {
                int support_Rlayer_idx = supp_temp_piece->Rlayer;
                //std::cout << "Support L" << support_Rlayer_idx << ", state=" << supp_temp_piece->state << std::endl;
                //if ((print_Rlayer_idx - support_Rlayer_idx) <= 1 || (support_Rlayer_idx - print_Rlayer_idx) <= 1)
                if ((support_Rlayer_idx - print_Rlayer_idx) <= 1)
                {
                    std::cout << "-- S, RL" << support_Rlayer_idx << std::endl;
                    FINAL_MAP.append_node(
                        atc_final_map_counter, // consecutive number
                        supp_temp_piece->print_z, // print_z
                        false, // object layer
                        true, // support layer
                        supp_temp_piece->Rlayer, // layer,
                        supp_temp_piece->Blayer, // batch layer
                        obj_temp_piece->region,  // region
                        region_area,  // area
                        region_perimeter,  // perimeter
                        false, // node processed state
                        obj_temp_piece->batch, // batch
                        obj_temp_piece->need_wipe, // need_wipe
                        obj_temp_piece->region_intersection
                    );
                    atc_final_map_counter += 1;
                    supp_temp_piece->state = 1;
                    break;
                }

            }
        }
    }


    //this->ATC_printing_map = printing_map_batched;
    this->ATC_printing_map = FINAL_MAP;

    
    

    std::cout << "\n********** FINAL MAP ************\n" << std::endl;
    std::cout << "\FINAL MAP (" << this->ATC_printing_map.get_count() << "):" << std::endl;
    //atc_linked_list_UPD::display(this->ATC_printing_map.gethead());
    this->ATC_printing_map.display(this->ATC_printing_map.gethead());
    std::cout << "\n******** EOF FINAL MAP **********\n" << std::endl;

    std::cout << "*** END of COMBINE OBJECT AND SUPPORT PIECES INTO A FINAL SINGLE MAP ****" << std::endl;
    std::cout << "*************************************************************************" << std::endl;
}

void GCode::layer_batch_labeling_soluble_supports(Print& print)
{
    std::cout << "-- GCode:: layer_batch_labeling() --" << std::endl;

    std::vector<GCode::LayerToPrint> layers_to_print_ATC = collect_layers_to_print(*print.m_objects[0]);

    atc_linked_list_UPD printing_map_initial, printing_map_batched;
    atc_linked_list_UPD support_map;
    bool need_wipe = 0;


    int BL = 0; // here we add additional variable just for batching analysis
    bool state = false; // initially all nodes are not processed
    int batch = 0; // initial batch is zero
    size_t atc_map_number = 0;

    float region_area = 0;
    float region_perimeter = 0;

    float atc_region_order_flip = 1;


    for (size_t RL = 0; RL < layers_to_print_ATC.size(); RL++) {
        if (layers_to_print_ATC[RL].object_layer != NULL)
        {
            for (size_t R = 0; R < layers_to_print_ATC[RL].object_layer->regions().size(); R++)
            {
                //if (layers_to_print_ATC[RL].object_layer->regions()[R]->slices.surfaces.size() != 0)
                if (layers_to_print_ATC[RL].object_layer->regions()[R]->perimeters.entities.size() != 0)
                {
                    printing_map_initial.append_node(
                        atc_map_number, // consecutive number
                        layers_to_print_ATC[RL].object_layer->print_z, // print_z
                        true, // object layer
                        false, // support layer
                        RL, // layer,
                        BL, // batch layer
                        R,  // region
                        region_area,  // area
                        region_perimeter,  // perimeter
                        state, // node processed state
                        batch, // batch
                        need_wipe, // wiping layer
                        0 // region_intersection
                    );
                    atc_map_number += 1;
                }
            }

            BL += 1;
            atc_region_order_flip = atc_region_order_flip * (-1);
        }
    }

    atc_map_number = 0;
    for (size_t RL = 0; RL < layers_to_print_ATC.size(); RL++) {
        if (layers_to_print_ATC[RL].support_layer != NULL)
        {
            support_map.append_node(
                atc_map_number, // consecutive number
                -1, // print_z
                false, // object layer
                true, // support layer
                RL, // layer,
                -1, // batch layer
                -1,  // region
                region_area,  // area
                region_perimeter,  // perimeter
                false, // node processed state
                -1, // batch
                need_wipe, // wiping layer
                0 // region_intersection
            );
            atc_map_number += 1;
        }
    }

    std::cout << "PRINTING MAP INITIAL: (count=" << printing_map_initial.get_count() << ")" << std::endl;
    atc_linked_list_UPD::display(printing_map_initial.gethead());
    std::cout << "\n\nSUPPORT MAP: (count=" << support_map.get_count() << ")" << std::endl;
    atc_linked_list_UPD::display(support_map.gethead());


    int printing_pieces_count = printing_map_initial.get_count();
    int atc_appending_node_number = 0;
    float atc_print_z = 0;
    double cum_layer_height = 0;
    //float atc_batch_epsilon = 0.02; // in mm, additional batch height to make sure the batch size is a multiple of a layer height
    double atc_safe_height = print.m_objects[0]->config().atc_safe_batch_height.value; // 0.4 default value in mm
    double atc_running_height = 0; // check it more thoroughly at the later stages
    int batch_new = 0;
    double region_intersection = 0;
    double critical_intersection = print.m_objects[0]->config().atc_critical_intersection_area; // 0.5
    double intersection_self = 0;

    //this->config().bed_temperature;

    int number_of_colors = print.get_object(0)->all_regions().size();
    int max_layers_in_object = print.get_object(0)->layers().size();
    int intersected_node_state;


    int intersected_region;
    struct printing_piece_UPD* node;
    node = NULL;
    struct printing_piece_UPD* last_node;
    last_node = NULL;
    struct printing_piece_UPD* last_appended_node;
    last_appended_node = NULL;
    struct printing_piece_UPD* candidate_node;
    candidate_node = NULL;
    int overall_intersections_below = 0;


    int current_Rlayer_idx, current_Blayer_idx, current_region_idx, candidate_Blayer_idx, candidate_region_idx;

    std::cout << "*********************************************" << std::endl;
    std::cout << "************** UPD MAIN ALGORITHM ***************" << std::endl;
    while (printing_map_batched.get_count() <= printing_map_initial.get_count() - 1)
    {
        //atc_step += 1;
        // get the first node in the list with zero-state (which is not done)
        if (last_node != NULL)
            node = last_node;
        if (last_node == NULL)
            node = printing_map_initial.node_search(printing_map_initial.gethead(), 0); // get first node with zero-state

        atc_print_z = node->print_z;
        current_Rlayer_idx = node->Rlayer;
        current_Blayer_idx = node->Blayer;
        current_region_idx = node->region;
        candidate_Blayer_idx = node->Blayer + 1;
        candidate_region_idx = node->region; // the same region

        // find area and perimeter
        //Layer* layer_current_area = print.get_object(0)->layers()[current_Rlayer_idx];
        //LayerRegion& region_current_area = *layer_current_area->regions()[current_region_idx];
        //region_area = ATC_find_region_area(region_current_area);
        //region_perimeter = ATC_find_region_perimiter(region_current_area);

        //std::cout << "--STEP-- " << atc_step << ", PROCESSED NODES=" << atc_iterator << std::endl;
        std::cout << "got node {L" << current_Blayer_idx << ", R" << current_region_idx << "}" << " -- candidate {Lc" << candidate_Blayer_idx << ", Rc" << candidate_region_idx << "}" << std::endl;

        if (node->state == 0)
        {
            printing_map_batched.append_node(
                atc_appending_node_number,
                atc_print_z,
                true, // object_layer
                false, // support_layer
                current_Rlayer_idx, // regular layer idx
                current_Blayer_idx,
                current_region_idx,
                region_area,  // area
                region_perimeter,  // perimeter
                1, // state = 1
                batch_new,
                need_wipe,
                intersection_self
            );

            atc_appending_node_number += 1;
            last_node = printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, current_region_idx);
            printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, current_region_idx)->state = 1;
            std::cout << "appended node {L" << current_Blayer_idx << ", R" << current_region_idx << "}" << std::endl;
            //atc_iterator += 1;
            atc_running_height += print.get_object(0)->layers()[current_Blayer_idx]->height; //in mm
            std::cout << "===atc_running_height===" << atc_running_height << "mm" << std::endl;
        }

        if (printing_map_initial.node_search(printing_map_initial.gethead(), candidate_Blayer_idx, candidate_region_idx) && candidate_Blayer_idx < max_layers_in_object)
        {
            Layer* layer_candidate = print.get_object(0)->layers()[candidate_Blayer_idx];
            Layer* layer_current = print.get_object(0)->layers()[current_Blayer_idx];
            LayerRegion& region_candidate = *layer_candidate->regions()[current_region_idx];

            overall_intersections_below = 0;
            for (int color = 0; color < number_of_colors; color++)
            {
                LayerRegion& region_below = *layer_current->regions()[color];

                region_intersection = ATC_check_region_intersection2(region_candidate, region_below) / 1e+10;
                std::cout << "checking intersections for {L" << current_Blayer_idx << ", R" << color << "}: region_intersection=" << region_intersection << std::endl;
                if (printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, color))
                {
                    intersected_node_state = printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, color)->state;
                    if (color != current_region_idx && region_intersection > critical_intersection && intersected_node_state == 0)
                    {
                        overall_intersections_below += 1;
                        std::cout << "overall_intersections_below=" << overall_intersections_below << std::endl;
                    }
                }
            }

            for (int color = 0; color < number_of_colors; color++)
            {
                LayerRegion& region_below = *layer_current->regions()[color];
                region_intersection = ATC_check_region_intersection2(region_candidate, region_below) / 1e+10;
                std::cout << "checking intersections for {L" << current_Blayer_idx << ", R" << color << "}: region_intersection=" << region_intersection << std::endl;
                if (color == current_region_idx)
                {
                    intersection_self = 0;
                }
                if (printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, color))
                {
                    intersected_node_state = printing_map_initial.node_search(printing_map_initial.gethead(), current_Blayer_idx, color)->state;
                    if (color != current_region_idx && (region_intersection > critical_intersection && intersected_node_state == 0))
                    {
                        // stop, remap
                        std::cout << "detected intersection with {L" << current_Blayer_idx << ", R" << color << "}" << std::endl;
                        std::cout << "breaking" << std::endl;
                        last_node = NULL;
                        break;
                    }
                    if (color != current_region_idx && region_intersection <= critical_intersection && overall_intersections_below == 0)
                    {
                        candidate_node = printing_map_initial.node_search(printing_map_initial.gethead(), candidate_Blayer_idx, candidate_region_idx);
                        if (candidate_node)
                        {
                            region_area = ATC_find_region_area(region_candidate);
                            region_perimeter = ATC_find_region_perimiter(region_candidate);
                            // append new node to the batched map
                            printing_map_batched.append_node(
                                atc_appending_node_number,
                                candidate_node->print_z,
                                true,
                                false,
                                candidate_node->Rlayer,
                                candidate_Blayer_idx,
                                candidate_region_idx,
                                region_area,  // area
                                region_perimeter,  // perimeter
                                1, // state = 1
                                batch_new,
                                need_wipe,
                                region_intersection
                            );

                            atc_appending_node_number += 1;
                            last_node = printing_map_initial.node_search(printing_map_initial.gethead(), candidate_Blayer_idx, candidate_region_idx);
                            atc_running_height += print.get_object(0)->layers()[current_Blayer_idx]->height; //in mm
                            std::cout << "===atc_running_height===" << atc_running_height << "mm" << std::endl;

                            printing_map_initial.node_search(printing_map_initial.gethead(), candidate_Blayer_idx, candidate_region_idx)->state = 1;
                            //atc_iterator += 1;
                            std::cout << "no intersections --> appending node {L" << candidate_Blayer_idx << ", R" << candidate_region_idx << "}" << std::endl;
                            break;
                        }
                        else
                        {
                            std::cout << "no intersections, BUT the candidate node is not found: breaking" << std::endl;
                            last_node = NULL;
                            break;
                        }
                    }
                }
                else
                {
                    std::cout << "before intersection check the candidate node is not found: continuing" << std::endl;
                    last_node = NULL;
                    continue;
                }
            }
        }

        if (candidate_Blayer_idx >= max_layers_in_object || printing_map_initial.node_search(printing_map_initial.gethead(), candidate_Blayer_idx, candidate_region_idx) == NULL)
        {
            std::cout << "candidate_layer_idx >= max_layers_in_object OR printing node==NULL: continuing" << std::endl;
            last_node = NULL;
            continue;
        }

        if (atc_running_height >= atc_safe_height)
        {
            atc_running_height = 0;
            std::cout << "detected critical height: continuing" << std::endl;
            last_node = NULL;
            continue;
        }

        //this->ATC_printing_map = printing_map_batched;
    }
    std::cout << "********** END of UPD MAIN ALGORITHM ************" << std::endl;
    std::cout << "*********************************************" << std::endl;

    std::cout << "::::::::::::::::::::::::::::::::::\n\n\n" << std::endl;
    std::cout << "PRINTING MAP INITIAL (" << printing_map_initial.get_count() << "):" << std::endl;
    atc_linked_list_UPD::display(printing_map_initial.gethead());
    std::cout << "\nPRINTING MAP BATCHED (" << printing_map_batched.get_count() << "):" << std::endl;
    atc_linked_list_UPD::display(printing_map_batched.gethead());
    std::cout << "\nSUPPORT MAP (" << support_map.get_count() << "):" << std::endl;
    atc_linked_list_UPD::display(support_map.gethead());
    std::cout << "\n\n\n::::::::::::::::::::::::::::::::::" << std::endl;



    std::cout << "*************************************************************************" << std::endl;
    std::cout << "********** ANALYZE BATCHES BEFORE COMBINING INTO A FINAL MAP ************" << std::endl;

    struct printing_piece_UPD* temp_batch_piece;
    int running_batch = 0;
    int last_region = 0;
    size_t atc_temp_tool_changes = 0;
    for (int i = 0; i < printing_map_batched.get_count(); i++)
    {
        temp_batch_piece = printing_map_batched.get_node(i);
        int print_region_idx = temp_batch_piece->region;

        if (print_region_idx != last_region)
        {
            running_batch = running_batch + 1;
            temp_batch_piece->batch = running_batch;
            last_region = print_region_idx;
            atc_temp_tool_changes = atc_temp_tool_changes + 1;
        }
        temp_batch_piece->batch = running_batch;
    }

    std::cout << "\n NUMBER OF BATCHED TOOL CHANGES = " << atc_temp_tool_changes << std::endl;
    std::cout << "\n******* END of ANALYZE BATCHES BEFORE COMBINING INTO A FINAL MAP ********" << std::endl;
    std::cout << "*************************************************************************" << std::endl;



    std::cout << "*************************************************************************" << std::endl;
    std::cout << "****** COMBINE OBJECT AND SUPPORT PIECES INTO A FINAL SINGLE MAP ********" << std::endl;
    atc_linked_list_UPD FINAL_MAP;

    struct printing_piece_UPD* obj_temp_piece;
    struct printing_piece_UPD* supp_temp_piece;
    size_t atc_final_map_counter = 0;
    for (int i = 0; i < printing_map_batched.get_count(); i++)
    {
        obj_temp_piece = printing_map_batched.get_node(i);
        int print_Rlayer_idx = obj_temp_piece->Rlayer;
        int print_region_idx = obj_temp_piece->region;
        std::cout << "{RL" << print_Rlayer_idx << ", R" << print_region_idx << "}" << std::endl;
        FINAL_MAP.append_node(
            atc_final_map_counter, // consecutive number
            obj_temp_piece->print_z, // print_z
            true, // object layer
            false, // support layer
            obj_temp_piece->Rlayer, // layer,
            obj_temp_piece->Blayer, // batch layer
            obj_temp_piece->region,  // region
            region_area,  // area
            region_perimeter,  // perimeter
            false, // node processed state
            obj_temp_piece->batch, // batch
            obj_temp_piece->need_wipe, // need_wipe
            obj_temp_piece->region_intersection
        );
        atc_final_map_counter += 1;

        for (int k = 0; k < support_map.get_count(); k++)
        {
            supp_temp_piece = support_map.get_node(k);
            if (!supp_temp_piece->state)
            {
                int support_Rlayer_idx = supp_temp_piece->Rlayer;
                //std::cout << "Support L" << support_Rlayer_idx << ", state=" << supp_temp_piece->state << std::endl;
                //if ((print_Rlayer_idx - support_Rlayer_idx) <= 1 || (support_Rlayer_idx - print_Rlayer_idx) <= 1)
                if ((support_Rlayer_idx - print_Rlayer_idx) <= 1)
                {
                    std::cout << "-- S, RL" << support_Rlayer_idx << std::endl;
                    FINAL_MAP.append_node(
                        atc_final_map_counter, // consecutive number
                        supp_temp_piece->print_z, // print_z
                        false, // object layer
                        true, // support layer
                        supp_temp_piece->Rlayer, // layer,
                        supp_temp_piece->Blayer, // batch layer
                        //obj_temp_piece->region,  // region
                        3,
                        region_area,  // area
                        region_perimeter,  // perimeter
                        false, // node processed state
                        obj_temp_piece->batch, // batch
                        obj_temp_piece->need_wipe, // need_wipe
                        obj_temp_piece->region_intersection
                    );
                    atc_final_map_counter += 1;
                    supp_temp_piece->state = 1;
                    break;
                }

            }
        }
    }


    //this->ATC_printing_map = printing_map_batched;
    this->ATC_printing_map = FINAL_MAP;




    std::cout << "\n********** FINAL MAP ************\n" << std::endl;
    std::cout << "\FINAL MAP (" << this->ATC_printing_map.get_count() << "):" << std::endl;
    //atc_linked_list_UPD::display(this->ATC_printing_map.gethead());
    this->ATC_printing_map.display(this->ATC_printing_map.gethead());
    std::cout << "\n******** EOF FINAL MAP **********\n" << std::endl;

    std::cout << "*** END of COMBINE OBJECT AND SUPPORT PIECES INTO A FINAL SINGLE MAP ****" << std::endl;
    std::cout << "*************************************************************************" << std::endl;
}


// wipe tower as a single brick
void GCode::ATC_plan_wipe_toolchange(Print& print)
{
    std::cout << "\n\n\nvoid GCode::ATC_plan_wipe_toolchange()\n\n\n" << std::endl;
    // create atc_wipe_tower
    std::vector<float> wiping_matrix(cast<float>(print.m_config.wiping_volumes_matrix.values));
    std::vector<std::vector<float>> wipe_volumes;
    const unsigned int number_of_extruders = (unsigned int)(sqrt(wiping_matrix.size()) + EPSILON);
    for (unsigned int i = 0; i < number_of_extruders; ++i)
        wipe_volumes.push_back(std::vector<float>(wiping_matrix.begin() + i * number_of_extruders, wiping_matrix.begin() + (i + 1) * number_of_extruders));

    print.m_ATC_wipe_tower_data.tool_ordering = ToolOrdering(print, (unsigned int)-1, true);

    WipeTower atc_wipe_tower(print.m_config, wipe_volumes, print.m_ATC_wipe_tower_data.tool_ordering.first_extruder());
    for (size_t i = 0; i < number_of_extruders; ++i)
        atc_wipe_tower.set_extruder(i, print.m_config);


    // wiping parameters
    //bool need_wipe = true;
    float atc_wiping_volume = 320.0; // 140 hardcoded for now
    size_t atc_old_tool;
    size_t atc_new_tool;
    float atc_wiping_layer_height = 0.2; // hardcoded for now
    size_t atc_wipe_tower_idx = 0;
    float atc_print_z;

    // building wiping tool changes
    print.m_ATC_wipe_tower_data.clear();
    // iterate over the printing pieces
    struct printing_piece_UPD* printing_node;
    int prev_region_idx = 0;
    for (int printing_node_idx = 0; printing_node_idx < ATC_printing_map.get_count(); printing_node_idx++)
    {
        printing_node = ATC_printing_map.get_node(printing_node_idx);
        int print_Rlayer_idx = printing_node->Rlayer;
        int print_Blayer_idx = printing_node->Blayer;
        int print_region_idx = printing_node->region;

        //std::cout << "piece=" << printing_node_idx << " Rlayer=" << print_Rlayer_idx << " region=" << print_region_idx << std::endl;

        if (print_Blayer_idx != -1)
        {
            if (print_region_idx != prev_region_idx)
            {
                std::cout << "wipe tower here" << std::endl;
                atc_wipe_tower_idx += 1;
                atc_old_tool = prev_region_idx;
                atc_new_tool = print_region_idx;
                atc_print_z = atc_wiping_layer_height * atc_wipe_tower_idx;
                // printing_node->need_wipe = true;
                ATC_printing_map.get_node(printing_node_idx - 1)->need_wipe = true; // wipe before the tool change

                atc_wipe_tower.plan_toolchange(atc_print_z, atc_wiping_layer_height, atc_old_tool, atc_new_tool, atc_wiping_volume);
                //std::cout << "WTower: atc_print_z=" << atc_print_z << " atc_old_tool=" << atc_old_tool << " atc_new_tool=" << atc_new_tool << std::endl;
            }
            prev_region_idx = print_region_idx;
            //std::cout << "\n===================================\n\n\n" << std::endl;
            //std::cout << "Node=" << printing_node_idx << " Need wipe=" << printing_node->need_wipe << std::endl;
            //std::cout << "\n\n\n===================================\n" << std::endl;
        }
    }

    print.m_ATC_wipe_tower_data.tool_changes.reserve(atc_wipe_tower_idx + 1);
    atc_wipe_tower.generate(print.m_ATC_wipe_tower_data.tool_changes);
    std::cout << "\n\n\ncheck size = " << print.m_ATC_wipe_tower_data.tool_changes.size() << std::endl;



}

// wipe tower width (# of transition bricks) depends on # of colors
void GCode::ATC_plan_wipe_toolchange2(Print& print)
{
    std::cout << "\n\n\nvoid GCode::ATC_plan_wipe_toolchange()\n\n\n" << std::endl;
    // create atc_wipe_tower
    std::vector<float> wiping_matrix(cast<float>(print.m_config.wiping_volumes_matrix.values));
    std::vector<std::vector<float>> wipe_volumes;
    const unsigned int number_of_extruders = (unsigned int)(sqrt(wiping_matrix.size()) + EPSILON);
    for (unsigned int i = 0; i < number_of_extruders; ++i)
        wipe_volumes.push_back(std::vector<float>(wiping_matrix.begin() + i * number_of_extruders, wiping_matrix.begin() + (i + 1) * number_of_extruders));

    print.m_ATC_wipe_tower_data.tool_ordering = ToolOrdering(print, (unsigned int)-1, true);

    WipeTower atc_wipe_tower(print.m_config, wipe_volumes, print.m_ATC_wipe_tower_data.tool_ordering.first_extruder());
    for (size_t i = 0; i < number_of_extruders; ++i)
        atc_wipe_tower.set_extruder(i, print.m_config);

    // wiping parameters
    int atc_wipe_plan_total_bricks = print.get_object(0)->all_regions().size()-1; // max number of colors - 1
    int atc_wipe_plan_brick_idx = -1; // iterator over the bricks, starts with -1
    int atc_wipe_plan_layer = 1; // start building wipe tower with layer 1 (0.2 mm)

    //float atc_wiping_volume = print.m_config.option<ConfigOptionFloat>("atc_wiping_volume")->value; // hardcoded for now (140.0 default)
    float atc_wiping_volume = print.m_objects[0]->config().atc_wiping_volume;
    

    size_t atc_old_tool;
    size_t atc_new_tool;
    float atc_wiping_layer_height = 0.2; // hardcoded for now
    size_t atc_wipe_tower_idx = 0;
    float atc_print_z;

    // building wiping tool changes
    print.m_ATC_wipe_tower_data.clear();
    // iterate over the printing pieces
    struct printing_piece_UPD* printing_node;
    int prev_region_idx = 0;
    size_t atc_tool_change_counter = 0;
    for (int printing_node_idx = 0; printing_node_idx < ATC_printing_map.get_count(); printing_node_idx++)
    {
        printing_node = ATC_printing_map.get_node(printing_node_idx);
        int print_Rlayer_idx = printing_node->Rlayer;
        int print_Blayer_idx = printing_node->Blayer;
        int print_region_idx = printing_node->region;

        //std::cout << "piece=" << printing_node_idx << " Rlayer=" << print_Rlayer_idx << " region=" << print_region_idx << std::endl;

        if (print_Blayer_idx != -1)
        {
            if (print_region_idx != prev_region_idx)
            {
                //std::cout << "wipe tower here" << std::endl;
                atc_wipe_tower_idx += 1;
                atc_wipe_plan_brick_idx += 1;
                atc_old_tool = prev_region_idx;
                atc_new_tool = print_region_idx;
                //atc_print_z = atc_wiping_layer_height * atc_wipe_tower_idx;
                atc_print_z = atc_wiping_layer_height * atc_wipe_plan_layer;
                // printing_node->need_wipe = true;
                ATC_printing_map.get_node(printing_node_idx - 1)->need_wipe = true; // wipe before the tool change
                if (atc_wipe_plan_brick_idx >= atc_wipe_plan_total_bricks - 1)
                {
                    atc_wipe_plan_brick_idx = -1;
                    atc_wipe_plan_layer += 1;
                }

                atc_wipe_tower.plan_toolchange(atc_print_z, atc_wiping_layer_height, atc_old_tool, atc_new_tool, atc_wiping_volume);
                std::cout << "WTower: atc_print_z=" << atc_print_z << " atc_old_tool=" << atc_old_tool << " atc_new_tool=" << atc_new_tool << std::endl;
                atc_tool_change_counter += 1;
            }
            prev_region_idx = print_region_idx;
            //std::cout << "\n===================================\n\n\n" << std::endl;
            //std::cout << "Node=" << printing_node_idx << " Need wipe=" << printing_node->need_wipe << std::endl;
            //std::cout << "\n\n\n===================================\n" << std::endl;
        }
    }

    print.m_ATC_wipe_tower_data.tool_changes.reserve(atc_wipe_tower_idx + 1);
    atc_wipe_tower.generate(print.m_ATC_wipe_tower_data.tool_changes);
    std::cout << "\n\n\ncheck size = " << print.m_ATC_wipe_tower_data.tool_changes.size() << std::endl;
    std::cout << "\n\n# of TC = " << atc_wipe_tower_idx << std::endl;
    print.m_ATC_wipe_tower_data.number_of_toolchanges = atc_tool_change_counter;
    std::cout << "print.m_ATC_wipe_tower_data.number_of_toolchanges = " << atc_tool_change_counter << std::endl;
    std::cout << "\n\n\nEND of void GCode::ATC_plan_wipe_toolchange2()\n\n\n" << std::endl;
}







void GCode::atc_process_layers(Print& print, const ToolOrdering& tool_ordering, GCodeOutputStream& output_stream)
{
    bool ATC_soluble_supports = false;
    bool ATC_export_csv = true;

    this->ATC_export_CSV2(print); // export CSV
    
    // for non-soluble supports
    if (!ATC_soluble_supports) {
        this->layer_batch_labeling(print); // non-soluble supports
        this->ATC_plan_wipe_toolchange2(print); // non-soluble supports
    }

    // for soluble supports
    if (ATC_soluble_supports) {
        this->layer_batch_labeling_soluble_supports(print); // soluble supports
        this->ATC_plan_wipe_toolchange2_soluble_supports(print); // non-soluble supports
    }

    std::cout << "\n********** atc_process_layers ************" << std::endl; 
    std::cout << "********** FINAL MAP ************\n" << std::endl;
    //std::cout << "\FINAL MAP (" << this->ATC_printing_map.get_count() << "):" << std::endl;
    this->ATC_printing_map.display(this->ATC_printing_map.gethead());
    std::cout << "\n******** EOF FINAL MAP **********\n" << std::endl;

    // export CSV file
    if (ATC_export_csv) {
        std::ofstream myfile;
        myfile.open("FINAL_MAP_003.csv");
        //myfile << "-=0101=-\n";
        myfile << "No.,Layer,Region,Exist,X,Y,Tool,*R0,*R1,*R2,*R3,*R4,*R5,*R6,*R7,Area,Perimeter,\n";
        //myfile << "c,s,v,\n";
        struct printing_piece_UPD* ATC_CSV_printing_piece;
        for (int i = 0; i < this->ATC_printing_map.get_count(); i++)
        {
            ATC_CSV_printing_piece = this->ATC_printing_map.get_node(i);
            //int print_Rlayer_idx = obj_temp_piece->Rlayer;
            myfile
                << std::to_string(i) << ","
                << std::to_string(ATC_CSV_printing_piece->print_z) << ","
                << std::to_string(ATC_CSV_printing_piece->object) << ","
                << std::to_string(ATC_CSV_printing_piece->support) << ","
                << std::to_string(ATC_CSV_printing_piece->Rlayer) << ","
                << std::to_string(ATC_CSV_printing_piece->Blayer) << ","
                << std::to_string(ATC_CSV_printing_piece->region) << ","
                << std::to_string(ATC_CSV_printing_piece->batch) << ","
                << std::to_string(ATC_CSV_printing_piece->area) << ","
                << std::to_string(ATC_CSV_printing_piece->perimeter) << ","
                //<< std::to_string(ATC_CSV_printing_piece->shared_perimeter) << ","
                //<< std::to_string(ATC_CSV_printing_piece->intersection_self) << ","
                << "\n";
        }
        myfile.close();
    }
    /*
    if (ATC_export_csv) {
        std::ofstream myfile;
        myfile.open("FINAL_MAP_002.csv");
        myfile << "-=0101=-\n";
        myfile << "No.,Z height,Object,Support,RL,BL,R,Batch,Area,Perimeter,Shared perimeter,Intersection,\n";
        //myfile << "c,s,v,\n";
        struct printing_piece_UPD* ATC_CSV_printing_piece;
        for (int i = 0; i < this->ATC_printing_map.get_count(); i++)
        {
            ATC_CSV_printing_piece = this->ATC_printing_map.get_node(i);
            //int print_Rlayer_idx = obj_temp_piece->Rlayer;
            myfile
                << std::to_string(i) << ","
                << std::to_string(ATC_CSV_printing_piece->print_z) << ","
                << std::to_string(ATC_CSV_printing_piece->object) << ","
                << std::to_string(ATC_CSV_printing_piece->support) << ","
                << std::to_string(ATC_CSV_printing_piece->Rlayer) << ","
                << std::to_string(ATC_CSV_printing_piece->Blayer) << ","
                << std::to_string(ATC_CSV_printing_piece->region) << ","
                << std::to_string(ATC_CSV_printing_piece->batch) << ","
                << std::to_string(ATC_CSV_printing_piece->area) << ","
                << std::to_string(ATC_CSV_printing_piece->perimeter) << ","
                //<< std::to_string(ATC_CSV_printing_piece->shared_perimeter) << ","
                //<< std::to_string(ATC_CSV_printing_piece->intersection_self) << ","
                << "\n";
        }
        myfile.close();
    }
    */

    //std::cout << "--- GCode::atc_process_layers() ---" << std::endl;
    print.get_ATC_printing_map().display(print.get_ATC_printing_map().gethead());
    // get to the generated wipe tower
    print.m_ATC_wipe_tower_data.tool_changes[0][0];


    std::vector<GCode::LayerToPrint> layers_to_print = GCode::collect_layers_to_print(*print.m_objects[0]);
    struct printing_piece_UPD* printing_node;
    unsigned int atc_wiping_layer_idx = 0;
    int atc_wiping_brick_idx = -1; // for multiple bricks tower

    //for (size_t printing_node_idx = 0; printing_node_idx < print.m_ATC_printing_map.get_count(); printing_node_idx++)
    for (size_t printing_node_idx = 0; printing_node_idx < this->ATC_printing_map.get_count(); printing_node_idx++)
    {
        printing_node = this->ATC_printing_map.get_node(printing_node_idx);
        size_t print_layer_idx = printing_node->Rlayer;
        size_t print_region_idx = printing_node->region;
        unsigned int current_extruder_idx = print_region_idx;
        //std::cout << "~~~ NODE: print_layer_idx=" << print_layer_idx << ", print_region_idx=" << print_region_idx << ", current_extruder_idx=" << current_extruder_idx << std::endl;

        float atc_batch_number_in_the_cycle = printing_node->batch;
        float atc_region_intersection_in_the_cycle = printing_node->region_intersection;

        GCode::LayerResult my_atc_piece_result;
        GCode::LayerToPrint& layer_to_print = layers_to_print[print_layer_idx];


        {
            // ---!NB Input parameters
            // GCode::LayerToPrint& layer = layers_to_print[print_layer_idx]; - input param, so it is already here
            const LayerTools& layer_tools = tool_ordering.tools_for_layer(layer_to_print.print_z());
            const size_t single_object_instance_idx = 0;
            //const GCode::LayerToPrint& layer_to_print = layers_to_print[print_layer_idx];
            // ---!NB Input parameters


            const Layer* object_layer = nullptr;
            const SupportLayer* support_layer = nullptr;
            const SupportLayer* raft_layer = nullptr;

            if (layer_to_print.object_layer && !object_layer) object_layer = layer_to_print.object_layer;
            if (layer_to_print.support_layer) {
                if (!support_layer) support_layer = layer_to_print.support_layer;
                if (!raft_layer && support_layer->id() < support_layer->object()->slicing_parameters().raft_layers())
                    raft_layer = support_layer;
            }

            const Layer& layer = (object_layer != nullptr) ? *object_layer : *support_layer;
            //GCode::LayerResult result{ {}, layer.id(), false, &layers_to_print[print_layer_idx] == &layers_to_print.back() };

            coordf_t     print_z = layer.print_z;
            bool         first_layer = layer.id() == 0;

            this->m_config.apply(layer.object()->config(), true);
            // gcode->m_enable_loop_clipping = true; -- we probably dont need that
            //result.spiral_vase_enable = false;

            std::string gcode_string;
            gcode_string += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n"; // add tag for processor
            gcode_string += std::string(";Z:") + float_to_string_decimal_point(print_z) + "\n"; // export layer z
            float height = first_layer ? static_cast<float>(print_z) : static_cast<float>(print_z) - m_last_layer_z;
            gcode_string += std::string(";") + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + float_to_string_decimal_point(height) + "\n";
            m_last_layer_z = static_cast<float>(print_z);
            m_max_layer_z = std::max(m_max_layer_z, m_last_layer_z);
            m_last_height = height;

            if (!print.config().before_layer_gcode.value.empty()) {
                DynamicConfig config;
                config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index + 1));
                config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
                config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
                gcode_string += placeholder_parser_process("before_layer_gcode", print.config().before_layer_gcode.value, m_writer.extruder()->id(), &config) + "\n";
            }

            // what about (gcode->m_layer_index + 1) vs gcode->change_layer(print_z) ??
            gcode_string += change_layer(print_z); // this will increase m_layer_index
            m_layer = &layer;
            m_object_layer_over_raft = false;

            if (!print.config().layer_gcode.value.empty()) {
                DynamicConfig config;
                config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
                config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
                config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
                gcode_string += placeholder_parser_process("layer_gcode", print.config().layer_gcode.value, m_writer.extruder()->id(), &config) + "\n";
            }

            if (!first_layer && !m_second_layer_things_done) {
                // Transition from 1st to 2nd layer. Adjust nozzle temperatures as prescribed by the nozzle dependent first_layer_temperature vs temperature settings.
                for (const Extruder& extruder : m_writer.extruders()) {
                    if (print.config().single_extruder_multi_material.value && extruder.id() != m_writer.extruder()->id())
                        continue; // In single extruder multi material mode, set the temperature for the current extruder only.
                    int temperature = print.config().temperature.get_at(extruder.id());
                    if (temperature > 0 && temperature != print.config().first_layer_temperature.get_at(extruder.id()))
                        gcode_string += m_writer.set_temperature(temperature, false, extruder.id());
                }
                gcode_string += m_writer.set_bed_temperature(print.config().bed_temperature.get_at(current_extruder_idx));
                // Mark the temperature transition from 1st to 2nd layer to be finished.
                m_second_layer_things_done = true;
            }

            // Extrude skirt at the print_z of the raft layers and normal object layers not at the print_z of the interlaced support material layers.
            // Map from extruder ID to <begin, end> index of skirt loops to be extruded with that extruder.
            std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder;
            skirt_loops_per_extruder = Skirt::make_skirt_loops_per_extruder_1st_layer(print, layer_tools, m_skirt_done);

            // Group extrusions by an extruder, then by an object, an island and a region.
            std::map<unsigned int, std::vector<GCode::ObjectByExtruder>> by_extruder;
            bool is_anything_overridden = const_cast<LayerTools&>(layer_tools).wiping_extrusions().is_anything_overridden();


            if (layer_to_print.support_layer != nullptr) {
                const SupportLayer& support_layer = *layer_to_print.support_layer;
                const PrintObject& object = *support_layer.object();
                if (!support_layer.support_fills.entities.empty()) {
                    ExtrusionRole   role = support_layer.support_fills.role();
                    bool            has_support = role == erMixed || role == erSupportMaterial;
                    bool            has_interface = role == erMixed || role == erSupportMaterialInterface;
                    // Extruder ID of the support base. -1 if "don't care".
                    unsigned int    support_extruder = object.config().support_material_extruder.value - 1;
                    // Shall the support be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                    bool            support_dontcare = object.config().support_material_extruder.value == 0;
                    // Extruder ID of the support interface. -1 if "don't care".
                    unsigned int    interface_extruder = object.config().support_material_interface_extruder.value - 1;
                    // Shall the support interface be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                    bool            interface_dontcare = object.config().support_material_interface_extruder.value == 0;
                    if (support_dontcare || interface_dontcare) {
                        // Some support will be printed with "don't care" material, preferably non-soluble.
                        // Is the current extruder assigned a soluble filament?
                        unsigned int dontcare_extruder = current_extruder_idx;
                        if (print.config().filament_soluble.get_at(dontcare_extruder)) {
                            // The last extruder printed on the previous layer extrudes soluble filament.
                            // Try to find a non-soluble extruder on the same layer.

                            for (unsigned int extruder_id : layer_tools.extruders)
                                if (!print.config().filament_soluble.get_at(extruder_id)) {
                                    dontcare_extruder = extruder_id;
                                    break;
                                }

                            ;
                        }
                        if (support_dontcare)
                            support_extruder = dontcare_extruder;
                        if (interface_dontcare)
                            interface_extruder = dontcare_extruder;
                    }
                    //support_extruder = printing_node->region;
                    //interface_extruder = printing_node->region;
                    // Both the support and the support interface are printed with the same extruder, therefore
                    // the interface may be interleaved with the support base.
                    bool single_extruder = !has_support || support_extruder == interface_extruder;
                    // Assign an extruder to the base.
                    //GCode::ObjectByExtruder& obj = object_by_extruder(by_extruder, has_support ? support_extruder : interface_extruder, &layer_to_print - layers_to_print.data(), layers_to_print.size());
                    GCode::ObjectByExtruder& obj = object_by_extruder(by_extruder, has_support ? support_extruder : interface_extruder, 0, 1);

                    obj.support = &support_layer.support_fills;
                    obj.support_extrusion_role = single_extruder ? erMixed : erSupportMaterial;
                    if (!single_extruder && has_interface) {
                        //GCode::ObjectByExtruder& obj_interface = object_by_extruder(by_extruder, interface_extruder, &layer_to_print - layers_to_print.data(), layers_to_print.size());
                        GCode::ObjectByExtruder& obj_interface = object_by_extruder(by_extruder, interface_extruder, 0, 1);
                        obj_interface.support = &support_layer.support_fills;
                        obj_interface.support_extrusion_role = erSupportMaterialInterface;
                    }
                }
            }




            if (layer_to_print.object_layer != nullptr) {
                const Layer& layer = *layer_to_print.object_layer;
                size_t n_slices = layer.lslices.size();
                const std::vector<BoundingBox>& layer_surface_bboxes = layer.lslices_bboxes;
                // Traverse the slices in an increasing order of bounding box size, so that the islands inside another islands are tested first, so we can just test a point inside ExPolygon::contour and we may skip testing the holes.
                std::vector<size_t> slices_test_order;
                slices_test_order.reserve(n_slices);
                for (size_t i = 0; i < n_slices; ++i)
                    slices_test_order.emplace_back(i);
                std::sort(slices_test_order.begin(), slices_test_order.end(), [&layer_surface_bboxes](size_t i, size_t j) {const Vec2d s1 = layer_surface_bboxes[i].size().cast<double>(); const Vec2d s2 = layer_surface_bboxes[j].size().cast<double>(); return s1.x() * s1.y() < s2.x() * s2.y(); });
                auto point_inside_surface = [&layer, &layer_surface_bboxes](const size_t i, const Point& point) {const BoundingBox& bbox = layer_surface_bboxes[i]; return point(0) >= bbox.min(0) && point(0) < bbox.max(0) && point(1) >= bbox.min(1) && point(1) < bbox.max(1) && layer.lslices[i].contour.contains(point); };

                const LayerRegion* atc_layer_region = layer.regions()[print_region_idx];
                {
                    if (atc_layer_region == nullptr) continue;
                    const PrintRegion& atc_print_region = print.get_print_region(print_region_idx);
                    std::vector<unsigned int> printing_extruders;
                    for (const GCode::ObjectByExtruder::Island::Region::Type entity_type : {GCode::ObjectByExtruder::Island::Region::INFILL, GCode::ObjectByExtruder::Island::Region::PERIMETERS}) {
                        for (const ExtrusionEntity* ee : (entity_type == GCode::ObjectByExtruder::Island::Region::INFILL) ? atc_layer_region->fills.entities : atc_layer_region->perimeters.entities) {
                            assert(dynamic_cast<const ExtrusionEntityCollection*>(ee) != nullptr);
                            const auto* extrusions = static_cast<const ExtrusionEntityCollection*>(ee);
                            if (extrusions->entities.empty()) // This shouldn't happen but first_point() would fail.
                                continue;

                            const WipingExtrusions::ExtruderPerCopy* entity_overrides = nullptr;
                            printing_extruders.emplace_back(current_extruder_idx);

                            size_t atc_object_idx = 0;
                            std::vector<GCode::ObjectByExtruder::Island>& islands = object_islands_by_extruder(by_extruder, current_extruder_idx, atc_object_idx, 1, n_slices + 1);
                            for (size_t i = 0; i <= n_slices; ++i) {
                                bool   last = i == n_slices;
                                size_t island_idx = last ? n_slices : slices_test_order[i];
                                if (last || point_inside_surface(island_idx, extrusions->first_point())) { // extrusions->first_point does not fit inside any slice OR extrusions->first_point fits inside ith slice
                                    if (islands[island_idx].by_region.empty())
                                        islands[island_idx].by_region.assign(print.num_print_regions(), GCode::ObjectByExtruder::Island::Region());
                                    islands[island_idx].by_region[atc_print_region.print_region_id()].append(entity_type, extrusions, entity_overrides);
                                    break;
                                }
                            }
                        }
                    }
                } // for regions
            }
            // --- so now we have islands



            // Extrude the skirt, brim, support, perimeters, infill ordered by the extruders.
            // ---NB!----------------------------------------------------------------------------
            //gcode += (layer_tools.has_wipe_tower && gcode->m_wipe_tower) ? gcode->m_wipe_tower->tool_change(*this, extruder_id, extruder_id == layer_tools.extruders.back()) : this->set_extruder(extruder_id, print_z);
            //gcode_string += gcode->set_extruder(current_extruder_idx, print_z);


            if (auto loops_it = skirt_loops_per_extruder.find(current_extruder_idx); loops_it != skirt_loops_per_extruder.end()) {
                const std::pair<size_t, size_t> loops = loops_it->second;
                this->set_origin(0., 0.);
                m_avoid_crossing_perimeters.use_external_mp();
                Flow layer_skirt_flow = print.skirt_flow().with_height(float(m_skirt_done.back() - (m_skirt_done.size() == 1 ? 0. : m_skirt_done[m_skirt_done.size() - 2])));
                double mm3_per_mm = layer_skirt_flow.mm3_per_mm();
                for (size_t i = loops.first; i < loops.second; ++i) {
                    // Adjust flow according to this layer's layer height.
                    ExtrusionLoop loop = *dynamic_cast<const ExtrusionLoop*>(print.skirt().entities[i]);
                    for (ExtrusionPath& path : loop.paths) {
                        path.height = layer_skirt_flow.height();
                        path.mm3_per_mm = mm3_per_mm;
                    }
                    //FIXME using the support_material_speed of the 1st object printed.
                    gcode_string += this->extrude_loop(loop, "skirt"sv, m_config.support_material_speed.value);
                }
                m_avoid_crossing_perimeters.use_external_mp(false);
                // Allow a straight travel move to the first object point if this is the first layer (but don't in next layers).
                if (first_layer && loops.first == 0)
                    m_avoid_crossing_perimeters.disable_once();
            }

            // Extrude brim with the extruder of the 1st region.
            if (!m_brim_done) {
                this->set_origin(0., 0.);
                m_avoid_crossing_perimeters.use_external_mp();
                for (const ExtrusionEntity* ee : print.brim().entities) {
                    gcode_string += this->extrude_entity(*ee, "brim"sv, m_config.support_material_speed.value);
                }
                m_brim_done = true;
                m_avoid_crossing_perimeters.use_external_mp(false);
                // Allow a straight travel move to the first object point.
                m_avoid_crossing_perimeters.disable_once();
            }


            // =================== SUBSTITUTE for gcode->set_extruder(...) ====================
            //std::string GCode::set_extruder(unsigned int extruder_id, double print_z)
            //if (!m_writer.need_toolchange(current_extruder_idx))
                //std::cout << "return \"\";" << std::endl;

            // if we are running a single-extruder setup, just set the extruder and return nothing
            if (!m_writer.multiple_extruders) {
                m_placeholder_parser.set("current_extruder", current_extruder_idx);

                // Append the filament start G-code.
                const std::string& start_filament_gcode = print.m_config.start_filament_gcode.get_at(current_extruder_idx);
                if (!start_filament_gcode.empty()) {
                    // Process the start_filament_gcode for the filament.
                    DynamicConfig config;
                    config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(current_extruder_idx)));
                    gcode_string += placeholder_parser_process("start_filament_gcode", start_filament_gcode, current_extruder_idx, &config);
                    check_add_eol(gcode_string);
                }
                gcode_string += m_writer.toolchange(current_extruder_idx);
            }

            // prepend retraction on the current extruder
            //std::string gcode_string = gcode->retract(true);
            gcode_string += retract(true);

            // Always reset the extrusion path, even if the tool change retract is set to zero.
            m_wipe.reset_path();

            if (m_writer.extruder() != nullptr) {
                // Process the custom end_filament_gcode. set_extruder() is only called if there is no wipe tower
                // so it should not be injected twice.
                unsigned int        old_extruder_id = m_writer.extruder()->id();
                const std::string& end_filament_gcode = m_config.end_filament_gcode.get_at(old_extruder_id);
                if (!end_filament_gcode.empty()) {
                    gcode_string += placeholder_parser_process("end_filament_gcode", end_filament_gcode, old_extruder_id);
                    check_add_eol(gcode_string);
                }
            }


            // If ooze prevention is enabled, park current extruder in the nearest
            // standby point and set it to the standby temperature.
            if (m_ooze_prevention.enable && m_writer.extruder() != nullptr)
                gcode_string += m_ooze_prevention.pre_toolchange(*this);

            const std::string& toolchange_gcode = m_config.toolchange_gcode.value;
            std::string toolchange_gcode_parsed;

            // Process the custom toolchange_gcode. If it is empty, insert just a Tn command.
            if (!toolchange_gcode.empty()) {
                DynamicConfig config;
                config.set_key_value("previous_extruder", new ConfigOptionInt((int)(m_writer.extruder() != nullptr ? m_writer.extruder()->id() : -1)));
                config.set_key_value("next_extruder", new ConfigOptionInt((int)current_extruder_idx));
                config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
                config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
                config.set_key_value("toolchange_z", new ConfigOptionFloat(print_z));
                config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
                toolchange_gcode_parsed = placeholder_parser_process("toolchange_gcode", toolchange_gcode, current_extruder_idx, &config);
                gcode_string += toolchange_gcode_parsed;
                check_add_eol(gcode_string);
            }

            // We inform the writer about what is happening, but we may not use the resulting gcode.
            std::string toolchange_command = m_writer.toolchange(current_extruder_idx);
            if (!custom_gcode_changes_tool(toolchange_gcode_parsed, m_writer.toolchange_prefix(), current_extruder_idx))
                gcode_string += toolchange_command;
            else {
                // user provided his own toolchange gcode, no need to do anything
            }

            // Set the temperature if the wipe tower didn't (not needed for non-single extruder MM)
            if (m_config.single_extruder_multi_material && !m_config.wipe_tower) {
                int temp = (m_layer_index <= 0 ? m_config.first_layer_temperature.get_at(current_extruder_idx) :
                    m_config.temperature.get_at(current_extruder_idx));

                gcode_string += m_writer.set_temperature(temp, false);
            }

            m_placeholder_parser.set("current_extruder", current_extruder_idx);

            // Append the filament start G-code.
            const std::string& start_filament_gcode = m_config.start_filament_gcode.get_at(current_extruder_idx);
            if (!start_filament_gcode.empty()) {
                // Process the start_filament_gcode for the new filament.
                DynamicConfig config;
                config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(current_extruder_idx)));
                gcode_string += placeholder_parser_process("start_filament_gcode", start_filament_gcode, current_extruder_idx, &config);
                check_add_eol(gcode_string);
            }
            // Set the new extruder to the operating temperature.
            if (m_ooze_prevention.enable)
                gcode_string += m_ooze_prevention.post_toolchange(*this);



            // =================== END of SUBSTITUTE for gcode->set_extruder(...) =============


        // ---END NB!----------------------------------------------------------------------------
        //gfile.write(gcode->m_wipe_tower->append_tcr(*gcode, gcode->m_wipe_tower->m_tool_changes[0][0], 1, 0.2));

        // let analyzer tag generator aware of a role type change
            if (layer_tools.has_wipe_tower && m_wipe_tower)
                m_last_processor_extrusion_role = erWipeTower;


            auto objects_by_extruder_it = by_extruder.find(current_extruder_idx);
            if (objects_by_extruder_it == by_extruder.end()) continue;

            const std::vector<const PrintInstance*>* ordering = nullptr;
            std::vector<GCode::InstanceToPrint> instances_to_print = sort_print_object_instances(objects_by_extruder_it->second, { layer_to_print }, ordering, single_object_instance_idx);

            // We are almost ready to print. However, we must go through all the objects twice to print the the overridden extrusions first (infill/perimeter wiping feature):
            std::vector<GCode::ObjectByExtruder::Island::Region> by_region_per_copy_cache;
            for (int print_wipe_extrusions = is_anything_overridden; print_wipe_extrusions >= 0; --print_wipe_extrusions) {
                if (is_anything_overridden && print_wipe_extrusions == 0)
                    gcode_string += "; PURGING FINISHED\n";

                for (GCode::InstanceToPrint& instance_to_print : instances_to_print) {
                    //const GCode::LayerToPrint& layer_to_print = layers[instance_to_print.layer_id];
                    // To control print speed of the 1st object layer printed over raft interface.
                    bool object_layer_over_raft = layer_to_print.object_layer && layer_to_print.object_layer->id() > 0 && instance_to_print.print_object.slicing_parameters().raft_layers() == layer_to_print.object_layer->id();
                    m_config.apply(instance_to_print.print_object.config(), true);
                    m_layer = layer_to_print.layer();
                    m_object_layer_over_raft = object_layer_over_raft;
                    if (m_config.avoid_crossing_perimeters)
                        m_avoid_crossing_perimeters.init_layer(*m_layer);
                    if (config().gcode_label_objects)
                        gcode_string += std::string("; printing object ") + instance_to_print.print_object.model_object()->name + " id:" + std::to_string(instance_to_print.layer_id) + " copy " + std::to_string(instance_to_print.instance_id) + "\n";
                    // When starting a new object, use the external motion planner for the first travel move.
                    const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                    std::pair<const PrintObject*, Point>  this_object_copy(&instance_to_print.print_object, offset);
                    if (m_last_obj_copy != this_object_copy)
                        m_avoid_crossing_perimeters.use_external_mp_once();
                    m_last_obj_copy = this_object_copy;
                    set_origin(unscale(offset));
                    if (instance_to_print.object_by_extruder.support != nullptr && !print_wipe_extrusions) {
                        m_layer = layer_to_print.support_layer;
                        m_object_layer_over_raft = false;
                        // support_extrusion_role is erSupportMaterial, erSupportMaterialInterface or erMixed for all extrusion paths.
                        gcode_string += extrude_support(instance_to_print.object_by_extruder.support->chained_path_from(m_last_pos, instance_to_print.object_by_extruder.support_extrusion_role));
                        std::cout << "~~~ EXTRUDE SUPPORT" << std::endl;
                        m_layer = layer_to_print.layer();
                        m_object_layer_over_raft = object_layer_over_raft;
                    }
                    for (GCode::ObjectByExtruder::Island& island : instance_to_print.object_by_extruder.islands) {
                        const auto& by_region_specific = is_anything_overridden ? island.by_region_per_copy(by_region_per_copy_cache, static_cast<unsigned int>(instance_to_print.instance_id), current_extruder_idx, print_wipe_extrusions != 0) : island.by_region;
                        //gcode_string += extrude_infill(print, by_region_specific, false);
                        //gcode_string += extrude_perimeters(print, by_region_specific);

                        if (print.config().infill_first) {
                            gcode_string += extrude_infill(print, by_region_specific, false);
                            gcode_string += extrude_perimeters(print, by_region_specific);
                        }
                        else {
                            gcode_string += extrude_perimeters(print, by_region_specific);
                            gcode_string += extrude_infill(print, by_region_specific, false);
                        }


                        //std::cout << "~~~ EXTRUDE INFILL AND PERIMETERS" << std::endl;
                    }
                }
            }



            BOOST_LOG_TRIVIAL(trace) << "Exported layer " << layer.id() << " print_z " << print_z << log_memory_info();

            my_atc_piece_result.gcode = std::move(gcode_string);
            const bool last_layer = 0;
            //last_layer = &layer == &layers_to_print.back()
            my_atc_piece_result.cooling_buffer_flush = object_layer || raft_layer || last_layer;
            //return result;
        }

        m_processor.m_atc_critical_intersection = atc_region_intersection_in_the_cycle;
        
        m_processor.m_atc_batch_number = atc_batch_number_in_the_cycle;
        output_stream.write(my_atc_piece_result.gcode); // gcode for a single color piece

        // for a multiple-brick wipe tower
        if (printing_node->need_wipe)
        {
            atc_wiping_brick_idx += 1;
            //if (atc_wiping_brick_idx > print.atc_wipe_tower_data.tool_changes[atc_wiping_layer_idx].size()) break;
            output_stream.write(m_wipe_tower->append_tcr(*this,
                print.m_ATC_wipe_tower_data.tool_changes[atc_wiping_layer_idx][atc_wiping_brick_idx],
                print.m_ATC_wipe_tower_data.tool_changes[atc_wiping_layer_idx][atc_wiping_brick_idx].new_tool,
                print.m_ATC_wipe_tower_data.tool_changes[atc_wiping_layer_idx][atc_wiping_brick_idx].print_z));


            // # of bricks = max number of colors - 1
            if (atc_wiping_brick_idx >= print.get_object(0)->all_regions().size() - 2) // # of bricks - 1 = max number of colors - 2
            {
                atc_wiping_layer_idx += 1;
                atc_wiping_brick_idx = -1;
            }

        }

        // for a single-brick wipe tower
        /*
        if (printing_node->need_wipe)
        {
            output_stream.write(m_wipe_tower->append_tcr(*this,
                print.m_ATC_wipe_tower_data.tool_changes[atc_wiping_layer_idx][0],
                print.m_ATC_wipe_tower_data.tool_changes[atc_wiping_layer_idx][0].new_tool,
                print.m_ATC_wipe_tower_data.tool_changes[atc_wiping_layer_idx][0].print_z));
            atc_wiping_layer_idx += 1;
        }
        */
    }
}







// In sequential mode, process_layer is called once per each object and its copy,
// therefore layers will contain a single entry and single_object_instance_idx will point to the copy of the object.
// In non-sequential mode, process_layer is called per each print_z height with all object and support layers accumulated.
// For multi-material prints, this routine minimizes extruder switches by gathering extruder specific extrusion paths
// and performing the extruder specific extrusions together.
GCode::LayerResult GCode::process_layer(
    const Print                    			&print,
    // Set of object & print layers of the same PrintObject and with the same print_z.
    const std::vector<LayerToPrint> 		&layers,
    const LayerTools        		        &layer_tools,
    const bool                               last_layer,
    // Pairs of PrintObject index and its instance index.
    const std::vector<const PrintInstance*> *ordering,
    // If set to size_t(-1), then print all copies of all objects.
    // Otherwise print a single copy of a single object.
    const size_t                     		 single_object_instance_idx)
{
    assert(! layers.empty());
    // Either printing all copies of all objects, or just a single copy of a single object.
    assert(single_object_instance_idx == size_t(-1) || layers.size() == 1);

    // First object, support and raft layer, if available.
    const Layer         *object_layer  = nullptr;
    const SupportLayer  *support_layer = nullptr;
    const SupportLayer  *raft_layer    = nullptr;
    for (const LayerToPrint &l : layers) {
        if (l.object_layer && ! object_layer)
            object_layer = l.object_layer;
        if (l.support_layer) {
            if (! support_layer)
                support_layer = l.support_layer;
            if (! raft_layer && support_layer->id() < support_layer->object()->slicing_parameters().raft_layers())
                raft_layer = support_layer;
        }
    }
    const Layer         &layer         = (object_layer != nullptr) ? *object_layer : *support_layer;
    GCode::LayerResult   result { {}, layer.id(), false, last_layer };
    if (layer_tools.extruders.empty())
        // Nothing to extrude.
        return result;

    // Extract 1st object_layer and support_layer of this set of layers with an equal print_z.
    coordf_t             print_z       = layer.print_z;
    bool                 first_layer   = layer.id() == 0;
    unsigned int         first_extruder_id = layer_tools.extruders.front();

    // Initialize config with the 1st object to be printed at this layer.
    m_config.apply(layer.object()->config(), true);

    // Check whether it is possible to apply the spiral vase logic for this layer.
    // Just a reminder: A spiral vase mode is allowed for a single object, single material print only.
    m_enable_loop_clipping = true;
    if (m_spiral_vase && layers.size() == 1 && support_layer == nullptr) {
        bool enable = (layer.id() > 0 || !print.has_brim()) && (layer.id() >= (size_t)print.config().skirt_height.value && ! print.has_infinite_skirt());
        if (enable) {
            for (const LayerRegion *layer_region : layer.regions())
                if (size_t(layer_region->region().config().bottom_solid_layers.value) > layer.id() ||
                    layer_region->perimeters.items_count() > 1u ||
                    layer_region->fills.items_count() > 0) {
                    enable = false;
                    break;
                }
        }
        result.spiral_vase_enable = enable;
        // If we're going to apply spiralvase to this layer, disable loop clipping.
        m_enable_loop_clipping = !enable;
    }

    std::string gcode;
    assert(is_decimal_separator_point()); // for the sprintfs

    // add tag for processor
    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    // export layer z
    gcode += std::string(";Z:") + float_to_string_decimal_point(print_z) + "\n";

    // export layer height
    float height = first_layer ? static_cast<float>(print_z) : static_cast<float>(print_z) - m_last_layer_z;
    gcode += std::string(";") + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height)
        + float_to_string_decimal_point(height) + "\n";

    // update caches
    m_last_layer_z = static_cast<float>(print_z);
    m_max_layer_z  = std::max(m_max_layer_z, m_last_layer_z);
    m_last_height = height;

    // Set new layer - this will change Z and force a retraction if retract_layer_change is enabled.
    if (! print.config().before_layer_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num",   new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z",     new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("before_layer_gcode",
            print.config().before_layer_gcode.value, m_writer.extruder()->id(), &config)
            + "\n";
    }
    gcode += this->change_layer(print_z);  // this will increase m_layer_index
    m_layer = &layer;
    m_object_layer_over_raft = false;
    if (! print.config().layer_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z",   new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("layer_gcode",
            print.config().layer_gcode.value, m_writer.extruder()->id(), &config)
            + "\n";
    }

    if (! first_layer && ! m_second_layer_things_done) {
        // Transition from 1st to 2nd layer. Adjust nozzle temperatures as prescribed by the nozzle dependent
        // first_layer_temperature vs. temperature settings.
        for (const Extruder &extruder : m_writer.extruders()) {
            if (print.config().single_extruder_multi_material.value && extruder.id() != m_writer.extruder()->id())
                // In single extruder multi material mode, set the temperature for the current extruder only.
                continue;
            int temperature = print.config().temperature.get_at(extruder.id());
            if (temperature > 0 && temperature != print.config().first_layer_temperature.get_at(extruder.id()))
                gcode += m_writer.set_temperature(temperature, false, extruder.id());
        }
        gcode += m_writer.set_bed_temperature(print.config().bed_temperature.get_at(first_extruder_id));
        // Mark the temperature transition from 1st to 2nd layer to be finished.
        m_second_layer_things_done = true;
    }

    // Map from extruder ID to <begin, end> index of skirt loops to be extruded with that extruder.
    std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder;

    if (single_object_instance_idx == size_t(-1)) {
        // Normal (non-sequential) print.
        gcode += ProcessLayer::emit_custom_gcode_per_print_z(*this, layer_tools.custom_gcode, m_writer.extruder()->id(), first_extruder_id, print.config());
    }
    // Extrude skirt at the print_z of the raft layers and normal object layers
    // not at the print_z of the interlaced support material layers.
    skirt_loops_per_extruder = first_layer ?
        Skirt::make_skirt_loops_per_extruder_1st_layer(print, layer_tools, m_skirt_done) :
        Skirt::make_skirt_loops_per_extruder_other_layers(print, layer_tools, m_skirt_done);

    // Group extrusions by an extruder, then by an object, an island and a region.
    std::map<unsigned int, std::vector<ObjectByExtruder>> by_extruder;
    bool is_anything_overridden = const_cast<LayerTools&>(layer_tools).wiping_extrusions().is_anything_overridden();
    for (const LayerToPrint &layer_to_print : layers) {
        if (layer_to_print.support_layer != nullptr) {
            const SupportLayer &support_layer = *layer_to_print.support_layer;
            const PrintObject  &object = *support_layer.object();
            if (! support_layer.support_fills.entities.empty()) {
                ExtrusionRole   role               = support_layer.support_fills.role();
                bool            has_support        = role == erMixed || role == erSupportMaterial;
                bool            has_interface      = role == erMixed || role == erSupportMaterialInterface;
                // Extruder ID of the support base. -1 if "don't care".
                unsigned int    support_extruder   = object.config().support_material_extruder.value - 1;
                // Shall the support be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                bool            support_dontcare   = object.config().support_material_extruder.value == 0;
                // Extruder ID of the support interface. -1 if "don't care".
                unsigned int    interface_extruder = object.config().support_material_interface_extruder.value - 1;
                // Shall the support interface be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                bool            interface_dontcare = object.config().support_material_interface_extruder.value == 0;
                if (support_dontcare || interface_dontcare) {
                    // Some support will be printed with "don't care" material, preferably non-soluble.
                    // Is the current extruder assigned a soluble filament?
                    unsigned int dontcare_extruder = first_extruder_id;
                    if (print.config().filament_soluble.get_at(dontcare_extruder)) {
                        // The last extruder printed on the previous layer extrudes soluble filament.
                        // Try to find a non-soluble extruder on the same layer.
                        for (unsigned int extruder_id : layer_tools.extruders)
                            if (! print.config().filament_soluble.get_at(extruder_id)) {
                                dontcare_extruder = extruder_id;
                                break;
                            }
                    }
                    if (support_dontcare)
                        support_extruder = dontcare_extruder;
                    if (interface_dontcare)
                        interface_extruder = dontcare_extruder;
                }
                // Both the support and the support interface are printed with the same extruder, therefore
                // the interface may be interleaved with the support base.
                bool single_extruder = ! has_support || support_extruder == interface_extruder;
                // Assign an extruder to the base.
                ObjectByExtruder &obj = object_by_extruder(by_extruder, has_support ? support_extruder : interface_extruder, &layer_to_print - layers.data(), layers.size());
                obj.support = &support_layer.support_fills;
                obj.support_extrusion_role = single_extruder ? erMixed : erSupportMaterial;
                if (! single_extruder && has_interface) {
                    ObjectByExtruder &obj_interface = object_by_extruder(by_extruder, interface_extruder, &layer_to_print - layers.data(), layers.size());
                    obj_interface.support = &support_layer.support_fills;
                    obj_interface.support_extrusion_role = erSupportMaterialInterface;
                }
            }
        }
        if (layer_to_print.object_layer != nullptr) {
            const Layer &layer = *layer_to_print.object_layer;
            // We now define a strategy for building perimeters and fills. The separation
            // between regions doesn't matter in terms of printing order, as we follow
            // another logic instead:
            // - we group all extrusions by extruder so that we minimize toolchanges
            // - we start from the last used extruder
            // - for each extruder, we group extrusions by island
            // - for each island, we extrude perimeters first, unless user set the infill_first
            //   option
            // (Still, we have to keep track of regions because we need to apply their config)
            size_t n_slices = layer.lslices.size();
            const std::vector<BoundingBox> &layer_surface_bboxes = layer.lslices_bboxes;
            // Traverse the slices in an increasing order of bounding box size, so that the islands inside another islands are tested first,
            // so we can just test a point inside ExPolygon::contour and we may skip testing the holes.
            std::vector<size_t> slices_test_order;
            slices_test_order.reserve(n_slices);
            for (size_t i = 0; i < n_slices; ++ i)
                slices_test_order.emplace_back(i);
            std::sort(slices_test_order.begin(), slices_test_order.end(), [&layer_surface_bboxes](size_t i, size_t j) {
                const Vec2d s1 = layer_surface_bboxes[i].size().cast<double>();
                const Vec2d s2 = layer_surface_bboxes[j].size().cast<double>();
                return s1.x() * s1.y() < s2.x() * s2.y();
            });
            auto point_inside_surface = [&layer, &layer_surface_bboxes](const size_t i, const Point &point) {
                const BoundingBox &bbox = layer_surface_bboxes[i];
                return point(0) >= bbox.min(0) && point(0) < bbox.max(0) &&
                       point(1) >= bbox.min(1) && point(1) < bbox.max(1) &&
                       layer.lslices[i].contour.contains(point);
            };

            for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id) {
                const LayerRegion *layerm = layer.regions()[region_id];
                if (layerm == nullptr)
                    continue;
                // PrintObjects own the PrintRegions, thus the pointer to PrintRegion would be unique to a PrintObject, they would not
                // identify the content of PrintRegion accross the whole print uniquely. Translate to a Print specific PrintRegion.
                const PrintRegion &region = print.get_print_region(layerm->region().print_region_id());

                // Now we must process perimeters and infills and create islands of extrusions in by_region std::map.
                // It is also necessary to save which extrusions are part of MM wiping and which are not.
                // The process is almost the same for perimeters and infills - we will do it in a cycle that repeats twice:
                std::vector<unsigned int> printing_extruders;
                for (const ObjectByExtruder::Island::Region::Type entity_type : { ObjectByExtruder::Island::Region::INFILL, ObjectByExtruder::Island::Region::PERIMETERS }) {
                    for (const ExtrusionEntity *ee : (entity_type == ObjectByExtruder::Island::Region::INFILL) ? layerm->fills.entities : layerm->perimeters.entities) {
                        // extrusions represents infill or perimeter extrusions of a single island.
                        assert(dynamic_cast<const ExtrusionEntityCollection*>(ee) != nullptr);
                        const auto *extrusions = static_cast<const ExtrusionEntityCollection*>(ee);
                        if (extrusions->entities.empty()) // This shouldn't happen but first_point() would fail.
                            continue;

                        // This extrusion is part of certain Region, which tells us which extruder should be used for it:
                        int correct_extruder_id = layer_tools.extruder(*extrusions, region);

                        // Let's recover vector of extruder overrides:
                        const WipingExtrusions::ExtruderPerCopy *entity_overrides = nullptr;
                        if (! layer_tools.has_extruder(correct_extruder_id)) {
                            // this entity is not overridden, but its extruder is not in layer_tools - we'll print it
                            // by last extruder on this layer (could happen e.g. when a wiping object is taller than others - dontcare extruders are eradicated from layer_tools)
                            correct_extruder_id = layer_tools.extruders.back();
                        }
                        printing_extruders.clear();
                        if (is_anything_overridden) {
                            entity_overrides = const_cast<LayerTools&>(layer_tools).wiping_extrusions().get_extruder_overrides(extrusions, correct_extruder_id, layer_to_print.object()->instances().size());
                            if (entity_overrides == nullptr) {
                                printing_extruders.emplace_back(correct_extruder_id);
                            } else {
                                printing_extruders.reserve(entity_overrides->size());
                                for (int extruder : *entity_overrides)
                                    printing_extruders.emplace_back(extruder >= 0 ?
                                        // at least one copy is overridden to use this extruder
                                        extruder :
                                        // at least one copy would normally be printed with this extruder (see get_extruder_overrides function for explanation)
                                        static_cast<unsigned int>(- extruder - 1));
                                Slic3r::sort_remove_duplicates(printing_extruders);
                            }
                        } else
                            printing_extruders.emplace_back(correct_extruder_id);

                        // Now we must add this extrusion into the by_extruder map, once for each extruder that will print it:
                        for (unsigned int extruder : printing_extruders)
                        {
                            std::vector<ObjectByExtruder::Island> &islands = object_islands_by_extruder(
                                by_extruder,
                                extruder,
                                &layer_to_print - layers.data(),
                                layers.size(), n_slices+1);
                            for (size_t i = 0; i <= n_slices; ++ i) {
                                bool   last = i == n_slices;
                                size_t island_idx = last ? n_slices : slices_test_order[i];
                                if (// extrusions->first_point does not fit inside any slice
                                    last ||
                                    // extrusions->first_point fits inside ith slice
                                    point_inside_surface(island_idx, extrusions->first_point())) {
                                    if (islands[island_idx].by_region.empty())
                                        islands[island_idx].by_region.assign(print.num_print_regions(), ObjectByExtruder::Island::Region());
                                    islands[island_idx].by_region[region.print_region_id()].append(entity_type, extrusions, entity_overrides);
                                    break;
                                }
                            }
                        }
                    }
                }
            } // for regions
        }
    } // for objects

    // Extrude the skirt, brim, support, perimeters, infill ordered by the extruders.
    for (unsigned int extruder_id : layer_tools.extruders)
    {
        gcode += (layer_tools.has_wipe_tower && m_wipe_tower) ? m_wipe_tower->tool_change(*this, extruder_id, extruder_id == layer_tools.extruders.back()) :  this->set_extruder(extruder_id, print_z);

        // let analyzer tag generator aware of a role type change
        if (layer_tools.has_wipe_tower && m_wipe_tower)
            m_last_processor_extrusion_role = erWipeTower;

        if (auto loops_it = skirt_loops_per_extruder.find(extruder_id); loops_it != skirt_loops_per_extruder.end()) {
            const std::pair<size_t, size_t> loops = loops_it->second;
            this->set_origin(0., 0.);
            m_avoid_crossing_perimeters.use_external_mp();
            Flow layer_skirt_flow = print.skirt_flow().with_height(float(m_skirt_done.back() - (m_skirt_done.size() == 1 ? 0. : m_skirt_done[m_skirt_done.size() - 2])));
            double mm3_per_mm = layer_skirt_flow.mm3_per_mm();
            for (size_t i = loops.first; i < loops.second; ++i) {
                // Adjust flow according to this layer's layer height.
                ExtrusionLoop loop = *dynamic_cast<const ExtrusionLoop*>(print.skirt().entities[i]);
                for (ExtrusionPath &path : loop.paths) {
                    path.height = layer_skirt_flow.height();
                    path.mm3_per_mm = mm3_per_mm;
                }
                //FIXME using the support_material_speed of the 1st object printed.
                gcode += this->extrude_loop(loop, "skirt"sv, m_config.support_material_speed.value);
            }
            m_avoid_crossing_perimeters.use_external_mp(false);
            // Allow a straight travel move to the first object point if this is the first layer (but don't in next layers).
            if (first_layer && loops.first == 0)
                m_avoid_crossing_perimeters.disable_once();
        }

        // Extrude brim with the extruder of the 1st region.
        if (! m_brim_done) {
            this->set_origin(0., 0.);
            m_avoid_crossing_perimeters.use_external_mp();
            for (const ExtrusionEntity *ee : print.brim().entities) {
                gcode += this->extrude_entity(*ee, "brim"sv, m_config.support_material_speed.value);
            }
            m_brim_done = true;
            m_avoid_crossing_perimeters.use_external_mp(false);
            // Allow a straight travel move to the first object point.
            m_avoid_crossing_perimeters.disable_once();
        }


        auto objects_by_extruder_it = by_extruder.find(extruder_id);
        if (objects_by_extruder_it == by_extruder.end())
            continue;

        std::vector<InstanceToPrint> instances_to_print = sort_print_object_instances(objects_by_extruder_it->second, layers, ordering, single_object_instance_idx);

        // We are almost ready to print. However, we must go through all the objects twice to print the the overridden extrusions first (infill/perimeter wiping feature):
        std::vector<ObjectByExtruder::Island::Region> by_region_per_copy_cache;
        for (int print_wipe_extrusions = is_anything_overridden; print_wipe_extrusions>=0; --print_wipe_extrusions) {
            if (is_anything_overridden && print_wipe_extrusions == 0)
                gcode+="; PURGING FINISHED\n";

            for (InstanceToPrint &instance_to_print : instances_to_print) {
                const LayerToPrint &layer_to_print = layers[instance_to_print.layer_id];
                // To control print speed of the 1st object layer printed over raft interface.
                bool object_layer_over_raft = layer_to_print.object_layer && layer_to_print.object_layer->id() > 0 && 
                    instance_to_print.print_object.slicing_parameters().raft_layers() == layer_to_print.object_layer->id();
                m_config.apply(instance_to_print.print_object.config(), true);
                m_layer = layer_to_print.layer();
                m_object_layer_over_raft = object_layer_over_raft;
                if (m_config.avoid_crossing_perimeters)
                    m_avoid_crossing_perimeters.init_layer(*m_layer);
                if (this->config().gcode_label_objects)
                    gcode += std::string("; printing object ") + instance_to_print.print_object.model_object()->name + " id:" + std::to_string(instance_to_print.layer_id) + " copy " + std::to_string(instance_to_print.instance_id) + "\n";
                // When starting a new object, use the external motion planner for the first travel move.
                const Point &offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                if (m_last_obj_copy != this_object_copy)
                    m_avoid_crossing_perimeters.use_external_mp_once();
                m_last_obj_copy = this_object_copy;
                this->set_origin(unscale(offset));
                if (instance_to_print.object_by_extruder.support != nullptr && !print_wipe_extrusions) {
                    m_layer = layer_to_print.support_layer;
                    m_object_layer_over_raft = false;
                    gcode += this->extrude_support(
                        // support_extrusion_role is erSupportMaterial, erSupportMaterialInterface or erMixed for all extrusion paths.
                        instance_to_print.object_by_extruder.support->chained_path_from(m_last_pos, instance_to_print.object_by_extruder.support_extrusion_role));
                    m_layer = layer_to_print.layer();
                    m_object_layer_over_raft = object_layer_over_raft;
                }
                //FIXME order islands?
                // Sequential tool path ordering of multiple parts within the same object, aka. perimeter tracking (#5511)
                for (ObjectByExtruder::Island &island : instance_to_print.object_by_extruder.islands) {
                    const auto& by_region_specific = is_anything_overridden ? island.by_region_per_copy(by_region_per_copy_cache, static_cast<unsigned int>(instance_to_print.instance_id), extruder_id, print_wipe_extrusions != 0) : island.by_region;
                    //FIXME the following code prints regions in the order they are defined, the path is not optimized in any way.
                    if (print.config().infill_first) {
                        gcode += this->extrude_infill(print, by_region_specific, false);
                        gcode += this->extrude_perimeters(print, by_region_specific);
                    } else {
                        gcode += this->extrude_perimeters(print, by_region_specific);
                        gcode += this->extrude_infill(print,by_region_specific, false);
                    }
                    // ironing
                    gcode += this->extrude_infill(print,by_region_specific, true);
                }
                if (this->config().gcode_label_objects)
                    gcode += std::string("; stop printing object ") + instance_to_print.print_object.model_object()->name + " id:" + std::to_string(instance_to_print.layer_id) + " copy " + std::to_string(instance_to_print.instance_id) + "\n";
            }
        }
    }

#if 0
    // Apply spiral vase post-processing if this layer contains suitable geometry
    // (we must feed all the G-code into the post-processor, including the first
    // bottom non-spiral layers otherwise it will mess with positions)
    // we apply spiral vase at this stage because it requires a full layer.
    // Just a reminder: A spiral vase mode is allowed for a single object per layer, single material print only.
    if (m_spiral_vase)
        gcode = m_spiral_vase->process_layer(std::move(gcode));

    // Apply cooling logic; this may alter speeds.
    if (m_cooling_buffer)
        gcode = m_cooling_buffer->process_layer(std::move(gcode), layer.id(),
            // Flush the cooling buffer at each object layer or possibly at the last layer, even if it contains just supports (This should not happen).
            object_layer || last_layer);

#ifdef HAS_PRESSURE_EQUALIZER
    // Apply pressure equalization if enabled;
    // printf("G-code before filter:\n%s\n", gcode.c_str());
    if (m_pressure_equalizer)
        gcode = m_pressure_equalizer->process(gcode.c_str(), false);
    // printf("G-code after filter:\n%s\n", out.c_str());
#endif /* HAS_PRESSURE_EQUALIZER */

    file.write(gcode);
#endif

    BOOST_LOG_TRIVIAL(trace) << "Exported layer " << layer.id() << " print_z " << print_z <<
    log_memory_info();

    result.gcode = std::move(gcode);
    result.cooling_buffer_flush = object_layer || raft_layer || last_layer;
    return result;
}


GCode::LayerResult GCode::process_layer_batched_region(
    const Print& print,
    // Set of object & print layers of the same PrintObject and with the same print_z.
    const std::vector<LayerToPrint>& layers,
    const LayerTools& layer_tools,
    const bool                               last_layer,
    // Pairs of PrintObject index and its instance index.
    const std::vector<const PrintInstance*>* ordering,
    // If set to size_t(-1), then print all copies of all objects.
    // Otherwise print a single copy of a single object.
    size_t region_in_batch,
    const size_t                     		 single_object_instance_idx)
{
    std::cout << "============= process_layer_batched_region() =============" << std::endl;
    assert(!layers.empty());
    // Either printing all copies of all objects, or just a single copy of a single object.
    assert(single_object_instance_idx == size_t(-1) || layers.size() == 1);

    // First object, support and raft layer, if available.
    const Layer* object_layer = nullptr;
    const SupportLayer* support_layer = nullptr;
    const SupportLayer* raft_layer = nullptr;
    for (const LayerToPrint& l : layers) {
        if (l.object_layer && !object_layer)
            object_layer = l.object_layer;
        if (l.support_layer) {
            if (!support_layer)
                support_layer = l.support_layer;
            if (!raft_layer && support_layer->id() < support_layer->object()->slicing_parameters().raft_layers())
                raft_layer = support_layer;
        }
    }
    const Layer& layer = (object_layer != nullptr) ? *object_layer : *support_layer;
    GCode::LayerResult   result{ {}, layer.id(), false, last_layer };
    if (layer_tools.extruders.empty())
        // Nothing to extrude.
        return result;

    // Extract 1st object_layer and support_layer of this set of layers with an equal print_z.
    coordf_t             print_z = layer.print_z;
    bool                 first_layer = layer.id() == 0;
    unsigned int         first_extruder_id = layer_tools.extruders.front();

    // Initialize config with the 1st object to be printed at this layer.
    m_config.apply(layer.object()->config(), true);

    // Check whether it is possible to apply the spiral vase logic for this layer.
    // Just a reminder: A spiral vase mode is allowed for a single object, single material print only.
    m_enable_loop_clipping = true;
    if (m_spiral_vase && layers.size() == 1 && support_layer == nullptr) {
        bool enable = (layer.id() > 0 || !print.has_brim()) && (layer.id() >= (size_t)print.config().skirt_height.value && !print.has_infinite_skirt());
        if (enable) {
            for (const LayerRegion* layer_region : layer.regions())
                if (size_t(layer_region->region().config().bottom_solid_layers.value) > layer.id() ||
                    layer_region->perimeters.items_count() > 1u ||
                    layer_region->fills.items_count() > 0) {
                    enable = false;
                    break;
                }
        }
        result.spiral_vase_enable = enable;
        // If we're going to apply spiralvase to this layer, disable loop clipping.
        m_enable_loop_clipping = !enable;
    }

    std::string gcode;
    assert(is_decimal_separator_point()); // for the sprintfs

    // add tag for processor
    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    // export layer z
    gcode += std::string(";Z:") + float_to_string_decimal_point(print_z) + "\n";

    // export layer height
    float height = first_layer ? static_cast<float>(print_z) : static_cast<float>(print_z) - m_last_layer_z;
    gcode += std::string(";") + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height)
        + float_to_string_decimal_point(height) + "\n";

    // update caches
    m_last_layer_z = static_cast<float>(print_z);
    m_max_layer_z = std::max(m_max_layer_z, m_last_layer_z);
    m_last_height = height;

    // Set new layer - this will change Z and force a retraction if retract_layer_change is enabled.
    if (!print.config().before_layer_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("before_layer_gcode",
            print.config().before_layer_gcode.value, m_writer.extruder()->id(), &config)
            + "\n";
    }
    gcode += this->change_layer(print_z);  // this will increase m_layer_index
    m_layer = &layer;
    m_object_layer_over_raft = false;
    if (!print.config().layer_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("layer_gcode",
            print.config().layer_gcode.value, m_writer.extruder()->id(), &config)
            + "\n";
    }

    if (!first_layer && !m_second_layer_things_done) {
        // Transition from 1st to 2nd layer. Adjust nozzle temperatures as prescribed by the nozzle dependent
        // first_layer_temperature vs. temperature settings.
        for (const Extruder& extruder : m_writer.extruders()) {
            if (print.config().single_extruder_multi_material.value && extruder.id() != m_writer.extruder()->id())
                // In single extruder multi material mode, set the temperature for the current extruder only.
                continue;
            int temperature = print.config().temperature.get_at(extruder.id());
            if (temperature > 0 && temperature != print.config().first_layer_temperature.get_at(extruder.id()))
                gcode += m_writer.set_temperature(temperature, false, extruder.id());
        }
        gcode += m_writer.set_bed_temperature(print.config().bed_temperature.get_at(first_extruder_id));
        // Mark the temperature transition from 1st to 2nd layer to be finished.
        m_second_layer_things_done = true;
    }

    // Map from extruder ID to <begin, end> index of skirt loops to be extruded with that extruder.
    std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder;

    if (single_object_instance_idx == size_t(-1)) {
        // Normal (non-sequential) print.
        gcode += ProcessLayer::emit_custom_gcode_per_print_z(*this, layer_tools.custom_gcode, m_writer.extruder()->id(), first_extruder_id, print.config());
    }
    // Extrude skirt at the print_z of the raft layers and normal object layers
    // not at the print_z of the interlaced support material layers.
    skirt_loops_per_extruder = first_layer ?
        Skirt::make_skirt_loops_per_extruder_1st_layer(print, layer_tools, m_skirt_done) :
        Skirt::make_skirt_loops_per_extruder_other_layers(print, layer_tools, m_skirt_done);

    // Group extrusions by an extruder, then by an object, an island and a region.
    std::map<unsigned int, std::vector<ObjectByExtruder>> by_extruder;
    bool is_anything_overridden = const_cast<LayerTools&>(layer_tools).wiping_extrusions().is_anything_overridden();
    for (const LayerToPrint& layer_to_print : layers) {
        if (layer_to_print.support_layer != nullptr) {
            const SupportLayer& support_layer = *layer_to_print.support_layer;
            const PrintObject& object = *support_layer.object();
            if (!support_layer.support_fills.entities.empty()) {
                ExtrusionRole   role = support_layer.support_fills.role();
                bool            has_support = role == erMixed || role == erSupportMaterial;
                bool            has_interface = role == erMixed || role == erSupportMaterialInterface;
                // Extruder ID of the support base. -1 if "don't care".
                unsigned int    support_extruder = object.config().support_material_extruder.value - 1;
                // Shall the support be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                bool            support_dontcare = object.config().support_material_extruder.value == 0;
                // Extruder ID of the support interface. -1 if "don't care".
                unsigned int    interface_extruder = object.config().support_material_interface_extruder.value - 1;
                // Shall the support interface be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                bool            interface_dontcare = object.config().support_material_interface_extruder.value == 0;
                if (support_dontcare || interface_dontcare) {
                    // Some support will be printed with "don't care" material, preferably non-soluble.
                    // Is the current extruder assigned a soluble filament?
                    unsigned int dontcare_extruder = first_extruder_id;
                    if (print.config().filament_soluble.get_at(dontcare_extruder)) {
                        // The last extruder printed on the previous layer extrudes soluble filament.
                        // Try to find a non-soluble extruder on the same layer.
                        for (unsigned int extruder_id : layer_tools.extruders)
                            if (!print.config().filament_soluble.get_at(extruder_id)) {
                                dontcare_extruder = extruder_id;
                                break;
                            }
                    }
                    if (support_dontcare)
                        support_extruder = dontcare_extruder;
                    if (interface_dontcare)
                        interface_extruder = dontcare_extruder;
                }
                // Both the support and the support interface are printed with the same extruder, therefore
                // the interface may be interleaved with the support base.
                bool single_extruder = !has_support || support_extruder == interface_extruder;
                // Assign an extruder to the base.
                ObjectByExtruder& obj = object_by_extruder(by_extruder, has_support ? support_extruder : interface_extruder, &layer_to_print - layers.data(), layers.size());
                obj.support = &support_layer.support_fills;
                obj.support_extrusion_role = single_extruder ? erMixed : erSupportMaterial;
                if (!single_extruder && has_interface) {
                    ObjectByExtruder& obj_interface = object_by_extruder(by_extruder, interface_extruder, &layer_to_print - layers.data(), layers.size());
                    obj_interface.support = &support_layer.support_fills;
                    obj_interface.support_extrusion_role = erSupportMaterialInterface;
                }
            }
        }
        if (layer_to_print.object_layer != nullptr) {
            const Layer& layer = *layer_to_print.object_layer;
            // We now define a strategy for building perimeters and fills. The separation
            // between regions doesn't matter in terms of printing order, as we follow
            // another logic instead:
            // - we group all extrusions by extruder so that we minimize toolchanges
            // - we start from the last used extruder
            // - for each extruder, we group extrusions by island
            // - for each island, we extrude perimeters first, unless user set the infill_first
            //   option
            // (Still, we have to keep track of regions because we need to apply their config)
            size_t n_slices = layer.lslices.size();
            const std::vector<BoundingBox>& layer_surface_bboxes = layer.lslices_bboxes;
            // Traverse the slices in an increasing order of bounding box size, so that the islands inside another islands are tested first,
            // so we can just test a point inside ExPolygon::contour and we may skip testing the holes.
            std::vector<size_t> slices_test_order;
            slices_test_order.reserve(n_slices);
            for (size_t i = 0; i < n_slices; ++i)
                slices_test_order.emplace_back(i);
            std::sort(slices_test_order.begin(), slices_test_order.end(), [&layer_surface_bboxes](size_t i, size_t j) {
                const Vec2d s1 = layer_surface_bboxes[i].size().cast<double>();
                const Vec2d s2 = layer_surface_bboxes[j].size().cast<double>();
                return s1.x() * s1.y() < s2.x() * s2.y();
                });
            auto point_inside_surface = [&layer, &layer_surface_bboxes](const size_t i, const Point& point) {
                const BoundingBox& bbox = layer_surface_bboxes[i];
                return point(0) >= bbox.min(0) && point(0) < bbox.max(0) &&
                    point(1) >= bbox.min(1) && point(1) < bbox.max(1) &&
                    layer.lslices[i].contour.contains(point);
            };

            //for (size_t region_id = 0; region_id < layer.regions().size(); ++region_id) 
             size_t region_id = region_in_batch;
            {
                const LayerRegion* layerm = layer.regions()[region_id];
                if (layerm == nullptr)
                    continue;
                // PrintObjects own the PrintRegions, thus the pointer to PrintRegion would be unique to a PrintObject, they would not
                // identify the content of PrintRegion accross the whole print uniquely. Translate to a Print specific PrintRegion.
                const PrintRegion& region = print.get_print_region(layerm->region().print_region_id());

                // Now we must process perimeters and infills and create islands of extrusions in by_region std::map.
                // It is also necessary to save which extrusions are part of MM wiping and which are not.
                // The process is almost the same for perimeters and infills - we will do it in a cycle that repeats twice:
                std::vector<unsigned int> printing_extruders;
                for (const ObjectByExtruder::Island::Region::Type entity_type : { ObjectByExtruder::Island::Region::INFILL, ObjectByExtruder::Island::Region::PERIMETERS }) {
                    for (const ExtrusionEntity* ee : (entity_type == ObjectByExtruder::Island::Region::INFILL) ? layerm->fills.entities : layerm->perimeters.entities) {
                        // extrusions represents infill or perimeter extrusions of a single island.
                        assert(dynamic_cast<const ExtrusionEntityCollection*>(ee) != nullptr);
                        const auto* extrusions = static_cast<const ExtrusionEntityCollection*>(ee);
                        if (extrusions->entities.empty()) // This shouldn't happen but first_point() would fail.
                            continue;

                        // This extrusion is part of certain Region, which tells us which extruder should be used for it:
                        int correct_extruder_id = layer_tools.extruder(*extrusions, region);

                        // Let's recover vector of extruder overrides:
                        const WipingExtrusions::ExtruderPerCopy* entity_overrides = nullptr;
                        if (!layer_tools.has_extruder(correct_extruder_id)) {
                            // this entity is not overridden, but its extruder is not in layer_tools - we'll print it
                            // by last extruder on this layer (could happen e.g. when a wiping object is taller than others - dontcare extruders are eradicated from layer_tools)
                            correct_extruder_id = layer_tools.extruders.back();
                        }
                        printing_extruders.clear();
                        if (is_anything_overridden) {
                            entity_overrides = const_cast<LayerTools&>(layer_tools).wiping_extrusions().get_extruder_overrides(extrusions, correct_extruder_id, layer_to_print.object()->instances().size());
                            if (entity_overrides == nullptr) {
                                printing_extruders.emplace_back(correct_extruder_id);
                            }
                            else {
                                printing_extruders.reserve(entity_overrides->size());
                                for (int extruder : *entity_overrides)
                                    printing_extruders.emplace_back(extruder >= 0 ?
                                        // at least one copy is overridden to use this extruder
                                        extruder :
                                        // at least one copy would normally be printed with this extruder (see get_extruder_overrides function for explanation)
                                        static_cast<unsigned int>(-extruder - 1));
                                Slic3r::sort_remove_duplicates(printing_extruders);
                            }
                        }
                        else
                            printing_extruders.emplace_back(correct_extruder_id);

                        // Now we must add this extrusion into the by_extruder map, once for each extruder that will print it:
                        for (unsigned int extruder : printing_extruders)
                        {
                            std::vector<ObjectByExtruder::Island>& islands = object_islands_by_extruder(
                                by_extruder,
                                extruder,
                                &layer_to_print - layers.data(),
                                layers.size(), n_slices + 1);
                            for (size_t i = 0; i <= n_slices; ++i) {
                                bool   last = i == n_slices;
                                size_t island_idx = last ? n_slices : slices_test_order[i];
                                if (// extrusions->first_point does not fit inside any slice
                                    last ||
                                    // extrusions->first_point fits inside ith slice
                                    point_inside_surface(island_idx, extrusions->first_point())) {
                                    if (islands[island_idx].by_region.empty())
                                        islands[island_idx].by_region.assign(print.num_print_regions(), ObjectByExtruder::Island::Region());
                                    islands[island_idx].by_region[region.print_region_id()].append(entity_type, extrusions, entity_overrides);
                                    break;
                                }
                            }
                        }
                    }
                }
            } // for regions
        }
    } // for objects

    // Extrude the skirt, brim, support, perimeters, infill ordered by the extruders.
    for (unsigned int extruder_id : layer_tools.extruders)
    {
        // ATC fork: We implemented the TC gcode above in this function
        // search "if (print_region_idx != atc_current_tool){...}"
        gcode += (layer_tools.has_wipe_tower && m_wipe_tower) ? m_wipe_tower->tool_change(*this, extruder_id, extruder_id == layer_tools.extruders.back()) : this->set_extruder(extruder_id, print_z);

        // let analyzer tag generator aware of a role type change
        if (layer_tools.has_wipe_tower && m_wipe_tower)
            m_last_processor_extrusion_role = erWipeTower;

        if (auto loops_it = skirt_loops_per_extruder.find(extruder_id); loops_it != skirt_loops_per_extruder.end()) {
            const std::pair<size_t, size_t> loops = loops_it->second;
            this->set_origin(0., 0.);
            m_avoid_crossing_perimeters.use_external_mp();
            Flow layer_skirt_flow = print.skirt_flow().with_height(float(m_skirt_done.back() - (m_skirt_done.size() == 1 ? 0. : m_skirt_done[m_skirt_done.size() - 2])));
            double mm3_per_mm = layer_skirt_flow.mm3_per_mm();
            for (size_t i = loops.first; i < loops.second; ++i) {
                // Adjust flow according to this layer's layer height.
                ExtrusionLoop loop = *dynamic_cast<const ExtrusionLoop*>(print.skirt().entities[i]);
                for (ExtrusionPath& path : loop.paths) {
                    path.height = layer_skirt_flow.height();
                    path.mm3_per_mm = mm3_per_mm;
                }
                //FIXME using the support_material_speed of the 1st object printed.
                gcode += this->extrude_loop(loop, "skirt"sv, m_config.support_material_speed.value);
            }
            m_avoid_crossing_perimeters.use_external_mp(false);
            // Allow a straight travel move to the first object point if this is the first layer (but don't in next layers).
            if (first_layer && loops.first == 0)
                m_avoid_crossing_perimeters.disable_once();
        }

        // Extrude brim with the extruder of the 1st region.
        if (!m_brim_done) {
            this->set_origin(0., 0.);
            m_avoid_crossing_perimeters.use_external_mp();
            for (const ExtrusionEntity* ee : print.brim().entities) {
                gcode += this->extrude_entity(*ee, "brim"sv, m_config.support_material_speed.value);
            }
            m_brim_done = true;
            m_avoid_crossing_perimeters.use_external_mp(false);
            // Allow a straight travel move to the first object point.
            m_avoid_crossing_perimeters.disable_once();
        }


        auto objects_by_extruder_it = by_extruder.find(extruder_id);
        if (objects_by_extruder_it == by_extruder.end())
            continue;

        std::vector<InstanceToPrint> instances_to_print = sort_print_object_instances(objects_by_extruder_it->second, layers, ordering, single_object_instance_idx);

        // We are almost ready to print. However, we must go through all the objects twice to print the the overridden extrusions first (infill/perimeter wiping feature):
        std::vector<ObjectByExtruder::Island::Region> by_region_per_copy_cache;
        for (int print_wipe_extrusions = is_anything_overridden; print_wipe_extrusions >= 0; --print_wipe_extrusions) {
            if (is_anything_overridden && print_wipe_extrusions == 0)
                gcode += "; PURGING FINISHED\n";

            for (InstanceToPrint& instance_to_print : instances_to_print) {
                const LayerToPrint& layer_to_print = layers[instance_to_print.layer_id];
                // To control print speed of the 1st object layer printed over raft interface.
                bool object_layer_over_raft = layer_to_print.object_layer && layer_to_print.object_layer->id() > 0 &&
                    instance_to_print.print_object.slicing_parameters().raft_layers() == layer_to_print.object_layer->id();
                m_config.apply(instance_to_print.print_object.config(), true);
                m_layer = layer_to_print.layer();
                m_object_layer_over_raft = object_layer_over_raft;
                if (m_config.avoid_crossing_perimeters)
                    m_avoid_crossing_perimeters.init_layer(*m_layer);
                if (this->config().gcode_label_objects)
                    gcode += std::string("; printing object ") + instance_to_print.print_object.model_object()->name + " id:" + std::to_string(instance_to_print.layer_id) + " copy " + std::to_string(instance_to_print.instance_id) + "\n";
                // When starting a new object, use the external motion planner for the first travel move.
                const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                if (m_last_obj_copy != this_object_copy)
                    m_avoid_crossing_perimeters.use_external_mp_once();
                m_last_obj_copy = this_object_copy;
                this->set_origin(unscale(offset));
                if (instance_to_print.object_by_extruder.support != nullptr && !print_wipe_extrusions) {
                    m_layer = layer_to_print.support_layer;
                    m_object_layer_over_raft = false;
                    gcode += this->extrude_support(
                        // support_extrusion_role is erSupportMaterial, erSupportMaterialInterface or erMixed for all extrusion paths.
                        instance_to_print.object_by_extruder.support->chained_path_from(m_last_pos, instance_to_print.object_by_extruder.support_extrusion_role));
                    m_layer = layer_to_print.layer();
                    m_object_layer_over_raft = object_layer_over_raft;
                }
                //FIXME order islands?
                // Sequential tool path ordering of multiple parts within the same object, aka. perimeter tracking (#5511)
                for (ObjectByExtruder::Island& island : instance_to_print.object_by_extruder.islands) {
                    const auto& by_region_specific = is_anything_overridden ? island.by_region_per_copy(by_region_per_copy_cache, static_cast<unsigned int>(instance_to_print.instance_id), extruder_id, print_wipe_extrusions != 0) : island.by_region;
                    //FIXME the following code prints regions in the order they are defined, the path is not optimized in any way.
                    if (print.config().infill_first) {
                        gcode += this->extrude_infill(print, by_region_specific, false);
                        gcode += this->extrude_perimeters(print, by_region_specific);
                    }
                    else {
                        gcode += this->extrude_perimeters(print, by_region_specific);
                        gcode += this->extrude_infill(print, by_region_specific, false);
                    }
                    // ironing
                    gcode += this->extrude_infill(print, by_region_specific, true);
                }
                if (this->config().gcode_label_objects)
                    gcode += std::string("; stop printing object ") + instance_to_print.print_object.model_object()->name + " id:" + std::to_string(instance_to_print.layer_id) + " copy " + std::to_string(instance_to_print.instance_id) + "\n";
            }
        }
    }

#if 0
    // Apply spiral vase post-processing if this layer contains suitable geometry
    // (we must feed all the G-code into the post-processor, including the first
    // bottom non-spiral layers otherwise it will mess with positions)
    // we apply spiral vase at this stage because it requires a full layer.
    // Just a reminder: A spiral vase mode is allowed for a single object per layer, single material print only.
    if (m_spiral_vase)
        gcode = m_spiral_vase->process_layer(std::move(gcode));

    // Apply cooling logic; this may alter speeds.
    if (m_cooling_buffer)
        gcode = m_cooling_buffer->process_layer(std::move(gcode), layer.id(),
            // Flush the cooling buffer at each object layer or possibly at the last layer, even if it contains just supports (This should not happen).
            object_layer || last_layer);

#ifdef HAS_PRESSURE_EQUALIZER
    // Apply pressure equalization if enabled;
    // printf("G-code before filter:\n%s\n", gcode.c_str());
    if (m_pressure_equalizer)
        gcode = m_pressure_equalizer->process(gcode.c_str(), false);
    // printf("G-code after filter:\n%s\n", out.c_str());
#endif /* HAS_PRESSURE_EQUALIZER */

    file.write(gcode);
#endif

    BOOST_LOG_TRIVIAL(trace) << "Exported layer " << layer.id() << " print_z " << print_z <<
        log_memory_info();

    result.gcode = std::move(gcode);
    result.cooling_buffer_flush = object_layer || raft_layer || last_layer;
    return result;
}




void GCode::apply_print_config(const PrintConfig &print_config)
{
    m_writer.apply_print_config(print_config);
    m_config.apply(print_config);
    m_scaled_resolution = scaled<double>(print_config.gcode_resolution.value);
}

void GCode::append_full_config(const Print &print, std::string &str)
{
    const DynamicPrintConfig &cfg = print.full_print_config();
    // Sorted list of config keys, which shall not be stored into the G-code. Initializer list.
    static constexpr auto banned_keys = {
        "compatible_printers"sv,
        "compatible_prints"sv,
        //FIXME The print host keys should not be exported to full_print_config anymore. The following keys may likely be removed.
        "print_host"sv,
        "printhost_apikey"sv,
        "printhost_cafile"sv
    };
    assert(std::is_sorted(banned_keys.begin(), banned_keys.end()));
    auto is_banned = [](const std::string &key) {
        return std::binary_search(banned_keys.begin(), banned_keys.end(), key);
    };
    for (const std::string &key : cfg.keys())
        if (! is_banned(key) && ! cfg.option(key)->is_nil())
            str += "; " + key + " = " + cfg.opt_serialize(key) + "\n";
}

void GCode::set_extruders(const std::vector<unsigned int> &extruder_ids)
{
    m_writer.set_extruders(extruder_ids);

    // enable wipe path generation if any extruder has wipe enabled
    m_wipe.enable = false;
    for (auto id : extruder_ids)
        if (m_config.wipe.get_at(id)) {
            m_wipe.enable = true;
            break;
        }
}

void GCode::set_origin(const Vec2d &pointf)
{
    // if origin increases (goes towards right), last_pos decreases because it goes towards left
    const Point translate(
        scale_(m_origin(0) - pointf(0)),
        scale_(m_origin(1) - pointf(1))
    );
    m_last_pos += translate;
    m_wipe.path.translate(translate);
    m_origin = pointf;
}

std::string GCode::preamble()
{
    std::string gcode = m_writer.preamble();

    /*  Perform a *silent* move to z_offset: we need this to initialize the Z
        position of our writer object so that any initial lift taking place
        before the first layer change will raise the extruder from the correct
        initial Z instead of 0.  */
    m_writer.travel_to_z(m_config.z_offset.value);

    return gcode;
}

// called by GCode::process_layer()
std::string GCode::change_layer(coordf_t print_z)
{
    std::string gcode;
    if (m_layer_count > 0)
        // Increment a progress bar indicator.
        gcode += m_writer.update_progress(++ m_layer_index, m_layer_count);
    coordf_t z = print_z + m_config.z_offset.value;  // in unscaled coordinates
    if (EXTRUDER_CONFIG(retract_layer_change) && m_writer.will_move_z(z))
        gcode += this->retract();

    {
        std::ostringstream comment;
        comment << "move to next layer (" << m_layer_index << ")";
        gcode += m_writer.travel_to_z(z, comment.str());
    }

    // forget last wiping path as wiping after raising Z is pointless
    m_wipe.reset_path();

    return gcode;
}

static const auto comment_perimeter = "perimeter"sv;
// Comparing string_view pointer & length for speed.
static inline bool comment_is_perimeter(const std::string_view comment) {
    return comment.data() == comment_perimeter.data() && comment.size() == comment_perimeter.size();
}

std::string GCode::extrude_loop(ExtrusionLoop loop, const std::string_view description, double speed)
{
    // get a copy; don't modify the orientation of the original loop object otherwise
    // next copies (if any) would not detect the correct orientation

    // extrude all loops ccw
    bool was_clockwise = loop.make_counter_clockwise();

    // find the point of the loop that is closest to the current extruder position
    // or randomize if requested
    Point last_pos = this->last_pos();
    if (! m_config.spiral_vase && comment_is_perimeter(description)) {
        assert(m_layer != nullptr);
        m_seam_placer.place_seam(m_layer, loop, m_config.external_perimeters_first, this->last_pos());
    } else
        loop.split_at(last_pos, false);

    // clip the path to avoid the extruder to get exactly on the first point of the loop;
    // if polyline was shorter than the clipping distance we'd get a null polyline, so
    // we discard it in that case
    double clip_length = m_enable_loop_clipping ?
        scale_(EXTRUDER_CONFIG(nozzle_diameter)) * LOOP_CLIPPING_LENGTH_OVER_NOZZLE_DIAMETER :
        0;

    // get paths
    ExtrusionPaths paths;
    loop.clip_end(clip_length, &paths);
    if (paths.empty()) return "";

    // apply the small perimeter speed
    if (is_perimeter(paths.front().role()) && loop.length() <= SMALL_PERIMETER_LENGTH && speed == -1)
        speed = m_config.small_perimeter_speed.get_abs_value(m_config.perimeter_speed);

    // extrude along the path
    std::string gcode;
    for (ExtrusionPath &path : paths) {
        path.simplify(m_scaled_resolution);
        gcode += this->_extrude(path, description, speed);
    }

    // reset acceleration
    gcode += m_writer.set_acceleration((unsigned int)(m_config.default_acceleration.value + 0.5));

    if (m_wipe.enable)
        m_wipe.path = paths.front().polyline;  // TODO: don't limit wipe to last path

    // make a little move inwards before leaving loop
    if (paths.back().role() == erExternalPerimeter && m_layer != NULL && m_config.perimeters.value > 1 && paths.front().size() >= 2 && paths.back().polyline.points.size() >= 3) {
        // detect angle between last and first segment
        // the side depends on the original winding order of the polygon (left for contours, right for holes)
        //FIXME improve the algorithm in case the loop is tiny.
        //FIXME improve the algorithm in case the loop is split into segments with a low number of points (see the Point b query).
        // Angle from the 2nd point to the last point.
        double angle_inside = angle(paths.front().polyline.points[1]        - paths.front().first_point(),
                                    *(paths.back().polyline.points.end()-3) - paths.front().first_point());
        assert(angle_inside >= -M_PI && angle_inside <= M_PI);
        // 3rd of this angle will be taken, thus make the angle monotonic before interpolation.
        if (was_clockwise) {
            if (angle_inside > 0)
                angle_inside -= 2.0 * M_PI;
        } else {
            if (angle_inside < 0)
                angle_inside += 2.0 * M_PI;
        }

        // create the destination point along the first segment and rotate it
        // we make sure we don't exceed the segment length because we don't know
        // the rotation of the second segment so we might cross the object boundary
        Vec2d  p1 = paths.front().polyline.points.front().cast<double>();
        Vec2d  p2 = paths.front().polyline.points[1].cast<double>();
        Vec2d  v  = p2 - p1;
        double nd = scale_(EXTRUDER_CONFIG(nozzle_diameter));
        double l2 = v.squaredNorm();
        // Shift by no more than a nozzle diameter.
        //FIXME Hiding the seams will not work nicely for very densely discretized contours!
        Point  pt = ((nd * nd >= l2) ? p2 : (p1 + v * (nd / sqrt(l2)))).cast<coord_t>();
        // Rotate pt inside around the seam point.
        pt.rotate(angle_inside / 3., paths.front().polyline.points.front());
        // generate the travel move
        gcode += m_writer.travel_to_xy(this->point_to_gcode(pt), "move inwards before travel");
    }

    return gcode;
}

std::string GCode::extrude_multi_path(ExtrusionMultiPath multipath, const std::string_view description, double speed)
{
    // extrude along the path
    std::string gcode;
    for (ExtrusionPath path : multipath.paths) {
        path.simplify(m_scaled_resolution);
        gcode += this->_extrude(path, description, speed);
    }
    if (m_wipe.enable) {
        m_wipe.path = std::move(multipath.paths.back().polyline);  // TODO: don't limit wipe to last path
        m_wipe.path.reverse();
    }
    // reset acceleration
    gcode += m_writer.set_acceleration((unsigned int)floor(m_config.default_acceleration.value + 0.5));
    return gcode;
}

std::string GCode::extrude_entity(const ExtrusionEntity &entity, const std::string_view description, double speed)
{
    if (const ExtrusionPath* path = dynamic_cast<const ExtrusionPath*>(&entity))
        return this->extrude_path(*path, description, speed);
    else if (const ExtrusionMultiPath* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity))
        return this->extrude_multi_path(*multipath, description, speed);
    else if (const ExtrusionLoop* loop = dynamic_cast<const ExtrusionLoop*>(&entity))
        return this->extrude_loop(*loop, description, speed);
    else
        throw Slic3r::InvalidArgument("Invalid argument supplied to extrude()");
    return "";
}

std::string GCode::extrude_path(ExtrusionPath path, std::string_view description, double speed)
{
    path.simplify(m_scaled_resolution);
    std::string gcode = this->_extrude(path, description, speed);
    if (m_wipe.enable) {
        m_wipe.path = std::move(path.polyline);
        m_wipe.path.reverse();
    }
    // reset acceleration
    gcode += m_writer.set_acceleration((unsigned int)floor(m_config.default_acceleration.value + 0.5));
    return gcode;
}

// Extrude perimeters: Decide where to put seams (hide or align seams).
std::string GCode::extrude_perimeters(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region)
{
    std::string gcode;
    for (const ObjectByExtruder::Island::Region &region : by_region)
        if (! region.perimeters.empty()) {
            m_config.apply(print.get_print_region(&region - &by_region.front()).config());

            for (const ExtrusionEntity* ee : region.perimeters)
                gcode += this->extrude_entity(*ee, comment_perimeter, -1.);
        }
    return gcode;
}

// Chain the paths hierarchically by a greedy algorithm to minimize a travel distance.
std::string GCode::extrude_infill(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region, bool ironing)
{
    std::string 		 gcode;
    ExtrusionEntitiesPtr extrusions;
    const auto           extrusion_name = ironing ? "ironing"sv : "infill"sv;
    for (const ObjectByExtruder::Island::Region &region : by_region)
        if (! region.infills.empty()) {
            extrusions.clear();
            extrusions.reserve(region.infills.size());
            for (ExtrusionEntity *ee : region.infills)
                if ((ee->role() == erIroning) == ironing)
                    extrusions.emplace_back(ee);
            if (! extrusions.empty()) {
                m_config.apply(print.get_print_region(&region - &by_region.front()).config());
                chain_and_reorder_extrusion_entities(extrusions, &m_last_pos);
                for (const ExtrusionEntity *fill : extrusions) {
                    auto *eec = dynamic_cast<const ExtrusionEntityCollection*>(fill);
                    if (eec) {
                        for (ExtrusionEntity *ee : eec->chained_path_from(m_last_pos).entities)
                            gcode += this->extrude_entity(*ee, extrusion_name);
                    } else
                        gcode += this->extrude_entity(*fill, extrusion_name);
                }
            }
        }
    return gcode;
}

std::string GCode::extrude_support(const ExtrusionEntityCollection &support_fills)
{
    static constexpr const auto support_label            = "support material"sv;
    static constexpr const auto support_interface_label  = "support material interface"sv;

    std::string gcode;
    if (! support_fills.entities.empty()) {
        const double  support_speed            = m_config.support_material_speed.value;
        const double  support_interface_speed  = m_config.support_material_interface_speed.get_abs_value(support_speed);
        for (const ExtrusionEntity *ee : support_fills.entities) {
            ExtrusionRole role = ee->role();
            assert(role == erSupportMaterial || role == erSupportMaterialInterface);
            const auto   label = (role == erSupportMaterial) ? support_label : support_interface_label;
            const double speed = (role == erSupportMaterial) ? support_speed : support_interface_speed;
            const ExtrusionPath *path = dynamic_cast<const ExtrusionPath*>(ee);
            if (path)
                gcode += this->extrude_path(*path, label, speed);
            else {
                const ExtrusionMultiPath *multipath = dynamic_cast<const ExtrusionMultiPath*>(ee);
                if (multipath)
                    gcode += this->extrude_multi_path(*multipath, label, speed);
                else {
                    const ExtrusionEntityCollection *eec = dynamic_cast<const ExtrusionEntityCollection*>(ee);
                    assert(eec);
                    if (eec)
                        gcode += this->extrude_support(*eec);
                }
            }
        }
    }
    return gcode;
}

bool GCode::GCodeOutputStream::is_error() const 
{
    return ::ferror(this->f);
}

void GCode::GCodeOutputStream::flush()
{ 
    ::fflush(this->f);
}

void GCode::GCodeOutputStream::close()
{ 
    if (this->f) {
        ::fclose(this->f);
        this->f = nullptr;
    }
}

void GCode::GCodeOutputStream::write(const char *what)
{
    if (what != nullptr) {
        //FIXME don't allocate a string, maybe process a batch of lines?
        std::string gcode(m_find_replace ? m_find_replace->process_layer(what) : what);
        // writes string to file
        fwrite(gcode.c_str(), 1, gcode.size(), this->f);
        m_processor.process_buffer(gcode);
    }
}

void GCode::GCodeOutputStream::writeln(const std::string &what)
{
    if (! what.empty())
        this->write(what.back() == '\n' ? what : what + '\n');
}

void GCode::GCodeOutputStream::write_format(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    int buflen;
    {
        va_list args2;
        va_copy(args2, args);
        buflen =
    #ifdef _MSC_VER
            ::_vscprintf(format, args2)
    #else
            ::vsnprintf(nullptr, 0, format, args2)
    #endif
            + 1;
        va_end(args2);
    }

    char buffer[1024];
    bool buffer_dynamic = buflen > 1024;
    char *bufptr = buffer_dynamic ? (char*)malloc(buflen) : buffer;
    int res = ::vsnprintf(bufptr, buflen, format, args);
    if (res > 0)
        this->write(bufptr);

    if (buffer_dynamic)
        free(bufptr);

    va_end(args);
}

std::string GCode::_extrude(const ExtrusionPath &path, const std::string_view description, double speed)
{
    std::string gcode;
    const std::string_view description_bridge = is_bridge(path.role()) ? " (bridge)"sv : ""sv;

    // go to first point of extrusion path
    if (!m_last_pos_defined || m_last_pos != path.first_point()) {
        std::string comment = "move to first ";
        comment += description;
        comment += description_bridge;
        comment += " point";
        gcode += this->travel_to(path.first_point(), path.role(), comment);
    }

    // compensate retraction
    gcode += this->unretract();

    // adjust acceleration
    if (m_config.default_acceleration.value > 0) {
        double acceleration;
        if (this->on_first_layer() && m_config.first_layer_acceleration.value > 0) {
            acceleration = m_config.first_layer_acceleration.value;
        } else if (this->object_layer_over_raft() && m_config.first_layer_acceleration_over_raft.value > 0) {
            acceleration = m_config.first_layer_acceleration_over_raft.value;
        } else if (m_config.perimeter_acceleration.value > 0 && is_perimeter(path.role())) {
            acceleration = m_config.perimeter_acceleration.value;
        } else if (m_config.bridge_acceleration.value > 0 && is_bridge(path.role())) {
            acceleration = m_config.bridge_acceleration.value;
        } else if (m_config.infill_acceleration.value > 0 && is_infill(path.role())) {
            acceleration = m_config.infill_acceleration.value;
        } else {
            acceleration = m_config.default_acceleration.value;
        }
        gcode += m_writer.set_acceleration((unsigned int)floor(acceleration + 0.5));
    }

    // calculate extrusion length per distance unit
    double e_per_mm = m_writer.extruder()->e_per_mm3() * path.mm3_per_mm;
    if (m_writer.extrusion_axis().empty())
        // gcfNoExtrusion
        e_per_mm = 0;

    // set speed
    if (speed == -1) {
        if (path.role() == erPerimeter) {
            speed = m_config.get_abs_value("perimeter_speed");
        } else if (path.role() == erExternalPerimeter) {
            speed = m_config.get_abs_value("external_perimeter_speed");
        } else if (path.role() == erOverhangPerimeter || path.role() == erBridgeInfill) {
            speed = m_config.get_abs_value("bridge_speed");
        } else if (path.role() == erInternalInfill) {
            speed = m_config.get_abs_value("infill_speed");
        } else if (path.role() == erSolidInfill) {
            speed = m_config.get_abs_value("solid_infill_speed");
        } else if (path.role() == erTopSolidInfill) {
            speed = m_config.get_abs_value("top_solid_infill_speed");
        } else if (path.role() == erIroning) {
            speed = m_config.get_abs_value("ironing_speed");
        } else if (path.role() == erGapFill) {
            speed = m_config.get_abs_value("gap_fill_speed");
        } else {
            throw Slic3r::InvalidArgument("Invalid speed");
        }
    }
    if (m_volumetric_speed != 0. && speed == 0)
        speed = m_volumetric_speed / path.mm3_per_mm;
    if (this->on_first_layer())
        speed = m_config.get_abs_value("first_layer_speed", speed);
    else if (this->object_layer_over_raft())
        speed = m_config.get_abs_value("first_layer_speed_over_raft", speed);
    if (m_config.max_volumetric_speed.value > 0) {
        // cap speed with max_volumetric_speed anyway (even if user is not using autospeed)
        speed = std::min(
            speed,
            m_config.max_volumetric_speed.value / path.mm3_per_mm
        );
    }
    if (EXTRUDER_CONFIG(filament_max_volumetric_speed) > 0) {
        // cap speed with max_volumetric_speed anyway (even if user is not using autospeed)
        speed = std::min(
            speed,
            EXTRUDER_CONFIG(filament_max_volumetric_speed) / path.mm3_per_mm
        );
    }
    double F = speed * 60;  // convert mm/sec to mm/min

    // extrude arc or line
    if (m_enable_extrusion_role_markers)
    {
        if (path.role() != m_last_extrusion_role)
        {
            m_last_extrusion_role = path.role();
            if (m_enable_extrusion_role_markers)
            {
                char buf[32];
                sprintf(buf, ";_EXTRUSION_ROLE:%d\n", int(m_last_extrusion_role));
                gcode += buf;
            }
        }
    }

    // adds processor tags and updates processor tracking data
    // PrusaMultiMaterial::Writer may generate GCodeProcessor::Height_Tag lines without updating m_last_height
    // so, if the last role was erWipeTower we force export of GCodeProcessor::Height_Tag lines
    bool last_was_wipe_tower = (m_last_processor_extrusion_role == erWipeTower);
    assert(is_decimal_separator_point());

    if (path.role() != m_last_processor_extrusion_role) {
        m_last_processor_extrusion_role = path.role();
        char buf[64];
        sprintf(buf, ";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(m_last_processor_extrusion_role).c_str());
        gcode += buf;
    }

    if (last_was_wipe_tower || m_last_width != path.width) {
        m_last_width = path.width;
        gcode += std::string(";") + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width)
               + float_to_string_decimal_point(m_last_width) + "\n";
    }

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    if (last_was_wipe_tower || (m_last_mm3_per_mm != path.mm3_per_mm)) {
        m_last_mm3_per_mm = path.mm3_per_mm;
        gcode += std::string(";") + GCodeProcessor::Mm3_Per_Mm_Tag
            + float_to_string_decimal_point(m_last_mm3_per_mm) + "\n";
    }
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    if (last_was_wipe_tower || std::abs(m_last_height - path.height) > EPSILON) {
        m_last_height = path.height;

        gcode += std::string(";") + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height)
            + float_to_string_decimal_point(m_last_height) + "\n";
    }

    std::string comment;
    if (m_enable_cooling_markers) {
        if (is_bridge(path.role()))
            gcode += ";_BRIDGE_FAN_START\n";
        else
            comment = ";_EXTRUDE_SET_SPEED";
        if (path.role() == erExternalPerimeter)
            comment += ";_EXTERNAL_PERIMETER";
    }

    // F is mm per minute.
    gcode += m_writer.set_speed(F, "", comment);
    double path_length = 0.;
    {
        std::string comment;
        if (m_config.gcode_comments) {
            comment = description;
            comment += description_bridge;
        }
        Vec2d prev = this->point_to_gcode_quantized(path.polyline.points.front());
        auto  it   = path.polyline.points.begin();
        auto  end  = path.polyline.points.end();
        for (++ it; it != end; ++ it) {
            Vec2d p = this->point_to_gcode_quantized(*it);
            const double line_length = (p - prev).norm();
            path_length += line_length;
            gcode += m_writer.extrude_to_xy(p, e_per_mm * line_length, comment);
            prev = p;
        }
    }
    if (m_enable_cooling_markers)
        gcode += is_bridge(path.role()) ? ";_BRIDGE_FAN_END\n" : ";_EXTRUDE_END\n";

    this->set_last_pos(path.last_point());
    return gcode;
}

// This method accepts &point in print coordinates.
std::string GCode::travel_to(const Point &point, ExtrusionRole role, std::string comment)
{
    /*  Define the travel move as a line between current position and the taget point.
        This is expressed in print coordinates, so it will need to be translated by
        this->origin in order to get G-code coordinates.  */
    Polyline travel { this->last_pos(), point };

    // check whether a straight travel move would need retraction
    bool needs_retraction             = this->needs_retraction(travel, role);
    // check whether wipe could be disabled without causing visible stringing
    bool could_be_wipe_disabled       = false;
    // Save state of use_external_mp_once for the case that will be needed to call twice m_avoid_crossing_perimeters.travel_to.
    const bool used_external_mp_once  = m_avoid_crossing_perimeters.used_external_mp_once();

    // if a retraction would be needed, try to use avoid_crossing_perimeters to plan a
    // multi-hop travel path inside the configuration space
    if (needs_retraction
        && m_config.avoid_crossing_perimeters
        && ! m_avoid_crossing_perimeters.disabled_once()) {
        travel = m_avoid_crossing_perimeters.travel_to(*this, point, &could_be_wipe_disabled);
        // check again whether the new travel path still needs a retraction
        needs_retraction = this->needs_retraction(travel, role);
        //if (needs_retraction && m_layer_index > 1) exit(0);
    }

    // Re-allow avoid_crossing_perimeters for the next travel moves
    m_avoid_crossing_perimeters.reset_once_modifiers();

    // generate G-code for the travel move
    std::string gcode;
    if (needs_retraction) {
        if (m_config.avoid_crossing_perimeters && could_be_wipe_disabled)
            m_wipe.reset_path();

        Point last_post_before_retract = this->last_pos();
        gcode += this->retract();
        // When "Wipe while retracting" is enabled, then extruder moves to another position, and travel from this position can cross perimeters.
        // Because of it, it is necessary to call avoid crossing perimeters again with new starting point after calling retraction()
        // FIXME Lukas H.: Try to predict if this second calling of avoid crossing perimeters will be needed or not. It could save computations.
        if (last_post_before_retract != this->last_pos() && m_config.avoid_crossing_perimeters) {
            // If in the previous call of m_avoid_crossing_perimeters.travel_to was use_external_mp_once set to true restore this value for next call.
            if (used_external_mp_once)
                m_avoid_crossing_perimeters.use_external_mp_once();
            travel = m_avoid_crossing_perimeters.travel_to(*this, point);
            // If state of use_external_mp_once was changed reset it to right value.
            if (used_external_mp_once)
                m_avoid_crossing_perimeters.reset_once_modifiers();
        }
    } else
        // Reset the wipe path when traveling, so one would not wipe along an old path.
        m_wipe.reset_path();

    // use G1 because we rely on paths being straight (G0 may make round paths)
    if (travel.size() >= 2) {
        for (size_t i = 1; i < travel.size(); ++ i)
            gcode += m_writer.travel_to_xy(this->point_to_gcode(travel.points[i]), comment);
        this->set_last_pos(travel.points.back());
    }
    return gcode;
}

bool GCode::needs_retraction(const Polyline &travel, ExtrusionRole role)
{
    if (travel.length() < scale_(EXTRUDER_CONFIG(retract_before_travel))) {
        // skip retraction if the move is shorter than the configured threshold
        return false;
    }

    if (role == erSupportMaterial) {
        const SupportLayer* support_layer = dynamic_cast<const SupportLayer*>(m_layer);
        //FIXME support_layer->support_islands.contains should use some search structure!
        if (support_layer != NULL && ! intersection_pl(travel, support_layer->support_islands).empty())
            // skip retraction if this is a travel move inside a support material island
            //FIXME not retracting over a long path may cause oozing, which in turn may result in missing material
            // at the end of the extrusion path!
            return false;
    }

    if (m_config.only_retract_when_crossing_perimeters && m_layer != nullptr &&
        m_config.fill_density.value > 0 && m_layer->any_internal_region_slice_contains(travel))
        // Skip retraction if travel is contained in an internal slice *and*
        // internal infill is enabled (so that stringing is entirely not visible).
        //FIXME any_internal_region_slice_contains() is potentionally very slow, it shall test for the bounding boxes first.
        return false;

    // retract if only_retract_when_crossing_perimeters is disabled or doesn't apply
    return true;
}

std::string GCode::retract(bool toolchange)
{
    std::string gcode;

    if (m_writer.extruder() == nullptr)
        return gcode;

    // wipe (if it's enabled for this extruder and we have a stored wipe path)
    if (EXTRUDER_CONFIG(wipe) && m_wipe.has_path()) {
        gcode += toolchange ? m_writer.retract_for_toolchange(true) : m_writer.retract(true);
        gcode += m_wipe.wipe(*this, toolchange);
    }

    /*  The parent class will decide whether we need to perform an actual retraction
        (the extruder might be already retracted fully or partially). We call these
        methods even if we performed wipe, since this will ensure the entire retraction
        length is honored in case wipe path was too short.  */
    gcode += toolchange ? m_writer.retract_for_toolchange() : m_writer.retract();

    gcode += m_writer.reset_e();
    if (m_writer.extruder()->retract_length() > 0 || m_config.use_firmware_retraction)
        gcode += m_writer.lift();

    return gcode;
}

std::string GCode::set_extruder(unsigned int extruder_id, double print_z)
{
    if (!m_writer.need_toolchange(extruder_id))
        return "";

    // if we are running a single-extruder setup, just set the extruder and return nothing
    if (!m_writer.multiple_extruders) {
        m_placeholder_parser.set("current_extruder", extruder_id);

        std::string gcode;
        // Append the filament start G-code.
        const std::string &start_filament_gcode = m_config.start_filament_gcode.get_at(extruder_id);
        if (! start_filament_gcode.empty()) {
            // Process the start_filament_gcode for the filament.
            DynamicConfig config;
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(extruder_id)));
            gcode += this->placeholder_parser_process("start_filament_gcode", start_filament_gcode, extruder_id, &config);
            check_add_eol(gcode);
        }
        gcode += m_writer.toolchange(extruder_id);
        return gcode;
    }

    // prepend retraction on the current extruder
    std::string gcode = this->retract(true);

    // Always reset the extrusion path, even if the tool change retract is set to zero.
    m_wipe.reset_path();

    if (m_writer.extruder() != nullptr) {
        // Process the custom end_filament_gcode. set_extruder() is only called if there is no wipe tower
        // so it should not be injected twice.
        unsigned int        old_extruder_id     = m_writer.extruder()->id();
        const std::string  &end_filament_gcode  = m_config.end_filament_gcode.get_at(old_extruder_id);
        if (! end_filament_gcode.empty()) {
            gcode += placeholder_parser_process("end_filament_gcode", end_filament_gcode, old_extruder_id);
            check_add_eol(gcode);
        }
    }


    // If ooze prevention is enabled, park current extruder in the nearest
    // standby point and set it to the standby temperature.
    if (m_ooze_prevention.enable && m_writer.extruder() != nullptr)
        gcode += m_ooze_prevention.pre_toolchange(*this);

    const std::string& toolchange_gcode = m_config.toolchange_gcode.value;
    std::string toolchange_gcode_parsed;

    // Process the custom toolchange_gcode. If it is empty, insert just a Tn command.
    if (!toolchange_gcode.empty()) {
        DynamicConfig config;
        config.set_key_value("previous_extruder", new ConfigOptionInt((int)(m_writer.extruder() != nullptr ? m_writer.extruder()->id() : -1 )));
        config.set_key_value("next_extruder",     new ConfigOptionInt((int)extruder_id));
        config.set_key_value("layer_num",         new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z",           new ConfigOptionFloat(print_z));
        config.set_key_value("toolchange_z",      new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z",       new ConfigOptionFloat(m_max_layer_z));
        toolchange_gcode_parsed = placeholder_parser_process("toolchange_gcode", toolchange_gcode, extruder_id, &config);
        gcode += toolchange_gcode_parsed;
        check_add_eol(gcode);
    }

    // We inform the writer about what is happening, but we may not use the resulting gcode.
    std::string toolchange_command = m_writer.toolchange(extruder_id);
    if (! custom_gcode_changes_tool(toolchange_gcode_parsed, m_writer.toolchange_prefix(), extruder_id))
        gcode += toolchange_command;
    else {
        // user provided his own toolchange gcode, no need to do anything
    }

    // Set the temperature if the wipe tower didn't (not needed for non-single extruder MM)
    if (m_config.single_extruder_multi_material && !m_config.wipe_tower) {
        int temp = (m_layer_index <= 0 ? m_config.first_layer_temperature.get_at(extruder_id) :
                                         m_config.temperature.get_at(extruder_id));

        gcode += m_writer.set_temperature(temp, false);
    }

    m_placeholder_parser.set("current_extruder", extruder_id);

    // Append the filament start G-code.
    const std::string &start_filament_gcode = m_config.start_filament_gcode.get_at(extruder_id);
    if (! start_filament_gcode.empty()) {
        // Process the start_filament_gcode for the new filament.
        DynamicConfig config;
        config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(extruder_id)));
        gcode += this->placeholder_parser_process("start_filament_gcode", start_filament_gcode, extruder_id, &config);
        check_add_eol(gcode);
    }
    // Set the new extruder to the operating temperature.
    if (m_ooze_prevention.enable)
        gcode += m_ooze_prevention.post_toolchange(*this);

    return gcode;
}

// convert a model-space scaled point into G-code coordinates
Vec2d GCode::point_to_gcode(const Point &point) const
{
    Vec2d extruder_offset = EXTRUDER_CONFIG(extruder_offset);
    return unscaled<double>(point) + m_origin - extruder_offset;
}

Vec2d GCode::point_to_gcode_quantized(const Point &point) const
{
    Vec2d p = this->point_to_gcode(point);
    return { GCodeFormatter::quantize_xyzf(p.x()), GCodeFormatter::quantize_xyzf(p.y()) };
}

// convert a model-space scaled point into G-code coordinates
Point GCode::gcode_to_point(const Vec2d &point) const
{
    Vec2d extruder_offset = EXTRUDER_CONFIG(extruder_offset);
    return Point(
        scale_(point(0) - m_origin(0) + extruder_offset(0)),
        scale_(point(1) - m_origin(1) + extruder_offset(1)));
}

// Goes through by_region std::vector and returns reference to a subvector of entities, that are to be printed
// during infill/perimeter wiping, or normally (depends on wiping_entities parameter)
// Fills in by_region_per_copy_cache and returns its reference.
const std::vector<GCode::ObjectByExtruder::Island::Region>& GCode::ObjectByExtruder::Island::by_region_per_copy(std::vector<Region> &by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities) const
{
    bool has_overrides = false;
    for (const auto& reg : by_region)
        if (! reg.infills_overrides.empty() || ! reg.perimeters_overrides.empty()) {
            has_overrides = true;
            break;
        }

    // Data is cleared, but the memory is not.
    by_region_per_copy_cache.clear();

    if (! has_overrides)
        // Simple case. No need to copy the regions.
        return wiping_entities ? by_region_per_copy_cache : this->by_region;

    // Complex case. Some of the extrusions of some object instances are to be printed first - those are the wiping extrusions.
    // Some of the extrusions of some object instances are printed later - those are the clean print extrusions.
    // Filter out the extrusions based on the infill_overrides / perimeter_overrides:

    for (const auto& reg : by_region) {
        by_region_per_copy_cache.emplace_back(); // creates a region in the newly created Island

        // Now we are going to iterate through perimeters and infills and pick ones that are supposed to be printed
        // References are used so that we don't have to repeat the same code
        for (int iter = 0; iter < 2; ++iter) {
            const ExtrusionEntitiesPtr&										entities    = (iter ? reg.infills : reg.perimeters);
            ExtrusionEntitiesPtr&   										target_eec  = (iter ? by_region_per_copy_cache.back().infills : by_region_per_copy_cache.back().perimeters);
            const std::vector<const WipingExtrusions::ExtruderPerCopy*>& 	overrides   = (iter ? reg.infills_overrides : reg.perimeters_overrides);

            // Now the most important thing - which extrusion should we print.
            // See function ToolOrdering::get_extruder_overrides for details about the negative numbers hack.
            if (wiping_entities) {
                // Apply overrides for this region.
                for (unsigned int i = 0; i < overrides.size(); ++ i) {
                    const WipingExtrusions::ExtruderPerCopy *this_override = overrides[i];
                    // This copy (aka object instance) should be printed with this extruder, which overrides the default one.
                    if (this_override != nullptr && (*this_override)[copy] == int(extruder))
                        target_eec.emplace_back(entities[i]);
                }
            } else {
                // Apply normal extrusions (non-overrides) for this region.
                unsigned int i = 0;
                for (; i < overrides.size(); ++ i) {
                    const WipingExtrusions::ExtruderPerCopy *this_override = overrides[i];
                    // This copy (aka object instance) should be printed with this extruder, which shall be equal to the default one.
                    if (this_override == nullptr || (*this_override)[copy] == -int(extruder)-1)
                        target_eec.emplace_back(entities[i]);
                }
                for (; i < entities.size(); ++ i)
                    target_eec.emplace_back(entities[i]);
            }
        }
    }
    return by_region_per_copy_cache;
}

// This function takes the eec and appends its entities to either perimeters or infills of this Region (depending on the first parameter)
// It also saves pointer to ExtruderPerCopy struct (for each entity), that holds information about which extruders should be used for which copy.
void GCode::ObjectByExtruder::Island::Region::append(const Type type, const ExtrusionEntityCollection* eec, const WipingExtrusions::ExtruderPerCopy* copies_extruder)
{
    // We are going to manipulate either perimeters or infills, exactly in the same way. Let's create pointers to the proper structure to not repeat ourselves:
    ExtrusionEntitiesPtr*									perimeters_or_infills;
    std::vector<const WipingExtrusions::ExtruderPerCopy*>* 	perimeters_or_infills_overrides;

    switch (type) {
    case PERIMETERS:
        perimeters_or_infills 			= &perimeters;
        perimeters_or_infills_overrides = &perimeters_overrides;
        break;
    case INFILL:
        perimeters_or_infills 			= &infills;
        perimeters_or_infills_overrides = &infills_overrides;
        break;
    default:
    	throw Slic3r::InvalidArgument("Unknown parameter!");
    }

    // First we append the entities, there are eec->entities.size() of them:
    size_t old_size = perimeters_or_infills->size();
    size_t new_size = old_size + (eec->can_reverse() ? eec->entities.size() : 1);
    perimeters_or_infills->reserve(new_size);
    if (eec->can_reverse()) {
        for (auto* ee : eec->entities)
            perimeters_or_infills->emplace_back(ee);
    } else
        perimeters_or_infills->emplace_back(const_cast<ExtrusionEntityCollection*>(eec));

    if (copies_extruder != nullptr) {
        // Don't reallocate overrides if not needed.
        // Missing overrides are implicitely considered non-overridden.
        perimeters_or_infills_overrides->reserve(new_size);
        perimeters_or_infills_overrides->resize(old_size, nullptr);
        perimeters_or_infills_overrides->resize(new_size, copies_extruder);
    }
}

}   // namespace Slic3r
