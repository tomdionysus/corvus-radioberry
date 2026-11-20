// SPDX-License-Identifier: GPL-2.0-or-later
//
// The pruned variable CIC (rx_cic_int.sv and rx_varcomb.sv) against the
// exact one (their versions at 12c6864, modules rx_cic_int_x and
// rx_varcomb_x, built from git by tools/sim.sh), NR receivers each with
// its own tones and noise, at each decimation in turn. Both engines run
// the same schedule, so each last-comb result of one comes with the
// other's; their difference before the output rounding, in output LSBs
// (bit 27 of the 43), is the pruning error. After each change of
// decimation the first outputs are left out (the integrators' old state
// takes the combs a few outputs to cancel). Prints, per decimation, the
// rms and worst error, how many rounded outputs differ, and PASS when
// every rms is at most 0.144 LSB (the tolerance: a quarter of the output
// rounding's variance).
//
//   +N=<outputs>   outputs per decimation and receiver (default 400)
`timescale 1ps / 1ps

module cic_prune_tb;
    localparam int NR = 6;
    localparam int HALF_2X = 3255;  // ps
    logic clock = 1'b0, clock_2x = 1'b1;
    always #(HALF_2X) clock_2x = ~clock_2x;
    always #(2 * HALF_2X) clock = ~clock;

    logic       rst_all = 1'b1;
    logic [5:0] decimation = 6'd40;

    // Inputs: two tones and noise for each receiver, all different, near
    // full scale (18 bits).
    localparam real TWO_PI = 6.283185307179586;
    int unsigned ph1 [0:NR-1];
    int unsigned ph2 [0:NR-1];
    logic [31:0] lfsr = 32'hC1C0_2026;
    logic signed [17:0] mix_i [0:NR-1];
    logic signed [17:0] mix_q [0:NR-1];
    initial for (int r = 0; r < NR; r++) begin
        ph1[r] = 0;
        ph2[r] = 0;
        mix_i[r] = '0;
        mix_q[r] = '0;
    end
    always @(posedge clock) begin
        lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
        for (int r = 0; r < NR; r++) begin
            real a, b;
            int ni, nq;
            ph1[r] <= ph1[r] + 32'd2236962 + 32'(r) * 32'd559241;     // 40 kHz and up
            ph2[r] <= ph2[r] - 32'd447392 - 32'(r) * 32'd111848;      // -8 kHz and down
            a = TWO_PI * real'(ph1[r]) / 4294967296.0;
            b = TWO_PI * real'(ph2[r]) / 4294967296.0;
            ni = $signed(14'(lfsr >> (2 * r)));
            nq = $signed(14'(lfsr >> (2 * r + 14)));
            mix_i[r] <= 18'($rtoi(70000.0 * $cos(a) + 40000.0 * $cos(b)) + ni);
            mix_q[r] <= 18'($rtoi(70000.0 * $sin(a) + 40000.0 * $sin(b)) + nq);
        end
    end

    // The exact pair and the pruned pair.
    logic [43*NR-1:0] xi_i, xi_q;
    logic [25*NR-1:0] pi_i, pi_q;
    logic             x_rdy [0:NR-1];
    logic             p_rdy [0:NR-1];
    logic             x_strobe, p_strobe;
    logic [16*NR-1:0] x_oi, x_oq, p_oi, p_oq;
    genvar g;
    generate
        for (g = 0; g < NR; g++) begin : RX
            logic [42:0] ii, iq;
            logic [24:0] ji, jq;
            rx_cic_int_x #(.CICRATE(5)) x (
                .rst_all(rst_all), .clock(clock), .mixdata_I(mix_i[g]), .mixdata_Q(mix_q[g]),
                .int_strobe(x_rdy[g]), .int_i(ii), .int_q(iq));
            rx_cic_int p (
                .rst_all(rst_all), .clock(clock), .decimation(decimation),
                .mixdata_I(mix_i[g]), .mixdata_Q(mix_q[g]),
                .int_strobe(p_rdy[g]), .int_i(ji), .int_q(jq));
            assign xi_i[43*g +: 43] = ii;
            assign xi_q[43*g +: 43] = iq;
            assign pi_i[25*g +: 25] = ji;
            assign pi_q[25*g +: 25] = jq;
        end
    endgenerate
    rx_varcomb_x #(.NR(NR), .CICRATE(5)) xc (
        .clock(clock), .clock_2x(clock_2x), .rst_all(rst_all), .decimation(decimation),
        .in_strobe(x_rdy[0]), .int_i(xi_i), .int_q(xi_q),
        .out_strobe(x_strobe), .out_i(x_oi), .out_q(x_oq));
    rx_varcomb #(.NR(NR)) pc (
        .clock(clock), .clock_2x(clock_2x), .rst_all(rst_all), .decimation(decimation),
        .in_strobe(p_rdy[0]), .int_i(pi_i), .int_q(pi_q),
        .out_strobe(p_strobe), .out_i(p_oi), .out_q(p_oq));

    // The first CICs' comb output registers start unknown in a simulator
    // (the FPGA's at zero).
`define ZERO_STAGE(C, I) \
    C.cic_stages[I].cic_comb_inst.out_data = '0;
