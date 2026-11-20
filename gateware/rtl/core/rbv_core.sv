// SPDX-License-Identifier: GPL-2.0-or-later
//
// The radio core: up to six receivers and one transmitter between the
// AD9866's 12-bit converters and the image's links to the Pi, on the
// 76.8 MHz radio clock (and 153.6 MHz for the shared blocks). Takeover
// stage 7 (TODO/2026-11-19-core-top.md): this project's own top in place
// of the Hermes-Lite 2 core's radio.v, with its own registers; the
// driver turns the Hermes-Lite 2 commands clients send into them. The
// keying state machine keeps radio.v's states and timings (radio.v is
// GPL-2.0-or-later, Hermes-Lite 2 at commit 3e467fd, the archive in
// gateware/source/).
//
// Receive: the ADC word to each receiver's mixer and NCO (mix2.sv, two
// receivers a mixer), each receiver's first CIC and variable-CIC
// integrators (rx_cic_int.sv), the variable CIC's combs shared by all
// (rx_varcomb.sv), one folded receive filter shared by all
// (firsharedfold.sv), and the sender (rx_sender.sv), which puts every
// receiver's I and Q on the stream to the Pi on the filter's strobe.
//
// Transmit: samples from the Pi's FIFO (I in 31..16, Q in 15..0) through
// the interpolating filter (tx_fir.sv) and CIC (tx_cic.sv) to 76.8 MHz,
// or the CW envelope (cw_shape.sv) when the key is down, then the
// up-converter (tx_upconv.sv: mixer, level, dither, saturation) to the
// DAC word.
//
// Keying, one state machine: software PTT (ds_cmd_ptt, with the samples'
// PTT flag) waits the TX start latency, so the FIFO fills, then sends the
// samples, and holds for the PTT hang time after the flag drops. The
// jack's key (ext_keydown) sends CW: the envelope ramps up over 4.1 ms
// while the key is down and down after it, then holds for the CW hang
// time. The jack's PTT (ext_ptt) keys without samples, and makes the key
// time its own CW. Run clear stops everything. The image gates every
// keying input with the transmit permission and its watchdog.
//
// Registers (board space; written through cmd_*, one request a write):
//   20 RX_CONFIG   3..0 receivers less one, 9..8 rate (48, 96, 192, 384 ksps)
//   21 TX_PHASE    the transmitter's NCO phase step a clock
//   22+r RX_PHASE  receiver r's NCO phase step a clock
//   32 TX_TIMING   6..0 TX start latency (ms), 12..8 PTT hang (ms); hang
//                  31 holds the transmitter keyed until software PTT drops
//   33 CW_HANG     9..0 CW hang time (quarter milliseconds, times 4)
// RX_CONFIG, TX_TIMING and CW_HANG take effect the clock after the
// request; a phase step four clocks after it (the transmitter's and
// receiver 0's five), as the Hermes-Lite 2 core loaded its frequencies,
// so the two produce the same outputs from the same commands
// (sim/core_cmp_tb.sv).
module rbv_core #(
    parameter int NR = 6                   // receivers, 1 to 6
) (
    input  logic               clk,          // 76.8 MHz
    input  logic               clk_2x,       // 153.6 MHz, edges aligned with clk
    input  logic               rst_channels, // the sender's restart (receiver count changed)
    input  logic               rst_all,      // start-up reset
    input  logic               rst_nco,
    input  logic               run,
    input  logic               qmsec_pulse,  // one clock every quarter millisecond

    input  logic               ds_cmd_ptt,   // software PTT (KEYING, gated)
    input  logic               ext_keydown,  // the jack's key (gated)
    input  logic               ext_ptt,      // the jack's PTT (gated)
    output logic               tx_on,
    output logic               cw_on,

    input  logic [31:0]        tx_tdata,
    input  logic               tx_tptt,      // the samples' PTT flag
    input  logic               tx_tvalid,
    output logic               tx_tready,
    output logic signed [11:0] tx_data_dac = '0,

    input  logic [11:0]        rx_data_adc,
    output logic [23:0]        rx_tdata,
    output logic               rx_tlast,
    input  logic               rx_tready,
    output logic               rx_tvalid,
    output logic [1:0]         rx_tuser,

    input  logic [5:0]         cmd_addr,
    input  logic [31:0]        cmd_data,
    input  logic               cmd_rqst
);
    localparam logic [5:0] A_RX_CONFIG = 6'h20, A_TX_PHASE = 6'h21, A_RX_PHASE = 6'h22,
                           A_TX_TIMING = 6'h32, A_CW_HANG = 6'h33;

    // ---------------- registers ----------------
    logic [3:0]  last_chan = 4'd0;
    logic [1:0]  rx_rate = 2'd0;
    logic [6:0]  tx_latency = 7'd20;       // ms
    logic [4:0]  ptt_hang = 5'd12;         // ms
    logic [9:0]  cw_hang = 10'd0;

    always_ff @(posedge clk) begin
        if (cmd_rqst) begin
            case (cmd_addr)
                A_RX_CONFIG: begin
                    last_chan <= cmd_data[3:0];
                    rx_rate   <= cmd_data[9:8];
                end
                A_TX_TIMING: begin
                    tx_latency <= cmd_data[6:0];
                    ptt_hang   <= cmd_data[12:8];
                end
                A_CW_HANG: cw_hang <= cmd_data[9:0];
                default: ;
            endcase
        end
    end

    // Phase steps: three clocks in a pipeline, then loaded.
    logic        ph_v [0:2];
    logic [3:0]  ph_n [0:2];               // 15 the transmitter, else the receiver
    logic [31:0] ph_d [0:2];
    initial for (int k = 0; k < 3; k++) begin
        ph_v[k] = 1'b0;
        ph_n[k] = 4'd0;
        ph_d[k] = 32'd0;
    end
    always_ff @(posedge clk) begin
        ph_v[0] <= cmd_rqst && (cmd_addr == A_TX_PHASE ||
                                (cmd_addr >= A_RX_PHASE && cmd_addr < A_RX_PHASE + 6'(NR)));
        ph_n[0] <= (cmd_addr == A_TX_PHASE) ? 4'hf : 4'(cmd_addr - A_RX_PHASE);
        ph_d[0] <= cmd_data;
        for (int k = 1; k < 3; k++) begin
            ph_v[k] <= ph_v[k-1];
            ph_n[k] <= ph_n[k-1];
            ph_d[k] <= ph_d[k-1];
        end
    end

    logic [31:0] rx_phase [0:NR-1];
    logic [31:0] tx_phase = 32'd0;
    initial for (int k = 0; k < NR; k++) rx_phase[k] = 32'd0;
    always_ff @(posedge clk) begin
        if (ph_v[2]) begin
            if (ph_n[2] == 4'hf) tx_phase <= ph_d[2];
            for (int k = 0; k < NR; k++)
                if (ph_n[2] == 4'(k)) rx_phase[k] <= ph_d[2];
        end
    end
    // The transmitter's and receiver 0's a clock later.
    logic [31:0] tx0_phase = 32'd0, rx0_phase = 32'd0;
    always_ff @(posedge clk) begin
        tx0_phase <= tx_phase;
        rx0_phase <= rx_phase[0];
    end

    // Decimation of the variable CIC for the rate.
    logic [5:0] rate;
    always_comb begin
        case (rx_rate)
            2'd0: rate = 6'd40;    // 48 ksps
            2'd1: rate = 6'd20;    // 96 ksps
            2'd2: rate = 6'd10;    // 192 ksps
            default: rate = 6'd5;  // 384 ksps
        endcase
    end

    // ---------------- receive ----------------
    // The ADC word, a copy for each mixer.
    logic [11:0] adcpipe [0:2];
    initial for (int k = 0; k < 3; k++) adcpipe[k] = '0;
    always_ff @(posedge clk) for (int k = 0; k < 3; k++) adcpipe[k] <= rx_data_adc;

    // Mixers: receivers 0 and 2, 1 and 3, 4 and 5 (the pairing of the
    // core this replaced; each receiver's output is its own).
    logic signed [17:0] mix_i [0:5];
    logic signed [17:0] mix_q [0:5];
    logic [31:0] phase_of [0:5];
    always_comb
        for (int k = 0; k < 6; k++) phase_of[k] = (k == 0) ? rx0_phase : ((k < NR) ? rx_phase[k] : 32'd0);

    mix2 #(.CALCTYPE(0)) mix_02 (
        .clk(clk), .clk_2x(clk_2x), .rst(rst_nco), .phi0(phase_of[0]), .phi1(phase_of[2]),
        .adc(adcpipe[0]), .mixdata0_i(mix_i[0]), .mixdata0_q(mix_q[0]),
        .mixdata1_i(mix_i[2]), .mixdata1_q(mix_q[2]));
    generate
        if (NR >= 2) begin : MIX13
            mix2 #(.CALCTYPE(0)) mix_13 (
                .clk(clk), .clk_2x(clk_2x), .rst(rst_nco), .phi0(phase_of[1]), .phi1(phase_of[3]),
                .adc(adcpipe[1]), .mixdata0_i(mix_i[1]), .mixdata0_q(mix_q[1]),
                .mixdata1_i(mix_i[3]), .mixdata1_q(mix_q[3]));
        end
        if (NR >= 5) begin : MIX45
            mix2 #(.CALCTYPE(0)) mix_45 (
                .clk(clk), .clk_2x(clk_2x), .rst(rst_nco), .phi0(phase_of[4]), .phi1(phase_of[5]),
                .adc(adcpipe[2]), .mixdata0_i(mix_i[4]), .mixdata0_q(mix_q[4]),
                .mixdata1_i(mix_i[5]), .mixdata1_q(mix_q[5]));
        end
    endgenerate

    // Each receiver's first CIC and variable-CIC integrators.
    localparam int VW = 25;
    logic          int_rdy [0:NR-1];
    logic [VW-1:0] int_i [0:NR-1];
    logic [VW-1:0] int_q [0:NR-1];
    genvar r, v;
    generate
        for (r = 0; r < NR; r = r + 1) begin : RX
            rx_cic_int receiver (
                .rst_all(rst_all), .clock(clk), .decimation(rate),
                .mixdata_I(mix_i[r]), .mixdata_Q(mix_q[r]),
                .int_strobe(int_rdy[r]), .int_i(int_i[r]), .int_q(int_q[r]));
        end
    endgenerate

    // The variable CIC's combs, the filter and the sender, shared.
    logic [VW*NR-1:0] vc_ii, vc_iq;
    logic [16*NR-1:0] fs_xr, fs_xi;
    logic             fs_x;
    logic [24*NR-1:0] fs_yr, fs_yi;
    logic             fs_y;
    generate
        for (v = 0; v < NR; v = v + 1) begin : VCIN
            assign vc_ii[VW*v +: VW] = int_i[v];
            assign vc_iq[VW*v +: VW] = int_q[v];
        end
    endgenerate
    rx_varcomb #(.NR(NR)) varcomb (
        .clock(clk), .clock_2x(clk_2x), .rst_all(rst_all), .decimation(rate),
        .in_strobe(int_rdy[0]), .int_i(vc_ii), .int_q(vc_iq),
        .out_strobe(fs_x), .out_i(fs_xr), .out_q(fs_xi));
    firshared #(.NR(NR)) fir_shared (
        .rst_all(rst_all), .clock(clk), .clock_2x(clk_2x), .last_chan(last_chan),
        .x_avail(fs_x), .x_real(fs_xr), .x_imag(fs_xi),
        .y_avail(fs_y), .y_real(fs_yr), .y_imag(fs_yi));
    rx_sender #(.NR(NR)) sender (
        .clk(clk), .rst(rst_all || rst_channels), .last_chan(last_chan),
        .y_avail(fs_y), .y_real(fs_yr), .y_imag(fs_yi),
        .rx_tdata(rx_tdata), .rx_tlast(rx_tlast), .rx_tvalid(rx_tvalid),
        .rx_tuser(rx_tuser), .rx_tready(rx_tready));

    // ---------------- transmit ----------------
    localparam logic [2:0] NOTX = 3'd0, PRETX = 3'd1, PTTTX = 3'd2, CWTX = 3'd3, CWHANG = 3'd4;
    localparam logic [18:0] CW_MAX = 19'h4d800;     // the envelope's top, 4.1 ms of clocks

    logic [2:0]  state = NOTX, state_next;
    logic [8:0]  timer = 9'd0, timer_next;          // quarter milliseconds
    logic [18:0] cwlevel = 19'd0, cwlevel_next;
    logic signed [15:0] fir_i = '0, fir_i_next, fir_q = '0, fir_q_next;
    logic        fir_tready, cw_key, ptt;
    logic        hang_hold;                         // hang 31: stay keyed until software PTT drops
    assign hang_hold = &ptt_hang;
    assign ptt = tx_tptt & tx_tvalid;

    always_ff @(posedge clk) begin
        state   <= run ? state_next : NOTX;
        timer   <= timer_next;
        cwlevel <= cwlevel_next;
        fir_i   <= fir_i_next;
        fir_q   <= fir_q_next;
    end

    always_comb begin
        state_next   = state;
        timer_next   = timer;
        cwlevel_next = cwlevel;
        fir_i_next   = fir_i;
        fir_q_next   = fir_q;
        tx_on        = 1'b1;
        cw_on        = 1'b0;
        cw_key       = 1'b0;
        tx_tready    = fir_tready;
        case (state)
            NOTX: begin
                fir_i_next   = '0;
                fir_q_next   = '0;
                cwlevel_next = '0;
                timer_next   = {tx_latency, 2'b00};
                tx_on        = 1'b0;
                if (ext_keydown | ext_ptt | (ds_cmd_ptt & ptt)) state_next = PRETX;
            end
            PRETX: begin
                // The FIFO fills for the start latency.
                tx_tready = 1'b0;
                if (ext_keydown & ext_ptt) begin
                    timer_next = '0;
                    state_next = CWTX;
                end else if (timer != '0) begin
                    if (qmsec_pulse) timer_next = timer - 9'd1;
                    if (!(ext_keydown | ext_ptt | ptt)) state_next = NOTX;
                end else if (ext_keydown) state_next = CWTX;
                else if (ptt)            state_next = PTTTX;
                else if (ext_ptt)        state_next = PRETX;   // a keyer may take a while
                else                     state_next = NOTX;
            end
            PTTTX: begin
                if (ext_keydown) state_next = CWTX;
                else if (hang_hold & ~ds_cmd_ptt) state_next = NOTX;
                else if (ptt) begin
                    timer_next = {2'b00, ptt_hang, 2'b00};
                    if (fir_tready) begin
                        fir_i_next = tx_tdata[31:16];
                        fir_q_next = tx_tdata[15:0];
                    end
                end else if (timer != '0) begin
                    if (qmsec_pulse) timer_next = timer - 9'd1;
                end else if (!hang_hold) state_next = NOTX;
            end
            CWTX: begin
                cw_on  = 1'b1;
                cw_key = 1'b1;
                if (ext_keydown) begin
                    if (cwlevel != CW_MAX) cwlevel_next = cwlevel + 19'd1;
                    timer_next = ext_ptt ? 9'd0 : {tx_latency, 2'b00};
                end else if (timer != '0) begin
                    if (qmsec_pulse) timer_next = timer - 9'd1;
                end else if (cwlevel != '0) cwlevel_next = cwlevel - 19'd1;
                else begin
                    timer_next   = {tx_latency, 2'b00};
                    cwlevel_next = ext_ptt ? 19'd0 : {7'd0, cw_hang, 2'b00};
                    state_next   = CWHANG;
                end
            end
            CWHANG: begin
                // The level counter times the hang.
                cw_on = 1'b1;
                if (ext_keydown & ext_ptt) begin
                    timer_next   = '0;
                    cwlevel_next = '0;
                    state_next   = CWTX;
                end else if (ext_keydown) begin
                    if (timer != '0) begin
                        if (qmsec_pulse) timer_next = timer - 9'd1;
                    end else begin
                        timer_next   = {tx_latency, 2'b00};
                        cwlevel_next = '0;
                        state_next   = CWTX;
                    end
                end else if (cwlevel != '0) begin
                    if (qmsec_pulse) cwlevel_next = cwlevel - 19'd1;
                end else if (!ext_ptt) state_next = NOTX;
            end
            default: state_next = NOTX;
        endcase
    end

    // Interpolate by 8 (48 kHz to 384 kHz), then by 200 (to 76.8 MHz).
    logic        req2;
    logic [19:0] y1_r, y1_i;
    logic [15:0] y2_r, y2_i;
    tx_fir fi (
        .clock(clk), .calc(req2), .req(fir_tready), .x_real(fir_i), .x_imag(fir_q),
        .y_real(y1_r), .y_imag(y1_i));
    tx_cic in2 (
        .clock(clk), .req(req2), .x_real(y1_r), .x_imag(y1_i), .y_real(y2_r), .y_imag(y2_i));

    // The CW envelope, fed 0 outside CWTX (where the level counter times
    // the hang), so its pipeline holds 0 when keying starts.
    logic [15:0] cw_amp;
    cw_shape cws (.clk(clk), .level(cw_key ? cwlevel : 19'd0), .amp(cw_amp));

    // Up-conversion. The samples' I and Q enter swapped, which puts the
    // signal on the side Hermes-Lite 2 clients expect.
    logic signed [15:0] up_i, up_q;
    logic signed [11:0] upconv_dac;
    assign up_i = cw_key ? cw_amp : y2_i;
    assign up_q = cw_key ? 16'sd0 : y2_r;
    tx_upconv upconv (
        .clk(clk), .dither(1'b1), .frequency(tx0_phase),
        .in_i(up_i), .in_q(up_q), .dac(upconv_dac));
    always_ff @(posedge clk) tx_data_dac <= upconv_dac;
endmodule
