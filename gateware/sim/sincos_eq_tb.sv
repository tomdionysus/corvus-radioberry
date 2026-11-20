// SPDX-License-Identifier: GPL-2.0-or-later
//
// rtl/core/sincos.sv against the Hermes-Lite 2 core's sincos.v (module
// sincos_ref, made from the archive by tools/sim.sh), side by side on the
// same angles: random ones, every angle once, then phase ramps of several
// steps as the NCOs make them (one angle a clock, as each nco2 alternates
// two receivers'), both outputs compared on every clock once both are
// known.
//
//   +N=<clocks>   clocks of random angles and of ramps (default 200000)
`timescale 1ns / 1ps

module sincos_eq_tb;
    logic clk = 1'b0;
    always #3.255 clk = ~clk;  // 153.6 MHz

    logic [19:0] angle = '0;
    logic [18:0] a_sin, a_cos, b_sin, b_cos;
    sincos #(.CALCTYPE(0)) dut (.clk(clk), .angle(angle), .cos(a_cos), .sin(a_sin));
    sincos_ref #(.CALCTYPE(0)) ref_i (.clk(clk), .angle(angle), .cos(b_cos), .sin(b_sin));

    int errors = 0, compared = 0;
    always @(negedge clk) begin
        // (Known: no X or Z bit makes the XOR of all the bits unknown.
        // Icarus 12's $isunknown gave 1 here for fully known values.)
        if ((^{b_sin, b_cos}) !== 1'bx) begin
            compared = compared + 1;
            if ({a_sin, a_cos} !== {b_sin, b_cos}) begin
                if (errors < 10)
                    $display("MISMATCH t=%0t: new sin %h cos %h, ref sin %h cos %h",
                             $time, a_sin, a_cos, b_sin, b_cos);
                errors = errors + 1;
            end
        end
    end

    int n;
    logic [31:0] acc0 = '0, acc1 = '0;
    initial begin
        if (!$value$plusargs("N=%d", n)) n = 200000;
        // Random angles.
        repeat (n) begin
            @(posedge clk);
            #0.1 angle = 20'($urandom);
        end
        // Every angle once.
        for (int a = 0; a < (1 << 20); a++) begin
            @(posedge clk);
            #0.1 angle = 20'(a);
        end
        // Two interleaved phase ramps, as nco2 makes them (angle1[31:12]
        // of an accumulator stepped by the phase word), for five random
        // pairs of steps.
        for (int k = 0; k < 5; k++) begin
            logic [31:0] p0, p1;
            p0 = $urandom;
            p1 = $urandom;
            repeat (n / 5) begin
                @(posedge clk);
                #0.1;
                acc0 = acc0 + p0;
                acc1 = acc1 + p1;
                angle = (compared % 2) ? acc1[31:12] : acc0[31:12];
            end
        end
        repeat (20) @(posedge clk);
        $display("%0d clocks compared, %0d mismatches", compared, errors);
        if (errors == 0 && compared > n + (1 << 20)) $display("PASS");
        else $display("FAIL");
        $finish;
    end
endmodule
