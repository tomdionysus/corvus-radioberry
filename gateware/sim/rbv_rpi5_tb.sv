// SPDX-License-Identifier: GPL-2.0-or-later
//
// Testbench for the rpi5-pio build, with the radio core replaced by
// sim/radio_stub.sv. Models: the AD9866's serial port and the clock it gives
// the FPGA, the Pi's SPI master with CRC, and the Pi 5's two RP1 PIO state
// machines running the driver's own programs (sim/pio_sm_model.sv), with
// wire and buffer delays both ways. Checks the receive stream across
// pauses, a long pause that fills the FIFO, receiver-count changes with the
// clock running, a program restart in the middle of a value and run going
// off and on; then the transmit wire, including the FIFO filling up.
`timescale 1ns / 1ps
module rbv_rpi5_tb;

    // ---- board clock: 38.4 MHz until the AD9866 doubles it ----
    logic clk53 = 1'b0;
    real  half = 13.02;
    always #(half) clk53 = ~clk53;

    // ---- the Pi's PIO state machines and the wires ----
    logic rx_en = 1'b0, rx_restart = 1'b0, tx_en = 1'b0;
    int   rx_period = 10000, tx_period = 40000;    // clkdiv 2 and 8
    wire  rx_side, tx_side, tx_out;
    wire  [3:0] rx_nib_dut;
    wire  rx_rdy_dut, tx_rdy_dut;
    wire  [3:0] rx_nib_pi;
    wire  rx_rdy_pi, tx_rdy_pi;
    wire  pi_rx_clk, pi_tx_clk, pi_tx_data;
    // Pi to FPGA pin 3 ns; FPGA to the Pi's pad 14 ns after the edge it
    // answers (clock network, I/O register, buffer and wire).
    assign #3  pi_rx_clk  = rx_side;
    assign #3  pi_tx_clk  = tx_side;
    assign #3  pi_tx_data = tx_out;
    assign #11 rx_nib_pi  = rx_nib_dut;
    assign #11 rx_rdy_pi  = rx_rdy_dut;
    assign #11 tx_rdy_pi  = tx_rdy_dut;

    pio_sm_model #(.IS_TX(1'b0)) pio_rx (
        .en(rx_en), .restart(rx_restart), .period_ps(rx_period), .side_pin(rx_side),
        .jmp_pin(rx_rdy_pi), .in_pins(rx_nib_pi), .out_pin()
    );
    pio_sm_model #(.IS_TX(1'b1)) pio_tx (
        .en(tx_en), .restart(1'b0), .period_ps(tx_period), .side_pin(tx_side),
        .jmp_pin(tx_rdy_pi), .in_pins(4'd0), .out_pin(tx_out)
    );

    // ---- DUT ----
    logic sck = 1'b1, ce_n = 1'b1, mosi = 1'b0;
    wire  miso;
    logic jack_tip = 1'b1, jack_ring = 1'b1;
    wire [3:0] pa_out;
    wire ad_rst_n, ad_mode, ad_sen_n, ad_sclk, ad_sdio, ad_txsync, ad_txquiet_n;
    wire [5:0] ad_tx;
    logic [5:0] ad_rx = 6'd0;
    logic ad_rxsync = 1'b0;

    rbv_rpi5_top #(.TX(1'b1), .NR(6), .RESET_CYCLES(64), .LOCK_CYCLES(256)) dut (
        .clk53(clk53), .spi_sck(sck), .spi_ce0_n(ce_n), .spi_mosi(mosi), .spi_miso(miso),
        .pi_rx_clk(pi_rx_clk), .pi_rx_data(rx_nib_dut), .pi_rx_ready(rx_rdy_dut),
        .pi_tx_clk(pi_tx_clk), .pi_tx_data(pi_tx_data), .pi_tx_ready(tx_rdy_dut),
        .jack_tip(jack_tip), .jack_ring(jack_ring), .pa_out(pa_out), .ad_rst_n(ad_rst_n),
        .ad_mode(ad_mode), .ad_sen_n(ad_sen_n), .ad_sclk(ad_sclk), .ad_sdio(ad_sdio),
        .ad_rx(ad_rx), .ad_rxsync(ad_rxsync),
        .ad_tx(ad_tx), .ad_txsync(ad_txsync), .ad_txquiet_n(ad_txquiet_n)
    );
    // The core drains transmit samples slowly here, so that the FIFO fills.
    defparam dut.radio_i.TX_PERIOD = 16384;

    int errors = 0;
    task automatic check(input string what, input logic [31:0] got, input logic [31:0] want);
        if (got !== want) begin
            $display("FAIL %s: got %08x, want %08x", what, got, want);
            errors++;
        end else $display("ok   %s: %08x", what, got);
    endtask

    // ---- AD9866 serial port model ----
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
                        output logic [63:0] rep);
        logic [55:0] body;
        logic [63:0] out;
        body = {wr, sp, a, d, seq, 8'h00};
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
    endtask

    task automatic rd(input logic sp, input logic [5:0] a, output logic [31:0] q);
        logic [63:0] r;
        xfer(1'b0, sp, a, 32'h0, r);
        if (r[63:56] !== 8'hA5 || crc8(r[63:8]) !== r[7:0]) begin
            $display("FAIL reply: %016x", r);
            errors++;
        end
        q = r[47:16];
    endtask

    task automatic wr(input logic sp, input logic [5:0] a, input logic [31:0] d);
        logic [63:0] r;
        xfer(1'b1, sp, a, d, r);
    endtask

    // ---- receive checker ----
    // Each word: index in 27..24, value {k, frame number} with k = index.
    // strict: frames must also follow one another without a gap.
    task automatic rx_check(input string what, input int nvals, input int skip,
                            input logic strict, input int min_frames);
        int expect_idx, bad, frames_ok, gaps;
        logic [15:0] fno, last_fno;
        logic have_last;
        expect_idx = -1; bad = 0; frames_ok = 0; gaps = 0; have_last = 1'b0;
        for (int i = skip; i < pio_rx.rxq.size(); i++) begin
            logic [31:0] w;
            logic ok;
            w = pio_rx.rxq[i];
            ok = 1'b1;
            if (expect_idx < 0) begin
                // Wait for a frame start.
                if (w[27:24] == 4'd0) expect_idx = 0;
                else ok = 1'b0;
            end
            if (ok && (w[27:24] != expect_idx[3:0] || w[23:16] != expect_idx[7:0] ||
                       w[31:28] != 4'd0 || (expect_idx != 0 && w[15:0] != fno))) begin
                bad++;
                expect_idx = -1;
                ok = 1'b0;
            end
            if (ok) begin
                if (expect_idx == 0) fno = w[15:0];
                if (expect_idx == nvals - 1) begin
                    if (have_last && fno != last_fno + 16'd1) gaps++;
                    last_fno = fno; have_last = 1'b1;
                    frames_ok++;
                    expect_idx = 0;
                end else expect_idx++;
            end
        end
        if (bad != 0 || frames_ok < min_frames || (strict && gaps != 0)) begin
            $display("FAIL %s: %0d frames, %0d bad words, %0d gaps", what, frames_ok, bad, gaps);
            errors++;
        end else $display("ok   %s: %0d frames in order, %0d gaps", what, frames_ok, gaps);
    endtask

    // ---- transmit monitor: what the image writes into its FIFO ----
    logic [31:0] tx_got [$];
    always @(posedge pi_tx_clk)
        if (dut.txf_wr) tx_got.push_back(dut.txf_wdata);
    int tx_ready_lows = 0;
    always @(negedge tx_rdy_dut) if ($time > 1000) tx_ready_lows++;

    logic [31:0] q, full0, full1;
    int n0;

    initial begin
        #3000;
        rd(1'b1, 6'h00, q); check("ID", q, 32'h52425632);
        rd(1'b1, 6'h02, q); check("CAPS (6 rx, tx, PIO, 10CL025)", q, 32'h00002216);
        wait (dut.setup_done === 1'b1);
        #2000;
        rd(1'b1, 6'h03, q); check("STATUS locked, set up, running, link", q & 32'hF, 32'hF);

        // Two receivers: frames of 4 values every 256 radio clocks
        // (1.2 M values/s), against the wire's 2.38 M.
        wr(1'b0, 6'h00, 32'h0000_0008);
        wr(1'b1, 6'h04, 32'h1);
        rx_en = 1'b1;
        #300000;
        rx_check("RX stream, 2 receivers", 4, 0, 1'b1, 80);

        // Short pauses (the driver's RX hold) at uneven points.
        n0 = pio_rx.rxq.size();
        for (int p = 0; p < 6; p++) begin
            #(13370 + p * 1731);
            rx_en = 1'b0;
            #(20000 + p * 977);
            rx_en = 1'b1;
        end
        #50000;
        rx_check("RX across 6 pauses, nothing lost", 4, n0, 1'b1, 80);

        // A pause longer than the FIFO lasts: whole frames refused, order kept.
        rd(1'b1, 6'h09, full0);
        pio_rx.rxq.delete();
        rx_en = 1'b0;
        #1200000;
        rx_en = 1'b1;
        #100000;
        rd(1'b1, 6'h09, full1);
        if (full1 == full0) begin $display("FAIL long pause: FIFO never refused"); errors++; end
        else $display("ok   long pause: FIFO refused frames (%0d times)", full1 - full0);
        rx_check("RX after a long pause", 4, 0, 1'b0, 50);
        // Read out what is left in the FIFO with run off, as the driver
        // would after a client goes.
        wr(1'b1, 6'h04, 32'h0);
        #800000;
        wr(1'b1, 6'h04, 32'h1);

        // Receiver-count change with the clock running (no hold): three
        // receivers, frames of 6.
        wr(1'b0, 6'h00, 32'h0000_0010);
        #20000;
        n0 = pio_rx.rxq.size();
        #200000;
        rx_check("RX after 2 -> 3 receivers, clock running", 6, n0, 1'b1, 60);
        // And back to one, with the clock held across it.
        rx_en = 1'b0;
        wr(1'b0, 6'h00, 32'h0000_0000);
        #200;
        rx_en = 1'b1;
        #20000;
        n0 = pio_rx.rxq.size();
        #100000;
        rx_check("RX after 3 -> 1 receivers, clock held", 2, n0, 1'b1, 28);

        // The Pi restarts its program in the middle of a value.
        for (int p = 0; p < 5; p++) begin
            #(7013 + p * 113);
            @(posedge pio_rx.pclk);
            rx_restart = 1'b1;
            @(posedge pio_rx.pclk);
            rx_restart = 1'b0;
            #10000;
            n0 = pio_rx.rxq.size();
            #30000;
            rx_check($sformatf("RX after program restart %0d", p), 2, n0, 1'b1, 7);
        end

        // Run off: the driver stops the state machine; then on again.
        wr(1'b1, 6'h04, 32'h0);
        rx_en = 1'b0;
        #200000;
        wr(1'b1, 6'h04, 32'h1);
        rx_en = 1'b1;
        #20000;
        n0 = pio_rx.rxq.size();
        #100000;
        rx_check("RX after run off and on", 2, n0, 1'b1, 28);
        // Each program restart dropped the Pi's half-read value.
        rx_en = 1'b0;
        #1000;
        rd(1'b1, 6'h0A, q);
        $display("info LINK_TX %0d values, model pushed %0d", q, pio_rx.pushes);
        if (q < pio_rx.pushes || q > pio_rx.pushes + 5) begin
            $display("FAIL LINK_TX count"); errors++;
        end
        wr(1'b1, 6'h04, 32'h0);

        // Transmit wire at the driver's divider (8): words arrive exactly.
        for (int i = 0; i < 64; i++) pio_tx.txq.push_back(32'h1234_0000 + i * 32'h0001_0003);
        tx_en = 1'b1;
        #(64 * 9500 + 20000);
        if (tx_got.size() != 64) begin
            $display("FAIL TX: %0d of 64 words", tx_got.size()); errors++;
        end else begin
            int bad;
            bad = 0;
            for (int i = 0; i < 64; i++) if (tx_got[i] != 32'h1234_0000 + i * 32'h0001_0003) bad++;
            if (bad) begin $display("FAIL TX: %0d words wrong", bad); errors++; end
            else $display("ok   TX: 64 words exact at divider 8");
        end

        // Faster than the core drains: the FIFO fills, ready holds the Pi
        // back, nothing is lost or dropped.
        tx_got.delete();
        tx_period = 10000;
        for (int i = 0; i < 2300; i++) pio_tx.txq.push_back(32'hA000_0000 ^ (i * 32'h0001_0001));
        #5600000;
        begin
            int bad;
            bad = 0;
            for (int i = 0; i < tx_got.size(); i++)
                if (tx_got[i] != (32'hA000_0000 ^ (i * 32'h0001_0001))) bad++;
            $display("info TX fill: %0d words in, %0d left at the Pi, ready fell %0d times",
                     tx_got.size(), pio_tx.txq.size(), tx_ready_lows);
            // The FIFO (2048) still held most of the first 64 words.
            if (bad || tx_got.size() < 1990 || pio_tx.txq.size() == 0 || tx_ready_lows == 0) begin
                $display("FAIL TX fill: %0d wrong", bad); errors++;
            end else $display("ok   TX fill: in order, ready held the Pi back");
        end
        rd(1'b1, 6'h10, q); check("TX_OVF", q, 32'h0);
        rd(1'b1, 6'h0B, q);
        if (q != tx_got.size() + 64) begin $display("FAIL LINK_RX %0d", q); errors++; end
        else $display("ok   LINK_RX %0d", q);

        if (pa_out !== 4'b0) begin $display("FAIL PA lines moved"); errors++; end
        if (errors == 0) $display("PASS");
        else $display("FAILED: %0d errors", errors);
        $finish;
    end

    // Nothing here keys the radio: the PA lines and the AD9866 transmit bus
    // stay low throughout.
    always @(pa_out or ad_txquiet_n)
        if ($time > 0 && (pa_out !== 4'b0 || ad_txquiet_n !== 1'b0)) begin
            $display("FAIL transmit-side pin moved at %0t", $time); errors++;
        end

endmodule
