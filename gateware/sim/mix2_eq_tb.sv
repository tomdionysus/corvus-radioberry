// SPDX-License-Identifier: GPL-2.0-or-later
//
// rtl/core/mix2.sv (with rtl/core/sincos.sv) against the Hermes-Lite 2
// core's mix2.v, nco2.v and sincos.v (modules mix2_ref, nco2_ref and
// sincos_ref, made from the archive by tools/sim.sh), side by side: the
// same ADC samples (random, 76.8 MHz), the same two phase words (changed
// now and then) and resets, the four outputs compared on every 153.6 MHz
// clock once the reference's are known.
//
//   +N=<clocks>   76.8 MHz clocks (default 200000)
`timescale 1ps / 1ps

module mix2_eq_tb;
    localparam int HALF_2X = 3255;  // ps
    logic clk = 1'b0, clk_2x = 1'b1;
    always #(HALF_2X) clk_2x = ~clk_2x;
    always #(2 * HALF_2X) clk = ~clk;

    logic               rst = 1'b0;
    logic [31:0]        phi0 = 32'd397062997, phi1 = 32'd123456789;
    logic signed [11:0] adc = '0;

    logic signed [17:0] a0i, a0q, a1i, a1q, b0i, b0q, b1i, b1q;
    mix2 #(.CALCTYPE(0)) dut (
        .clk(clk), .clk_2x(clk_2x), .rst(rst), .phi0(phi0), .phi1(phi1), .adc(adc),
        .mixdata0_i(a0i), .mixdata0_q(a0q), .mixdata1_i(a1i), .mixdata1_q(a1q));
    mix2_ref #(.CALCTYPE(0)) ref_i (
        .clk(clk), .clk_2x(clk_2x), .rst(rst), .phi0(phi0), .phi1(phi1), .adc(adc),
        .mixdata0_i(b0i), .mixdata0_q(b0q), .mixdata1_i(b1i), .mixdata1_q(b1q));

    int errors = 0, compared = 0;
    always @(negedge clk_2x) begin
        // (Known: no X or Z bit makes the XOR of all the bits unknown.)
        if ((^{b0i, b0q, b1i, b1q}) !== 1'bx) begin
            compared = compared + 1;
            if ({a0i, a0q, a1i, a1q} !== {b0i, b0q, b1i, b1q}) begin
                if (errors < 10)
                    $display("MISMATCH t=%0t: new %h %h %h %h, ref %h %h %h %h", $time,
                             a0i, a0q, a1i, a1q, b0i, b0q, b1i, b1q);
                errors = errors + 1;
            end
        end
    end

    int n;
    initial begin
        if (!$value$plusargs("N=%d", n)) n = 200000;
        for (int c = 0; c < n; c++) begin
            @(posedge clk);
            #1;
            adc = 12'($urandom);
            if (($urandom % 5000) == 0) phi0 = $urandom;
            if (($urandom % 5000) == 0) phi1 = $urandom;
            rst = (($urandom % 20000) == 0);
        end
        repeat (20) @(posedge clk);
        $display("%0d clocks compared, %0d mismatches", compared, errors);
        if (errors == 0 && compared > n) $display("PASS");
        else $display("FAIL");
        $finish;
    end
endmodule
