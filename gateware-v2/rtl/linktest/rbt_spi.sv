// SPDX-License-Identifier: GPL-2.0-or-later
//
// SPI slave for the link test image. SPI mode 0, 48-bit transfers, most
// significant bit first, framed by chip select: the bit count is cleared
// whenever chip select is high, so a stray clock edge costs one transfer.
//
//   to FPGA:  byte 0      bit 7 write, bits 6..0 register address
//             byte 1      ignored
//             bytes 2..5  write data
//   to host:  byte 0      0xA5
//             byte 1      0x5A
//             bytes 2..5  read data of the addressed register
//
// The shifter runs on the SPI clock alone, so bytes 0 and 1 of the reply
// are correct even when the system clock is not running. Register access
// crosses to the system clock by toggle handshakes.
module rbt_spi (
    input  logic        sck,
    input  logic        cs_n,
    input  logic        mosi,
    output logic        miso,
    output logic        miso_oe,

    input  logic        clk,
    output logic [6:0]  addr,
    input  logic [31:0] rdata,
    output logic        wr_stb,
    output logic [31:0] wdata
);

    // ---- SPI clock domain ----
    logic [5:0]  bitcnt  = 6'd0;
    logic [7:0]  cmd_sr;
    logic [7:0]  cmd_q   = 8'h00;
    logic [31:0] data_sr;
    logic [31:0] wdata_q = 32'h0;
    logic        cmd_tgl = 1'b0;
    logic        wr_tgl  = 1'b0;

    always_ff @(posedge sck or posedge cs_n) begin
        if (cs_n) bitcnt <= 6'd0;
        else if (bitcnt != 6'd48) bitcnt <= bitcnt + 6'd1;
    end

    always_ff @(posedge sck) begin
        if (!cs_n) begin
            if (bitcnt < 6'd8) cmd_sr <= {cmd_sr[6:0], mosi};
            if (bitcnt == 6'd7) begin
                cmd_q   <= {cmd_sr[6:0], mosi};
                cmd_tgl <= ~cmd_tgl;
            end
            if (bitcnt >= 6'd16 && bitcnt < 6'd48) data_sr <= {data_sr[30:0], mosi};
            if (bitcnt == 6'd47) begin
                wdata_q <= {data_sr[30:0], mosi};
                if (cmd_q[7]) wr_tgl <= ~wr_tgl;
            end
        end
    end

    // Read data is fetched by the system side after byte 0 and is stable
    // long before byte 2 is shifted.
    logic [31:0] rhold = 32'h0;
    logic [47:0] out_word;
    logic        miso_q = 1'b1;

    assign out_word = {8'hA5, 8'h5A, rhold};

    always_ff @(negedge sck or posedge cs_n) begin
        if (cs_n) miso_q <= 1'b1;
        else if (bitcnt < 6'd48) miso_q <= out_word[6'd47 - bitcnt];
        else miso_q <= 1'b0;
    end

    assign miso    = miso_q;
    assign miso_oe = ~cs_n;

    // ---- system clock domain ----
    logic [2:0] cmd_sync = 3'b000;
    logic [2:0] wr_sync  = 3'b000;
    logic       rd_pend  = 1'b0;

    always_ff @(posedge clk) begin
        cmd_sync <= {cmd_sync[1:0], cmd_tgl};
        wr_sync  <= {wr_sync[1:0], wr_tgl};

        rd_pend <= 1'b0;
        if (cmd_sync[2] ^ cmd_sync[1]) begin
            addr    <= cmd_q[6:0];
            rd_pend <= 1'b1;
        end
        if (rd_pend) rhold <= rdata;

        wr_stb <= 1'b0;
        if (wr_sync[2] ^ wr_sync[1]) begin
            wdata  <= wdata_q;
            wr_stb <= 1'b1;
        end
    end

endmodule
