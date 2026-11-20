// SPDX-License-Identifier: GPL-2.0-or-later
//
// One Hermes-Lite 2 receiver (receiver_nco, CICRATE 5, as in both images)
// driven with a fixed input, at the images' clocks: 76.8 MHz and the
// filter's 153.6 MHz. Writes each output sample as hex to +OUT=<file> and
// prints a checksum, so a reworked receive chain can be compared with the
// original sample for sample.
//
// Simulated with the vendor models of the filter's memories (altera_mf.v,
// USE_ALTSYNCRAM), as the images build it: the core's own plain models
// read one clock earlier and give a different filter.
//
//   +RATE=40|20|10|5   48, 96, 192 or 384 ksps (the varcic decimation)
//   +N=<samples>       output samples to keep (default 200)
//   +OUT=<file>        where to write them
`timescale 1ps / 1ps

module rx_ref_tb;
    // 76.8 MHz and 153.6 MHz with their rising edges at the same instant, as
    // the PLL gives them. Both are generated here: a clock derived from the
    // other by a register would edge a delta later, after the fast clock's
    // registers have changed, and the filter would read its sums too late.
    localparam int HALF_2X = 3255;  // ps
    // clock_2x starts high so that both rise at 2, 6, 10... half-periods.
    logic clock = 1'b0, clock_2x = 1'b1;
    always #(HALF_2X) clock_2x = ~clock_2x;
    always #(2 * HALF_2X) clock = ~clock;

    logic rst_all = 1'b1;
    logic [5:0] rate;
    logic signed [17:0] mix_i = '0, mix_q = '0;
    wire out_strobe;
    wire [23:0] out_i, out_q;
    wire [33:0] debug;

    receiver_nco #(.CICRATE(5)) dut (
        .rst_all(rst_all),
        .clock(clock),
        .clock_2x(clock_2x),
        .rate(rate),
        .mixdata_I(mix_i),
        .mixdata_Q(mix_q),
        .out_strobe(out_strobe),
        .out_data_I(out_i),
        .out_data_Q(out_q),
        .debug(debug)
    );

    // The input: two tones and pseudo-random noise, from integer phase
    // accumulators and a 32-bit LFSR, so every run gives the same samples.
    // Tone A sits in the passband of every rate (+3 kHz), tone B outside
    // the 48 ksps passband (+31 kHz), the noise everywhere.
    localparam real FS = 76.8e6;
    localparam real TWO_PI = 6.283185307179586;
    int unsigned phase_a = 0, phase_b = 0;
    localparam int unsigned STEP_A = 32'd167772;    // 3 kHz at 76.8 MHz
    localparam int unsigned STEP_B = 32'd1733312;   // 31 kHz
    logic [31:0] lfsr = 32'hACE1_2025;
    always @(posedge clock) begin
        real a, b;
        int ni, nq;
        phase_a <= phase_a + STEP_A;
        phase_b <= phase_b + STEP_B;
        lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
        a = TWO_PI * real'(phase_a) / 4294967296.0;
        b = TWO_PI * real'(phase_b) / 4294967296.0;
        ni = $signed(lfsr[11:0]) >>> 2;
        nq = $signed(lfsr[23:12]) >>> 2;
        mix_i <= 18'($rtoi(60000.0 * $cos(a) + 30000.0 * $cos(b)) + ni);
        mix_q <= 18'($rtoi(60000.0 * $sin(a) + 30000.0 * $sin(b)) + nq);
    end

    int n_keep, fd, count;
    longint unsigned sum;
    string out;
    initial begin
        if (!$value$plusargs("RATE=%d", rate)) rate = 6'd40;
        if (!$value$plusargs("N=%d", n_keep)) n_keep = 200;
        if (!$value$plusargs("OUT=%s", out)) out = "rx_ref.hex";
        fd = $fopen(out, "w");
        count = 0;
        sum = 0;
        repeat (20) @(posedge clock);
        rst_all = 1'b0;
    end

    // The FPGA powers every register up at 0. cic_comb's output register
    // has no initial value in the source, and the next stage samples it on
    // the first strobe, before it is written, so a simulator would carry an
    // unknown value through the integrators for good. Set it as the device
    // does.
    initial begin
        dut.cic_inst_I2.cic_stages[0].cic_comb_inst.out_data = '0;
        dut.cic_inst_I2.cic_stages[1].cic_comb_inst.out_data = '0;
        dut.cic_inst_I2.cic_stages[2].cic_comb_inst.out_data = '0;
        dut.cic_inst_Q2.cic_stages[0].cic_comb_inst.out_data = '0;
        dut.cic_inst_Q2.cic_stages[1].cic_comb_inst.out_data = '0;
        dut.cic_inst_Q2.cic_stages[2].cic_comb_inst.out_data = '0;
        dut.varcic_inst_I1.cic_stages[0].cic_comb_inst.out_data = '0;
        dut.varcic_inst_I1.cic_stages[1].cic_comb_inst.out_data = '0;
        dut.varcic_inst_I1.cic_stages[2].cic_comb_inst.out_data = '0;
        dut.varcic_inst_I1.cic_stages[3].cic_comb_inst.out_data = '0;
        dut.varcic_inst_I1.cic_stages[4].cic_comb_inst.out_data = '0;
        dut.varcic_inst_Q1.cic_stages[0].cic_comb_inst.out_data = '0;
        dut.varcic_inst_Q1.cic_stages[1].cic_comb_inst.out_data = '0;
        dut.varcic_inst_Q1.cic_stages[2].cic_comb_inst.out_data = '0;
        dut.varcic_inst_Q1.cic_stages[3].cic_comb_inst.out_data = '0;
        dut.varcic_inst_Q1.cic_stages[4].cic_comb_inst.out_data = '0;
        dut.varcic_inst_I1.out_data = '0;
        dut.varcic_inst_Q1.out_data = '0;
`ifndef FIR_FOLD
        // The original firX8R8's output accumulators: the first output
        // strobe comes before they are first loaded. (The folded filter
        // gives its registers values itself.)
        dut.fir2.Racc = '0;
        dut.fir2.Iacc = '0;
