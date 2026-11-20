// SPDX-License-Identifier: GPL-2.0-or-later
//
// The receive sender (takeover stage 5): hands every receiver's sample to
// the transport, the same words on the same clocks as the Hermes-Lite 2
// core's sender in radio.v.
//
// On the receive filter's strobe (one for all the receivers), with the
// transport ready, it sends receiver 0's I and then Q, then receiver 1's,
// and so on to receiver last_chan, one word a clock, rx_tlast with the
// last Q; then it waits for the strobe to fall before the next. A reset or
// a channel reset returns it to waiting. Receivers above NR - 1 (a
// last_chan the daemon never sends) give zeros (radio.v's array of ten
// receivers left them undriven).
module rx_sender #(
    parameter int NR = 6                    // receivers, 1 to 10
) (
    input  logic               clk,         // 76.8 MHz
    input  logic               rst,         // reset or channel reset
    input  logic [3:0]         last_chan,   // receivers in use, less one
    input  logic               y_avail,     // the filter's strobe
    input  logic [24*NR-1:0]   y_real,      // receiver k in bits 24k + 23..24k
    input  logic [24*NR-1:0]   y_imag,
    output logic [23:0]        rx_tdata,
    output logic               rx_tlast,
    output logic               rx_tvalid,
    output logic [1:0]         rx_tuser,
    input  logic               rx_tready
);
    localparam logic [1:0] WAIT1 = 2'd0, SEND_I = 2'd1, SEND_Q = 2'd2, WAIT0 = 2'd3;
    logic [1:0] state = WAIT1, state_next;
    logic [3:0] chan = 4'h0, chan_next;

    always_ff @(posedge clk) begin
        if (rst) begin
            state <= WAIT1;
            chan  <= 4'h0;
        end else begin
            state <= state_next;
            chan  <= chan_next;
        end
    end

    // The current receiver's I and Q (zero above NR - 1).
    logic [23:0] cur_i, cur_q;
    always_comb begin
        cur_i = 24'd0;
        cur_q = 24'd0;
        for (int k = 0; k < NR; k++) begin
            if (chan == 4'(k)) begin
                cur_i = y_real[24*k +: 24];
                cur_q = y_imag[24*k +: 24];
            end
        end
    end

    always_comb begin
        state_next = state;
        chan_next  = chan;
        rx_tdata   = 24'h0;
        rx_tlast   = 1'b0;
        rx_tvalid  = 1'b0;
        rx_tuser   = 2'b00;
        case (state)
            WAIT1: begin
                chan_next = 4'h0;
                if (y_avail && rx_tready) state_next = SEND_I;
            end
            SEND_I: begin
                rx_tvalid  = 1'b1;
                rx_tdata   = cur_i;
                state_next = SEND_Q;
            end
            SEND_Q: begin
                rx_tvalid = 1'b1;
                rx_tdata  = cur_q;
                if (chan >= last_chan) begin
                    rx_tlast   = 1'b1;
                    state_next = WAIT0;
                end else begin
                    chan_next  = chan + 4'h1;
                    state_next = SEND_I;
                end
            end
            default: begin  // WAIT0
                chan_next = 4'h0;
                if (!y_avail) state_next = WAIT1;
            end
        endcase
    end
endmodule