`define ZERO_CIC3(C) `ZERO_STAGE(C, 0) `ZERO_STAGE(C, 1) `ZERO_STAGE(C, 2)
`define ZERO_RX(K) \
    `ZERO_CIC3(RX[K].x.cic_inst_I2) `ZERO_CIC3(RX[K].x.cic_inst_Q2)
    initial begin
        #1;
        `ZERO_RX(0) `ZERO_RX(1) `ZERO_RX(2) `ZERO_RX(3) `ZERO_RX(4) `ZERO_RX(5)
    end

    // At each last-comb result: the error before rounding, in output LSBs.
    // The exact engine's result is ready in its v3 cycle, the pruned one's
    // (its difference registered) a cycle later, in v4.
    int shift;
    always_comb case (decimation)
        6'd5: shift = 15; 6'd10: shift = 10; 6'd20: shift = 5; default: shift = 0;
    endcase
    logic counting = 1'b0;
    real  se, emax, sm;
    int   ne, bad;
    logic        xr_v = 1'b0;
    logic [2:0]  xr_s;
    logic [42:0] xr_i, xr_q;
    always @(posedge clock_2x) begin
        if (counting && xr_v) begin
            logic [42:0] pe_i, pe_q;
            real di, dq;
            if (!pc.v4 || pc.k4 != 3'd4 || pc.s4 != xr_s) begin
                $display("FAIL: engines out of step at %0t", $time);
                $finish;
            end
            pe_i = {pc.new_i, 20'd0};
            pe_q = {pc.new_q, 20'd0};
            di = real'($signed(xr_i - pe_i)) / 134217728.0;    // 2^27
            dq = real'($signed(xr_q - pe_q)) / 134217728.0;
            se = se + di * di + dq * dq;
            sm = sm + di + dq;
            ne = ne + 2;
            if (di > emax) emax = di;
            if (-di > emax) emax = -di;
            if (dq > emax) emax = dq;
            if (-dq > emax) emax = -dq;
        end
        xr_v <= xc.v3 && xc.k3 == 3'd4;
        xr_s <= xc.s3;
        xr_i <= xc.new_i << shift;
        xr_q <= xc.new_q << shift;
    end
    // The first CIC alone: its last comb before rounding, against cic.v's,
    // in its output LSBs (bit 9 of 25), on every clock (each value is
    // held for five).
    real  se1, emax1;
    int   ne1;
    always @(posedge clock) begin
        if (counting) begin
            for (int r = 0; r < NR; r++) begin
                logic [24:0] xa, pa;
                real e;
                case (r)
                    0: begin xa = RX[0].x.cic_inst_I2.comb_data[3]; pa = {RX[0].p.di3, 6'd0}; end
                    1: begin xa = RX[1].x.cic_inst_I2.comb_data[3]; pa = {RX[1].p.di3, 6'd0}; end
                    2: begin xa = RX[2].x.cic_inst_I2.comb_data[3]; pa = {RX[2].p.di3, 6'd0}; end
                    3: begin xa = RX[3].x.cic_inst_I2.comb_data[3]; pa = {RX[3].p.di3, 6'd0}; end
                    4: begin xa = RX[4].x.cic_inst_I2.comb_data[3]; pa = {RX[4].p.di3, 6'd0}; end
                    default: begin xa = RX[5].x.cic_inst_I2.comb_data[3]; pa = {RX[5].p.di3, 6'd0}; end
                endcase
                e = real'($signed(xa - pa)) / 512.0;
                se1 = se1 + e * e;
                ne1 = ne1 + 1;
                if (e > emax1) emax1 = e;
                if (-e > emax1) emax1 = -e;
            end
        end
    end

    // Rounded outputs that differ, at the filter's reading (the strobe),
    // and the mean of the difference: a DC offset the filter would pass.
    real sr;
    int  nr;
    always @(posedge clock) begin
        if (counting && x_strobe) begin
            for (int r = 0; r < NR; r++) begin
                if (x_oi[16*r +: 16] != p_oi[16*r +: 16]) bad = bad + 1;
                if (x_oq[16*r +: 16] != p_oq[16*r +: 16]) bad = bad + 1;
                sr = sr + real'($signed(16'(p_oi[16*r +: 16] - x_oi[16*r +: 16])))
                        + real'($signed(16'(p_oq[16*r +: 16] - x_oq[16*r +: 16])));
                nr = nr + 2;
            end
        end
    end

    int n_out;
    int fails = 0;
    initial begin
        if (!$value$plusargs("N=%d", n_out)) n_out = 400;
        repeat (50) @(posedge clock);
        for (int d = 40; d >= 5; d = d / 2) begin
            real rms;
            rst_all = 1'b1;
            decimation = 6'(d);
            repeat (10) @(posedge clock);
            rst_all = 1'b0;
            // Settle: the integrators' old state cancels in a few outputs.
            repeat (12) @(posedge x_strobe);
            se = 0.0; emax = 0.0; sm = 0.0; ne = 0; bad = 0;
            se1 = 0.0; emax1 = 0.0; ne1 = 0;
            sr = 0.0; nr = 0;
            counting = 1'b1;
            repeat (n_out) @(posedge x_strobe);
            counting = 1'b0;
            rms = $sqrt(se / ne);
            $display("decimation %0d: %0d values, error rms %f LSB, mean %f LSB, worst %f LSB; %0d of %0d rounded outputs differ",
                     d, ne, rms, sm / ne, emax, bad, ne);
            $display("  first CIC (I): %0d values, error rms %f LSB, worst %f LSB",
                     ne1, $sqrt(se1 / ne1), emax1);
            $display("  rounded outputs, pruned less exact: mean %f LSB over %0d", sr / nr, nr);
            if (!(rms <= 0.144)) fails = fails + 1;
            if (!($sqrt(se1 / ne1) <= 0.144)) fails = fails + 1;
        end
        if (fails == 0) $display("PASS");
        else $display("FAIL");
        $finish;
    end
endmodule
