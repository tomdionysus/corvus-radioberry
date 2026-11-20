// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Hermes-Lite 2 core's receiver_nco up to its filter: its two CIC
// stages, unchanged, with the filter's input as the output. For the
// shared receive filters (firshared.sv, firsharedfold.sv), connected in
// radio.sv.
module rx_cic #(
    parameter CICRATE = 5,
    parameter REGISTER_OUTPUT = 0   // unused: kept for radio.v's instances
) (
    input  logic               rst_all,
    input  logic               clock,       // 76.8 MHz
    input  logic               clock_2x,    // unused
    input  logic [5:0]         rate,
    input  logic signed [17:0] mixdata_I,
    input  logic signed [17:0] mixdata_Q,
    output logic               out_strobe,
    output logic signed [15:0] out_data_I,
    output logic signed [15:0] out_data_Q,
    output logic [33:0]        debug
);
    localparam VARCICWIDTH = (CICRATE == 10) ? 36 : (CICRATE == 13) ? 36 : (CICRATE == 5) ? 43 : 39;
    localparam ACCWIDTH = (CICRATE == 10) ? 28 : (CICRATE == 13) ? 30 : (CICRATE == 5) ? 25 : 27;

    wire decimA_avail;
    wire signed [15:0] decimA_real, decimA_imag;

    assign debug = {decimA_avail, decimA_real, out_strobe, out_data_I};

    cic #(.STAGES(3), .DECIMATION(CICRATE), .IN_WIDTH(18), .ACC_WIDTH(ACCWIDTH), .OUT_WIDTH(16))
    cic_inst_I2 (.rst_all(rst_all), .clock(clock), .in_strobe(1'b1), .out_strobe(decimA_avail),
                 .in_data(mixdata_I), .out_data(decimA_real));
    cic #(.STAGES(3), .DECIMATION(CICRATE), .IN_WIDTH(18), .ACC_WIDTH(ACCWIDTH), .OUT_WIDTH(16))
    cic_inst_Q2 (.rst_all(rst_all), .clock(clock), .in_strobe(1'b1), .out_strobe(),
                 .in_data(mixdata_Q), .out_data(decimA_imag));

    varcic #(.STAGES(5), .IN_WIDTH(16), .ACC_WIDTH(VARCICWIDTH), .OUT_WIDTH(16), .CICRATE(CICRATE))
    varcic_inst_I1 (.rst_all(rst_all), .clock(clock), .in_strobe(decimA_avail), .decimation(rate),
                    .out_strobe(out_strobe), .in_data(decimA_real), .out_data(out_data_I));
    varcic #(.STAGES(5), .IN_WIDTH(16), .ACC_WIDTH(VARCICWIDTH), .OUT_WIDTH(16), .CICRATE(CICRATE))
    varcic_inst_Q1 (.rst_all(rst_all), .clock(clock), .in_strobe(decimA_avail), .decimation(rate),
                    .out_strobe(), .in_data(decimA_imag), .out_data(out_data_Q));
endmodule
