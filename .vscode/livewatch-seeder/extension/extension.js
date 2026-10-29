const vscode = require("vscode");

const EXPRESSIONS = [
    ["gyro_x_dps", "gsa200_gyro_x_dps"],
    ["gyro_y_dps", "gsa200_gyro_y_dps"],
    ["gyro_z_dps", "gsa200_gyro_z_dps"],
    ["gyro_x_bias_dps", "gsa200_gyro_x_bias_dps"],
    ["gyro_x_corrected_dps", "gsa200_gyro_x_corrected_dps"],
    ["angle_x_deg", "gsa200_angle_x_deg"],
    ["angle_y_deg", "gsa200_angle_y_deg"],
    ["angle_z_deg", "gsa200_angle_z_deg"],
    ["accel_x_g", "gsa200_accel_x_g"],
    ["accel_y_g", "gsa200_accel_y_g"],
    ["accel_z_g", "gsa200_accel_z_g"],
    ["temperature_c", "gsa200_temperature_c"],
    ["protocol_word0", "gsa200_frame_word0"],
    ["protocol_word1", "gsa200_frame_word1"],
    ["protocol_word2", "gsa200_frame_word2"],
    ["protocol_word3", "gsa200_frame_word3"],
    ["protocol_word4", "gsa200_frame_word4"],
    ["protocol_word5", "gsa200_frame_word5"],
    ["protocol_word6", "gsa200_frame_word6"],
    ["protocol_word7", "gsa200_frame_word7"],
    ["sample_counter", "gsa200_sample_counter"],
    ["bias_calibration_samples", "gsa200_bias_calibration_samples"],
    ["bias_calibration_done", "gsa200_bias_calibration_done"],
    ["bias_calibration_state", "gsa200_bias_calibration_state"],
    ["valid_frames", "gsa200_valid_frames"],
    ["checksum_errors", "gsa200_checksum_errors"],
    ["format_errors", "gsa200_format_errors"],
    ["dropped_frames", "gsa200_dropped_frames"],
    ["speed_reference_rpm", "pcu_speed_reference_rpm"],
    ["speed_feedback_rpm", "pcu_speed_feedback_rpm"],
    ["speed_pcu_differential_rpm", "pcu_speed_pcu_differential_rpm"],
    ["speed_gyro_rpm", "pcu_speed_gyro_rpm"],
    ["speed_feedback_age_ms", "pcu_speed_feedback_age_ms"],
    ["speed_error_rpm", "pcu_speed_error_rpm"],
    ["speed_current_a", "pcu_speed_current_a"],
    ["speed_output_norm", "pcu_speed_output_norm"],
    ["speed_current_raw", "pcu_speed_current_raw"],
    ["speed_control_updates", "pcu_speed_control_updates"],
    ["speed_feedback_timeouts", "pcu_speed_feedback_timeouts"],
    ["speed_command_errors", "pcu_speed_command_errors"]
];

async function seedLiveWatch() {
    const folder = vscode.workspace.workspaceFolders?.[0];
    if (!folder) {
        return;
    }

    const cortexExtension = vscode.extensions.getExtension("marus25.cortex-debug");
    if (!cortexExtension) {
        console.error("[LiveWatchSeeder] Cortex-Debug is not installed.");
        return;
    }

    const cortex = await cortexExtension.activate();
    const provider = cortex?.liveWatchProvider;
    const root = provider?.variables;
    if (!provider || !root) {
        console.error("[LiveWatchSeeder] Cortex-Debug Live Watch provider is unavailable.");
        return;
    }

    for (const [name, expression] of EXPRESSIONS) {
        const existing = root.children?.find((node) => node.expr === expression);
        if (existing) {
            existing.name = name;
        } else {
            root.addChild(name, expression);
        }
    }

    provider.saveState();
    provider.fire();
    console.log(`[LiveWatchSeeder] Ready: ${EXPRESSIONS.length} expressions.`);
}

async function activate(context) {
    context.subscriptions.push(vscode.window.registerUriHandler({
        handleUri: seedLiveWatch
    }));
    await seedLiveWatch();
}

function deactivate() {}

module.exports = { activate, deactivate };
