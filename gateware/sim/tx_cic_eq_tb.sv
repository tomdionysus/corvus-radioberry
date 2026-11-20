// SPDX-License-Identifier: GPL-2.0-or-later
//
// rtl/core/tx_cic.sv against the Hermes-Lite 2 core's CicInterpM5.v as the
// images use it (interpolation 200, 20-bit input, 16-bit output, 31 growth
// bits), side by side on the same input samples, a new one on each sample
// request, both outputs compared on every clock. Each input changes on the
// third clock after the request rises, the earliest tx_cic.sv allows. The
// inputs are in turn: full-scale positive and negative DC, full-scale
// alternation, random full-scale signs, random values, and runs of random
// length switching between full-scale DC, zero and random values; the
// largest magnitude each of the original's 51-bit stages reaches is printed
// against the width tx_cic.sv gives it.
//
//   +N=<samples>   input samples of each kind (default 2000)
`timescale 1ns / 1ps

module tx_cic_eq_tb;
    logic clk = 1'b0;
    always #6.51 clk = ~clk;  // 76.8 MHz

    logic signed [19:0] xr = '0, xi = '0;
    logic               a_req, b_req;
    logic signed [15:0] a_yr, a_yi, b_yr, b_yi;
    tx_cic dut (.clock(clk), .req(a_req), .x_real(xr), .x_imag(xi), .y_real(a_yr), .y_imag(a_yi));
    CicInterpM5 #(.RRRR(200), .IBITS(20), .OBITS(16), .GBITS(31)) ref_i (
        .clock(clk), .clock_en(1'b1), .req(b_req), .x_real(xr), .x_imag(xi),
        .y_real(b_yr), .y_imag(b_yi));

    int errors = 0, compared = 0;
    // The largest magnitude of each of the original's stages, as bits.
    longint unsigned mx [0:9];
    initial for (int k = 0; k < 10; k++) mx[k] = 0;
    function automatic longint unsigned mag(input logic signed [50:0] v);
        return (v < 0) ? longint'(-v) : longint'(v);
    endfunction
    always @(negedge clk) begin
        compared = compared + 1;
        if ({a_req, a_yr, a_yi} !== {b_req, b_yr, b_yi}) begin
            if (errors < 10)
                $display("MISMATCH t=%0t: new %b %h %h, ref %b %h %h", $time, a_req, a_yr, a_yi,
                         b_req, b_yr, b_yi);
            errors = errors + 1;
        end
        if (mag(ref_i.x1) > mx[0]) mx[0] = mag(ref_i.x1);
        if (mag(ref_i.x2) > mx[1]) mx[1] = mag(ref_i.x2);
        if (mag(ref_i.x3) > mx[2]) mx[2] = mag(ref_i.x3);
        if (mag(ref_i.x4) > mx[3]) mx[3] = mag(ref_i.x4);
        if (mag(ref_i.x5) > mx[4]) mx[4] = mag(ref_i.x5);
        if (mag(ref_i.y1) > mx[5]) mx[5] = mag(ref_i.y1);
        if (mag(ref_i.y2) > mx[6]) mx[6] = mag(ref_i.y2);
        if (mag(ref_i.y3) > mx[7]) mx[7] = mag(ref_i.y3);
        if (mag(ref_i.y4) > mx[8]) mx[8] = mag(ref_i.y4);
        if (mag(ref_i.y5) > mx[9]) mx[9] = mag(ref_i.y5);
    end

    function automatic int bits_for(input longint unsigned m);
        // Signed bits needed to hold +-m.
        int b = 1;
        while ((longint'(1) << (b - 1)) <= longint'(m)) b++;
        return b;
    endfunction

    // The widths tx_cic.sv keeps: combs 21 to 24 (its 24-bit memory words),
    // x5 none (it cancels against the first integrator, which is x4
    // held), integrators 24, 31, 38, 44, 51.
    function automatic int width_of(input int k);
        case (k)
            0: return 21; 1: return 22; 2: return 23; 3: return 24; 4: return 0;
            5: return 24; 6: return 31; 7: return 38; 8: return 44; default: return 51;
        endcase
    endfunction

    int n, run = 0, mode = 0;
    initial begin
        if (!$value$plusargs("N=%d", n)) n = 2000;
        for (int kind = 0; kind < 6; kind++) begin
            for (int s = 0; s < n; s++) begin
                @(posedge b_req);
                @(posedge clk);
                @(posedge clk);
                #1;
                case (kind)
                    0: begin xr = 20'sh7FFFF; xi = 20'sh7FFFF; end
                    1: begin xr = -20'sh80000; xi = -20'sh80000; end
                    2: begin xr = s[0] ? 20'sh7FFFF : -20'sh80000; xi = s[0] ? -20'sh80000 : 20'sh7FFFF; end
                    3: begin
                        xr = ($urandom % 2) ? 20'sh7FFFF : -20'sh80000;
                        xi = ($urandom % 2) ? 20'sh7FFFF : -20'sh80000;
                    end
                    4: begin xr = 20'($urandom); xi = 20'($urandom); end
                    default: begin
                        if (run == 0) begin
                            run  = 1 + $urandom % 40;
                            mode = $urandom % 4;
                        end
                        run = run - 1;
                        case (mode)
                            0: begin xr = 20'sh7FFFF; xi = -20'sh80000; end
                            1: begin xr = -20'sh80000; xi = 20'sh7FFFF; end
                            2: begin xr = '0; xi = '0; end
                            default: begin xr = 20'($urandom); xi = 20'($urandom); end
                        endcase
                    end
                endcase
            end
        end
        repeat (2000) @(posedge clk);
        for (int k = 0; k < 10; k++)
            if (width_of(k) == 0)
                $display("stage %0d: largest magnitude needs %0d bits, tx_cic.sv keeps none",
                         k + 1, bits_for(mx[k]));
            else
                $display("stage %0d: largest magnitude needs %0d bits, tx_cic.sv keeps %0d", k + 1,
                         bits_for(mx[k]), width_of(k));
        $display("%0d clocks compared, %0d mismatches", compared, errors);
        if (errors == 0 && compared > 6 * n * 200) $display("PASS");
        else $display("FAIL");
        $finish;
    end
endmodule
