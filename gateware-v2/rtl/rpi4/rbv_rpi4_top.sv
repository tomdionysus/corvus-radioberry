// SPDX-License-Identifier: GPL-2.0-or-later
//
// rpi4-i2s, receive build: the Hermes-Lite 2 radio core (six receivers)
// with this project's control, AD9866 start-up and I2S transport, for a
// Raspberry Pi 4. Transmit is not wired in this build: nothing can key it.
//
// Clock domains:
//   cfg    the board clock as it arrives on pin 53 (38.4 MHz, then 76.8 MHz
//          once the AD9866 is programmed): control registers, AD9866
//          start-up. Needs no PLL.
//   radio  PLL 76.8 MHz and 153.6 MHz: radio core, AD9866 receive bus.
//   link   PLL 122.88 MHz: I2S.
//   spi    the Pi's SPI clock: frame shifting only.
//
// Board registers (space 1):
//   00 ID        ro  0x52425632 ("RBV2")
//   01 VERSION   ro  major in 31..16, minor in 15..0
//   02 CAPS      ro  3..0 receivers, 4 transmit available (0 here),
//                    11..8 transport (1 = I2S), 15..12 FPGA (2 = 10CL025)
//   03 STATUS    rw  0 PLL locked, 1 AD9866 set up, 2 radio running,
//                    3 I2S enabled, 4 receive FIFO was full, 5 ADC hit full
//                    scale, 6 a frame had a bad CRC; write 1s to clear 4..6
//   04 CONTROL   rw  0 run (receivers stream), 1 restart (self-clearing:
//                    AD9866 start-up and radio reset)
//   05 KEYING    rw  0 PTT, 1 transmit permission; stored, no effect here
//   06 I2S       rw  0 enable, 15..8 fast clocks per bit (122.88 MHz /
//                    div), 23..16 return sampling point, 29..24 return word
//                    offset; change the others only while disabled
//   07 CRC_ERR   ro  frames with a bad CRC
//   08 FRAMES    ro  frames accepted
//   09 FIFO_FULL ro  times the receive FIFO refused a frame
//   0A I2S_TX    ro  words sent to the Pi with data
//   0B I2S_RX    ro  words received from the Pi (discarded)
//   0C AD_READ   rw  write 4..0 = AD9866 register to read; reads
//                    31 busy, 7..0 value
//   0D SCRATCH   rw
//   0E AD_WRITES ro  AD9866 register writes made
// Radio space (space 0): a write is passed to the radio core as a
// Hermes-Lite 2 command (address, data); a read returns the last value
// written to that address. Address 0x0a also sets the AD9866 receive gain.
module rbv_rpi4_top #(
    parameter int NR = 6,
    parameter int RESET_CYCLES = 4096,
    parameter int LOCK_CYCLES  = 131072
) (
    input  logic       clk53,

    input  logic       spi_sck,
    input  logic       spi_ce0_n,
    input  logic       spi_mosi,
    output logic       spi_miso,

    output logic       i2s_bclk,      // GPIO 18
    output logic       i2s_fs,        // GPIO 19
    output logic       i2s_to_pi,     // GPIO 20
    input  logic       i2s_from_pi,   // GPIO 21

    output logic [3:0] pa_out,

    output logic       ad_rst_n,
    output logic       ad_mode,
    output logic       ad_sen_n,
    output logic       ad_sclk,
    inout  wire        ad_sdio,
    input  logic [5:0] ad_rx,
    input  logic       ad_rxsync,
    output logic [5:0] ad_tx,
    output logic       ad_txsync,
    output logic       ad_txquiet_n
);

    localparam logic [31:0] ID      = 32'h52425632;
    localparam logic [31:0] VERSION = 32'h0001_0000;
    localparam logic [3:0]  NR4     = NR;
    localparam logic [31:0] CAPS    = {16'd0, 4'd2, 4'd1, 3'd0, 1'b0, NR4};

    // ---- transmit side: not wired in this build ----
    assign pa_out       = 4'b0000;
    assign ad_tx        = 6'd0;
    assign ad_txsync    = 1'b0;
    assign ad_txquiet_n = 1'b0;
    assign ad_mode      = 1'b1;

    // ---- clocks ----
    logic clk, clk_2x, clk_f, pll_locked;
    rbt_pll pll_i (.inclk0(clk53), .c0(clk), .c1(clk_2x), .c2(clk_f), .locked(pll_locked));

    // ======================= cfg domain (clk53) =======================
    logic        rd_req, rd_space, frame_stb, frame_bad, f_write, f_space;
    logic [5:0]  rd_addr, f_addr;
    logic [31:0] f_data, rd_data;
    logic [7:0]  status_byte;

    rbv_spi spi_i (
        .sck(spi_sck), .cs_n(spi_ce0_n), .mosi(spi_mosi), .miso(spi_miso),
        .clk(clk53), .rd_req(rd_req), .rd_space(rd_space), .rd_addr(rd_addr),
        .rd_data(rd_data), .status(status_byte), .frame_stb(frame_stb),
        .frame_bad(frame_bad), .f_write(f_write), .f_space(f_space),
        .f_addr(f_addr), .f_data(f_data)
    );

    logic [31:0] shadow [0:63];
    logic [31:0] shadow_q;
    logic [31:0] control = 32'd0, keying = 32'd0, i2s_reg = 32'h0000_0200, scratch = 32'd0;
    logic [31:0] crc_err = 32'd0, frames = 32'd0;
    logic [2:0]  sticky = 3'd0;
    logic        restart = 1'b1;
    logic        gain_req = 1'b0, adrd_req = 1'b0;
    logic [5:0]  gain_code = 6'd0;
    logic [4:0]  adrd_addr = 5'd0;
    logic [7:0]  ad_value;
    logic        ad_busy, setup_done;
    logic [15:0] ad_writes;
    logic        sdio_out, sdio_oe;

    // Radio command handshake to the radio domain.
    logic        cmd_tgl = 1'b0;
    logic [5:0]  cmd_addr_c = 6'd0;
    logic [31:0] cmd_data_c = 32'd0;

    // Status from the other domains.
    logic [1:0]  lock_c = 2'b00, run_ok_c = 2'b00, full_c = 2'b00, ovl_c = 2'b00;
    logic        radio_running, fifo_full_r, adc_ovl_r;
    logic [31:0] full_count_c = 32'd0;
    logic        full_prev = 1'b0;
    logic [31:0] i2s_tx_words, i2s_rx_words;

    always_ff @(posedge clk53) begin
        lock_c   <= {lock_c[0], pll_locked};
        run_ok_c <= {run_ok_c[0], radio_running};
        full_c   <= {full_c[0], fifo_full_r};
        ovl_c    <= {ovl_c[0], adc_ovl_r};
        full_prev <= full_c[1];
        if (full_c[1] && !full_prev) begin
            sticky[0]    <= 1'b1;
            full_count_c <= full_count_c + 32'd1;
        end
        if (ovl_c[1]) sticky[1] <= 1'b1;

        restart  <= 1'b0;
        gain_req <= 1'b0;
        adrd_req <= 1'b0;

        if (frame_bad) begin
            crc_err   <= crc_err + 32'd1;
            sticky[2] <= 1'b1;
        end
        if (frame_stb) begin
            frames <= frames + 32'd1;
            if (f_write && !f_space) begin
                shadow[f_addr] <= f_data;
                cmd_addr_c <= f_addr;
                cmd_data_c <= f_data;
                cmd_tgl    <= ~cmd_tgl;
                // Hermes-Lite 2 receive gain (LNA) word to AD9866 gain code.
                if (f_addr == 6'h0a) begin
                    gain_code <= f_data[6] ? f_data[5:0] :
                                 (f_data[5] ? ~f_data[5:0] : {1'b1, f_data[4:0]});
                    gain_req  <= 1'b1;
                end
            end else if (f_write && f_space) begin
                case (f_addr)
                    6'h03: sticky <= sticky & ~f_data[6:4];
                    6'h04: begin
                        control <= {f_data[31:2], 1'b0, f_data[0]};
                        if (f_data[1]) restart <= 1'b1;
                    end
                    6'h05: keying  <= f_data;
                    6'h06: i2s_reg <= f_data;
                    6'h0C: begin
                        adrd_addr <= f_data[4:0];
                        adrd_req  <= 1'b1;
                    end
                    6'h0D: scratch <= f_data;
                    default: ;
                endcase
            end
        end
    end

    assign status_byte = {1'b0, sticky[2], sticky[1], sticky[0], i2s_reg[0],
                          run_ok_c[1], setup_done, lock_c[1]};

    // Register reads: address registered, then the shadow RAM and the mux.
    logic        rsp_q = 1'b0;
    logic [5:0]  rad_q = 6'd0;
    always_ff @(posedge clk53) begin
        rsp_q    <= rd_space;
        rad_q    <= rd_addr;
        shadow_q <= shadow[rd_addr];
        if (!rsp_q) rd_data <= shadow_q;
        else case (rad_q)
            6'h00: rd_data <= ID;
            6'h01: rd_data <= VERSION;
            6'h02: rd_data <= CAPS;
            6'h03: rd_data <= {25'd0, sticky, i2s_reg[0], run_ok_c[1], setup_done, lock_c[1]};
            6'h04: rd_data <= control;
            6'h05: rd_data <= keying;
            6'h06: rd_data <= i2s_reg;
            6'h07: rd_data <= crc_err;
            6'h08: rd_data <= frames;
            6'h09: rd_data <= full_count_c;
            6'h0A: rd_data <= i2s_tx_words;
            6'h0B: rd_data <= i2s_rx_words;
            6'h0C: rd_data <= {ad_busy, 23'd0, ad_value};
            6'h0D: rd_data <= scratch;
            6'h0E: rd_data <= {16'd0, ad_writes};
            default: rd_data <= 32'hDEADBEEF;
        endcase
    end

    rbv_adseq #(.RESET_CYCLES(RESET_CYCLES), .LOCK_CYCLES(LOCK_CYCLES)) adseq_i (
        .clk(clk53), .restart(restart), .pll_locked(pll_locked),
        .gain_req(gain_req), .gain_code(gain_code),
        .rd_req(adrd_req), .rd_addr(adrd_addr), .rd_value(ad_value), .rd_busy(ad_busy),
        .done(setup_done), .writes(ad_writes),
        .ad_rst_n(ad_rst_n), .ad_sen_n(ad_sen_n), .ad_sclk(ad_sclk),
        .ad_sdio_out(sdio_out), .ad_sdio_oe(sdio_oe), .ad_sdio_in(ad_sdio)
    );
    assign ad_sdio = sdio_oe ? sdio_out : 1'bz;

    // ===================== radio domain (clk, clk_2x) =====================
    logic [2:0]  rst_s = 3'b111;
    logic        rst_r;
    always_ff @(posedge clk or negedge pll_locked) begin
        if (!pll_locked) rst_s <= 3'b111;
        else rst_s <= {rst_s[1:0], ~setup_done};
    end
    assign rst_r = rst_s[2];
    assign radio_running = ~rst_r;

    logic [2:0]  cmd_s = 3'b000;
    logic [1:0]  run_s = 2'b00;
    logic        cmd_rqst = 1'b0, rst_channels = 1'b0;
    logic [5:0]  cmd_addr = 6'd0;
    logic [31:0] cmd_data = 32'd0;
    logic [3:0]  nrx = 4'd0;

    always_ff @(posedge clk) begin
        cmd_s        <= {cmd_s[1:0], cmd_tgl};
        run_s        <= {run_s[0], control[0]};
        cmd_rqst     <= 1'b0;
        rst_channels <= 1'b0;
        if (cmd_s[2] ^ cmd_s[1]) begin
            cmd_addr <= cmd_addr_c;
            cmd_data <= cmd_data_c;
            cmd_rqst <= 1'b1;
            if (cmd_addr_c == 6'h00 && cmd_data_c[6:3] != nrx) begin
                nrx          <= cmd_data_c[6:3];
                rst_channels <= 1'b1;
            end
        end
        if (rst_r) nrx <= 4'd0;
    end

    // Quarter-millisecond tick for the radio's timers.
    logic [14:0] qms = 15'd0;
    logic        qmsec_pulse;
    always_ff @(posedge clk) qms <= (qms == 15'd19199) ? 15'd0 : qms + 15'd1;
    assign qmsec_pulse = (qms == 15'd0);

    // AD9866 receive bus: both data and RX sync in the I/O-cell registers
    // (the sync changes up to about 3 ns after the data). The half with
    // RX sync high is the low half of a sample.
    logic [5:0]  rx_d1, rx_d2;
    logic        rxsync_d1;
    logic [11:0] rx_asm = 12'd0, adc = 12'd0;
    always_ff @(posedge clk_2x) begin
        rx_d1     <= ad_rx;
        rx_d2     <= rx_d1;
        rxsync_d1 <= ad_rxsync;
        if (rxsync_d1) rx_asm <= {rx_d2, rx_d1};
    end
    always_ff @(posedge clk) begin
        adc       <= rx_asm;
        adc_ovl_r <= (adc == 12'h7FF) || (adc == 12'h800);
    end

    // Radio core. Transmit inputs are tied off.
    logic [23:0] rx_tdata;
    logic        rx_tlast, rx_tvalid, rx_tready;
    logic [1:0]  rx_tuser;

    radio #(
        .NR(NR), .NT(1), .CLK_FREQ(76800000), .VNA(0), .LRDATA(0)
    ) radio_i (
        .clk(clk), .clk_2x(clk_2x),
        .rst_channels(rst_channels | rst_r), .rst_all(rst_r), .rst_nco(1'b0),
        .link_running(1'b0), .link_master(1'b0), .lm_data(24'd0), .lm_valid(1'b0),
        .ls_valid(), .ls_done(1'b0),
        .ds_cmd_ptt(1'b0), .run(1'b0), .qmsec_pulse(qmsec_pulse),
        .ext_keydown(1'b0), .ext_ptt(1'b0),
        .tx_on(), .cw_on(), .cw_profile(),
        .tx_tdata(32'd0), .tx_tlast(1'b1), .tx_tready(), .tx_tvalid(1'b0),
        .tx_tuser(4'd0), .tx_twait(), .tx_data_dac(),
        .clk_envelope(clk), .tx_envelope_pwm_out(), .tx_envelope_pwm_out_inv(),
        .lr_tdata(32'd0), .lr_tid(3'd0), .lr_tlast(1'b0), .lr_tready(), .lr_tvalid(1'b0),
        .rx_data_adc(adc),
        .rx_tdata(rx_tdata), .rx_tlast(rx_tlast), .rx_tready(rx_tready),
        .rx_tvalid(rx_tvalid), .rx_tuser(rx_tuser),
        .cmd_addr(cmd_addr), .cmd_data(cmd_data), .cmd_rqst(cmd_rqst), .cmd_ack(),
        .debug_out()
    );

    // Receive FIFO: {index in frame, value}. The index restarts after the
    // last value of a frame and on every receiver-count change.
    logic [3:0]  widx = 4'd0;
    logic [10:0] wfree;
    always_ff @(posedge clk) begin
        if (rst_r || rst_channels) widx <= 4'd0;
        else if (rx_tvalid) widx <= rx_tlast ? 4'd0 : widx + 4'd1;
        fifo_full_r <= run_s[1] && (wfree < 11'd16);
    end
    assign rx_tready = run_s[1] && (wfree >= 11'd16);

    logic        lk_valid, lk_take;
    logic [27:0] lk_word;
    rbv_afifo #(.W(28), .AW(10)) fifo_i (
        .wclk(clk), .wclr(rst_r), .wen(rx_tvalid), .wdata({widx, rx_tdata}), .wfree(wfree),
        .rclk(clk_f), .rtake(lk_take), .rvalid(lk_valid), .rdata(lk_word)
    );

    // ========================= link domain (clk_f) =========================
    rbv_i2s i2s_i (
        .clk_f(clk_f), .enable(i2s_reg[0] & pll_locked), .div(i2s_reg[15:8]),
        .rxph(i2s_reg[23:16]), .rxoff(i2s_reg[29:24]),
        .word_valid(lk_valid), .word({3'b000, lk_word}), .word_take(lk_take),
        .bclk(i2s_bclk), .fs(i2s_fs), .sd_out(i2s_to_pi), .sd_in(i2s_from_pi),
        .tx_words(i2s_tx_words), .rx_words(i2s_rx_words)
    );

endmodule
