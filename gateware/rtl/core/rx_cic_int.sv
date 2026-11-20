// SPDX-License-Identifier: GPL-2.0-or-later
//
// One receiver's decimation up to the variable CIC's combs: the Hermes-Lite
// 2 core's first CIC (cic.v: 3 stages, decimating by 5, 25 bits) and the
// five integrators of its variable CIC (varcic.v), their registers pruned
// (Hogenauer, his error budget less two bits;
// TODO/2026-11-17-core-takeover.md stages 2b and 2c). The combs, the
// variable decimation's control and the output rounding are shared by all
// the receivers (rx_varcomb.sv), which reads the last integrator.
//
// cic.v: a counter of the input clocks raises the output strobe for one
// clock in five; the integrators add on every clock, each the stage
// before's value from before the clock; the combs take theirs at the end
// of the strobe's clock (out = in - prev, prev = in), the first from the
// last integrator; the output, not registered, is the last comb's bits
// 24..9 rounded by bit 8. Here the integrators keep all 25 bits (pruning
// them would drop one or two bits a stage, whose rounding is biased by a
// quarter or an eighth of a step, which the integrators sum: 0.14 output
// LSB of offset measured), and the combs 21, 20 and 19, truncated.
//
// varcic.v keeps 43 bits in every stage and takes its output from bits
// 27..12, 32..17, 37..22 or 42..27 for decimation 5, 10, 20 or 40. Here
// the input is shifted left by 15, 10, 5 or 0 bits instead, which gives
// exactly the same output bits, always from bits 42..27; and each stage
// keeps only its top bits (Hogenauer's register pruning, worked out for
// decimation 40 with his error budget less two bits, in
// TODO/2026-11-17-core-takeover.md stage 2b): integrators 43, 37, 33, 29,
// 25 bits, each taking the stage before's value rounded to its own
// lowest bit. Rounded, not truncated: an integrator sums any bias in its
// input, which truncation's would leave as an offset at the output (0.38
// output LSB measured at decimation 40). Only for the 76.8 MHz clock (the
// first CIC decimating by 5).
module rx_cic_int (
    input  logic               rst_all,
    input  logic               clock,       // 76.8 MHz
    input  logic [5:0]         decimation,  // 40, 20, 10 or 5 (48 to 384 ksps)
    input  logic signed [17:0] mixdata_I,
    input  logic signed [17:0] mixdata_Q,
    output logic               int_strobe,  // the integrators have added a sample
    output logic [24:0]        int_i,       // the last integrator: bits 42..18
    output logic [24:0]        int_q
);
    // ------------------------------------------------------------------
    // The first CIC.
    logic [15:0] sample_no = 16'd0;
    logic        decimA_avail = 1'b0;
    always_ff @(posedge clock) begin
        if (rst_all) begin
            sample_no    <= 16'd0;
            decimA_avail <= 1'b0;
        end else if (sample_no == 16'd4) begin
            sample_no    <= 16'd0;
            decimA_avail <= 1'b1;
        end else begin
            sample_no    <= sample_no + 16'd1;
            decimA_avail <= 1'b0;
        end
    end

    logic [24:0] ci1 = '0, cq1 = '0;   // integrators: bits 24..0
    logic [24:0] ci2 = '0, cq2 = '0;
    logic [24:0] ci3 = '0, cq3 = '0;
    logic [20:0] di1 = '0, dq1 = '0, pi1 = '0, pq1 = '0;   // combs: bits 24..4
    logic [19:0] di2 = '0, dq2 = '0, pi2 = '0, pq2 = '0;   //        bits 24..5
    logic [18:0] di3 = '0, dq3 = '0, pi3 = '0, pq3 = '0;   //        bits 24..6
    always_ff @(posedge clock) begin
        ci1 <= ci1 + 25'(mixdata_I);
        cq1 <= cq1 + 25'(mixdata_Q);
        ci2 <= ci2 + ci1;
        cq2 <= cq2 + cq1;
        ci3 <= ci3 + ci2;
        cq3 <= cq3 + cq2;
        if (decimA_avail) begin
            di1 <= ci3[24:4] - pi1;
            dq1 <= cq3[24:4] - pq1;
            pi1 <= ci3[24:4];
            pq1 <= cq3[24:4];
            di2 <= di1[20:1] - pi2;
            dq2 <= dq1[20:1] - pq2;
            pi2 <= di1[20:1];
            pq2 <= dq1[20:1];
            di3 <= di2[19:1] - pi3;
            dq3 <= dq2[19:1] - pq3;
            pi3 <= di2[19:1];
            pq3 <= dq2[19:1];
        end
    end

    // Bits 24..9 of the last comb, rounded by bit 8, halves to even: the
    // comb keeps only three bits below the output, so an exact half comes
    // one time in eight, and cic.v's rounding of halves up would be biased
    // by a sixteenth of an output LSB (0.047 LSB measured after the
    // variable CIC).
    logic signed [15:0] decimA_real, decimA_imag;
    assign decimA_real = di3[18:3] + 16'(di3[2] & (di3[1] | di3[0] | di3[3]));
    assign decimA_imag = dq3[18:3] + 16'(dq3[2] & (dq3[1] | dq3[0] | dq3[3]));

    // The input, sign-extended to 43 bits and shifted for the decimation.
    logic [42:0] xi, xq;
    always_comb begin
        case (decimation)
            6'd5:    begin xi = 43'(decimA_real) << 15; xq = 43'(decimA_imag) << 15; end
            6'd10:   begin xi = 43'(decimA_real) << 10; xq = 43'(decimA_imag) << 10; end
            6'd20:   begin xi = 43'(decimA_real) << 5;  xq = 43'(decimA_imag) << 5;  end
            default: begin xi = 43'(decimA_real);       xq = 43'(decimA_imag);       end
        endcase
    end

    // Each stage adds the stage before's value from before this clock,
    // rounded to this stage's lowest bit (the dropped bits' top one added
    // in); never reset.
    logic [42:0] i1 = '0, q1 = '0;     // bits 42..0
    logic [36:0] i2 = '0, q2 = '0;     // bits 42..6
    logic [32:0] i3 = '0, q3 = '0;     // bits 42..10
    logic [28:0] i4 = '0, q4 = '0;     // bits 42..14
    logic [24:0] i5 = '0, q5 = '0;     // bits 42..18
    always_ff @(posedge clock) begin
        if (decimA_avail) begin
            i1 <= i1 + xi;
            q1 <= q1 + xq;
            i2 <= i2 + i1[42:6] + 37'(i1[5]);
            q2 <= q2 + q1[42:6] + 37'(q1[5]);
            i3 <= i3 + i2[36:4] + 33'(i2[3]);
            q3 <= q3 + q2[36:4] + 33'(q2[3]);
            i4 <= i4 + i3[32:4] + 29'(i3[3]);
            q4 <= q4 + q3[32:4] + 29'(q3[3]);
            i5 <= i5 + i4[28:4] + 25'(i4[3]);
            q5 <= q5 + q4[28:4] + 25'(q4[3]);
        end
    end

    assign int_strobe = decimA_avail;
    assign int_i = i5;
    assign int_q = q5;
endmodule
