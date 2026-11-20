// SPDX-License-Identifier: GPL-2.0-or-later
//
// rbv_i2s against a reference copy of an earlier version (module
// rbv_i2s_ref, built from git by the caller), side by side: the same
// settings, the same words to send and random data from the Pi, every
// output compared on every clock. For each divider tried, every sampling
// phase (and one beyond the divider) and a set of bit offsets, with the
// link disabled and enabled again between settings.
`timescale 1ns / 1ps

module rbv_i2s_eq_tb;
    logic clk = 1'b0;
    always #4.069 clk = ~clk;  // 122.88 MHz

    logic        enable = 1'b0;
    logic [7:0]  div = 8'd4, rxph = 8'd1;
    logic [5:0]  rxoff = 6'd0;
    logic        word_valid = 1'b0;
    logic [30:0] word = '0;
    logic        sd_in = 1'b0;

    logic        a_take, a_bclk, a_fs, a_sd, a_wr;
    logic [31:0] a_txw, a_rxw, a_sample, a_gaps, a_bad, a_realigns;
    logic        b_take, b_bclk, b_fs, b_sd, b_wr;
    logic [31:0] b_txw, b_rxw, b_sample, b_gaps, b_bad, b_realigns;

    rbv_i2s dut (
        .clk_f(clk), .enable(enable), .div(div), .rxph(rxph), .rxoff(rxoff),
        .word_valid(word_valid), .word(word), .word_take(a_take),
        .bclk(a_bclk), .fs(a_fs), .sd_out(a_sd), .sd_in(sd_in),
        .tx_words(a_txw), .rx_words(a_rxw), .tx_wr(a_wr), .tx_sample(a_sample),
        .seq_gaps(a_gaps), .tx_bad(a_bad), .realigns(a_realigns)
    );
    rbv_i2s_ref ref_i (
        .clk_f(clk), .enable(enable), .div(div), .rxph(rxph), .rxoff(rxoff),
        .word_valid(word_valid), .word(word), .word_take(b_take),
        .bclk(b_bclk), .fs(b_fs), .sd_out(b_sd), .sd_in(sd_in),
        .tx_words(b_txw), .rx_words(b_rxw), .tx_wr(b_wr), .tx_sample(b_sample),
        .seq_gaps(b_gaps), .tx_bad(b_bad), .realigns(b_realigns)
    );

    int errors = 0, cycles = 0, samples = 0;
    always @(posedge clk) begin
        cycles = cycles + 1;
        if (a_wr) samples = samples + 1;
        if ({a_take, a_bclk, a_fs, a_sd, a_wr, a_txw, a_rxw, a_gaps, a_bad, a_realigns} !==
                {b_take, b_bclk, b_fs, b_sd, b_wr, b_txw, b_rxw, b_gaps, b_bad, b_realigns} ||
            (a_wr && a_sample !== b_sample)) begin
            if (errors < 10)
                $display("MISMATCH t=%0t div %0d rxph %0d rxoff %0d: new %b%b%b%b%b rx %0d bad %0d, ref %b%b%b%b%b rx %0d bad %0d",
                         $time, div, rxph, rxoff, a_take, a_bclk, a_fs, a_sd, a_wr, a_rxw, a_bad,
                         b_take, b_bclk, b_fs, b_sd, b_wr, b_rxw, b_bad);
            errors = errors + 1;
        end
    end

    // Data from the Pi: random bits, with a well-formed sample and flags
    // pair now and then so that the check, lock and counters all work.
    logic [31:0] lfsr = 32'h1234_5678;
    always @(posedge clk) lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
    always @(posedge clk) begin
        word_valid <= lfsr[3];
        word       <= {lfsr[30:0]};
    end

    // The Pi side: from the frame sync's rising edge, one frame of a sample
    // and its flags, shifted out on bclk's falling edge. Each frame is, at
    // random: a valid pair (most), a valid pair with its sequence number
    // skipped, a pair with a bad check, random bits, or the pair one word
    // out (flags first, as the Pi's PCM block sometimes starts).
    logic [63:0] frame = '0;
    logic [15:0] seq = 16'd0;
    logic        prev_bclk = 1'b0, prev_fs = 1'b0;
    always @(posedge clk) begin
        prev_bclk <= a_bclk;
        if (prev_bclk && !a_bclk) begin
            if (a_fs && !prev_fs) begin
                logic [31:0] s, f;
                logic [7:0]  x;
                logic [2:0]  kind;
                s = lfsr;
                kind = lfsr[12:10];
                if (kind == 3'd1) seq = seq + 16'd1;  // a skipped number
                x = s[31:24] ^ s[23:16] ^ s[15:8] ^ s[7:0] ^ seq[15:8] ^ seq[7:0];
                if (kind == 3'd2) x = ~x;              // a bad check
                f = {1'b1, 7'h2a, x, seq};
                case (kind)
                    3'd3: frame = {lfsr, ~lfsr};       // random bits
                    3'd4: frame = {f, s};              // one word out
                    default: frame = {s, f};
                endcase
                seq = seq + 16'd1;
            end
            prev_fs <= a_fs;
            sd_in <= frame[63];
            frame <= {frame[62:0], 1'b0};
        end
    end

    task automatic run(input int d, input int ph, input int off, input int n);
        enable = 1'b0;
        repeat (8) @(posedge clk);
        div = 8'(d); rxph = 8'(ph); rxoff = 6'(off);
        repeat (8) @(posedge clk);
        enable = 1'b1;
        repeat (n) @(posedge clk);
    endtask

    int divs[] = '{2, 3, 4, 5, 8, 13};
    int offs[] = '{0, 1, 2, 5, 31, 32, 63};
    initial begin
        foreach (divs[i])
            for (int ph = 0; ph <= divs[i]; ph++)
                foreach (offs[k])
                    run(divs[i], ph, offs[k], 64 * divs[i] * 12);
        enable = 1'b0;
        repeat (8) @(posedge clk);
        $display("%0d cycles, %0d samples passed, %0d bad, %0d sequence gaps, %0d realigns, %0d mismatches",
                 cycles, samples, a_bad, a_gaps, a_realigns, errors);
        if (errors == 0) $display("PASS");
        else $display("FAIL");
        $finish;
    end
endmodule
