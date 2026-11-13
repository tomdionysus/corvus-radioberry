// SPDX-License-Identifier: GPL-2.0-or-later
//
// Testbench for the link test image, stage 1. The host side is an SPI
// master; the AD9866 side is a small model that sends a ramp on the
// receive bus and loops the serial data line back on the data-out line.
`timescale 1ns / 1ps
module rbt_top_tb;

    localparam int NIO = 19;
    localparam int NIN = 3;

    logic clk53 = 1'b0;
    always #6.51 clk53 = ~clk53;

    logic sck = 1'b0, ce_n = 1'b1, mosi = 1'b0;
    wire  miso;

    wire  [NIO-1:0] pin_io;
    logic [NIO-1:0] pin_drv = '0, pin_drv_en = '0;
    logic [NIN-1:0] pin_in = '0;
    logic [9:0]     cn301 = 10'h3FF;
    wire  [3:0]     pa_out;

    wire ad_rst_n, ad_mode, ad_sen_n, ad_sclk, ad_sdio, ad_txsync, ad_txquiet_n;
    wire [5:0] ad_tx;
    logic [5:0] ad_rx = 6'd0;
    logic ad_rxsync = 1'b0;

    genvar g;
    generate
        for (g = 0; g < NIO; g++) begin : g_drv
            assign (weak1, weak0) pin_io[g] = 1'b1;
            assign pin_io[g] = pin_drv_en[g] ? pin_drv[g] : 1'bz;
        end
    endgenerate

    // AD9866 model: a 12-bit ramp, high half first, RX sync with the low
    // half; data-out follows the serial data line.
    logic        clk2 = 1'b0;
    logic        half = 1'b0;
    logic [11:0] ramp = 12'h800;
    always @(clk53) begin
        clk2 = 1'b1;
        #3.255 clk2 = 1'b0;
    end
    always @(posedge clk2) begin
        half <= ~half;
        if (half) begin
            ad_rx     <= ramp[5:0];
            ad_rxsync <= 1'b1;
            ramp      <= ramp + 12'd37;
        end else begin
            ad_rx     <= ramp[11:6];
            ad_rxsync <= 1'b0;
        end
    end

    rbt_top #(.NIO(NIO), .NIN(NIN)) dut (
        .clk53(clk53),
        .spi_sck(sck), .spi_ce0_n(ce_n), .spi_mosi(mosi), .spi_miso(miso),
        .pin_io(pin_io), .pin_in(pin_in),
        .cn301_in(cn301), .pa_out(pa_out),
        .ad_rst_n(ad_rst_n), .ad_mode(ad_mode), .ad_sen_n(ad_sen_n), .ad_sclk(ad_sclk),
        .ad_sdio(ad_sdio), .ad_sdo(ad_sdio), .ad_clkout1b(clk53), .ad_rxclk(clk2),
        .ad_rx(ad_rx), .ad_rxsync(ad_rxsync),
        .ad_tx(ad_tx), .ad_txsync(ad_txsync), .ad_txquiet_n(ad_txquiet_n)
    );

    // Host model of the PCM block as a slave: samples frame sync and
    // data on the falling bit clock edge, drives its own counting
    // sequence 5 ns after the rising edge, word-aligned with the frame.
    logic        i2s_on = 1'b0, h_corrupt = 1'b0;
    logic        h_fs_prev = 1'b0;
    int          h_bit = -1, h_words = 0, h_errs = 0;
    logic [31:0] h_rx = 32'h0, h_rx_prev = 32'h0;
    logic [31:0] h_tx = 32'h100, h_tx_sr = 32'h0;

    always @(negedge pin_io[2]) if (i2s_on) begin
        if (pin_io[3] && !h_fs_prev) h_bit = 0;
        else if (h_bit >= 0) h_bit = (h_bit + 1) % 64;
        h_fs_prev = pin_io[3];
        if (h_bit >= 0) begin
            h_rx = {h_rx[30:0], pin_io[4]};
            if (h_bit % 32 == 31) begin
                h_words++;
                if (h_words > 1 && h_rx !== h_rx_prev + 32'd1) h_errs++;
                h_rx_prev = h_rx;
            end
        end
    end

    always @(posedge pin_io[2]) if (i2s_on && h_bit >= 0) begin
        if ((h_bit + 1) % 32 == 0) begin
            h_tx_sr = h_tx;
            h_tx    = h_tx + 32'd1;
        end
        #5 pin_drv[5] = h_tx_sr[31] ^ (h_corrupt && h_bit == 7);
        h_tx_sr = {h_tx_sr[30:0], 1'b0};
    end

    int errors = 0;

    task automatic check(input string what, input logic [31:0] got, input logic [31:0] want);
        if (got !== want) begin
            $display("FAIL %s: got %08x, want %08x", what, got, want);
            errors++;
        end else $display("ok   %s: %08x", what, got);
    endtask

    // One 48-bit transfer; extra_edges adds stray clock edges after it,
    // inside the same chip select.
    task automatic xfer(input logic wr, input logic [6:0] a, input logic [31:0] d,
                        output logic [15:0] head, output logic [31:0] q);
        logic [47:0] out, in;
        out = {wr, a, 8'h00, d};
        ce_n = 1'b0;
        #40;
        for (int i = 47; i >= 0; i--) begin
            mosi = out[i];
            #40 sck = 1'b1;
            in[i] = miso;
            #40 sck = 1'b0;
        end
        #40 ce_n = 1'b1;
        #200;
        head = in[47:32];
        q    = in[31:0];
    endtask

    task automatic rd(input logic [6:0] a, output logic [31:0] q);
        logic [15:0] head;
        xfer(1'b0, a, 32'h0, head, q);
        if (head !== 16'hA55A) begin
            $display("FAIL reply header %04x", head);
            errors++;
        end
    endtask

    task automatic wr(input logic [6:0] a, input logic [31:0] d);
        logic [15:0] head;
        logic [31:0] q;
        xfer(1'b1, a, d, head, q);
    endtask

    logic [31:0] q, q2;

    initial begin
        #2000;
        rd(7'h00, q); check("ID", q, 32'h52425431);
        rd(7'h01, q); check("VERSION", q, 32'h00040001);
        wr(7'h02, 32'hCAFE1234);
        rd(7'h02, q); check("SCRATCH", q, 32'hCAFE1234);

        // A stray clock edge inside one transfer must not affect the next.
        ce_n = 1'b0; #40;
        repeat (5) begin #40 sck = 1'b1; #40 sck = 1'b0; end
        #40 ce_n = 1'b1; #200;
        rd(7'h02, q); check("SCRATCH after a short transfer", q, 32'hCAFE1234);

        rd(7'h04, q); rd(7'h04, q2);
        if (q2 == q) begin $display("FAIL SYSCNT does not count"); errors++; end
        else $display("ok   SYSCNT counts");

        rd(7'h03, q); check("STATUS locked", q & 32'h1, 32'h1);

        // Header pins: undriven pins read high through the pull-ups.
        rd(7'h08, q); check("PIN_IN idle", q, {10'd0, 3'b000, 19'h7FFFF});
        pin_drv_en[4] = 1'b1; pin_drv[4] = 1'b0;
        pin_in = 3'b101;
        #200;
        rd(7'h08, q); check("PIN_IN driven", q, {10'd0, 3'b101, 19'h7FFEF});
        pin_drv_en[4] = 1'b0;
        wr(7'h0A, 32'h00000000);
        wr(7'h09, 32'h00000004);
        #200;
        rd(7'h08, q); check("PIN_IN from own output", q & 32'h7FFFF, 32'h7FFFB);
        wr(7'h09, 32'h0);

        rd(7'h0B, q); check("CN301_IN", q, 32'h3FF);

        // AD9866 control and serial port.
        rd(7'h10, q); check("AD_CTRL default", q, 32'h2);
        wr(7'h10, 32'h3);
        if (ad_rst_n !== 1'b1 || ad_mode !== 1'b1) begin $display("FAIL AD control pins"); errors++; end
        wr(7'h11, {16'hFFFF, 16'hA5C3});
        #8000;
        rd(7'h03, q); check("serial idle", q & 32'h4, 32'h0);
        rd(7'h12, q); check("AD_SER_RES loopback", q, 32'hA5C3A5C3);
        wr(7'h11, {16'hFF00, 16'h8100});
        #8000;
        rd(7'h12, q); check("AD_SER_RES released half", q[31:24], 8'h81);

        // Receive bus.
        wr(7'h18, 32'h0);
        #200000;
        rd(7'h18, q); check("RX_PINS", q, {17'd0, 7'h7F, 1'b0, 7'h7F});
        rd(7'h19, q); check("RX_BITS", q, 32'h00000FFF);
        rd(7'h1A, q);
        if ($signed(q[11:0]) > -12'sd2000 || $signed(q[27:16]) < 12'sd2000) begin
            $display("FAIL RX_MINMAX %08x", q); errors++;
        end else $display("ok   RX_MINMAX %08x", q);

        // Raw capture: RX sync at the rising edge alternates, and each
        // low half (with sync) follows the high half of the same ramp
        // value.
        wr(7'h40, 32'h0);
        #20000;
        rd(7'h40, q); check("capture done", q, 32'h1);
        begin
            int nsync, nbad;
            logic [6:0] e0, e1;
            nsync = 0; nbad = 0;
            for (int i = 0; i < 64; i++) begin
                wr(7'h41, i);
                rd(7'h42, q);
                e1 = q[6:0];
                if (e1[6]) nsync++;
                if (i > 0 && e1[6] == e0[6]) nbad++;
                e0 = e1;
            end
            if (nsync < 30 || nsync > 34 || nbad != 0) begin
                $display("FAIL capture: %0d of 64 with sync, %0d without alternation", nsync, nbad);
                errors++;
            end else $display("ok   capture: sync alternates");
        end

        // Transmit side: nothing moves while disarmed.
        wr(7'h51, 32'hF);
        wr(7'h54, 32'h1789ABCD);
        wr(7'h52, {4'd0, 12'h800, 15'd0, 1'b1});
        #2000;
        if (pa_out !== 4'b0000 || ad_tx !== 6'd0 || ad_txsync !== 1'b0 || ad_txquiet_n !== 1'b0) begin
            $display("FAIL transmit-side pins moved while disarmed"); errors++;
        end else $display("ok   transmit-side pins low while disarmed");

        // The second key alone does not arm.
        wr(7'h50, 32'h54583221);
        rd(7'h50, q); check("not armed by the second key alone", q[31:30], 2'b00);

        // Arm for 2 ms; requests made before arming are cleared.
        wr(7'h53, 32'd2);
        wr(7'h50, 32'h41524D31);
        rd(7'h50, q); check("first key taken", q[31:30], 2'b01);
        wr(7'h50, 32'h54583221);
        rd(7'h50, q); check("armed", q[31:30], 2'b10);
        #2000;
        if (pa_out !== 4'b0000) begin $display("FAIL old PA request survived arming"); errors++; end
        else $display("ok   requests cleared by arming");
        wr(7'h51, 32'h5);
        #500;
        check("PA lines follow the request while armed", {28'd0, pa_out}, 32'h5);

        // The tone needs the transmit gain written low first.
        wr(7'h54, 32'h1789ABCD);
        wr(7'h52, {4'd0, 12'h800, 15'd0, 1'b1});
        #2000;
        if (ad_txquiet_n !== 1'b0) begin $display("FAIL tone ran without the gain written"); errors++; end
        else $display("ok   tone refused without the gain written");
        wr(7'h11, {16'hFFFF, 16'h0A7F});
        rd(7'h50, q); check("high gain not accepted", q[29], 1'b0);
        wr(7'h11, {16'hFFFF, 16'h0A40});
        #8000;
        rd(7'h50, q); check("low gain accepted", q[29], 1'b1);
        #2000;
        begin
            int nsync, nnz;
            nsync = 0; nnz = 0;
            for (int i = 0; i < 200; i++) begin
                @(posedge clk2);
                if (ad_txsync) nsync++;
                if (ad_tx != 6'd0) nnz++;
            end
            if (ad_txquiet_n !== 1'b1 || nsync < 95 || nsync > 105 || nnz < 50) begin
                $display("FAIL tone: quiet_n %b, %0d sync, %0d nonzero halves", ad_txquiet_n, nsync, nnz);
                errors++;
            end else $display("ok   tone runs: %0d of 200 halves with sync", nsync);
        end

        // Quiet mode: the bus runs with TX quiet held low.
        wr(7'h52, {4'd0, 12'h800, 14'd0, 2'b11});
        #2000;
        if (ad_txquiet_n !== 1'b0) begin $display("FAIL quiet mode released TX quiet"); errors++; end
        else begin
            int nnz2;
            nnz2 = 0;
            for (int i = 0; i < 200; i++) begin
                @(posedge clk2);
                if (ad_tx != 6'd0) nnz2++;
            end
            if (nnz2 < 50) begin $display("FAIL quiet mode: bus idle"); errors++; end
            else $display("ok   quiet mode: bus runs, TX quiet low");
        end

        // The arming lapses by itself and everything returns to low.
        #2500000;
        rd(7'h50, q); check("disarmed after the time", q[31], 1'b0);
        rd(7'h51, q); check("PA request cleared", q, 32'h0);
        #2000;
        if (pa_out !== 4'b0000 || ad_tx !== 6'd0 || ad_txsync !== 1'b0 || ad_txquiet_n !== 1'b0) begin
            $display("FAIL transmit-side pins still active after disarming"); errors++;
        end else $display("ok   transmit-side pins low after disarming");

        // Any other write to ARM disarms at once.
        wr(7'h53, 32'd50);
        wr(7'h50, 32'h41524D31);
        wr(7'h50, 32'h54583221);
        wr(7'h51, 32'h2);
        wr(7'h50, 32'h0);
        #500;
        rd(7'h50, q); check("disarmed by another write", q[31], 1'b0);
        if (pa_out !== 4'b0000) begin $display("FAIL PA lines after disarm write"); errors++; end

        // I2S: 5 fast clocks per bit, sample point 3, counting sequence.
        i2s_on = 1'b1;
        pin_drv_en[5] = 1'b1;
        wr(7'h30, {2'd0, 6'd0, 8'd3, 8'd5, 5'd0, 1'b0, 1'b0, 1'b1});
        // The first words after the start may break the sequence while
        // the host model finds the frame; after that, none may.
        #200000;
        wr(7'h31, 32'h0);
        #2000;
        rd(7'h34, q2);
        #200000;
        wr(7'h31, 32'h0);
        #2000;
        rd(7'h31, q); check("I2S snapshot taken", q, 32'h1);
        rd(7'h33, q);
        if (q < 32'd50) begin $display("FAIL I2S words received %0d", q); errors++; end
        else $display("ok   I2S words received %0d", q);
        rd(7'h34, q); check("I2S receive errors after start", q, q2);
        if (q2 > 32'd2) begin $display("FAIL I2S start-up errors %0d", q2); errors++; end
        if (h_words < 50 || h_errs != 0) begin
            $display("FAIL host model: %0d words, %0d errors", h_words, h_errs); errors++;
        end else $display("ok   host model: %0d words, 0 errors", h_words);
        // A corrupted bit from the host must be counted.
        h_corrupt = 1'b1;
        #100000;
        h_corrupt = 1'b0;
        #100000;
        wr(7'h31, 32'h0);
        #2000;
        rd(7'h34, q);
        if (q == 32'd0) begin $display("FAIL I2S errors not counted"); errors++; end
        else $display("ok   I2S errors counted: %0d", q);
        wr(7'h30, 32'h0);
        i2s_on = 1'b0;
        pin_drv_en[5] = 1'b0;

        // Self-test.
        wr(7'h20, 32'h1);
        #2000000;
        rd(7'h03, q); check("BIST done", q & 32'h18, 32'h10);
        rd(7'h20, q); check("BIST errors", q, 32'h0);

        if (errors == 0) $display("PASS");
        else $display("FAILED: %0d errors", errors);
        $finish;
    end

endmodule
