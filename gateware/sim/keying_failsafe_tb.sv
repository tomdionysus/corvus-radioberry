// SPDX-License-Identifier: GPL-2.0-or-later
//
// The keying fail-safe (takeover stage 6d): the rpi4-i2s transmit build
// (TX = 1) with the real radio core (rtl/core/rbv_core.sv), not the stub.
// Every path that can put the core into transmit is started, then left:
// the host stops rewriting KEYING, as a host that has crashed would. Each
// must end with the core idle, the PA lines low and the AD9866's transmit
// path powered down, within the keying watchdog plus what the core takes
// to finish (its CW ramp down and hang time), and never later.
//
// Paths: software PTT (KEYING PTT + permission) with samples flowing; the
// same with the PTT hang time at 31, which keeps the core keyed until
// software PTT drops (the daemon refuses 31; the gateware must still be
// safe); the jack's PTT with permission; the jack's key (CW) with
// permission. Also: the jack keys nothing without permission, and
// clearing KEYING by hand stops the jack's key at once.
//
// Models as sim/rbv_rpi4tx_tb.sv: the AD9866 serial port and clock, the
// Pi's SPI master, and the Pi's PCM block receiving and sending.
`timescale 1ns / 1ps
module keying_failsafe_tb;

    logic clk53 = 1'b0;
    real  half = 13.02;
    always #(half) clk53 = ~clk53;

    logic sck = 1'b1, ce_n = 1'b1, mosi = 1'b0;
    wire  miso, bclk, fs, to_pi;
    logic from_pi = 1'b0;
    logic jack_tip = 1'b1, jack_ring = 1'b1;
    wire [3:0] pa_out;
    wire ad_rst_n, ad_mode, ad_sen_n, ad_sclk, ad_sdio, ad_txsync, ad_txquiet_n;
    wire [5:0] ad_tx;
    logic [5:0] ad_rx = 6'd0;
    logic ad_rxsync = 1'b0;

    // Watchdog 40000 board clocks: about 0.52 ms once the clock is 76.8 MHz.
    localparam int WD = 40000;
    rbv_rpi4_top #(.TX(1'b1), .NR(6), .RESET_CYCLES(64), .LOCK_CYCLES(256),
                   .KEY_WD_CYCLES(WD), .MS_CYCLES(200)) dut (
        .clk53(clk53), .spi_sck(sck), .spi_ce0_n(ce_n), .spi_mosi(mosi), .spi_miso(miso),
        .i2s_bclk(bclk), .i2s_fs(fs), .i2s_to_pi(to_pi), .i2s_from_pi(from_pi),
        .jack_tip(jack_tip), .jack_ring(jack_ring), .pa_out(pa_out), .ad_rst_n(ad_rst_n),
        .ad_mode(ad_mode), .ad_sen_n(ad_sen_n), .ad_sclk(ad_sclk), .ad_sdio(ad_sdio),
        .ad_rx(ad_rx), .ad_rxsync(ad_rxsync),
        .ad_tx(ad_tx), .ad_txsync(ad_txsync), .ad_txquiet_n(ad_txquiet_n)
    );

    int errors = 0;
    task automatic check(input string what, input logic [31:0] got, input logic [31:0] want);
        if (got !== want) begin
            $display("FAIL %s: got %08x, want %08x", what, got, want);
            errors++;
        end else $display("ok   %s: %08x", what, got);
    endtask

    // ---- AD9866 serial port ----
    logic [7:0] ad_regs [0:31];
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
            if (i == 7 && w[15]) ad_drive = 1'b1;
            if (ad_drive) ad_dout = ad_regs[w[12:8]][i];
            @(posedge ad_sclk or posedge ad_sen_n);
            if (ad_sen_n) disable ad_frame;
            w[i] = ad_sdio;
            @(negedge ad_sclk or posedge ad_sen_n);
        end
        ad_drive = 1'b0;
        if (!w[15]) begin
            ad_regs[w[12:8]] = w[7:0];
            if (w[12:8] == 5'h04 && w[7:0] == 8'h36) #2000 half = 6.51;
        end
    end
    always @(posedge ad_sen_n) ad_drive = 1'b0;
    function automatic logic [7:0] ad_reg(input int a);
        return ad_regs[a];
    endfunction

    // ---- AD9866 receive bus: a ramp ----
    logic        clk2 = 1'b0, hph = 1'b0;
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

    // ---- SPI master, framed protocol ----
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
    logic       spi_busy = 1'b0;   // the keepalive and the test share the bus
    task automatic xfer(input logic w, input logic sp, input logic [5:0] a, input logic [31:0] d,
                        output logic [63:0] rep);
        logic [55:0] body;
        logic [63:0] out;
        while (spi_busy) #100;
        spi_busy = 1'b1;
        body = {w, sp, a, d, seq, 8'h00};
        out  = {body, crc8(body)};
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
        spi_busy = 1'b0;
    endtask
    task automatic rd(input logic sp, input logic [5:0] a, output logic [31:0] q);
        logic [63:0] r;
        xfer(1'b0, sp, a, 32'h0, r);
        q = r[47:16];
    endtask
    task automatic wr(input logic sp, input logic [5:0] a, input logic [31:0] d);
        logic [63:0] r;
        xfer(1'b1, sp, a, d, r);
    endtask

    // ---- the Pi's PCM block: frame position, and samples to the image ----
    logic fs_prev = 1'b0;
    int   pbit = -1;
    always @(negedge bclk) begin
        if (fs && !fs_prev) pbit = 0;
        else if (pbit >= 0) pbit = (pbit + 1) % 64;
        fs_prev = fs;
    end
    logic        tx_on_host = 1'b0;
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
                    tx_seq = tx_seq + 16'd1;
                    tx_i   = tx_i + 16'h0101;
                end else tx_frame = 64'd0;
            end
            #14 from_pi = tx_frame[63 - nb];
        end
    end

    // ---- the fail-safe property, checked on every clock ----
    // Once KEYING has gone to 0 (the watchdog, or by hand) and LIMIT has
    // passed, the core must be idle, the PA lines low and TX quiet low.
    realtime cleared_at = 0;
    logic    cleared = 1'b0;
    realtime limit_ns = 0;
    always @(posedge clk53) begin
        if (dut.keying == 2'b00) begin
            if (!cleared) begin cleared = 1'b1; cleared_at = $realtime; end
        end else cleared = 1'b0;
        if (cleared && limit_ns > 0 && $realtime - cleared_at > limit_ns &&
            (dut.tx_on !== 1'b0 || pa_out !== 4'b0 || ad_txquiet_n !== 1'b0)) begin
            $display("FAIL still transmitting %0.0f ns after KEYING cleared (tx_on %b pa %b quiet %b)",
                     $realtime - cleared_at, dut.tx_on, pa_out, ad_txquiet_n);
            errors++;
            limit_ns = 0;  // once per case
        end
    end
    // How long transmit lasted after KEYING cleared, per case.
    realtime last_on = 0;
    always @(posedge clk53) if (dut.tx_on === 1'b1) last_on = $realtime;

    task automatic expect_stop(input string what, input realtime within_ns);
        realtime start;
        logic [31:0] q;
        start = $realtime;
        limit_ns = within_ns;
        wait (dut.keying == 2'b00);
        #(within_ns + 100000);
        check({what, ": core idle"}, {31'd0, dut.tx_on}, 32'h0);
        check({what, ": PA lines low"}, {28'd0, pa_out}, 32'h0);
        check({what, ": TX quiet low"}, {31'd0, ad_txquiet_n}, 32'h0);
        check({what, ": AD9866 transmit path off"}, ad_reg(1), 8'h60);
        $display("info %s: transmit ended %0.3f ms after KEYING cleared", what,
                 (last_on - cleared_at) / 1e6);
        limit_ns = 0;
        rd(1'b1, 6'h12, q);
        wr(1'b1, 6'h12, 32'h10);       // clear the watchdog flag
    endtask

    // The host's keepalive: rewrites KEYING every 200 us while refresh is
    // set (the driver does it every RB_KEEPALIVE_MS); clearing refresh is
    // the host stopping.
    logic        refresh = 1'b0;
    logic [31:0] keying_val = 32'h0;
    initial forever begin
        #200000;
        if (refresh) wr(1'b1, 6'h05, keying_val);
    end
    task automatic key(input logic [31:0] v);
        keying_val = v;
        refresh = 1'b1;
        wr(1'b1, 6'h05, v);
    endtask

    logic [31:0] q;
    initial begin
        #3000;
        wait (dut.setup_done === 1'b1);
        #20000;
        wr(1'b1, 6'h06, {2'd0, 6'd1, 8'd0, 8'd8, 7'd0, 1'b1});  // the link, div 8
        wr(1'b1, 6'h20, 32'h0);                       // one receiver
        // Short timings for the test: TX start latency 1 ms, PTT hang 2 ms,
        // CW hang 0; drive 128, PA enable set.
        wr(1'b1, 6'h32, {19'd0, 5'd2, 1'b0, 7'd1});
        wr(1'b1, 6'h33, 32'h0);
        wr(1'b1, 6'h31, 32'h180);
        wr(1'b1, 6'h04, 32'h5);                       // run, PA allowed
        tx_on_host = 1'b1;
        #100000;

        // 1. Software PTT, then the host stops.
        key(32'h3);
        wait (dut.tx_on === 1'b1);
        #500000;
        check("software PTT keys, PA lines up", {28'd0, pa_out}, 32'hF);
        refresh = 1'b0;
        expect_stop("software PTT", 3e6);   // the hang time and the FIFO, at most

        // 2. The same with the PTT hang time at 31 (stays keyed until
        //    software PTT drops).
        wr(1'b1, 6'h32, {19'd0, 5'd31, 1'b0, 7'd1});
        #20000;
        key(32'h3);
        wait (dut.tx_on === 1'b1);
        #500000;
        refresh = 1'b0;
        expect_stop("PTT hang 31", 1e6);
        wr(1'b1, 6'h32, {19'd0, 5'd2, 1'b0, 7'd1});
        tx_on_host = 1'b0;

        // 3. The jack's PTT without permission keys nothing.
        jack_ring = 1'b0;
        #2000000;
        check("jack PTT without permission: core idle", {31'd0, dut.tx_on}, 32'h0);
        check("jack PTT without permission: PA lines low", {28'd0, pa_out}, 32'h0);
        check("jack PTT without permission: transmit path off", ad_reg(1), 8'h60);
        // With permission it keys; then the host stops.
        key(32'h2);
        wait (dut.tx_on === 1'b1);
        #500000;
        refresh = 1'b0;
        expect_stop("jack PTT", 3e6);
        jack_ring = 1'b1;
        #100000;

        // 4. The jack's key (CW) with permission, to the full level; then the
        //    host stops. The core ramps down (4.1 ms) and leaves.
        jack_tip = 1'b0;
        key(32'h2);
        wait (dut.tx_on === 1'b1);
        #6000000;
        check("jack key: CW at full level", {13'd0, dut.radio_i.cwlevel}, 32'h4d800);
        refresh = 1'b0;
        expect_stop("jack key", 7e6);
        jack_tip = 1'b1;
        #100000;

        // 5. Clearing KEYING by hand stops the jack's key the same way.
        jack_tip = 1'b0;
        key(32'h2);
        wait (dut.tx_on === 1'b1);
        #500000;
        refresh = 1'b0;
        wr(1'b1, 6'h05, 32'h0);
        expect_stop("jack key, permission cleared", 7e6);
        jack_tip = 1'b1;

        if (errors == 0) $display("PASS");
        else $display("FAILED: %0d errors", errors);
        $finish;
    end
endmodule
