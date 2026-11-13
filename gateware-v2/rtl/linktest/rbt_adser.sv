// SPDX-License-Identifier: GPL-2.0-or-later
//
// Generic 16-bit serial transaction for the AD9866's serial port. The
// host supplies the word to send and, bit by bit, whether the FPGA
// drives the data line; the level of the data line and of the separate
// data-out line is captured at every rising serial clock edge. Knowledge
// of the chip's registers stays in the host tool.
//
// Serial clock: system clock / 16 (4.8 MHz at 76.8 MHz). Data changes
// while the serial clock is low.
module rbt_adser (
    input  logic        clk,
    input  logic        rst,

    input  logic        start,
    input  logic [15:0] txword,
    input  logic [15:0] oemask,
    output logic        busy,
    output logic [15:0] cap_sdio,
    output logic [15:0] cap_sdo,

    output logic        sen_n,
    output logic        sclk,
    output logic        sdio_out,
    output logic        sdio_oe,
    input  logic        sdio_in,
    input  logic        sdo_in
);

    typedef enum logic [1:0] {IDLE, LOW, HIGH, DONE} state_t;
    state_t      state = IDLE;
    logic [2:0]  div   = 3'd0;
    logic [3:0]  bitn  = 4'd0;
    logic [15:0] sr    = 16'h0;
    logic [15:0] oe_sr = 16'h0;
    logic        tick;

    assign tick = (div == 3'd7);

    always_ff @(posedge clk) begin
        if (rst) begin
            state   <= IDLE;
            sen_n   <= 1'b1;
            sclk    <= 1'b0;
            sdio_oe <= 1'b0;
            busy    <= 1'b0;
            div     <= 3'd0;
        end else begin
            div <= div + 3'd1;
            case (state)
                IDLE: begin
                    sen_n   <= 1'b1;
                    sclk    <= 1'b0;
                    sdio_oe <= 1'b0;
                    busy    <= 1'b0;
                    if (start) begin
                        sr    <= txword;
                        oe_sr <= oemask;
                        bitn  <= 4'd15;
                        busy  <= 1'b1;
                        div   <= 3'd0;
                        state <= LOW;
                        sen_n <= 1'b0;
                        sdio_out <= txword[15];
                        sdio_oe  <= oemask[15];
                    end
                end
                LOW: if (tick) begin
                    sclk     <= 1'b1;
                    cap_sdio <= {cap_sdio[14:0], sdio_in};
                    cap_sdo  <= {cap_sdo[14:0], sdo_in};
                    state    <= HIGH;
                end
                HIGH: if (tick) begin
                    sclk <= 1'b0;
                    if (bitn == 4'd0) begin
                        state <= DONE;
                    end else begin
                        bitn     <= bitn - 4'd1;
                        sr       <= {sr[14:0], 1'b0};
                        oe_sr    <= {oe_sr[14:0], 1'b0};
                        sdio_out <= sr[14];
                        sdio_oe  <= oe_sr[14];
                        state    <= LOW;
                    end
                end
                DONE: if (tick) begin
                    sen_n   <= 1'b1;
                    sdio_oe <= 1'b0;
                    state   <= IDLE;
                end
                default: state <= IDLE;
            endcase
        end
    end

endmodule
