// SPDX-License-Identifier: GPL-2.0-or-later
//
// AD9866 start-up and register access. Runs on the board's own clock as it
// arrives at the FPGA (pin 53), which is 38.4 MHz until register 0x04 is
// written and 76.8 MHz after; it needs no PLL.
//
// Start-up: hold the chip in reset, release it, write 0x04 = 0x36 (PLL x4,
// ADC from the PLL: 76.8 Msps), wait for the FPGA's PLL to lock on the new
// clock, write the rest of the set-up list, then report done. Afterwards it
// serves receive gain writes and register reads.
//
// The set-up leaves every block but the PLL, the reference and the ADC
// common mode powered down (0x01 = 0x67) and the transmit gain at its
// lowest (0x0a = 0x40). Afterwards three registers are kept where the
// inputs say, written whenever they differ:
//   0x01  power-down bits (datasheet Table 24; bit 7, the PLL, is never set:
//         it makes the clock this module and the FPGA run on):
//           0x67  rx_on clear: receive and transmit paths off
//           0x40  loopback (TX set): TxDAC and IAMP off, the rest on
//           0x00  tx_power (TX set): everything on
//           0x60  otherwise: receive path on, transmit path off
//   0x0d  0x43 (digital loopback) with loopback set, else 0x03
//   0x0a  {01, tx_code} with TX set, else 0x40 (lowest)
// The TxDAC is powered on only after loopback is off, and loopback goes on
// only with the TxDAC off. dac_ready rises POWER_UP_CYCLES after the write
// that powered the TxDAC on has finished (its power-up takes 13 us) and
// falls as soon as a power-down is wanted. With TX clear (the receive
// build) the transmit path and 0x0a never leave off and lowest.
module rbv_adseq #(
    parameter bit TX = 1'b0,
    parameter int RESET_CYCLES = 4096,
    parameter int LOCK_CYCLES  = 131072,   // about 1.7 ms at 76.8 MHz
    parameter int POWER_UP_CYCLES = 1536   // 20 us at 76.8 MHz
) (
    input  logic        clk,
    input  logic        restart,           // pulse: run the start-up again
    input  logic        pll_locked,        // asynchronous

    input  logic        rx_on,             // asynchronous: the radio is running
    input  logic        tx_power,          // asynchronous: power the transmit path
    input  logic        loopback,          // asynchronous: digital loopback, TxDAC and IAMP off
    output logic        dac_ready = 1'b0,  // TxDAC powered and settled
    input  logic [5:0]  tx_code,           // transmit gain code wanted now (0: lowest)
    input  logic        gain_req,          // pulse: write gain_code
    input  logic [5:0]  gain_code,         // receive gain code, 0 to 63
    input  logic        rd_req,            // pulse: read rd_addr
    input  logic [4:0]  rd_addr,
    output logic [7:0]  rd_value = 8'd0,
    output logic        rd_busy = 1'b0,

    output logic        done = 1'b0,
    output logic [15:0] writes = 16'd0,    // register writes made

    output logic        ad_rst_n = 1'b0,
    output logic        ad_sen_n,
    output logic        ad_sclk,
    output logic        ad_sdio_out,
    output logic        ad_sdio_oe,
    input  logic        ad_sdio_in
);

    localparam int NINIT = 10;
    logic [12:0] init [0:NINIT-1];
    initial begin
        init[0] = {5'h01, 8'h67};   // all but PLL, reference and ADC common mode off
        init[1] = {5'h06, 8'h14};   // CLKOUT2 off, CLKOUT1 = DAC clock / 2
        init[2] = {5'h07, 8'h21};   // offset calibration, receive filter on
        init[3] = {5'h08, 8'h40};   // receive filter cut-off
        init[4] = {5'h09, 8'h40};   // receive gain from the serial port, code 0
        init[5] = {5'h0a, 8'h40};   // transmit gain from the serial port, lowest
        init[6] = {5'h0b, 8'h00};   // no receive gain from the transmit port
        init[7] = {5'h0c, 8'h43};   // transmit: 2x interpolation, two's complement
        init[8] = {5'h0d, 8'h03};   // receive: two's complement
        init[9] = {5'h12, 8'h00};
    end

    logic        ser_start = 1'b0, ser_busy;
    logic [15:0] ser_word = 16'h0, ser_oe = 16'h0, cap_sdio, cap_sdo;

    rbt_adser ser_i (
        .clk(clk), .rst(1'b0), .start(ser_start), .txword(ser_word), .oemask(ser_oe),
        .busy(ser_busy), .cap_sdio(cap_sdio), .cap_sdo(cap_sdo),
        .sen_n(ad_sen_n), .sclk(ad_sclk), .sdio_out(ad_sdio_out), .sdio_oe(ad_sdio_oe),
        .sdio_in(ad_sdio_in), .sdo_in(1'b1)
    );

    typedef enum logic [3:0] {
        S_RESET, S_RELEASE, S_W04, S_LOCK, S_INIT, S_IDLE, S_WAIT, S_READ_DONE
    } state_t;
    state_t      state = S_RESET;
    logic [17:0] cnt = 18'd0;
    logic [3:0]  idx = 4'd0;
    logic [1:0]  lock_s = 2'b00;
    logic        gain_pend = 1'b0, rd_pend = 1'b0;
    logic [5:0]  gain_q = 6'd0;
    logic [4:0]  rd_addr_q = 5'd0;
    logic        reading = 1'b0;
    state_t      after = S_IDLE;
    logic [7:0]  r01 = 8'h67, r0d = 8'h03, r0a = 8'h40;   // as written
    logic [7:0]  w01, w0d, w0a;                            // as wanted
    logic [1:0]  lb_s = 2'b00, rx_s = 2'b00, txp_s = 2'b00;
    logic        lb, rxo, txp;

    always_ff @(posedge clk) begin
        lb_s  <= {lb_s[0], loopback};
        rx_s  <= {rx_s[0], rx_on};
        txp_s <= {txp_s[0], tx_power};
    end
    assign lb  = TX && lb_s[1];
    assign rxo = rx_s[1];
    assign txp = TX && txp_s[1];
    assign w01 = !rxo ? 8'h67 : lb ? 8'h40 : txp ? 8'h00 : 8'h60;
    assign w0d = lb ? 8'h43 : 8'h03;
    assign w0a = TX ? {2'b01, tx_code} : 8'h40;

    // TxDAC settling: counts while the TxDAC is on and no write to 0x01 is
    // under way.
    logic        last01 = 1'b0;          // the last write issued was to 0x01
    logic [11:0] pu_cnt = 12'd0;

    // A write: instruction byte {0, 00, address} then data, all driven.
    function automatic logic [15:0] wr(input logic [4:0] a, input logic [7:0] d);
        return {3'b000, a, d};
    endfunction

    always_ff @(posedge clk) begin
        lock_s    <= {lock_s[0], pll_locked};
        ser_start <= 1'b0;
        if (gain_req) begin
            gain_pend <= 1'b1;
            gain_q    <= gain_code;
        end
        if (rd_req) begin
            rd_pend   <= 1'b1;
            rd_addr_q <= rd_addr;
            rd_busy   <= 1'b1;
        end

        case (state)
            S_RESET: begin
                done     <= 1'b0;
                ad_rst_n <= 1'b0;
                // The set-up list leaves these as the chip will hold them.
                r01      <= 8'h67;
                r0d      <= 8'h03;
                r0a      <= 8'h40;
                if (cnt == RESET_CYCLES[17:0]) begin
                    cnt   <= 18'd0;
                    state <= S_RELEASE;
                end else cnt <= cnt + 18'd1;
            end
            S_RELEASE: begin
                ad_rst_n <= 1'b1;
                if (cnt == RESET_CYCLES[17:0]) begin
                    cnt   <= 18'd0;
                    state <= S_W04;
                end else cnt <= cnt + 18'd1;
            end
            S_W04: if (!ser_busy && !ser_start) begin
                ser_word  <= wr(5'h04, 8'h36);
                ser_oe    <= 16'hFFFF;
                ser_start <= 1'b1;
                writes    <= writes + 16'd1;
                after     <= S_LOCK;
                state     <= S_WAIT;
            end
            S_LOCK: begin
                // The FPGA's PLL loses lock as the clock doubles and locks
                // again; wait until it has been locked for LOCK_CYCLES.
                if (!lock_s[1]) cnt <= 18'd0;
                else if (cnt == LOCK_CYCLES[17:0]) begin
                    cnt   <= 18'd0;
                    idx   <= 4'd0;
                    state <= S_INIT;
                end else cnt <= cnt + 18'd1;
            end
            S_INIT: if (!ser_busy && !ser_start) begin
                ser_word  <= wr(init[idx][12:8], init[idx][7:0]);
                ser_oe    <= 16'hFFFF;
                ser_start <= 1'b1;
                writes    <= writes + 16'd1;
                if (idx == NINIT[3:0] - 4'd1) after <= S_IDLE;
                else after <= S_INIT;
                idx   <= idx + 4'd1;
                state <= S_WAIT;
            end
            S_IDLE: begin
                done <= 1'b1;
                if (!ser_busy && !ser_start) begin
                    // TxDAC off before loopback on; loopback off before
                    // TxDAC on.
                    if (r01 != w01 && (w01[6] || r0d == w0d)) begin
                        r01       <= w01;
                        last01    <= 1'b1;
                        ser_word  <= wr(5'h01, w01);
                        ser_oe    <= 16'hFFFF;
                        ser_start <= 1'b1;
                        writes    <= writes + 16'd1;
                        after     <= S_IDLE;
                        state     <= S_WAIT;
                    end else if (r0d != w0d && (w0d == 8'h03 || r01[6])) begin
                        r0d       <= w0d;
                        last01    <= 1'b0;
                        ser_word  <= wr(5'h0d, w0d);
                        ser_oe    <= 16'hFFFF;
                        ser_start <= 1'b1;
                        writes    <= writes + 16'd1;
                        after     <= S_IDLE;
                        state     <= S_WAIT;
                    end else if (r0a != w0a) begin
                        r0a       <= w0a;
                        last01    <= 1'b0;
                        ser_word  <= wr(5'h0a, w0a);
                        ser_oe    <= 16'hFFFF;
                        ser_start <= 1'b1;
                        writes    <= writes + 16'd1;
                        after     <= S_IDLE;
                        state     <= S_WAIT;
                    end else if (gain_pend) begin
                        gain_pend <= 1'b0;
                        last01    <= 1'b0;
                        ser_word  <= wr(5'h09, {2'b01, gain_q});
                        ser_oe    <= 16'hFFFF;
                        ser_start <= 1'b1;
                        writes    <= writes + 16'd1;
                        after     <= S_IDLE;
                        state     <= S_WAIT;
                    end else if (rd_pend) begin
                        rd_pend   <= 1'b0;
                        last01    <= 1'b0;
                        ser_word  <= {3'b100, rd_addr_q, 8'h00};
                        ser_oe    <= 16'hFF00;
                        ser_start <= 1'b1;
                        after     <= S_READ_DONE;
                        state     <= S_WAIT;
                    end
                end
            end
            // Wait for the transfer to start and to finish.
            S_WAIT: if (!ser_start && !ser_busy && cnt[2]) begin
                cnt   <= 18'd0;
                state <= after;
            end else cnt <= cnt + 18'd1;
            S_READ_DONE: begin
                rd_value <= cap_sdio[7:0];
                rd_busy  <= 1'b0;
                state    <= S_IDLE;
            end
            default: state <= S_RESET;
        endcase

        if (restart) begin
            state <= S_RESET;
            cnt   <= 18'd0;
            done  <= 1'b0;
        end
    end

    always_ff @(posedge clk) begin
        // Only the write to 0x01 itself holds the count: other writes and
        // reads while transmitting leave dac_ready alone.
        if (!TX || !done || r01[6] || w01[6] || (last01 && state == S_WAIT))
            pu_cnt <= 12'd0;
        else if (pu_cnt != POWER_UP_CYCLES[11:0])
            pu_cnt <= pu_cnt + 12'd1;
        dac_ready <= TX && !w01[6] && pu_cnt == POWER_UP_CYCLES[11:0];
    end

endmodule
