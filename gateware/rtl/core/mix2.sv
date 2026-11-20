// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Hermes-Lite 2 core's double mixer (gateware/rtl/nco/mix2.v with its
// nco2.v), the same values on the same clocks (takeover stage 3): one ADC
// stream mixed with two NCOs on alternate clocks of the 153.6 MHz clock,
// giving two receivers' I and Q at 76.8 MHz.
//
// The NCO keeps two phases, stepped on alternate 153.6 MHz clocks by the
// two receivers' phase words; its sine and cosine (rtl/core/sincos.sv)
// multiply the ADC sample, and the products, rounded to 18 bits, go to
// receiver 0's outputs on one clock and receiver 1's on the other.
//
// mix2.v toggles a state register on every 153.6 MHz clock from zero, as
// the images' transmit bus does its sync: Quartus may merge such
// registers into one that drives all three mixers and the bus (in the
// v0.6.0 sweep, one seed missed the 153.6 MHz clock from the bus's sync
// into the NCO phases). Here each mixer's state register is kept as its
// own.
module mix2 #(
    parameter CALCTYPE = 0              // only 0 (sincos.sv)
) (
    input  logic                clk,    // 76.8 MHz (unused: as mix2.v)
    input  logic                clk_2x, // 153.6 MHz
    input  logic                rst,
    input  logic [31:0]         phi0,
    input  logic [31:0]         phi1,
    input  logic signed [11:0]  adc,
    output logic signed [17:0]  mixdata0_i = '0,
    output logic signed [17:0]  mixdata0_q = '0,
    output logic signed [17:0]  mixdata1_i = '0,
    output logic signed [17:0]  mixdata1_q = '0
);
    // Which receiver this 153.6 MHz clock serves.
    (* preserve *) logic state = 1'b0;
    always_ff @(posedge clk_2x) state <= ~state;

    // The NCO (nco2.v): angle1 is the phase being stepped, angle0 the
    // other receiver's, swapped each clock; a reset clears the phase.
    logic [31:0] angle0 = '0, angle1 = '0;
    always_ff @(posedge clk_2x) begin
        angle1 <= angle0 + (state ? phi1 : phi0);
        angle0 <= rst ? 32'd0 : angle1;
    end

    logic [18:0] sin, cos;
    sincos #(.CALCTYPE(CALCTYPE)) sincos_i (
        .clk(clk_2x), .angle(angle1[31:12]), .cos(cos), .sin(sin)
    );

    // Sign and magnitude to two's complement, halved to 18 bits.
    logic [18:0] ssin, scos;
    assign ssin = {sin[18], ~sin[17:0]} + 19'h01;
    assign scos = {cos[18], ~cos[17:0]} + 19'h01;
    logic signed [17:0] ssin_q = '0, scos_q = '0;
    always_ff @(posedge clk_2x) begin
        ssin_q <= sin[18] ? ssin[18:1] : sin[18:1];
        scos_q <= cos[18] ? scos[18:1] : cos[18:1];
    end

    // The products (multipliers with input and output registers), rounded
    // to 18 bits.
    logic signed [17:0] adc_a = '0, ci = '0, cq = '0;
    logic signed [35:0] i_data_d = '0, q_data_d = '0;
    always_ff @(posedge clk_2x) begin
        adc_a    <= 18'(adc);
        ci       <= scos_q;
        cq       <= ssin_q;
        i_data_d <= adc_a * ci;
        q_data_d <= adc_a * cq;
    end
    logic signed [18:0] i_rounded, q_rounded;
    assign i_rounded = i_data_d[28:11] + {17'h00, i_data_d[10]};
    assign q_rounded = q_data_d[28:11] + {17'h00, q_data_d[10]};

    always_ff @(posedge clk_2x) begin
        if (state) begin
            mixdata0_i <= i_rounded[17:0];
            mixdata0_q <= q_rounded[17:0];
        end else begin
            mixdata1_i <= i_rounded[17:0];
            mixdata1_q <= q_rounded[17:0];
        end
    end
endmodule
