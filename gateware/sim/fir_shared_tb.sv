// SPDX-License-Identifier: GPL-2.0-or-later
//
// The shared receive filter against the original: NR Hermes-Lite 2
// receivers (receiver_nco, each with its own filter) beside NR rx_cic and
// the one shared filter, every receiver with its own input. Each output of
// the receivers 0..LAST is compared with the original's, value for value,
// after the first (the original's first output comes before its filter
// has summed anything). With REF_FOLD, design A+B (firsharedfold.sv)
// against receivers with design A's filter (firfold.sv), which computes
// the same sums.
//
//   +RATE=40|20|10|5   48, 96, 192 or 384 ksps
//   +N=<outputs>       outputs to compare a receiver (default 100)
//   +LAST=<k>          receivers in use, less one (default NR - 1)
`timescale 1ps / 1ps

module fir_shared_tb;
    localparam int NR = 6;
    localparam int HALF_2X = 3255;  // ps
    logic clock = 1'b0, clock_2x = 1'b1;
    always #(HALF_2X) clock_2x = ~clock_2x;
    always #(2 * HALF_2X) clock = ~clock;

    logic rst_all = 1'b1;
    logic [5:0] rate;
    logic [3:0] last_chan;

    // Inputs: a tone and noise for each receiver, all different.
    localparam real TWO_PI = 6.283185307179586;
    int unsigned phase [0:NR-1];
    logic [31:0] lfsr = 32'hACE1_2025;
    logic signed [17:0] mix_i [0:NR-1];
    logic signed [17:0] mix_q [0:NR-1];
    initial for (int k = 0; k < NR; k++) begin
        phase[k] = 0;
        mix_i[k] = '0;
        mix_q[k] = '0;
    end
    always @(posedge clock) begin
        lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
        for (int k = 0; k < NR; k++) begin
            real a;
            int ni, nq;
            // 3 kHz, 5.5 kHz, 8 kHz ... above the receiver's centre
            phase[k] <= phase[k] + 32'd167772 + 32'(k) * 32'd139810;
            a = TWO_PI * real'(phase[k]) / 4294967296.0;
            ni = $signed(12'(lfsr >> (2 * k))) >>> 2;
            nq = $signed(12'(lfsr >> (2 * k + 12))) >>> 2;
            mix_i[k] <= 18'($rtoi((60000.0 - 7000.0 * k) * $cos(a)) + ni);
            mix_q[k] <= 18'($rtoi((60000.0 - 7000.0 * k) * $sin(a)) + nq);
        end
    end

    // The original receivers, and design B.
    logic        o_strobe [0:NR-1];
    logic [23:0] o_i [0:NR-1];
    logic [23:0] o_q [0:NR-1];
    logic        c_strobe [0:NR-1];
    logic signed [15:0] c_i [0:NR-1];
    logic signed [15:0] c_q [0:NR-1];
    logic [16*NR-1:0] xr, xi;
    logic [24*NR-1:0] yr, yi;
    logic        y_avail;

    for (genvar k = 0; k < NR; k++) begin : RX
        wire [33:0] dbg_o, dbg_c;
        receiver_nco #(.CICRATE(5)) orig (
            .rst_all(rst_all), .clock(clock), .clock_2x(clock_2x), .rate(rate),
            .mixdata_I(mix_i[k]), .mixdata_Q(mix_q[k]),
            .out_strobe(o_strobe[k]), .out_data_I(o_i[k]), .out_data_Q(o_q[k]), .debug(dbg_o)
        );
        rx_cic #(.CICRATE(5)) cic (
            .rst_all(rst_all), .clock(clock), .clock_2x(clock_2x), .rate(rate),
            .mixdata_I(mix_i[k]), .mixdata_Q(mix_q[k]),
            .out_strobe(c_strobe[k]), .out_data_I(c_i[k]), .out_data_Q(c_q[k]), .debug(dbg_c)
        );
        assign xr[16*k +: 16] = c_i[k];
        assign xi[16*k +: 16] = c_q[k];

        // The FPGA powers every register up at 0; these have no initial
        // value in the core's source (see rx_ref_tb.sv).
        initial begin
            orig.cic_inst_I2.cic_stages[0].cic_comb_inst.out_data = '0;
            orig.cic_inst_I2.cic_stages[1].cic_comb_inst.out_data = '0;
            orig.cic_inst_I2.cic_stages[2].cic_comb_inst.out_data = '0;
            orig.cic_inst_Q2.cic_stages[0].cic_comb_inst.out_data = '0;
            orig.cic_inst_Q2.cic_stages[1].cic_comb_inst.out_data = '0;
            orig.cic_inst_Q2.cic_stages[2].cic_comb_inst.out_data = '0;
            orig.varcic_inst_I1.cic_stages[0].cic_comb_inst.out_data = '0;
            orig.varcic_inst_I1.cic_stages[1].cic_comb_inst.out_data = '0;
            orig.varcic_inst_I1.cic_stages[2].cic_comb_inst.out_data = '0;
            orig.varcic_inst_I1.cic_stages[3].cic_comb_inst.out_data = '0;
            orig.varcic_inst_I1.cic_stages[4].cic_comb_inst.out_data = '0;
            orig.varcic_inst_Q1.cic_stages[0].cic_comb_inst.out_data = '0;
            orig.varcic_inst_Q1.cic_stages[1].cic_comb_inst.out_data = '0;
            orig.varcic_inst_Q1.cic_stages[2].cic_comb_inst.out_data = '0;
            orig.varcic_inst_Q1.cic_stages[3].cic_comb_inst.out_data = '0;
            orig.varcic_inst_Q1.cic_stages[4].cic_comb_inst.out_data = '0;
            orig.varcic_inst_I1.out_data = '0;
            orig.varcic_inst_Q1.out_data = '0;
`ifndef REF_FOLD
            orig.fir2.Racc = '0;
            orig.fir2.Iacc = '0;
