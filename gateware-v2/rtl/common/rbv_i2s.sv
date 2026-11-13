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
// From the Pi: sampled rxph fast-clock periods into the bit period rxoff
// bits after the one being driven, assembled into words and counted. This
// build discards them: transmit is not wired.
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
    output logic [31:0] rx_words     // words received from the Pi
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
    assign rxbit = bitn - rxoff_q;

    initial begin
        bclk = 1'b0; fs = 1'b0; sd_out = 1'b0;
        tx_words = 32'd0; rx_words = 32'd0; word_take = 1'b0;
    end

    always_ff @(posedge clk_f) begin
        in_s      <= {in_s[0], sd_in};
        word_take <= 1'b0;
        if (!en) begin
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
            if (ph == rxph_q && rxbit[4:0] == 5'd31) rx_words <= rx_words + 32'd1;
        end
    end

endmodule
