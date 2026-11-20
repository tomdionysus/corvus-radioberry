// SPDX-License-Identifier: GPL-2.0-or-later
//
// rtl/core/tx_fir.sv against the Hermes-Lite 2 core's FirInterp8_1024.v as
// the images build it (USE_ALTSYNCRAM, Quartus's memory models), side by
// side: calc every 200 clocks (384 ksps at 76.8 MHz, as the transmit CIC
// asks), a new input sample on each request, req and both outputs compared
// on every clock. The inputs are in turn: full-scale positive and negative
// DC, full-scale alternation, random full-scale signs, random values, and
// an impulse every 256 samples (each coefficient reaches the output).
//
//   +N=<samples>   input samples of each kind (default 2000)
`timescale 1ns / 1ps

module tx_fir_eq_tb;
    logic clk = 1'b0;
    always #6.51 clk = ~clk;  // 76.8 MHz

    logic               calc = 1'b0;
    logic signed [15:0] xr = '0, xi = '0;
    logic               a_req, b_req;
    logic signed [19:0] a_yr, a_yi, b_yr, b_yi;
    tx_fir dut (.clock(clk), .calc(calc), .req(a_req), .x_real(xr), .x_imag(xi),
                .y_real(a_yr), .y_imag(a_yi));
    FirInterp8_1024 ref_i (.clock(clk), .calc(calc), .req(b_req), .x_real(xr), .x_imag(xi),
                           .y_real(b_yr), .y_imag(b_yi));

    // The original's write enable has no initial value; the FPGA's is 0.
    initial ref_i.we = 1'b0;

    int ccount = 0;
    always @(posedge clk) begin
        ccount <= (ccount == 199) ? 0 : ccount + 1;
        calc   <= (ccount == 199);
    end

    int errors = 0, compared = 0, nonzero = 0;
    always @(negedge clk) begin
        compared = compared + 1;
        if (b_yr != 0 || b_yi != 0) nonzero = nonzero + 1;
        if ({a_req, a_yr, a_yi} !== {b_req, b_yr, b_yi}) begin
            if (errors < 10)
                $display("MISMATCH t=%0t: new %b %h %h, ref %b %h %h", $time, a_req, a_yr, a_yi,
                         b_req, b_yr, b_yi);
            errors = errors + 1;
        end
    end

    int n;
    initial begin
        if (!$value$plusargs("N=%d", n)) n = 2000;
        for (int kind = 0; kind < 6; kind++) begin
            for (int s = 0; s < n; s++) begin
                @(posedge b_req);
                #1;
                case (kind)
                    0: begin xr = 16'sh7FFF; xi = 16'sh7FFF; end
                    1: begin xr = -16'sh8000; xi = -16'sh8000; end
                    2: begin xr = s[0] ? 16'sh7FFF : -16'sh8000; xi = s[0] ? -16'sh8000 : 16'sh7FFF; end
                    3: begin
                        xr = ($urandom % 2) ? 16'sh7FFF : -16'sh8000;
                        xi = ($urandom % 2) ? 16'sh7FFF : -16'sh8000;
                    end
                    4: begin xr = 16'($urandom); xi = 16'($urandom); end
                    default: begin
                        xr = (s % 256 == 0) ? 16'sh7FFF : '0;
                        xi = (s % 256 == 128) ? -16'sh8000 : '0;
                    end
                endcase
            end
        end
        repeat (4000) @(posedge clk);
        $display("%0d clocks compared (%0d with an output not zero), %0d mismatches",
                 compared, nonzero, errors);
        if (errors == 0 && compared > 6 * n * 8 * 200) $display("PASS");
        else $display("FAIL");
        $finish;
    end
endmodule
