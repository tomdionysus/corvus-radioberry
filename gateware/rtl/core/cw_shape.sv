// SPDX-License-Identifier: GPL-2.0-or-later
//
// The CW keying envelope's shape (takeover stage 6d). The radio core's CW
// state machine ramps its level counter by one a clock from 0 to LEVEL_MAX
// on key down and back to 0 on key up (about 4.1 ms at 76.8 MHz); the
// Hermes-Lite 2 core used the counter itself as the amplitude, a straight
// line. Here the counter is the time along a raised cosine of the same
// length and the same peak:
//
//   amp = PEAK * (1 - cos(pi * level / LEVEL_MAX)) / 2
//
// which keeps the key clicks much narrower than the straight line's
// corners (measured: sim/cw_shape_tb.sv, TODO/2026-11-17-core-takeover.md
// stage 6d). The cosine is the receive NCOs' table (sincos.sv), addressed
// over half a turn; the angle is level * ANGLE_K / 2^14, just under half a
// turn at LEVEL_MAX (short of it by 1 part in 10^6). Latency: 10 clocks (the angle 1, the table 5, the
// rest 4).
module cw_shape #(
    parameter logic [18:0] LEVEL_MAX = 19'h4d800,
    parameter logic [15:0] PEAK      = 16'h4d80,    // the straight line's top
    parameter logic [14:0] ANGLE_K   = 15'd27060    // 2^19 * 2^14 / LEVEL_MAX, rounded down
) (
    input  logic        clk,
    input  logic [18:0] level,
    output logic [15:0] amp = '0
);
    // Stage 1: the angle, over half a turn (bit 19 stays clear).
    logic [33:0] ang_p = '0;
    always_ff @(posedge clk) ang_p <= level * ANGLE_K;
    logic [19:0] angle;
    assign angle = {1'b0, ang_p[32:14]};

    // Five clocks: the cosine, sign and 18-bit magnitude.
    logic [18:0] cos_sm, sin_unused;
    sincos sc (.clk(clk), .angle(angle), .cos(cos_sm), .sin(sin_unused));

    // (1 - cos) / 2 on 2^18: 0 at level 0, 2^18 - 1 at the top.
    logic [18:0] one_minus = '0;
    always_ff @(posedge clk)
        one_minus <= cos_sm[18] ? 19'd262143 + 19'(cos_sm[17:0])
                                : 19'd262143 - 19'(cos_sm[17:0]);

    // Scaled to the peak (a multiplier with its registers).
    logic [17:0] om = '0;
    logic [33:0] prod = '0;
    always_ff @(posedge clk) begin
        om   <= one_minus[18:1];
        prod <= om * PEAK;
        amp  <= prod[33:18];
    end
endmodule
