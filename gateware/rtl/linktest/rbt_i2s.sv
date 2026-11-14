// SPDX-License-Identifier: GPL-2.0-or-later
//
// I2S link tester. The FPGA is clock master: it drives the bit clock and
// the frame sync, sends a known word sequence to the host and checks the
// sequence the host sends back.
//
// Timing, matching the BCM2711 PCM block as a slave: everything the FPGA
// drives changes on the rising edge of the bit clock, and the host
// samples on the falling edge. Frame sync is high for the first 32 bit
// periods of a 64-bit frame (or for one, if fs_short); bit 0 of the
// frame is the most significant bit of the first word. The host's data
// is sampled rxph fast-clock periods after the rising edge of each bit.
//
// Sequences: mode 0 counts up by one per word; mode 1 is xorshift32. The
// checker predicts each received word from the one before it, so a lost
// or repeated word costs one error and the check resynchronises.
//
// Everything here runs on clk_f. Settings are taken while disabled.
// Results cross to the reader by a snapshot handshake.
module rbt_i2s (
    input  logic        clk_f,

    // settings (quasi-static)
    input  logic        enable,
    input  logic        mode,
    input  logic        fs_short,
    input  logic [7:0]  div,       // fast clocks per bit, 2 to 255
    input  logic [7:0]  rxph,      // sample point, 1 to div-1
    input  logic [5:0]  rxoff,     // bit of our frame at which the host's word starts

    // pins
    output logic        bclk,
    output logic        fs,
    output logic        sd_out,
    input  logic        sd_in,

    // snapshot
    input  logic        snap_req,  // toggle
    output logic        snap_ack = 1'b0,  // toggle, follows snap_req
    output logic [31:0] s_tx_words,
    output logic [31:0] s_rx_words,
    output logic [31:0] s_rx_errors,
    output logic [31:0] s_rx_last,
    output logic [31:0] s_err_got,
    output logic [31:0] s_err_want
);

    function automatic logic [31:0] next_word(input logic [31:0] w, input logic m);
        logic [31:0] x;
        if (!m) return w + 32'd1;
        x = w ^ (w << 13);
        x = x ^ (x >> 17);
        x = x ^ (x << 5);
        return x;
    endfunction

    logic [1:0] en_s = 2'b00;
    logic       en;
    always_ff @(posedge clk_f) en_s <= {en_s[0], enable};
    assign en = en_s[1];

    logic [7:0]  ph      = 8'd0;
    logic [5:0]  bitn    = 6'd0;      // bit being driven, 0 to 63
    logic [31:0] tx_word = 32'h1;
    logic [31:0] tx_sr   = 32'h0;
    logic [31:0] tx_words = 32'd0;

    logic [1:0]  in_s;
    logic [5:0]  rxbit;
    assign rxbit = bitn - rxoff;
    logic [31:0] rx_sr;
    logic [31:0] rx_prev;
    logic        rx_seen = 1'b0;
    logic        rx_word_stb = 1'b0;
    logic [31:0] rx_word;
    logic [31:0] rx_words = 32'd0, rx_errors = 32'd0, rx_last = 32'd0;
    logic [31:0] err_got = 32'd0, err_want = 32'd0;
    logic [31:0] want;
    logic        check_stb = 1'b0;

    always_ff @(posedge clk_f) begin
        in_s        <= {in_s[0], sd_in};
        rx_word_stb <= 1'b0;
        check_stb   <= 1'b0;

        if (!en) begin
            ph       <= 8'd0;
            bitn     <= 6'd63;
            bclk     <= 1'b0;
            fs       <= 1'b0;
            sd_out   <= 1'b0;
            tx_word  <= 32'h1;
            tx_words <= 32'd0;
            rx_seen  <= 1'b0;
            rx_words <= 32'd0;
            rx_errors <= 32'd0;
        end else begin
            ph <= (ph == div - 8'd1) ? 8'd0 : ph + 8'd1;

            // Rising edge of the bit clock: next bit.
            if (ph == 8'd0) begin
                bclk <= 1'b1;
                bitn <= bitn + 6'd1;
                if (bitn[4:0] == 5'd31) begin
                    // Starting bit 0 or 32: a new word.
                    sd_out   <= tx_word[31];
                    tx_sr    <= {tx_word[30:0], 1'b0};
                    tx_word  <= next_word(tx_word, mode);
                    tx_words <= tx_words + 32'd1;
                    fs       <= (bitn == 6'd63);
                end else begin
                    sd_out <= tx_sr[31];
                    tx_sr  <= {tx_sr[30:0], 1'b0};
                    if (fs_short || bitn == 6'd31) fs <= 1'b0;
                end
            end
            if (ph == {1'b0, div[7:1]}) bclk <= 1'b0;

            // Sample the host's data for the bit now being driven.
            if (ph == rxph) begin
                rx_sr <= {rx_sr[30:0], in_s[1]};
                if (rxbit[4:0] == 5'd31) begin
                    rx_word     <= {rx_sr[30:0], in_s[1]};
                    rx_word_stb <= 1'b1;
                end
            end

            // Check, one step after the word is complete.
            if (rx_word_stb) begin
                want      <= next_word(rx_prev, mode);
                check_stb <= 1'b1;
            end
            if (check_stb) begin
                rx_words <= rx_words + 32'd1;
                rx_last  <= rx_word;
                rx_prev  <= rx_word;
                rx_seen  <= 1'b1;
                if (rx_seen && rx_word != want) begin
                    if (rx_errors == 32'd0) begin
                        err_got  <= rx_word;
                        err_want <= want;
                    end
                    if (rx_errors != 32'hFFFFFFFF) rx_errors <= rx_errors + 32'd1;
                end
            end
        end
    end

    logic [2:0] req_s = 3'b000;
    always_ff @(posedge clk_f) begin
        req_s <= {req_s[1:0], snap_req};
        if (req_s[2] ^ req_s[1]) begin
            s_tx_words  <= tx_words;
            s_rx_words  <= rx_words;
            s_rx_errors <= rx_errors;
            s_rx_last   <= rx_last;
            s_err_got   <= err_got;
            s_err_want  <= err_want;
            snap_ack    <= req_s[1];
        end
    end

endmodule
