// SPDX-License-Identifier: GPL-2.0-or-later
//
// Receive wire to the Raspberry Pi 5's RP1 PIO (the driver's RX program,
// driver/kernel/rb_pio.h). Runs entirely on the Pi's RX clock, which the
// Pi drives and which stands still whenever the Pi stops its state machine:
// nothing here is ever reset or cleared, so a clock that stops at any edge
// and starts again later leaves it consistent.
//
// The wire, per value: eight rising edges. After the edge that ends a
// value (and after every edge while idle) the nibble is 0 and sample-ready
// tells the Pi whether a value follows; the Pi tests it before its next
// edge. If it was high, the next edge puts the index nibble out, and the
// six after it the 24-bit value, most significant nibble first; the eighth
// ends the value. Everything changes on the rising edge only, so the Pi
// samples each nibble during the clock's high phase.
//
// The Pi's state machine always starts idle; if this side is part-way
// through a value then (the Pi restarted its program), sample-ready stays
// low until this side's value ends, and both are aligned from there.
module rbv_pio_rx (
    input  logic        clk,          // the Pi's RX clock
    input  logic        word_valid,   // FIFO read side, this clock
    input  logic [27:0] word,         // {index, value}
    output logic        word_take,
    output logic [3:0]  nib,          // to the Pi, from the I/O registers
    output logic        ready,
    output logic [31:0] words         // values sent, this clock
);

    logic [2:0]  cnt = 3'd0;          // 0 between values, 1..7 in one
    logic [23:0] sh = 24'd0;
    logic [31:0] n = 32'd0;

    // The outputs and an internal copy of ready, kept apart so that the
    // outputs can sit in the I/O-cell registers (the project file asks).
    (* preserve *) logic       rdy = 1'b0;
    (* preserve *) logic [3:0] nib_q = 4'd0;
    (* preserve *) logic       rdy_q = 1'b0;

    assign word_take = (cnt == 3'd0) && rdy;

    always_ff @(posedge clk) begin
        if (cnt == 3'd0) begin
            if (rdy) begin
                // The Pi saw sample-ready: this edge starts the value.
                nib_q <= word[27:24];
                sh    <= word[23:0];
                cnt   <= 3'd1;
                rdy   <= 1'b0;
                rdy_q <= 1'b0;
                n     <= n + 32'd1;
            end else begin
                nib_q <= 4'd0;
                rdy   <= word_valid;
                rdy_q <= word_valid;
            end
        end else if (cnt == 3'd7) begin
            // The edge that ends the value: idle nibble, and whether the
            // next value is there.
            nib_q <= 4'd0;
            cnt   <= 3'd0;
            rdy   <= word_valid;
            rdy_q <= word_valid;
        end else begin
            nib_q <= sh[23:20];
            sh    <= {sh[19:0], 4'd0};
            cnt   <= cnt + 3'd1;
        end
    end

    assign nib   = nib_q;
    assign ready = rdy_q;
    assign words = n;

endmodule
