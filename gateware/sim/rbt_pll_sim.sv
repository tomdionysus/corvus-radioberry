// SPDX-License-Identifier: GPL-2.0-or-later
//
// Simulation stand-in for rbt_pll: passes the input clock through, makes
// a clock of twice its rate, the same shifted later by C3_PHASE_PS
// picoseconds, and reports lock after a few cycles.
`timescale 1ns / 1ps
module rbt_pll #(
    parameter string C3_PHASE_PS = "0"
) (
    input  logic inclk0,
    output logic c0,
    output logic c1,
    output logic c2,
    output logic c3,
    output logic locked
);

    assign c0 = inclk0;

    initial c1 = 1'b0;
    always @(inclk0) begin
        c1 = 1'b1;
        #3.255 c1 = 1'b0;
    end

    real shift_ns;
    initial shift_ns = C3_PHASE_PS.atoi() / 1000.0;
    initial c3 = 1'b0;
    always @(c1) c3 <= #(shift_ns) c1;

    initial c2 = 1'b0;
    always #4.069 c2 = ~c2;

    logic [3:0] n = 4'd0;
    always @(posedge inclk0) if (n != 4'd15) n <= n + 4'd1;
    assign locked = (n == 4'd15);

endmodule
