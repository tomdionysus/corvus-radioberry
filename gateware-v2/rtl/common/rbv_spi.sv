// SPDX-License-Identifier: GPL-2.0-or-later
//
// SPI0 slave for the framed control protocol. SPI mode 3 (clock idles
// high; data changes on the falling edge and is sampled on the rising
// edge), 64-bit frames, most significant bit first, framed by chip select:
// the bit counters clear whenever chip select is high, so a stray clock
// edge costs one frame.
//
// Host to FPGA:
//   byte 0     bit 7 write, bit 6 space (0 radio, 1 board), 5..0 address
//   bytes 1-4  data, most significant byte first
//   byte 5     sequence number, echoed in the next reply
//   byte 6     0
//   byte 7     CRC-8 of bytes 0 to 6
// FPGA to host, in the same frame:
//   byte 0     0xA5
//   byte 1     status
//   bytes 2-5  the addressed register's value, read before any write
//   byte 6     status 2
//   byte 7     CRC-8 of bytes 0 to 6
//
// A frame whose CRC does not match is counted and ignored. Accepted frames
// cross to the configuration clock by a toggle; the frame's fields stay
// unchanged until the next frame ends, which is at least 64 SPI clocks
// later.
module rbv_spi (
    input  logic        sck,
    input  logic        cs_n,
    input  logic        mosi,
    output logic        miso,

    // configuration clock domain
    input  logic        clk,
    output logic        rd_req,      // pulse: address valid, fetch rd_data
    output logic        rd_space,
    output logic [5:0]  rd_addr,
    input  logic [31:0] rd_data,     // valid 3 clk after rd_req
    input  logic [7:0]  status,
    input  logic [7:0]  status2,
    output logic        frame_stb,   // pulse: a frame with a good CRC
    output logic        frame_bad,   // pulse: a frame with a bad CRC
    output logic        f_write,
    output logic        f_space,
    output logic [5:0]  f_addr,
    output logic [31:0] f_data
);

    // ---- receive, on the rising edge ----
    logic [6:0]  rbit = 7'd0;
    logic [63:0] rsr = 64'd0;
    logic [7:0]  rcrc = 8'd0;
    logic [7:0]  hdr_q = 8'd0;
    logic        hdr_tgl = 1'b0;
    logic [55:0] frame_q = 56'd0;
    logic        good_tgl = 1'b0, bad_tgl = 1'b0;

    always_ff @(posedge sck or posedge cs_n) begin
        if (cs_n) begin
            rbit <= 7'd0;
            rcrc <= 8'd0;
        end else if (rbit != 7'd64) begin
            rbit <= rbit + 7'd1;
            rsr  <= {rsr[62:0], mosi};
            if (rbit < 7'd56) rcrc <= rbv_crc8::step(rcrc, mosi);
        end
    end

    always_ff @(posedge sck) begin
        if (!cs_n) begin
            if (rbit == 7'd7) begin
                hdr_q   <= {rsr[6:0], mosi};
                hdr_tgl <= ~hdr_tgl;
            end
            if (rbit == 7'd63) begin
                if ({rsr[6:0], mosi} == rcrc) begin
                    frame_q  <= rsr[62:7];
                    good_tgl <= ~good_tgl;
                end else begin
                    bad_tgl <= ~bad_tgl;
                end
            end
        end
    end

    // ---- transmit, on the falling edge ----
    logic [6:0]  obit = 7'd0;
    logic [7:0]  ocrc = 8'd0;
    logic [55:0] oword;
    logic [31:0] rhold = 32'd0;
    logic [7:0]  status_q = 8'd0, status2_q = 8'd0;
    logic        obit_now;

    assign oword = {8'hA5, status_q, rhold, status2_q};

    always_ff @(negedge sck or posedge cs_n) begin
        if (cs_n) begin
            obit <= 7'd0;
            ocrc <= 8'd0;
        end else if (obit != 7'd64) begin
            obit <= obit + 7'd1;
            if (obit < 7'd56) ocrc <= rbv_crc8::step(ocrc, obit_now);
        end
    end

    assign obit_now = (obit < 7'd56) ? oword[7'd55 - obit] : ocrc[7'd7 - (obit - 7'd56)];

    // The bit for the coming rising edge, registered on the falling edge.
    always_ff @(negedge sck) begin
        if (obit == 7'd0) begin
            status_q  <= status;
            status2_q <= status2;
        end
        miso <= obit_now;
    end

    // ---- configuration clock domain ----
    logic [2:0] hdr_s = 3'b000, good_s = 3'b000, bad_s = 3'b000;
    logic [2:0] rd_pipe = 3'b000;

    always_ff @(posedge clk) begin
        hdr_s  <= {hdr_s[1:0], hdr_tgl};
        good_s <= {good_s[1:0], good_tgl};
        bad_s  <= {bad_s[1:0], bad_tgl};

        rd_req    <= 1'b0;
        frame_stb <= 1'b0;
        frame_bad <= 1'b0;
        rd_pipe   <= {rd_pipe[1:0], 1'b0};

        if (hdr_s[2] ^ hdr_s[1]) begin
            rd_space <= hdr_q[6];
            rd_addr  <= hdr_q[5:0];
            rd_req   <= 1'b1;
            rd_pipe  <= 3'b001;
        end
        if (rd_pipe[2]) rhold <= rd_data;

        if (good_s[2] ^ good_s[1]) begin
            f_write   <= frame_q[55];
            f_space   <= frame_q[54];
            f_addr    <= frame_q[53:48];
            f_data    <= frame_q[47:16];
            frame_stb <= 1'b1;
        end
        if (bad_s[2] ^ bad_s[1]) frame_bad <= 1'b1;
    end

endmodule