`endif
            cic.cic_inst_I2.cic_stages[0].cic_comb_inst.out_data = '0;
            cic.cic_inst_I2.cic_stages[1].cic_comb_inst.out_data = '0;
            cic.cic_inst_I2.cic_stages[2].cic_comb_inst.out_data = '0;
            cic.cic_inst_Q2.cic_stages[0].cic_comb_inst.out_data = '0;
            cic.cic_inst_Q2.cic_stages[1].cic_comb_inst.out_data = '0;
            cic.cic_inst_Q2.cic_stages[2].cic_comb_inst.out_data = '0;
            cic.varcic_inst_I1.cic_stages[0].cic_comb_inst.out_data = '0;
            cic.varcic_inst_I1.cic_stages[1].cic_comb_inst.out_data = '0;
            cic.varcic_inst_I1.cic_stages[2].cic_comb_inst.out_data = '0;
            cic.varcic_inst_I1.cic_stages[3].cic_comb_inst.out_data = '0;
            cic.varcic_inst_I1.cic_stages[4].cic_comb_inst.out_data = '0;
            cic.varcic_inst_Q1.cic_stages[0].cic_comb_inst.out_data = '0;
            cic.varcic_inst_Q1.cic_stages[1].cic_comb_inst.out_data = '0;
            cic.varcic_inst_Q1.cic_stages[2].cic_comb_inst.out_data = '0;
            cic.varcic_inst_Q1.cic_stages[3].cic_comb_inst.out_data = '0;
            cic.varcic_inst_Q1.cic_stages[4].cic_comb_inst.out_data = '0;
            cic.varcic_inst_I1.out_data = '0;
            cic.varcic_inst_Q1.out_data = '0;
        end
    end

    firshared #(.NR(NR)) dut (
        .rst_all(rst_all), .clock(clock), .clock_2x(clock_2x), .last_chan(last_chan),
        .x_avail(c_strobe[0]), .x_real(xr), .x_imag(xi),
        .y_avail(y_avail), .y_real(yr), .y_imag(yi)
    );

    // The original's outputs, kept until design B's arrive.
    localparam int MAXN = 4096;
    logic [47:0] want [0:NR-1][0:MAXN-1];
    int n_orig = 0, n_new = 0, n_keep, last, bad = 0, compared = 0;
    always @(posedge clock) if (!rst_all && o_strobe[0]) begin
        for (int k = 0; k < NR; k++) begin
            if (o_strobe[k] !== 1'b1) begin
                $display("FAIL: original receiver %0d did not strobe with receiver 0", k);
                bad = bad + 1;
            end
            if (n_orig < MAXN) want[k][n_orig] = {o_i[k], o_q[k]};
        end
        n_orig = n_orig + 1;
    end
    // The CIC outputs of all receivers come together, as the shared filter
    // takes them.
    always @(posedge clock) if (!rst_all)
        for (int k = 1; k < NR; k++)
            if (c_strobe[k] !== c_strobe[0]) begin
                $display("FAIL: CIC of receiver %0d out of step with receiver 0", k);
                bad = bad + 1;
            end

    // Design B's outputs, kept too: either may come first (B's latency
    // depends on the receivers in use).
    logic [47:0] got [0:NR-1][0:MAXN-1];
    always @(posedge clock) if (!rst_all && y_avail) begin
        for (int k = 0; k < NR; k++)
            if (n_new < MAXN) got[k][n_new] = {yr[24*k +: 24], yi[24*k +: 24]};
        n_new = n_new + 1;
    end

    // Compared once both have n_keep outputs.
    always @(posedge clock) if (!rst_all && n_new >= n_keep && n_orig >= n_keep) begin
        for (int n = 1; n < n_keep; n++)
            for (int k = 0; k <= last; k++) begin
                if (got[k][n] !== want[k][n]) begin
                    if (bad < 10)
                        $display("FAIL: receiver %0d output %0d: %06x %06x, original %06x %06x",
                                 k, n, got[k][n][47:24], got[k][n][23:0],
                                 want[k][n][47:24], want[k][n][23:0]);
                    bad = bad + 1;
                end
                compared = compared + 1;
            end
        $display("rate %0d, receivers 0..%0d: %0d outputs a receiver, %0d values compared, %0d different",
                 rate, last, n_keep - 1, compared, bad);
        if (bad == 0) $display("PASS");
        $finish;
    end

    initial begin
        if (!$value$plusargs("RATE=%d", rate)) rate = 6'd40;
        if (!$value$plusargs("N=%d", n_keep)) n_keep = 100;
        if (!$value$plusargs("LAST=%d", last)) last = NR - 1;
        n_keep = n_keep + 1;
        last_chan = 4'(last);
        repeat (20) @(posedge clock);
        rst_all = 1'b0;
    end
endmodule