`endif
    end

    // +TRACE prints each stage's value for the first microseconds.
    initial if ($test$plusargs("TRACE")) begin
        repeat (200) @(posedge clock);
        repeat (12) begin
            @(posedge clock);
            $display("mix %0d  cicI out %0d avail %b  varcic out %0d avail %b",
                     mix_i, dut.decimA_real, dut.decimA_avail, dut.decimB_real, dut.decimB_avail);
        end
    end

    // +FIRIN=<file>: also write the samples entering the decimating FIR
    // (I and Q, 16 bits) and mark each output strobe, so a model of the
    // filter can be checked against the original.
    int fd_in = 0;
    string firin;
    initial if ($value$plusargs("FIRIN=%s", firin)) fd_in = $fopen(firin, "w");
    always @(posedge clock) begin
        if (fd_in != 0 && !rst_all) begin
            if (dut.decimB_avail) $fwrite(fd_in, "i %04x %04x\n", dut.decimB_real, dut.decimB_imag);
            if (out_strobe) $fwrite(fd_in, "o %06x %06x\n", out_i, out_q);
        end
    end

    // A zero output says nothing unless the filter was fed: count its
    // input samples that were not zero, and its outputs that were not.
    int fir_in_nonzero = 0, out_nonzero = 0;
    always @(posedge clock) begin
        if (dut.decimB_avail && dut.decimB_real != 0) fir_in_nonzero = fir_in_nonzero + 1;
        if (out_strobe && out_i != 0) out_nonzero = out_nonzero + 1;
    end

    always @(posedge clock) begin
        if (!rst_all && out_strobe && count + 1 == n_keep)
            $display("filter input samples not zero: %0d; outputs not zero: %0d",
                     fir_in_nonzero, out_nonzero + (out_i != 0));
        if (!rst_all && out_strobe) begin
            $fwrite(fd, "%06x %06x\n", out_i, out_q);
            sum = sum * 64'd1000003 + {out_i, out_q};
            count = count + 1;
            if (count == n_keep) begin
                $fclose(fd);
                $display("rate %0d: %0d samples, checksum %016x", rate, count, sum);
                $display("PASS");
                $finish;
            end
        end
    end
endmodule
