// SPDX-License-Identifier: GPL-2.0-or-later
//
// Link test image, stage 1. Not a radio. It answers a register map over
// SPI0 and tests the FPGA's own resources, the header wires, the clocks
// and the AD9866's serial port and receive bus.
//
// Its two transmit-side tests are armed (rbt_arm.sv): unless the image has
// been armed by two keys in order, and only for at most 10 s, the AD9866
// transmit data, TX sync and TX quiet pins and the four PA lines are held
// low whatever the registers say.
//
// Register map (address: name, access):
//   00 ID          ro  0x52425431
//   01 VERSION     ro  stage in bits 31..16, revision in bits 15..0
//   02 SCRATCH     rw
//   03 STATUS      ro  0 PLL locked, 1 PLL lock lost since cleared,
//                      2 AD9866 serial busy, 3 self-test busy,
//                      4 self-test done; write 1 to bit 1 to clear it
//   04 SYSCNT      ro  free-running count of the 76.8 MHz system clock
//   05 FCNT_CLK69  ro  edges/16 on FPGA pin 69 per 2^24 system clocks
//   06 FCNT_RXCLK  ro  the same for FPGA pin 71
//   07 FCNT_RXSYNC ro  the same for FPGA pin 77
//   08 PIN_IN      ro  levels of the header-side pins (see PIN_* below)
//   09 PIN_OE      rw  output enables for pins 0 to NIO-1
//   0A PIN_OUT     rw  output levels for pins 0 to NIO-1
//   0B CN301_IN    ro  levels of the PA header inputs
//   10 AD_CTRL     rw  0 AD9866 reset released, 1 mode pin level
//   11 AD_SER      wo  31..16 drive mask, 15..0 word; starts a transfer
//   12 AD_SER_RES  ro  31..16 data line samples, 15..0 data-out samples
//   18 RX_PINS     ro  6..0 pins seen high, 14..8 pins seen low
//                      ({rxsync, rx[5:0]}); any write clears 18 to 1B
//   19 RX_BITS     ro  11..0 OR, 27..16 AND of the 12-bit samples
//   1A RX_MINMAX   ro  11..0 minimum, 27..16 maximum (two's complement)
//   1B RX_SNAP     ro  the latest 12-bit sample
//   20 BIST        rw  write starts; 15..0 RAM errors, 31..16 multiplier
//                      errors
//   30 I2S_CTRL    rw  0 enable, 1 sequence (0 count, 1 xorshift32),
//                      2 short frame sync, 15..8 fast clocks per bit,
//                      23..16 sample point, 29..24 host word offset.
//                      While enabled, GPIO 18 is the bit clock, 19 the
//                      frame sync, 20 data to the host, 21 data from it
//   31 I2S_SNAP    rw  write takes a snapshot; bit 0 reads 1 when taken
//   32 I2S_TXW     ro  words sent               (snapshot)
//   33 I2S_RXW     ro  words received           (snapshot)
//   34 I2S_RXERR   ro  words that broke the sequence (snapshot)
//   35 I2S_RXLAST  ro  last word received       (snapshot)
//   36 I2S_ERRGOT  ro  first bad word           (snapshot)
//   37 I2S_ERRWANT ro  what it should have been (snapshot)
//   40 CAP_CTRL    rw  write starts a raw receive bus capture; bit 0
//                      reads 1 when it is complete
//   41 CAP_ADDR    rw  entry to read, 0 to 2047
//   42 CAP_DATA    ro  6..0 {RX sync, RX data} at the rising edge of the
//                      doubled clock, 13..7 the same at the falling edge
//                      before it
//   50 ARM         rw  write ARM_KEY1 (0x41524d31) then, within 100 ms,
//                      ARM_KEY2 (0x54583221) to arm; any other write
//                      disarms. Reads 31 armed, 30 first key taken,
//                      29 transmit gain written low since the AD9866's
//                      reset, 13..0 milliseconds left
//   51 PA_TEST     rw  3..0 exttr, inttr, envpa, envbias; reach the pins
//                      only while armed; cleared on disarming
//   52 TX_TEST     rw  0 run the tone, 1 keep TX quiet low (for the
//                      AD9866's digital loopback), 27..16 amplitude; runs only while
//                      armed and with the transmit gain written low;
//                      cleared on disarming
//   53 ARM_TIME    rw  13..0 milliseconds an arming lasts (capped at 10000)
//   54 TX_FREQ     rw  phase increment of the tone: f = value * 76.8 MHz / 2^32
module rbt_top #(
    parameter int NIO = 19,   // header-side pins that can be driven
    parameter int NIN = 2     // header-side pins that are inputs only
) (
    input  logic            clk53,

    // SPI0
    input  logic            spi_sck,
    input  logic            spi_ce0_n,
    input  logic            spi_mosi,
    inout  wire             spi_miso,

    // Header-side pins
    inout  wire  [NIO-1:0]  pin_io,
    input  logic [NIN-1:0]  pin_in,

    // PA header
    input  logic [9:0]      cn301_in,
    output logic [3:0]      pa_out,

    // AD9866
    output logic            ad_rst_n,
    output logic            ad_mode,
    output logic            ad_sen_n,
    output logic            ad_sclk,
    inout  wire             ad_sdio,
    input  logic            ad_sdo,
    input  logic            ad_clkout1b,
    input  logic            ad_rxclk,
    input  logic [5:0]      ad_rx,
    input  logic            ad_rxsync,
    output logic [5:0]      ad_tx,
    output logic            ad_txsync,
    output logic            ad_txquiet_n
);

    localparam logic [31:0] ID      = 32'h52425431;
    localparam logic [31:0] VERSION = 32'h0004_0001;
    localparam logic [7:0]  TXGAIN_MAX = 8'h48;  // register 0x0a: 0x40 + gain code


    // ---- clocks and reset ----
    logic clk, clk_2x, clk_f, pll_locked;

    rbt_pll pll_i (.inclk0(clk53), .c0(clk), .c1(clk_2x), .c2(clk_f), .locked(pll_locked));

    logic [3:0] rst_sr = 4'hF;
    logic       rst;
    always_ff @(posedge clk or negedge pll_locked) begin
        if (!pll_locked) rst_sr <= 4'hF;
        else rst_sr <= {rst_sr[2:0], 1'b0};
    end
    assign rst = rst_sr[3];

    logic [1:0] lock_sync = 2'b00;
    logic       lock_lost = 1'b0;
    logic       lock_lost_clr;

    // ---- SPI ----
    logic [6:0]  addr;
    logic [31:0] rdata, wdata;
    logic        wr_stb, miso_q, miso_oe;

    rbt_spi spi_i (
        .sck(spi_sck), .cs_n(spi_ce0_n), .mosi(spi_mosi), .miso(miso_q), .miso_oe(miso_oe),
        .clk(clk), .addr(addr), .rdata(rdata), .wr_stb(wr_stb), .wdata(wdata)
    );
    assign spi_miso = miso_oe ? miso_q : 1'bz;

    // ---- registers ----
    logic [31:0]    scratch = 32'h0;
    logic [31:0]    syscnt  = 32'h0;
    logic [NIO-1:0] pin_oe  = '0;
    logic [NIO-1:0] pin_out = '0;
    logic [1:0]     ad_ctrl = 2'b10;

    always_ff @(posedge clk) syscnt <= syscnt + 32'd1;

    logic window_stb;
    assign window_stb = (syscnt[23:0] == 24'hFFFFFF);

    // Header-side pins. While the I2S tester is enabled it owns pins 2
    // to 5 (GPIO 18 to 21).
    logic [29:0] i2s_ctrl = 30'd0;
    logic        i2s_bclk, i2s_fs, i2s_sdo;
    logic [NIO+NIN-1:0] pin_s1, pin_s2;
    genvar g;
    generate
        for (g = 0; g < NIO; g++) begin : g_pin
            if (g == 2)      assign pin_io[g] = i2s_ctrl[0] ? i2s_bclk : (pin_oe[g] ? pin_out[g] : 1'bz);
            else if (g == 3) assign pin_io[g] = i2s_ctrl[0] ? i2s_fs   : (pin_oe[g] ? pin_out[g] : 1'bz);
            else if (g == 4) assign pin_io[g] = i2s_ctrl[0] ? i2s_sdo  : (pin_oe[g] ? pin_out[g] : 1'bz);
            else if (g == 5) assign pin_io[g] = (pin_oe[g] & ~i2s_ctrl[0]) ? pin_out[g] : 1'bz;
            else             assign pin_io[g] = pin_oe[g] ? pin_out[g] : 1'bz;
        end
    endgenerate
    always_ff @(posedge clk) begin
        pin_s1 <= {pin_in, pin_io};
        pin_s2 <= pin_s1;
    end

    logic [9:0] cn_s1, cn_s2;
    always_ff @(posedge clk) begin
        cn_s1 <= cn301_in;
        cn_s2 <= cn_s1;
    end

    // AD9866 control lines and serial port.
    assign ad_rst_n = ad_ctrl[0];
    assign ad_mode  = ad_ctrl[1];

    logic        ser_start, ser_busy, sdio_out, sdio_oe;
    logic [15:0] cap_sdio, cap_sdo;
    logic [1:0]  sdio_s, sdo_s;

    always_ff @(posedge clk) begin
        sdio_s <= {sdio_s[0], ad_sdio};
        sdo_s  <= {sdo_s[0], ad_sdo};
    end

    rbt_adser adser_i (
        .clk(clk), .rst(rst), .start(ser_start), .txword(wdata[15:0]), .oemask(wdata[31:16]),
        .busy(ser_busy), .cap_sdio(cap_sdio), .cap_sdo(cap_sdo),
        .sen_n(ad_sen_n), .sclk(ad_sclk), .sdio_out(sdio_out), .sdio_oe(sdio_oe),
        .sdio_in(sdio_s[1]), .sdo_in(sdo_s[1])
    );
    assign ad_sdio = sdio_oe ? sdio_out : 1'bz;

    // Frequency counters.
    logic [31:0] fcnt_clk69, fcnt_rxclk, fcnt_rxsync;
    rbt_fcount fc0 (.sig(ad_clkout1b), .clk(clk), .window_stb(window_stb), .count(fcnt_clk69));
    rbt_fcount fc1 (.sig(ad_rxclk),    .clk(clk), .window_stb(window_stb), .count(fcnt_rxclk));
    rbt_fcount fc2 (.sig(ad_rxsync),   .clk(clk), .window_stb(window_stb), .count(fcnt_rxsync));

    // AD9866 receive bus, sampled at twice the sample clock. The half
    // that arrives with RX sync high is the low half of a sample.
    logic [5:0]  rx_d1, rx_d2;
    logic        rxsync_d1;
    logic [11:0] rx_asm, rx_sample;
    logic [6:0]  rx_seen1 = 7'h00, rx_seen0 = 7'h00;
    logic        rx_clr_2x;

    always_ff @(posedge clk_2x) begin
        rx_d1     <= ad_rx;
        rx_d2     <= rx_d1;
        rxsync_d1 <= ad_rxsync;
        if (rxsync_d1) rx_asm <= {rx_d2, rx_d1};
        if (rx_clr_2x) begin
            rx_seen1 <= 7'h00;
            rx_seen0 <= 7'h00;
        end else begin
            rx_seen1 <= rx_seen1 | {rxsync_d1, rx_d1};
            rx_seen0 <= rx_seen0 | ~{rxsync_d1, rx_d1};
        end
    end

    logic               rx_clr;
    logic [11:0]        rx_or, rx_and;
    logic signed [11:0] rx_min, rx_max;

    always_ff @(posedge clk_2x) rx_clr_2x <= rx_clr;

    always_ff @(posedge clk) begin
        rx_sample <= rx_asm;
        if (rx_clr) begin
            rx_or  <= 12'h000;
            rx_and <= 12'hFFF;
            rx_min <= 12'sh7FF;
            rx_max <= 12'sh800;
        end else begin
            rx_or  <= rx_or | rx_sample;
            rx_and <= rx_and & rx_sample;
            if ($signed(rx_sample) < rx_min) rx_min <= $signed(rx_sample);
            if ($signed(rx_sample) > rx_max) rx_max <= $signed(rx_sample);
        end
    end

    // Raw receive bus capture.
    logic        cap_tgl = 1'b0, cap_done;
    logic [10:0] cap_addr = 11'd0;
    logic [13:0] cap_q;
    rbt_rxcap rxcap_i (
        .clk_2x(clk_2x), .rx(ad_rx), .rxsync(ad_rxsync),
        .clk(clk), .start_tgl(cap_tgl), .done(cap_done), .raddr(cap_addr), .rdata(cap_q)
    );

    // Self-test.
    logic        bist_start, bist_busy, bist_done;
    logic [15:0] ram_errors, mul_errors;
    rbt_bist bist_i (
        .clk(clk), .rst(rst), .start(bist_start), .busy(bist_busy), .done(bist_done),
        .ram_errors(ram_errors), .mul_errors(mul_errors)
    );

    // I2S link tester.
    logic        snap_req = 1'b0, snap_ack;
    logic [1:0]  ack_s = 2'b00;
    logic [31:0] s_txw, s_rxw, s_rxerr, s_rxlast, s_errgot, s_errwant;

    rbt_i2s i2s_i (
        .clk_f(clk_f), .enable(i2s_ctrl[0]), .mode(i2s_ctrl[1]), .fs_short(i2s_ctrl[2]),
        .div(i2s_ctrl[15:8]), .rxph(i2s_ctrl[23:16]), .rxoff(i2s_ctrl[29:24]),
        .bclk(i2s_bclk), .fs(i2s_fs), .sd_out(i2s_sdo), .sd_in(pin_io[5]),
        .snap_req(snap_req), .snap_ack(snap_ack),
        .s_tx_words(s_txw), .s_rx_words(s_rxw), .s_rx_errors(s_rxerr),
        .s_rx_last(s_rxlast), .s_err_got(s_errgot), .s_err_want(s_errwant)
    );
    always_ff @(posedge clk) ack_s <= {ack_s[0], snap_ack};

    // ---- transmit side: armed tests only ----
    logic [16:0] ms_div = 17'd0;
    logic        ms_tick;
    always_ff @(posedge clk) ms_div <= (ms_div == 17'd76799) ? 17'd0 : ms_div + 17'd1;
    assign ms_tick = (ms_div == 17'd0);

    logic        wr_arm, armed, arm_stage1, disarm_stb;
    logic [13:0] arm_left, arm_time = 14'd2000;
    logic [3:0]  pa_req = 4'd0;
    logic [31:0] tx_test = 32'd0, tx_freq = 32'd0;
    logic        txgain_ok = 1'b0;

    assign wr_arm = wr_stb && addr == 7'h50;

    rbt_arm arm_i (
        .clk(clk), .rst(rst), .ms_tick(ms_tick), .wr_arm(wr_arm), .wdata(wdata),
        .arm_ms(arm_time), .armed(armed), .stage1(arm_stage1), .remaining_ms(arm_left),
        .disarm_stb(disarm_stb)
    );

    logic [3:0] pa_q = 4'd0;
    always_ff @(posedge clk) pa_q <= armed ? pa_req : 4'd0;
    assign pa_out = pa_q;

    logic tx_run;
    always_ff @(posedge clk) tx_run <= armed & txgain_ok & tx_test[0];

    rbt_txtone txtone_i (
        .clk(clk), .clk_2x(clk_2x), .run(tx_run), .quiet(tx_test[1]), .phase_inc(tx_freq),
        .amplitude(tx_test[27:16]),
        .ad_tx(ad_tx), .ad_txsync(ad_txsync), .ad_txquiet_n(ad_txquiet_n)
    );

    // Writes.
    always_ff @(posedge clk) begin
        ser_start     <= 1'b0;
        bist_start    <= 1'b0;
        rx_clr        <= 1'b0;
        lock_lost_clr <= 1'b0;
        if (wr_stb) begin
            case (addr)
                7'h02: scratch <= wdata;
                7'h03: lock_lost_clr <= wdata[1];
                7'h09: pin_oe  <= wdata[NIO-1:0];
                7'h0A: pin_out <= wdata[NIO-1:0];
                7'h10: ad_ctrl <= wdata[1:0];
                7'h11: ser_start <= 1'b1;
                7'h18, 7'h19, 7'h1A, 7'h1B: rx_clr <= 1'b1;
                7'h20: bist_start <= 1'b1;
                7'h30: i2s_ctrl <= wdata[29:0];
                7'h31: snap_req <= ~snap_req;
                7'h40: cap_tgl  <= ~cap_tgl;
                7'h41: cap_addr <= wdata[10:0];
                7'h51: pa_req   <= wdata[3:0];
                7'h52: tx_test  <= wdata;
                7'h53: arm_time <= wdata[13:0];
                7'h54: tx_freq  <= wdata;
                default: ;
            endcase
        end
        // Any write to ARM, and disarming, clears both test requests: a
        // request has to be made after arming to take effect.
        if (disarm_stb || wr_arm) begin
            pa_req  <= 4'd0;
            tx_test <= 32'd0;
        end
        // The transmit gain register is tracked from the writes this image
        // makes: written at or below TXGAIN_MAX since the reset was
        // released, and nothing higher since.
        if (!ad_ctrl[0]) txgain_ok <= 1'b0;
        else if (wr_stb && addr == 7'h11 && !wdata[15] && wdata[12:8] == 5'h0a) begin
            txgain_ok <= (wdata[31:16] == 16'hFFFF) && (wdata[7:0] <= TXGAIN_MAX);
        end
        // pin_oe and ad_ctrl keep their values through a loss of PLL lock:
        // programming the AD9866 changes the frequency of the clock it
        // gives this image, and resetting it here would undo that.
    end

    always_ff @(posedge clk) begin
        lock_sync <= {lock_sync[0], pll_locked};
        if (lock_lost_clr) lock_lost <= 1'b0;
        if (rst) lock_lost <= 1'b1;
    end

    // Reads.
    always_comb begin
        case (addr)
            7'h00:   rdata = ID;
            7'h01:   rdata = VERSION;
            7'h02:   rdata = scratch;
            7'h03:   rdata = {27'd0, bist_done, bist_busy, ser_busy, lock_lost, lock_sync[1]};
            7'h04:   rdata = syscnt;
            7'h05:   rdata = fcnt_clk69;
            7'h06:   rdata = fcnt_rxclk;
            7'h07:   rdata = fcnt_rxsync;
            7'h08:   rdata = {{(32-NIO-NIN){1'b0}}, pin_s2};
            7'h09:   rdata = {{(32-NIO){1'b0}}, pin_oe};
            7'h0A:   rdata = {{(32-NIO){1'b0}}, pin_out};
            7'h0B:   rdata = {22'd0, cn_s2};
            7'h10:   rdata = {30'd0, ad_ctrl};
            7'h12:   rdata = {cap_sdio, cap_sdo};
            7'h18:   rdata = {17'd0, rx_seen0, 1'b0, rx_seen1};
            7'h19:   rdata = {4'd0, rx_and, 4'd0, rx_or};
            7'h1A:   rdata = {4'd0, rx_max, 4'd0, rx_min};
            7'h1B:   rdata = {20'd0, rx_sample};
            7'h20:   rdata = {mul_errors, ram_errors};
            7'h30:   rdata = {2'd0, i2s_ctrl};
            7'h31:   rdata = {31'd0, ack_s[1] == snap_req};
            7'h32:   rdata = s_txw;
            7'h33:   rdata = s_rxw;
            7'h34:   rdata = s_rxerr;
            7'h35:   rdata = s_rxlast;
            7'h36:   rdata = s_errgot;
            7'h37:   rdata = s_errwant;
            7'h40:   rdata = {31'd0, cap_done};
            7'h41:   rdata = {21'd0, cap_addr};
            7'h42:   rdata = {18'd0, cap_q};
            7'h50:   rdata = {armed, arm_stage1, txgain_ok, 15'd0, arm_left};
            7'h51:   rdata = {28'd0, pa_req};
            7'h52:   rdata = tx_test;
            7'h53:   rdata = {18'd0, arm_time};
            7'h54:   rdata = tx_freq;
            default: rdata = 32'hDEADBEEF;
        endcase
    end

endmodule
