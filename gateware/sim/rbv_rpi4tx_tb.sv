// SPDX-License-Identifier: GPL-2.0-or-later
//
// Testbench for the rpi4-i2s transmit build (TX = 1), with the radio core replaced
// by sim/radio_stub.sv. Models: the AD9866's serial port and the clock it
// gives the FPGA (38.4 MHz, 76.8 MHz once register 0x04 is written with
// 0x36), the Pi's SPI master with CRC, and the Pi's PCM block receiving
// I2S words.
`timescale 1ns / 1ps
module rbv_rpi4tx_tb;

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

    rbv_rpi4_top #(.TX(1'b1), .NR(6), .RESET_CYCLES(64), .LOCK_CYCLES(256),
                   .KEY_WD_CYCLES(40000), .MS_CYCLES(200)) dut (
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
        wr(1'b1, 6'h04, 32'h0);
        #40000;
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


    // ---- the Pi's PCM block sending: word 0 a sample, word 1 its flags ----
    // Its data changes TX_DELAY after the bit clock's rising edge (about
    // what the hardware measured), frame-aligned with the frame sync it
    // receives. Every frame carries a valid sample while tx_on is set.
    logic        tx_on_host = 1'b0, slip = 1'b0;
    logic [15:0] tx_seq = 16'd0;
    logic [63:0] tx_frame = 64'd0;
    logic [15:0] tx_i = 16'h1000;
    int nb;
    always @(posedge bclk) begin
        if (pbit >= 0) begin
            nb = (pbit + 1) % 64;
            if (nb == 0) begin
                if (tx_on_host) begin
                    tx_frame = {tx_i, ~tx_i, 1'b1, 7'h2a,
                                tx_i[15:8] ^ tx_i[7:0] ^ ~tx_i[15:8] ^ ~tx_i[7:0] ^
                                tx_seq[15:8] ^ tx_seq[7:0], tx_seq};
                    // Slipped: this sample's flags, then the next sample.
                    if (slip) tx_frame = {tx_frame[31:0], tx_i + 16'h0101, ~(tx_i + 16'h0101)};
                    tx_seq   = tx_seq + 16'd1;
                    tx_i     = tx_i + 16'h0101;
                end else tx_frame = 64'd0;
            end
            #14 from_pi = tx_frame[63 - nb];
        end
    end

    function automatic logic [7:0] ad_reg(input int a);
        return ad_regs[a];
    endfunction

    logic [31:0] q, q2, gaps0;

    // Every sample the image passes on must be one the host sent: I in the
    // high half and its complement in the low half (the host model's
    // pattern), never a flags word or a mix of two samples.
    logic wrong_words = 1'b0;
    always @(posedge dut.clk_f)
        if (dut.txf_wr && dut.txf_wdata[15:0] !== ~dut.txf_wdata[31:16]) begin
            $display("FAIL image passed on %h at %0t", dut.txf_wdata, $time); wrong_words = 1'b1;
        end

    initial begin
        #3000;
        rd(1'b1, 6'h02, q); check("CAPS (transmit available)", q, 32'h00002116);
        wait (dut.setup_done === 1'b1);
        #20000;
        check("AD9866 0x01 after set-up, run clear: idle", ad_reg(1), 8'h67);
        check("AD9866 0x0a after set-up (lowest)", ad_reg(10), 8'h40);

        // The link with the return sampling point the driver uses at div 8.
        wr(1'b1, 6'h06, {2'd0, 6'd1, 8'd0, 8'd8, 7'd0, 1'b1});
        wr(1'b0, 6'h00, 32'h0);              // one receiver
        wr(1'b0, 6'h09, {8'd128, 24'd0});    // drive 128: code 20; PA enable clear
        tx_on_host = 1'b1;
        #200000;
        rd(1'b1, 6'h0B, q);
        if (q < 32'd20) begin $display("FAIL transmit samples received: %0d", q); errors++; end
        else $display("ok   transmit samples received while not keyed: %0d", q);
        rd(1'b1, 6'h0F, q); check("no sequence gaps", q, 32'h0);
        rd(1'b1, 6'h13, q); check("no frames failed their check", q, 32'h0);

        // A one-word slip, as the Pi's PCM block can start a stream: each
        // frame carries a flags word, then the next sample. With I negative
        // the sample word has bit 31 set, so a receiver that assumed the
        // phase would take Q and the next I as a sample. The image must move
        // its lock to the new phase and keep taking samples, none of them
        // from the wrong words.
        tx_i = 16'h8000;
        slip = 1'b1;
        #20000;
        rd(1'b1, 6'h14, q); check("realigned to the slipped phase", q, 32'h1);
        rd(1'b1, 6'h0B, q2);
        rd(1'b1, 6'h0F, gaps0);
        #100000;
        rd(1'b1, 6'h0B, q);
        if (q - q2 < 32'd20) begin $display("FAIL samples while slipped: %0d", q - q2); errors++; end
        else $display("ok   samples taken while slipped: %0d", q - q2);
        rd(1'b1, 6'h0F, q); check("no sequence gaps while locked to the slip", q - gaps0, 32'h0);
        check("no sample from the wrong words", {31'd0, wrong_words}, 32'h0);
        slip = 1'b0;
        #20000;
        rd(1'b1, 6'h14, q); check("realigned back", q, 32'h2);
        tx_i = 16'h1000;
        check("PA lines low, not keyed", {28'd0, pa_out}, 32'h0);
        check("TX quiet low, not keyed", {31'd0, ad_txquiet_n}, 32'h0);
        check("AD9866 0x0a lowest, not keyed", ad_reg(10), 8'h40);
        if (dut.radio_i.tx_taken != 0) begin
            $display("FAIL core took samples while not transmitting"); errors++;
        end else $display("ok   core offered nothing while not transmitting");

        // Run: the receive path powers up, the transmit path stays off.
        wr(1'b1, 6'h04, 32'h1);              // run
        #20000;
        check("AD9866 0x01 running, not keyed: transmit path off", ad_reg(1), 8'h60);

        // Keyed without PA allowed: the TxDAC powers up, then the core
        // transmits; no PA line moves.
        wr(1'b1, 6'h05, 32'h3);              // PTT and permission
        #60000;
        check("AD9866 0x01 keyed: everything on", ad_reg(1), 8'h00);
        check("core transmitting", {31'd0, dut.tx_on}, 32'h1);
        check("TX quiet high while transmitting", {31'd0, ad_txquiet_n}, 32'h1);
        check("AD9866 0x0a at the drive while keyed", ad_reg(10), {2'b01, 6'd20});
        check("PA lines low without PA allowed", {28'd0, pa_out}, 32'h0);
        if (dut.radio_i.tx_taken < 3) begin $display("FAIL core took no samples"); errors++; end
        else $display("ok   core took %0d samples", dut.radio_i.tx_taken);

        // PA allowed: the T/R lines follow; envpa/envbias need PA enable.
        wr(1'b1, 6'h04, 32'h5);              // run, PA allowed
        wr(1'b1, 6'h05, 32'h3);
        #2000;
        check("exttr and inttr with PA allowed", {28'd0, pa_out}, 32'h3);
        wr(1'b0, 6'h09, {8'd128, 4'd0, 1'b1, 19'd0});   // PA enable
        #2000;
        check("all four with PA enable", {28'd0, pa_out}, 32'hF);
        wr(1'b0, 6'h09, {8'd128, 24'd0});
        wr(1'b1, 6'h05, 32'h1);              // PTT without permission
        #2000;
        check("nothing without permission", {28'd0, pa_out}, 32'h0);
        check("core idle without permission", {31'd0, dut.tx_on}, 32'h0);

        // The keying watchdog: KEYING not rewritten clears itself.
        wr(1'b1, 6'h05, 32'h3);
        #60000;                              // the TxDAC powers up again first
        check("keyed again", {31'd0, dut.tx_on}, 32'h1);
        #600000;
        rd(1'b1, 6'h05, q); check("KEYING cleared by the watchdog", q, 32'h0);
        rd(1'b1, 6'h12, q); check("watchdog flag in status 2", q & 32'h10, 32'h10);
        #20000;
        check("core idle after the watchdog", {31'd0, dut.tx_on}, 32'h0);
        check("PA lines low after the watchdog", {28'd0, pa_out}, 32'h0);
        check("AD9866 0x0a lowest after the watchdog", ad_reg(10), 8'h40);
        check("AD9866 0x01 transmit path off after the watchdog", ad_reg(1), 8'h60);
        wr(1'b1, 6'h12, 32'h10);

        // Loopback: TxDAC off, digital loopback, TX quiet and PA lines low
        // even with PA allowed and PA enable.
        wr(1'b0, 6'h09, {8'd128, 4'd0, 1'b1, 19'd0});
        wr(1'b1, 6'h04, 32'hD);              // run, PA allowed, loopback
        wr(1'b1, 6'h05, 32'h3);
        #60000;
        check("AD9866 0x01 TxDAC off in loopback", ad_reg(1), 8'h40);
        check("AD9866 0x0d loopback on", ad_reg(13), 8'h43);
        check("core transmitting in loopback", {31'd0, dut.tx_on}, 32'h1);
        check("TX quiet follows the core in loopback", {31'd0, ad_txquiet_n}, 32'h1);
        check("PA lines low in loopback", {28'd0, pa_out}, 32'h0);
        if (ad_tx === 6'd0) begin $display("FAIL TX bus idle in loopback"); errors++; end
        else $display("ok   TX bus carries samples in loopback");
        check("AD9866 0x0a lowest in loopback", ad_reg(10), 8'h40);
        wr(1'b1, 6'h05, 32'h0);
        wr(1'b1, 6'h04, 32'h1);              // loopback off
        #60000;
        check("AD9866 0x0d loopback off", ad_reg(13), 8'h03);
        check("AD9866 0x01 after loopback, not keyed", ad_reg(1), 8'h60);
        check("loopback left in the right order", {31'd0, lb_order_bad}, 32'h0);

        // The jack's PTT keys the core with run set, as Hermes-Lite 2 does;
        // the PA lines need permission and PA allowed.
        wr(1'b0, 6'h09, {8'd128, 24'd0});
        jack_ring = 1'b0;
        #60000;                              // debounce, then the TxDAC powers up
        check("jack PTT keys the core", {31'd0, dut.tx_on}, 32'h1);
        check("jack PTT: PA lines low without permission", {28'd0, pa_out}, 32'h0);
        wr(1'b1, 6'h04, 32'h5);
        wr(1'b1, 6'h05, 32'h2);              // permission only
        #2000;
        check("jack PTT with permission and PA allowed", {28'd0, pa_out}, 32'h3);
        jack_ring = 1'b1;
        wr(1'b1, 6'h05, 32'h0);
        wr(1'b1, 6'h04, 32'h0);
        #30000;
        check("all quiet at the end", {27'd0, ad_txquiet_n, pa_out}, 32'h0);
        check("AD9866 0x01 run clear at the end: idle", ad_reg(1), 8'h67);
        check("core started only with the TxDAC settled", {31'd0, early_tx}, 32'h0);
        check("receivers held while run was clear", {31'd0, rx_while_idle}, 32'h0);

        if (errors == 0) $display("PASS");
        else $display("FAILED: %0d errors", errors);
        $finish;
    end

    // The AD9866 never has its TxDAC on (0x01 bit 6 clear) while in digital
    // loopback, and loopback is never switched on with the TxDAC on.
    logic lb_order_bad = 1'b0;
    always @(posedge ad_sen_n) if (!ad_regs[1][6] && ad_reg(13) == 8'h43) lb_order_bad = 1'b1;

    // Outside loopback the core starts transmitting only after the TxDAC
    // write has finished and settled, never with its power-down bit set.
    logic early_tx = 1'b0;
    always @(posedge dut.tx_on)
        if (!dut.control[3] && (!dut.dac_ready || ad_regs[1][6])) begin
            $display("FAIL core keyed before the TxDAC was ready at %0t", $time); early_tx = 1'b1;
        end

    // No receive word reaches the link while run is clear.
    logic rx_while_idle = 1'b0;
    always @(posedge dut.clk) if (dut.rx_tvalid && !dut.run_s[1]) rx_while_idle = 1'b1;

    // PA lines move only with permission, PA allowed and outside loopback.
    always @(pa_out)
        if (pa_out !== 4'b0 && !(dut.keying[1] && dut.control[2] && !dut.control[3])) begin
            $display("FAIL PA line moved without its gates at %0t", $time); errors++;
        end

endmodule
