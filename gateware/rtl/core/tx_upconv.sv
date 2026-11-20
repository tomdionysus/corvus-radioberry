// SPDX-License-Identifier: GPL-2.0-or-later
//
// The transmit up-converter: the baseband I/Q from the interpolating CIC
// (76.8 Msps) shifted to the transmit frequency and given to the AD9866's
// 12-bit DAC. It replaces the Hermes-Lite 2 core's cpl_cordic.v (a 19-stage
// CORDIC) and the core's cut of its output to the DAC's 12 bits (TODO/
// 2026-11-17-core-takeover.md stage 6c).
//
// - The NCO is the original's: a 32-bit phase advanced by the frequency
//   word every clock, held at 0 while the word is 0.
// - Its top 20 bits address sincos.sv (the receive NCOs' sine and cosine
//   tables); sine and cosine are rounded from sign and 18-bit magnitude to
//   18-bit two's complement.
// - The mixer: out = I cos - Q sin, two 16 x 18 products (multiplier
//   elements, their input and output registers).
// - The level: one more product scales the sum to the CORDIC path's level,
//   so a client's drive gives the same DAC level as before: the CORDIC's
//   gain (1.64676) through its 16-bit output and the core's shift by 2,
//   0.102923 of the input on the DAC's 12-bit scale (GAIN, from the
//   simulation's measured table amplitude).
// - The output stage: rounded to 12 bits (the original truncated), with an
//   added triangular dither (two 8-bit uniform values from an LFSR,
//   subtracted) before rounding, and saturated at the 12-bit limits (the
//   original wrapped).
// - The dither is +-1 LSB, switched on or off while running by the dither
//   input, so a control register can turn it off without changing this
//   module. Until a register drives it, the core ties it on.
module tx_upconv #(
    parameter logic [17:0] GAIN  = 18'd26981   // x 2^-18
) (
    input  logic               clk,          // 76.8 MHz
    input  logic               dither,       // 1 on (+-1 LSB), 0 off
    input  logic [31:0]        frequency,    // phase step a clock
    input  logic signed [15:0] in_i,
    input  logic signed [15:0] in_q,
    output logic signed [11:0] dac = '0
);
    // NCO.
    logic [31:0] phase = '0;
    always_ff @(posedge clk) phase <= (frequency == 32'd0) ? 32'd0 : phase + frequency;

    // Sine and cosine, five clocks after the angle.
    logic [18:0] cos_sm, sin_sm;
    sincos sc (.clk(clk), .angle(phase[31:12]), .cos(cos_sm), .sin(sin_sm));

    // The input delayed to meet them, in registers: Quartus would otherwise
    // make the delay a shift register in a memory block.
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic signed [15:0] di [0:4];
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic signed [15:0] dq [0:4];
    initial for (int k = 0; k < 5; k++) begin
        di[k] = '0;
        dq[k] = '0;
    end
    always_ff @(posedge clk) begin
        di[0] <= in_i;
        dq[0] <= in_q;
        for (int k = 1; k < 5; k++) begin
            di[k] <= di[k-1];
            dq[k] <= dq[k-1];
        end
    end

    // Sign and magnitude to two's complement, 18 bits, rounded.
    function automatic logic signed [17:0] to18(input logic [18:0] sm);
        logic [17:0] m;
        m = {1'b0, sm[17:1]} + 18'(sm[0]);
        if (m > 18'd131071) m = 18'd131071;
        return sm[18] ? -$signed(m) : $signed(m);
    endfunction

    // Multipliers: input registers, then products.
    logic signed [15:0] mi = '0, mq = '0;
    logic signed [17:0] mc = '0, ms = '0;
    logic signed [33:0] pc = '0, ps = '0;
    always_ff @(posedge clk) begin
        mi <= di[4];
        mq <= dq[4];
        mc <= to18(cos_sm);
        ms <= to18(sin_sm);
        pc <= mi * mc;
        ps <= mq * ms;
    end

    // The sum, then the level product.
    logic signed [34:0] sum = '0;
    logic signed [17:0] g_in = '0;
    logic signed [36:0] g_out = '0;
    always_ff @(posedge clk) begin
        sum   <= 35'(pc) - 35'(ps);
        g_in  <= sum[34:17];
        g_out <= g_in * $signed({1'b0, GAIN});
    end

    // Dither: a 32-bit LFSR (x^32 + x^22 + x^2 + x + 1) stepped 16 times a
    // clock, so each clock's two 8-bit values are new bits (stepped once, a
    // clock's values are the last clock's shifted by one bit: correlated,
    // and the dither had spurs of its own, measured).
    logic [31:0] lfsr = 32'h1;
    always_ff @(posedge clk) begin
        logic [31:0] l;
        l = lfsr;
        for (int k = 0; k < 16; k++) l = {l[30:0], l[31] ^ l[21] ^ l[1] ^ l[0]};
        lfsr <= l;
    end
    logic signed [9:0] dith;
    assign dith = dither ? (10'(lfsr[7:0]) - 10'(lfsr[15:8])) : 10'sd0;

    // g_out is the output with 18 fractional bits: keep 8 of them, add the
    // dither (8 fractional bits, -1 to +1 LSB), round, saturate.
    logic signed [20:0] v;      // 13 integer bits (with headroom), 8 fractional
    logic signed [20:0] vd;
    logic signed [12:0] r;
    assign v  = g_out[30:10];
    assign vd = v + 21'(dith) + 21'sd128;          // + 0.5 LSB: round
    assign r  = vd[20:8];
    always_ff @(posedge clk) begin
        if (g_out[36:30] != {7{g_out[36]}} || r > 13'sd2047) dac <= g_out[36] ? -12'sd2048 : 12'sd2047;
        else if (r < -13'sd2048)                              dac <= -12'sd2048;
        else                                                  dac <= r[11:0];
    end
endmodule
