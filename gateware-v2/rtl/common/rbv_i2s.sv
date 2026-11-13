// SPDX-License-Identifier: GPL-2.0-or-later
//
// I2S link to the Raspberry Pi 4's PCM block, the FPGA as clock master
// (measured in the link test: gateware-v2/linktest_results.md).
//
// Frame: 64 bit clocks, two 32-bit words, frame sync high for the first
// 32, most significant bit first, no delay after the frame sync edge.
// Everything the FPGA drives changes on the rising bit clock edge; the Pi
// samples on the falling edge.
//
// To the Pi: every word slot carries the next word from the FIFO with bit
// 31 set, or 0 when the FIFO is empty (an idle word, bit 31 clear).
// From the Pi: sampled rxoff bit periods plus rxph fast-clock periods
// after the rising clock edge that starts the bit, assembled into 32-bit
// words: a transmit sample (I in 31..16, Q in 15..0) followed by its flags:
//   31      valid
//   30..24  0x2a
//   23..16  check: XOR of the sample's four bytes and the sequence's two
//   15..0   sequence number
// The Pi sends the sample in the frame's first word, but its PCM block, a
// frame-sync consumer with the first channel at bit 0, can start a stream
// one word out (its Linux driver warns "L/R may be swapped" for this
// setup; measured here). So the pair's phase is found, not assumed: every
// word end is a candidate flags word, checked against the word before it.
// Candidates at the locked phase that pass are passed on with tx_wr; ones
// with bit 31 set that fail are dropped and counted in tx_bad. Two passes
// in a row at the other phase, with none at the locked phase between,
// move the lock there (counted in realigns); the first of the two is
// passed on too, so nothing is lost. A sequence number that does not follow
// the last one is counted in seq_gaps.
//
// Settings are taken while the link is disabled; enabling starts a frame.
module rbv_i2s (
    input  logic        clk_f,       // 122.88 MHz
    input  logic        enable,      // asynchronous
    input  logic [7:0]  div,         // fast clocks per bit, 2 to 255
    input  logic [7:0]  rxph,
    input  logic [5:0]  rxoff,

    input  logic        word_valid,
    input  logic [30:0] word,
    output logic        word_take,

    output logic        bclk,
    output logic        fs,
    output logic        sd_out,
    input  logic        sd_in,

    output logic [31:0] tx_words,    // words sent with data
    output logic [31:0] rx_words,    // valid samples received from the Pi

    output logic        tx_wr,       // a valid sample from the Pi
    output logic [31:0] tx_sample,
    output logic [31:0] seq_gaps,
    output logic [31:0] tx_bad,
    output logic [31:0] realigns
);

    logic [1:0]  en_s = 2'b00;
    logic        en;
    logic [7:0]  div_q = 8'd2, rxph_q = 8'd1;
    logic [5:0]  rxoff_q = 6'd0;
    logic [7:0]  ph = 8'd0;
    logic [5:0]  bitn = 6'd63;
    logic [31:0] sr = 32'd0;
    logic [1:0]  in_s = 2'b00;
    logic [5:0]  rxbit;

    always_ff @(posedge clk_f) en_s <= {en_s[0], enable};
    assign en    = en_s[1];
    // The edge with ph 0 starts the next bit while bitn still holds the
    // last one: count it as the new bit, so a sampling point of rxoff bits
    // plus rxph steps means the same thing for every rxph, 0 included.
    assign rxbit = ((ph == 8'd0) ? bitn + 6'd1 : bitn) - rxoff_q;

    initial begin
        bclk = 1'b0; fs = 1'b0; sd_out = 1'b0;
        tx_words = 32'd0; rx_words = 32'd0; word_take = 1'b0;
        tx_wr = 1'b0; seq_gaps = 32'd0; tx_bad = 32'd0; realigns = 32'd0;
    end

    logic [31:0] in_sr = 32'd0, w_prev = 32'd0;
    logic [15:0] seq_last = 16'd0;
    logic        seq_seen = 1'b0;
    logic        chk_stb = 1'b0, chk_q = 1'b1;
    logic [31:0] chk_a = 32'd0, chk_b = 32'd0;
    logic [7:0]  chk_x;
    logic        chk_ok;
    assign chk_x  = chk_a[31:24] ^ chk_a[23:16] ^ chk_a[15:8] ^ chk_a[7:0] ^
                    chk_b[15:8] ^ chk_b[7:0];
    assign chk_ok = chk_b[30:24] == 7'h2a && chk_b[23:16] == chk_x;

    // Phase lock: 1 when flags end at bit 63 (as sent), 0 at bit 31.
    logic        lock_q = 1'b1;
    logic        cand = 1'b0;               // one pass at the other phase
    logic [31:0] cand_s = 32'd0;
    logic [15:0] cand_seq = 16'd0;
    logic        pend = 1'b0;               // a second sample to pass on
    logic [31:0] pend_s = 32'd0;
    logic [15:0] pend_seq = 16'd0;

    always_ff @(posedge clk_f) begin
        in_s      <= {in_s[0], sd_in};
        word_take <= 1'b0;
        tx_wr     <= 1'b0;
        chk_stb   <= 1'b0;
        pend <= 1'b0;
        // Second stage: check the candidate taken in the first.
        if (pend) begin
            tx_wr     <= 1'b1;
            tx_sample <= pend_s;
            rx_words  <= rx_words + 32'd1;
            if (pend_seq != seq_last + 16'd1) seq_gaps <= seq_gaps + 32'd1;
            seq_last  <= pend_seq;
        end else if (chk_stb) begin
            if (chk_q == lock_q) begin
                if (chk_ok) begin
                    cand      <= 1'b0;   // the lock holds
                    tx_wr     <= 1'b1;
                    tx_sample <= chk_a;
                    rx_words  <= rx_words + 32'd1;
                    if (seq_seen && chk_b[15:0] != seq_last + 16'd1)
                        seq_gaps <= seq_gaps + 32'd1;
                    seq_last <= chk_b[15:0];
                    seq_seen <= 1'b1;
                end else tx_bad <= tx_bad + 32'd1;
            end else if (!chk_ok) begin
                cand <= 1'b0;
            end else if (!cand) begin
                cand     <= 1'b1;
                cand_s   <= chk_a;
                cand_seq <= chk_b[15:0];
            end else begin
                // The second in a row: the pairs are at this phase. Pass on
                // the first now and this one next.
                cand      <= 1'b0;
                lock_q    <= chk_q;
                realigns  <= realigns + 32'd1;
                tx_wr     <= 1'b1;
                tx_sample <= cand_s;
                rx_words  <= rx_words + 32'd1;
                seq_last  <= cand_seq;
                seq_seen  <= 1'b1;
                pend      <= 1'b1;
                pend_s    <= chk_a;
                pend_seq  <= chk_b[15:0];
            end
        end
        if (!en) begin
            seq_seen <= 1'b0;
            cand     <= 1'b0;
            div_q   <= (div < 8'd2) ? 8'd2 : div;
            rxph_q  <= rxph;
            rxoff_q <= rxoff;
            ph      <= 8'd0;
            bitn    <= 6'd63;
            bclk    <= 1'b0;
            fs      <= 1'b0;
            sd_out  <= 1'b0;
        end else begin
            ph <= (ph == div_q - 8'd1) ? 8'd0 : ph + 8'd1;
            if (ph == 8'd0) begin
                bclk <= 1'b1;
                bitn <= bitn + 6'd1;
                if (bitn[4:0] == 5'd31) begin
                    if (word_valid) begin
                        sd_out    <= 1'b1;
                        sr        <= {word, 1'b0};
                        word_take <= 1'b1;
                        tx_words  <= tx_words + 32'd1;
                    end else begin
                        sd_out <= 1'b0;
                        sr     <= 32'd0;
                    end
                    fs <= (bitn == 6'd63);
                end else begin
                    sd_out <= sr[31];
                    sr     <= {sr[30:0], 1'b0};
                end
            end
            if (ph == {1'b0, div_q[7:1]}) bclk <= 1'b0;
            if (ph == rxph_q) begin
                in_sr <= {in_sr[30:0], in_s[1]};
                if (rxbit[4:0] == 5'd31) begin
                    // A word ends: a candidate flags word if bit 31 (now
                    // in_sr[30]) is set, after the word before it.
                    w_prev <= {in_sr[30:0], in_s[1]};
                    if (in_sr[30]) begin
                        chk_stb <= 1'b1;
                        chk_q   <= rxbit[5];
                        chk_a   <= w_prev;
                        chk_b   <= {in_sr[30:0], in_s[1]};
                    end
                end
            end
        end
    end

endmodule
