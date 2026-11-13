// SPDX-License-Identifier: GPL-2.0-or-later
//
// Simulation stand-in for the Hermes-Lite 2 radio core: same ports; takes
// the receiver count (command address 0, data bits 6..3, plus one) and
// produces one frame of 2 x receivers values every PERIOD clocks, when
// rx_tready allows, in the core's order I0, Q0, I1, Q1 ... Each value is
// {receiver * 2 + (Q ? 1 : 0), frame number[15:0]} so a checker can tell
// exactly what it got. Records the last command for checks.
`timescale 1ns / 1ps
module radio #(
    parameter NR = 3, parameter NT = 1, parameter LRDATA = 0, parameter VNA = 1,
    parameter CLK_FREQ = 76800000, parameter PERIOD = 256
) (
    input clk, input clk_2x, input rst_channels, input rst_all, input rst_nco,
    input link_running, input link_master, input [23:0] lm_data, input lm_valid,
    output ls_valid, input ls_done,
    input ds_cmd_ptt, input run, input qmsec_pulse, input ext_keydown, input ext_ptt,
    output tx_on, output cw_on, output [18:0] cw_profile,
    input [31:0] tx_tdata, input tx_tlast, output tx_tready, input tx_tvalid,
    input [3:0] tx_tuser, output tx_twait, output [11:0] tx_data_dac,
    input clk_envelope, output tx_envelope_pwm_out, output tx_envelope_pwm_out_inv,
    input [31:0] lr_tdata, input [2:0] lr_tid, input lr_tlast, output lr_tready,
    input lr_tvalid,
    input [11:0] rx_data_adc,
    output logic [23:0] rx_tdata, output logic rx_tlast, input rx_tready,
    output logic rx_tvalid, output [1:0] rx_tuser,
    input [5:0] cmd_addr, input [31:0] cmd_data, input cmd_rqst, output cmd_ack,
    output [15:0] debug_out
);
    // Transmit, as the core's state machine starts it: from the jack (key
    // or PTT), or from the host's PTT only together with a sample carrying
    // PTT (tx_tuser[3] with tx_tvalid); it stays while run and a keying
    // input hold. Takes one sample every TX_PERIOD clocks, sending the top
    // 12 bits of I to the DAC, and drains samples at the same rate outside
    // transmit, as the core does.
    parameter TX_PERIOD = 64;
    logic        txs = 1'b0;
    int          ttick = 0;
    logic [31:0] tx_last = 32'd0;
    int          tx_taken = 0;
    assign tx_on = txs;
    assign cw_on = 1'b0;
    assign rx_tuser = 2'b00;
    assign tx_tready = (ttick == 0);
    assign tx_data_dac = txs ? tx_last[31:20] : 12'd0;
    always @(posedge clk) begin
        if (!txs) txs <= run && (ext_ptt || ext_keydown ||
                                 (ds_cmd_ptt && tx_tuser[3] && tx_tvalid));
        else      txs <= run && (ext_ptt || ext_keydown || ds_cmd_ptt);
        ttick <= (ttick == TX_PERIOD - 1) ? 0 : ttick + 1;
        if (tx_tready && tx_tvalid) begin
            if (txs) begin
                tx_last  <= tx_tdata;
                tx_taken <= tx_taken + 1;
            end
        end
    end

    logic [3:0]  last_chan = 4'd0;
    logic [5:0]  last_addr = 6'd0;
    logic [31:0] last_data = 32'd0;
    int          cmds = 0;
    always @(posedge clk) if (cmd_rqst) begin
        last_addr <= cmd_addr;
        last_data <= cmd_data;
        cmds      <= cmds + 1;
        if (cmd_addr == 6'h00) last_chan <= cmd_data[6:3];
    end

    int          tick = 0;
    logic [15:0] fno = 16'd0;
    int          k = -1;     // value being sent, -1 idle
    always @(posedge clk) begin
        rx_tvalid <= 1'b0;
        rx_tlast  <= 1'b0;
        if (rst_all || rst_channels) begin
            k    <= -1;
            tick <= 0;
        end else begin
            tick <= (tick == PERIOD - 1) ? 0 : tick + 1;
            if (k < 0) begin
                if (tick == 0 && rx_tready) k <= 0;
                else if (tick == 0) fno <= fno + 16'd1;   // frame lost
            end else begin
                rx_tvalid <= 1'b1;
                rx_tdata  <= {k[7:0], fno};
                rx_tlast  <= (k == 2 * last_chan + 1);
                if (k == 2 * last_chan + 1) begin
                    k   <= -1;
                    fno <= fno + 16'd1;
                end else k <= k + 1;
            end
        end
    end
endmodule
