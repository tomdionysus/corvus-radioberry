// SPDX-License-Identifier: GPL-2.0-or-later
//
// Transmit wire from the Raspberry Pi 5's RP1 PIO (the driver's TX
// program, driver/kernel/rb_pio.h). Runs entirely on the Pi's TX clock,
// which the Pi drives only while it has words to send (and polls with
// while ready is low); nothing here is ever reset or cleared.
//
// The wire: ready (registered on every rising edge) says a whole word fits.
// The rising edge that finds the shifter idle with ready high starts a word
// and carries no data; the Pi has tested ready for that same edge. The 32
// rising edges after it carry the word, most significant bit first: I in
// 31..16, Q in 15..0, as the Hermes-Lite 2 core takes it. The last bit is
// written to the FIFO on its own edge, so the clock may stop right after.
module rbv_pio_tx #(
    parameter int AW = 11
) (
    input  logic        clk,          // the Pi's TX clock
    input  logic        din,
    output logic        ready,        // to the Pi, from an I/O register
    input  logic [AW:0] wfree,        // FIFO write side, this clock
    output logic        wen,
    output logic [31:0] wdata,
    output logic [31:0] words,        // words received
    output logic [31:0] overflows     // words the FIFO had no room for
);

    logic        busy = 1'b0;
    logic [4:0]  bitn = 5'd0;
    logic [30:0] sh = 31'd0;
    logic [31:0] n = 32'd0, ovf = 32'd0;
    logic        room;

    // Ready, and an internal copy kept apart so that the output can sit in
    // its I/O-cell register (the project file asks).
    (* preserve *) logic rdy = 1'b0;
    (* preserve *) logic rdy_q = 1'b0;

    // Room for this word and the next: a write needs wfree >= 2 (the FIFO
    // keeps one word in hand) and wfree lags a word behind.
    assign room  = (wfree >= (AW+1)'(8));
    assign wen   = busy && (bitn == 5'd31);
    assign wdata = {sh, din};

    always_ff @(posedge clk) begin
        rdy   <= room;
        rdy_q <= room;
        if (!busy) begin
            if (rdy) begin
                busy <= 1'b1;
                bitn <= 5'd0;
            end
        end else begin
            sh   <= {sh[29:0], din};
            bitn <= bitn + 5'd1;
            if (bitn == 5'd31) begin
                busy <= 1'b0;
                n    <= n + 32'd1;
                if (wfree < (AW+1)'(2)) ovf <= ovf + 32'd1;
            end
        end
    end

    assign ready     = rdy_q;
    assign words     = n;
    assign overflows = ovf;

endmodule
