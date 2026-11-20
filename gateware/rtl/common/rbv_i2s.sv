// SPDX-License-Identifier: GPL-2.0-or-later
//
// I2S link to the Raspberry Pi 4's PCM block, the FPGA as clock master
// (measured in the link test: docs/linktest-results.md).
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
    logic [7:0]  ph = 8'd0;
    logic [5:0]  bitn = 6'd63;
    logic [31:0] sr = 32'd0;
    logic [1:0]  in_s = 2'b00;

    always_ff @(posedge clk_f) en_s <= {en_s[0], enable};
    assign en    = en_s[1];

    // The sampling instant is the cycle with ph == rxph. Everything about
    // it is worked out one cycle before, into registers, so the cycle that
    // samples only acts on them:
    //   rx_tick  ph == rxph in this cycle (never when rxph >= div);
    //   rx_end   the bit sampled is a word's last (bit 31 or 63);
    //   rx_hi    it is the frame's second word.
    // The bit sampled counts the edge with ph 0 as the new bit while bitn
    // still holds the last, so a sampling point of rxoff bits plus rxph
    // steps means the same thing for every rxph, 0 included. One cycle
    // before the sampling instant that bit is bitn + rx_ofs, where rx_ofs
    // is 1 - rxoff when rxph is 0 or 1 (bitn steps at the edge between)
    // and -rxoff otherwise. The settings change only while the link is
    // disabled, so rx_ofs, the cycle before rxph (rx_pre) and whether
    // rxph is reached at all (rx_ok) are worked out then, each from the
    // registered settings, so no long path runs from the setting inputs.
    // The link is disabled for many cycles before it starts.
    logic [5:0]  rxoff_q = 6'd0;
    logic [7:0]  rx_pre = 8'd0;
    logic [5:0]  rx_ofs = 6'd0;
    logic        rx_ok = 1'b1;
    logic        rx_tick = 1'b0, rx_end = 1'b0, rx_hi = 1'b0;
    logic [5:0]  rx_bit_next;
    assign rx_bit_next = bitn + rx_ofs;

    always_ff @(posedge clk_f) begin
        if (!en) begin
            rxoff_q <= rxoff;
            rx_pre  <= (rxph_q == 8'd0) ? div_q - 8'd1 : rxph_q - 8'd1;
            rx_ofs  <= {5'd0, rxph_q[7:1] == 7'd0} - rxoff_q;
            rx_ok   <= rxph_q < div_q;
        end
        // The link runs next cycle if en_s[0] is set; its first cycle has
        // ph 0, so it samples then if rxph is 0.
        rx_tick <= en_s[0] && rx_ok && (en ? ph == rx_pre : rxph_q == 8'd0);
        rx_end  <= rx_bit_next[4:0] == 5'd31;
        rx_hi   <= rx_bit_next[5];
    end

    initial begin
        bclk = 1'b0; fs = 1'b0; sd_out = 1'b0;
        tx_words = 32'd0; rx_words = 32'd0; word_take = 1'b0;
        tx_wr = 1'b0; seq_gaps = 32'd0; tx_bad = 32'd0; realigns = 32'd0;
    end

    logic [31:0] in_sr = 32'd0, w_prev = 32'd0;
    logic [15:0] seq_next = 16'd1;           // the sequence number expected next
    logic        seq_seen = 1'b0;
    logic        chk_stb = 1'b0, chk_q = 1'b1, chk_ok = 1'b0;
    logic [31:0] chk_a = 32'd0, chk_b = 32'd0;
    // The candidate flags word's check, worked out as the pair is taken
    // (from the word before and the word just ended) and registered with
    // it, so the next cycle's decision starts from a register.
    logic [31:0] w_now;
    logic [7:0]  w_x;
    assign w_now = {in_sr[30:0], in_s[1]};
    assign w_x   = w_prev[31:24] ^ w_prev[23:16] ^ w_prev[15:8] ^ w_prev[7:0] ^
                   w_now[15:8] ^ w_now[7:0];

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
            if (pend_seq != seq_next) seq_gaps <= seq_gaps + 32'd1;
            seq_next  <= pend_seq + 16'd1;
        end else if (chk_stb) begin
            if (chk_q == lock_q) begin
                if (chk_ok) begin
                    cand      <= 1'b0;   // the lock holds
                    tx_wr     <= 1'b1;
                    tx_sample <= chk_a;
                    rx_words  <= rx_words + 32'd1;
                    if (seq_seen && chk_b[15:0] != seq_next)
                        seq_gaps <= seq_gaps + 32'd1;
                    seq_next <= chk_b[15:0] + 16'd1;
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
                seq_next  <= cand_seq + 16'd1;
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
            if (rx_tick) begin
                in_sr <= {in_sr[30:0], in_s[1]};
                if (rx_end) begin
                    // A word ends: a candidate flags word if bit 31 (now
                    // in_sr[30]) is set, after the word before it.
                    w_prev <= w_now;
                    if (in_sr[30]) begin
                        chk_stb <= 1'b1;
                        chk_q   <= rx_hi;
                        chk_a   <= w_prev;
                        chk_b   <= w_now;
                        chk_ok  <= w_now[30:24] == 7'h2a && w_now[23:16] == w_x;
                    end
                end
            end
        end
    end

endmodule
