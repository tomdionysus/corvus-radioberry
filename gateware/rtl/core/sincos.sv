// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Hermes-Lite 2 core's NCO sine and cosine (gateware/rtl/nco/sincos.v,
// CALCTYPE 0, the one the images build), the same values on the same
// clocks, with the tables' outputs registered (takeover stage 3).
//
// The angle's top 10 bits address a quarter-wave table of coarse sine and
// cosine (256 x 36, magnitudes; the quadrant gives the signs), its low 10
// give a fine sine correction (sincos.v's 512 x 9 table sin_fine.txt); each
// coarse value is corrected by the other times the fine correction (two
// unsigned 18 x 18 products), added or subtracted by the quadrant and the
// fine sign.
//
// The fine corrections are computed, not read: sin_fine.txt holds
// round(sin(2 pi (i + 0.5) / 2^20) (2^17 - 2)) for i = 0..511, a straight
// line to within 0.0007 of a unit, and every entry equals
// (i * 3217 + 3648) >> 12 (checked against the file for all 512; sim/
// sincos_eq_tb.sv compares every angle). One multiplier with its input and
// output registers makes it in the two clocks the table took, so a sincos
// needs one memory block (the coarse table) instead of two.
//
// sincos.v reads both tables into the multipliers straight from the
// memories' outputs: the table to multiplier path is the core's worst
// internal one on the 153.6 MHz clock (one Raspberry Pi 4 image seed
// missed it by 0.171 ns). Here every pipeline register is one the device
// has: the addresses go straight from the angle into the memories'
// address registers (sincos.v registers them a clock earlier), the
// tables' outputs are registered in the memories' output registers, and
// the multipliers keep their input and output registers; so the values
// come on the same clocks. (One multiplier register less, the table
// output to product path missed by 0.322 ns.) sim/sincos_eq_tb.sv
// compares the two on every clock.
module sincos #(
    parameter CALCTYPE = 0              // only 0 (the table of fine corrections)
) (
    input  logic        clk,
    input  logic [19:0] angle,
    output logic [18:0] cos,
    output logic [18:0] sin
);
    // Stage 1: the table addresses, straight from the angle into the
    // memories' address registers (sincos.v registers them first and
    // reads a clock later), and the signs.
    logic [7:0] coarse_addr;
    logic [8:0] fine_addr;
    assign coarse_addr = angle[18] ? ~angle[17:10] : angle[17:10];
    assign fine_addr   = angle[9] ? angle[8:0] : ~angle[8:0];
    // The pipeline's registers are kept as registers: in an instance whose
    // sine or cosine output is unused, Quartus otherwise makes chains of
    // them a shift register in a memory block.
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic sin_sign = 1'b0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic cos_sign = 1'b0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic fine_sign = 1'b0;
    always_ff @(posedge clk) begin
        sin_sign  <= angle[19];
        cos_sign  <= angle[19] ^ angle[18];
        fine_sign <= !angle[9];
    end

    // Stages 1 and 2: the coarse table, read (address registered) and its
    // output registered, as sincos.v's table gives it at its stage 2; the
    // fine product (the multiplier's input and output registers) beside it.
    logic [35:0] coarse_rom [0:255];
    initial $readmemb("sincos_coarse.txt", coarse_rom);
    logic [35:0] coarse_q = '0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic [35:0] coarse_q2 = '0;
    logic [8:0]  fine_i = '0;
    (* multstyle = "dsp" *) logic [20:0] fine_p = '0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic sin_sign2 = 1'b0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic cos_sign2 = 1'b0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic fine_sign2 = 1'b0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic fine_sign3 = 1'b0;
    always_ff @(posedge clk) begin
        coarse_q   <= coarse_rom[coarse_addr];
        fine_i     <= fine_addr;
        coarse_q2  <= coarse_q;
        fine_p     <= fine_i * 12'd3217;
        sin_sign2  <= sin_sign;
        cos_sign2  <= cos_sign;
        fine_sign2 <= fine_sign;
    end
    // sin_fine.txt's entry fine_i, as the table's output register held it.
    logic [20:0] fine_r;
    logic [8:0]  fine_q2;
    assign fine_r  = fine_p + 21'd3648;
    assign fine_q2 = fine_r[20:12];

    // Stage 3: the multipliers' input registers, and the coarse values with
    // their signs, as sincos.v's coarse_sin and coarse_cos (sign and
    // magnitude).
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic [17:0] sin_a = '0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic [17:0] cos_a = '0;
    logic [17:0] fine_b = '0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic [18:0] coarse_sin = '0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic [18:0] coarse_cos = '0;
    always_ff @(posedge clk) begin
        sin_a      <= coarse_q2[35:18];
        cos_a      <= coarse_q2[17:0];
        fine_b     <= {9'd0, fine_q2};
        coarse_sin <= {sin_sign2, coarse_q2[35:18]};
        coarse_cos <= {cos_sign2, coarse_q2[17:0]};
        fine_sign3 <= fine_sign2;
    end

    // Stage 4: the products (the multipliers' output registers), and the
    // coarse values and the sum's operations registered beside them.
    logic [35:0] sin_prod = '0, cos_prod = '0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic [18:0] sc = '0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic [18:0] cc = '0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic sop = 1'b0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic cop = 1'b0;
    always_ff @(posedge clk) begin
        sin_prod <= sin_a * fine_b;
        cos_prod <= cos_a * fine_b;
        sc <= coarse_sin;
        cc <= coarse_cos;
        // sincos.v's tables of operations: the correction is subtracted
        // from the cosine when an odd number of {cos sign, sin sign, fine
        // sign} is clear... (the same eight cases, as XORs).
        cop <= !(coarse_cos[18] ^ coarse_sin[18] ^ fine_sign3);
        sop <= coarse_sin[18] ^ coarse_cos[18] ^ fine_sign3;
    end

    // Stage 5: each coarse value corrected by the other's product.
    logic [9:0] scsf, ccsf;
    assign scsf = sin_prod[26:17];
    assign ccsf = cos_prod[26:17];
    always_ff @(posedge clk) begin
        sin[17:0] <= sop ? (sc[17:0] - {8'd0, ccsf}) : (sc[17:0] + {8'd0, ccsf});
        cos[17:0] <= cop ? (cc[17:0] - {8'd0, scsf}) : (cc[17:0] + {8'd0, scsf});
        sin[18]   <= sc[18];
        cos[18]   <= cc[18];
    end
endmodule
