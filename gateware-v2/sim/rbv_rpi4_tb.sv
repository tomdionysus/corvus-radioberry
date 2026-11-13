// SPDX-License-Identifier: GPL-2.0-or-later
//
// Testbench for the rpi4-i2s receive build, with the radio core replaced
// by sim/radio_stub.sv. Models: the AD9866's serial port and the clock it
// gives the FPGA (38.4 MHz, 76.8 MHz once register 0x04 is written with
// 0x36), the Pi's SPI master with CRC, and the Pi's PCM block receiving
// I2S words.
`timescale 1ns / 1ps
module rbv_rpi4_tb;

    // ---- board clock: 38.4 MHz until the AD9866 doubles it ----
    logic clk53 = 1'b0;
    real  half = 13.02;
    always #(half) clk53 = ~clk53;

    // ---- DUT ----
    logic sck = 1'b1, ce_n = 1'b1, mosi = 1'b0;
    wire  miso, bclk, fs, to_pi;
    logic from_pi = 1'b0;
    logic jack_tip = 1'b1, jack_ring = 1'b1;
    wire [3:0] pa_out;
    wire ad_rst_n, ad_mode, ad_sen_n, ad_sclk, ad_sdio, ad_txsync, ad_txquiet_n;
    wire [5:0] ad_tx;
    logic [5:0] ad_rx = 6'd0;
    logic ad_rxsync = 1'b0;

    rbv_rpi4_top #(.NR(6), .RESET_CYCLES(64), .LOCK_CYCLES(256)) dut (
        .clk53(clk53), .spi_sck(sck), .spi_ce0_n(ce_n), .spi_mosi(mosi), .spi_miso(miso),
        .i2s_bclk(bclk), .i2s_fs(fs), .i2s_to_pi(to_pi), .i2s_from_pi(from_pi),
        .jack_tip(jack_tip), .jack_ring(jack_ring), .pa_out(pa_out), .ad_rst_n(ad_rst_n), .ad_mode(ad_mode), .ad_sen_n(ad_sen_n),
        .ad_sclk(ad_sclk), .ad_sdio(ad_sdio), .ad_rx(ad_rx), .ad_rxsync(ad_rxsync),
        .ad_tx(ad_tx), .ad_txsync(ad_txsync), .ad_txquiet_n(ad_txquiet_n)
    );

    int errors = 0;
    task automatic check(input string what, input logic [31:0] got, input logic [31:0] want);
        if (got !== want) begin
            $display("FAIL %s: got %08x, want %08x", what, got, want);
            errors++;
        end else $display("ok   %s: %08x", what, got);
    endtask

    // ---- AD9866 serial port model ----
    logic [7:0] ad_regs [0:31];
    int         ad_nwrites = 0;
    int         bad_txgain = 0;
    logic [4:0] ad_wlog_a [0:63];
    logic [7:0] ad_wlog_d [0:63];
    logic       ad_drive = 1'b0, ad_dout = 1'b0;
    assign (weak1, weak0) ad_sdio = 1'b1;
    assign ad_sdio = ad_drive ? ad_dout : 1'bz;

    initial begin
        for (int i = 0; i < 32; i++) ad_regs[i] = 8'h00;
        ad_regs[8'h0a] = 8'h7f;
        ad_regs[8'h08] = 8'h80;
    end

    always @(negedge ad_sen_n) begin : ad_frame
        logic [15:0] w;
        w = 16'h0;
        for (int i = 15; i >= 0; i--) begin
            if (i == 7 && w[15]) begin
                // Read: drive the register's bits on the data line.
                ad_drive = 1'b1;
            end
            if (ad_drive) ad_dout = ad_regs[w[12:8]][i];
            @(posedge ad_sclk or posedge ad_sen_n);
            if (ad_sen_n) disable ad_frame;
            w[i] = ad_sdio;
            @(negedge ad_sclk or posedge ad_sen_n);
        end
        ad_drive = 1'b0;
        if (!w[15]) begin
            ad_regs[w[12:8]] = w[7:0];
            if (w[12:8] == 5'h0a && w[7:0] != 8'h40) bad_txgain++;
            ad_wlog_a[ad_nwrites] = w[12:8];
            ad_wlog_d[ad_nwrites] = w[7:0];
            ad_nwrites++;
            if (w[12:8] == 5'h04 && w[7:0] == 8'h36) begin
                #2000 half = 6.51;
            end
        end
    end
    always @(posedge ad_sen_n) ad_drive = 1'b0;

    // ---- AD9866 receive bus: a ramp, high half then low half with sync ----
    logic        clk2 = 1'b0;
    logic        hph = 1'b0;
    logic [11:0] ramp = 12'h100;
    always @(clk53) begin
        clk2 = 1'b1;
        #(half / 2) clk2 = 1'b0;
    end
    always @(posedge clk2) begin
        hph <= ~hph;
        if (hph) begin ad_rx <= ramp[5:0]; ad_rxsync <= 1'b1; ramp <= ramp + 12'd1; end
        else     begin ad_rx <= ramp[11:6]; ad_rxsync <= 1'b0; end
    end

    // ---- SPI master, mode 3, framed protocol ----
    function automatic logic [7:0] crc8(input logic [55:0] d);
        logic [7:0] c;
        c = 8'h00;
        for (int i = 55; i >= 0; i--) begin
            logic fb;
            fb = c[7] ^ d[i];
            c = {c[6:0], 1'b0} ^ (fb ? 8'h07 : 8'h00);
        end
        return c;
    endfunction

    logic [7:0] seq = 8'd0;
    task automatic xfer(input logic wr, input logic sp, input logic [5:0] a, input logic [31:0] d,
                        input logic bad_crc, output logic [63:0] rep);
        logic [55:0] body;
        logic [63:0] out;
        body = {wr, sp, a, d, seq, 8'h00};
        out  = {body, crc8(body) ^ (bad_crc ? 8'h01 : 8'h00)};
        seq  = seq + 8'd1;
        ce_n = 1'b0;
        #50;
        for (int i = 63; i >= 0; i--) begin
            sck = 1'b0;
            mosi = out[i];
            #40 sck = 1'b1;
            rep[i] = miso;
            #40;
        end
        #50 ce_n = 1'b1;
        #400;
    endtask

    task automatic rd(input logic sp, input logic [5:0] a, output logic [31:0] q);
        logic [63:0] r;
        xfer(1'b0, sp, a, 32'h0, 1'b0, r);
        if (r[63:56] !== 8'hA5 || crc8(r[63:8]) !== r[7:0]) begin
            $display("FAIL reply: %016x (crc want %02x)", r, crc8(r[63:8]));
            errors++;
        end
        q = r[47:16];
    endtask

    task automatic wr(input logic sp, input logic [5:0] a, input logic [31:0] d);
        logic [63:0] r;
        xfer(1'b1, sp, a, d, 1'b0, r);
    endtask

    // ---- the Pi's PCM block: words on the falling bit clock edge ----
    logic        fs_prev = 1'b0;
    int          pbit = -1;
    logic [31:0] pw = 32'h0;
    logic [31:0] got [$];
    int          idle_words = 0;
    always @(negedge bclk) begin
        if (fs && !fs_prev) pbit = 0;
        else if (pbit >= 0) pbit = (pbit + 1) % 64;
        fs_prev = fs;
        if (pbit >= 0) begin
            pw = {pw[30:0], to_pi};
            if (pbit % 32 == 31) begin
                if (pw[31]) got.push_back(pw);
                else idle_words++;
            end
        end
    end

    task automatic stream_check(input logic [7:0] div, input int ns, input logic expect_full);
        logic [31:0] full0, full1;
        int expect_idx, bad;
        logic [15:0] fno;
        got.delete();
        idle_words = 0;
        rd(1'b1, 6'h09, full0);
        wr(1'b1, 6'h06, 32'h0);
        wr(1'b1, 6'h06, {8'd0, 8'd1, div, 7'd0, 1'b1});
        wr(1'b1, 6'h04, 32'h1);
        #(ns);
        check("AD9866 0x01 running: receive path on, transmit off", ad_regs[5'h01], 8'h60);
        wr(1'b1, 6'h04, 32'h0);
        #40000;
        check("AD9866 0x01 run clear: idle again", ad_regs[5'h01], 8'h67);
        rd(1'b1, 6'h09, full1);
        expect_idx = 0; bad = 0;
        for (int i = 0; i < got.size(); i++) begin
            logic [31:0] w;
            w = got[i];
            if (w[27:24] != expect_idx[3:0] || w[23:16] != expect_idx[7:0]) bad++;
            else if (expect_idx == 0) fno = w[15:0];
            else if (w[15:0] != fno) bad++;
            expect_idx = (expect_idx == 3) ? 0 : expect_idx + 1;
        end
        if (got.size() < 200 || bad != 0) begin
            $display("FAIL stream div %0d: %0d words, %0d out of order", div, got.size(), bad);
            errors++;
        end else $display("ok   stream div %0d: %0d words, in order, %0d idle", div, got.size(), idle_words);
        if ((full1 != full0) !== expect_full) begin
            $display("FAIL stream div %0d: FIFO refusals %0d, expected %s", div, full1 - full0,
                     expect_full ? "some" : "none");
            errors++;
        end else $display("ok   stream div %0d: FIFO refusals %0d", div, full1 - full0);
    endtask

    logic [31:0] q;

    initial begin
        #3000;
        rd(1'b1, 6'h00, q); check("ID", q, 32'h52425632);
        rd(1'b1, 6'h02, q); check("CAPS", q, 32'h00002106);

        // A frame with a bad CRC is ignored and counted.
        begin
            logic [63:0] r;
            xfer(1'b1, 1'b1, 6'h0D, 32'h11111111, 1'b1, r);
        end
        rd(1'b1, 6'h0D, q); check("bad-CRC write ignored", q, 32'h0);
        rd(1'b1, 6'h07, q); check("CRC errors counted", q, 32'h1);
        wr(1'b1, 6'h0D, 32'hCAFEF00D);
        rd(1'b1, 6'h0D, q); check("SCRATCH", q, 32'hCAFEF00D);

        // Start-up: the AD9866 is programmed and the radio comes out of reset.
        wait (dut.setup_done === 1'b1);
        #2000;
        rd(1'b1, 6'h03, q); check("STATUS locked, set up, running", q & 32'h7, 32'h7);
        check("first AD9866 write is 0x04", {ad_wlog_a[0], ad_wlog_d[0]}, {5'h04, 8'h36});
        check("AD9866 0x01 idle: all but PLL, REF, CML off", ad_regs[5'h01], 8'h67);
        check("AD9866 0x0a (TX gain lowest)", ad_regs[5'h0a], 8'h40);
        check("AD9866 0x0d", ad_regs[5'h0d], 8'h03);

        // Radio space: a command reaches the radio and reads back.
        wr(1'b0, 6'h00, 32'h0000_0008);   // two receivers (bits 6..3 = 1)
        rd(1'b0, 6'h00, q); check("radio shadow 0x00", q, 32'h8);
        #200;
        check("radio got the command", {26'd0, dut.radio_i.last_addr}, 32'h0);
        check("radio receiver count", {28'd0, dut.radio_i.last_chan}, 32'h1);

        // Receive gain: Hermes-Lite 2 LNA word 0x5f (bit 6 set: code 31).
        wr(1'b0, 6'h0a, 32'h0000_005f);
        #40000;
        check("AD9866 0x09 from the LNA command", ad_regs[5'h09], {2'b01, 6'd31});
        check("AD9866 0x0a still lowest", ad_regs[5'h0a], 8'h40);

        // AD9866 read through the board register.
        wr(1'b1, 6'h0C, 32'h08);
        #40000;
        rd(1'b1, 6'h0C, q); check("AD_READ 0x08", q, 32'h40);

        // The receive build refuses keying.
        wr(1'b1, 6'h05, 32'h3);
        rd(1'b1, 6'h05, q); check("KEYING refused (receive build)", q, 32'h0);

        // Stream that fits: two receivers, 4 words per 256 clocks (1.2 M
        // words/s) against I2S at 122.88 MHz / 2 (1.92 M words/s).
        stream_check(8'd2, 400000, 1'b0);
        // Stream that does not fit: I2S at 122.88 MHz / 16 (0.24 M words/s):
        // whole frames are refused, the order is kept.
        stream_check(8'd16, 1500000, 1'b1);

        // Transmit side never moved.
        if (pa_out !== 4'b0 || ad_tx !== 6'd0 || ad_txsync !== 1'b0 || ad_txquiet_n !== 1'b0) begin
            $display("FAIL transmit-side pins moved"); errors++;
        end else $display("ok   transmit-side pins low throughout");
        check("AD9866 0x0a never written other than lowest", bad_txgain, 0);

        if (errors == 0) $display("PASS");
        else $display("FAILED: %0d errors", errors);
        $finish;
    end

    // Transmit-side pins are watched all the time, not only at the end.
    always @(pa_out or ad_tx or ad_txsync or ad_txquiet_n)
        if ($time > 0 && (pa_out !== 4'b0 || ad_tx !== 6'd0 || ad_txsync !== 1'b0 || ad_txquiet_n !== 1'b0)) begin
            $display("FAIL transmit-side pin moved at %0t", $time); errors++;
        end

endmodule
