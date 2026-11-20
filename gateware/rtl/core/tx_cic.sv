// SPDX-License-Identifier: GPL-2.0-or-later
//
// The transmit CIC interpolator: the Hermes-Lite 2 core's CicInterpM5.v (by
// James Ahlstrom N2ADR and Phil Harman VK6PH) as the images use it,
// interpolating by 200 from 384 ksps to 76.8 MHz, five stages, 20-bit
// input, 16-bit output, the same values on the same clocks
// (sim/tx_cic_eq_tb.sv compares every clock).
//
// CicInterpM5.v: when its counter reaches 199 the input is taken into x0,
// a sample request goes out and the five pipelined combs step
// (x(k+1) <= xk - dxk, dxk <= xk); on the other clocks x5 is 0; five
// integrators step on every clock; the output is the last integrator's top
// 16 bits rounded by the next. It keeps 51 bits in every stage. Here:
//
// - Each value is only as wide as it can grow (Hogenauer's register growth
//   for an interpolating CIC): combs 21 to 24 bits, integrators 31, 38, 44
//   and 51. A value that fits is the same in its own width as in 51 bits.
// - The fifth comb and the first integrator cancel. x5 is nonzero on one
//   clock a sample and the first integrator sums it, so its sum telescopes:
//   one clock after the counter reaches 199 it becomes the x4 of before,
//   and holds. y1 is loaded with that x4 instead of adding.
// - The comb states (x0 to x4, dx0 to dx3, both channels) live in a memory
//   block, 24 bits a word, and one subtractor steps them in turn, four
//   clocks a stage, in the first 37 clocks of each 200. x0 has two words
//   used in turn, the new input going to one while the other is read.
// - The rounding: the last integrator starts at 2^34, so its top 16 bits
//   are the rounded output and need no adder.
//
// The input is taken after the request, not with it: x_real one clock
// after req rises, x_imag two clocks after. The transmit filter holds its
// output for more than a hundred clocks after req.
module tx_cic (
    input  logic               clock,     // 76.8 MHz
    output logic               req = 1'b0,// requests the next input sample
    input  logic signed [19:0] x_real,
    input  logic signed [19:0] x_imag,
    output logic signed [15:0] y_real,
    output logic signed [15:0] y_imag
);
    localparam int RRRR = 200;

    logic [8:0] counter = '0;

    // Comb state memory. Address {channel, dx, stage}: xk at {c, 0, k}
    // (k = 1..4), x0 at {c, 0, 0} or {c, 0, 5} in turn, dxk at {c, 1, k}.
    logic        we;
    logic [4:0]  waddr, raddr;
    logic [23:0] wdata, q;
    fir_sdpram #(.W(24), .D(32), .AW(5)) state (
        .clk(clock), .we(we), .waddr(waddr), .wdata(wdata), .raddr(raddr), .q(q));

    logic               par = 1'b0;   // the x0 word the next input goes to
    logic               started = 1'b0; // a request has gone out
    logic signed [23:0] a = '0;       // the stage's xk; the input on its way in

    // Stage steps: block b = counter[5:2] (1 to 8, counter 4 to 35) steps
    // channel op[2], stage 3 - op[1:0] (op = b - 1): read xk, read dxk,
    // xk into a, write x(k+1) = a - dxk; the next block's first clock
    // writes dxk = a (op = b - 2).
    logic       in_steps;
    logic [3:0] blk, rop, wop;
    logic [2:0] rk, wk;
    assign in_steps = counter[8:6] == 3'd0;
    assign blk = counter[5:2];
    assign rop = blk - 4'd1;
    assign wop = blk - 4'd2;
    assign rk  = {1'b0, ~rop[1:0]};
    assign wk  = {1'b0, ~wop[1:0]};

    function automatic logic [4:0] x_addr(input logic c, input logic [2:0] k, input logic p);
        return {c, 1'b0, (k == 3'd0) ? (p ? 3'd5 : 3'd0) : k};
    endfunction

    logic rd_blk, wdx_blk, sub;
    assign rd_blk  = in_steps && blk >= 4'd1 && blk <= 4'd8;
    assign wdx_blk = in_steps && blk >= 4'd2 && blk <= 4'd9;
    assign sub     = rd_blk && counter[1:0] == 2'd3;

    always_comb begin
        raddr = '0;
        waddr = '0;
        we    = 1'b0;
        if (counter == 9'(RRRR - 3)) raddr = x_addr(1'b0, 3'd4, 1'b0);
        else if (counter == 9'(RRRR - 2)) raddr = x_addr(1'b1, 3'd4, 1'b0);
        else if (rd_blk && counter[1:0] == 2'd0) raddr = x_addr(rop[2], rk, ~par);
        else if (rd_blk && counter[1:0] == 2'd1) raddr = {rop[2], 1'b1, rk};
        if (started && (counter == 9'd1 || counter == 9'd2)) begin
            we    = 1'b1;
            waddr = x_addr(counter[1], 3'd0, par);
        end else if (sub) begin
            we    = 1'b1;
            waddr = x_addr(rop[2], rk + 3'd1, 1'b0);
        end else if (wdx_blk && counter[1:0] == 2'd0) begin
            we    = 1'b1;
            waddr = {wop[2], 1'b1, wk};
        end
    end
    assign wdata = a - (sub ? q : 24'd0);

    logic signed [23:0] y1 = '0, s1 = '0;
    logic signed [30:0] y2 = '0, s2 = '0;
    logic signed [37:0] y3 = '0, s3 = '0;
    logic signed [43:0] y4 = '0, s4 = '0;
    logic signed [50:0] y5 = 51'sd1 <<< 34, s5 = 51'sd1 <<< 34;

    always_ff @(posedge clock) begin
        if (counter == 9'(RRRR - 1)) begin
            counter <= '0;
            req <= 1'b1;
            par <= ~par;
            started <= 1'b1;
        end else begin
            counter <= counter + 9'd1;
            req <= 1'b0;
        end
        // a: the real x4 (read at 197), the inputs, each stage's xk.
        if (counter == 9'(RRRR - 1) || (rd_blk && counter[1:0] == 2'd2)) a <= q;
        else if (counter == 9'd0) a <= 24'(x_real);
        else if (counter == 9'd1) a <= 24'(x_imag);
        if (started && counter == 9'd0) begin
            y1 <= a;
            s1 <= q;
        end
        y2 <= y2 + 31'(y1);
        y3 <= y3 + 38'(y2);
        y4 <= y4 + 44'(y3);
        y5 <= y5 + 51'(y4);
        s2 <= s2 + 31'(s1);
        s3 <= s3 + 38'(s2);
        s4 <= s4 + 44'(s3);
        s5 <= s5 + 51'(s4);
    end

    assign y_real = y5[50:35];
    assign y_imag = s5[50:35];
endmodule
