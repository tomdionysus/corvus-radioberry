// SPDX-License-Identifier: GPL-2.0-or-later
//
// The radio core (rtl/core/rbv_core.sv) against the Hermes-Lite 2 based
// core it replaced (radio.sv at a git revision, module radio_ref, built by
// tools/sim.sh), side by side as the images build them (six receivers):
// the reference takes Hermes-Lite 2 commands, the new core what the
// driver makes of the same commands (rb_hl2_translate() in
// driver/kernel/rb_frame.h, written out below), on the same clocks, with
// the same ADC samples, keying and transmit samples. Every receive output,
// the DAC word, tx_on and cw_on are compared on every clock.
//
// What differs by design, and how the stimulus keeps it out:
// - receiver 0 following the TX frequency (one receiver, not duplex) is
//   the driver's, two writes where the reference took one command: the
//   stimulus always sets duplex;
// - PureSignal and the VNA bit are gone: the stimulus keeps them clear;
// - the tape-recorder accounting is gone: the reference drains extra
//   samples after a transmission, so each core takes its samples from its
//   own generator, restarted when PTT rises; tx_tready is counted, not
//   compared.
//
//   +PHASE=<clocks>   clocks for each receive rate (default 120000)
`timescale 1ps / 1ps

module core_cmp_tb;
    localparam int NR = 6;
    localparam int HALF_2X = 3255;  // ps
    logic clk = 1'b0, clk_2x = 1'b1;
    always #(HALF_2X) clk_2x = ~clk_2x;
    always #(2 * HALF_2X) clk = ~clk;

    logic        run = 1'b0;
    logic        rst_channels_p = 1'b0;
    logic        ptt = 1'b0;
    logic        ext_keydown = 1'b0, ext_ptt = 1'b0;
    logic        qmsec_pulse = 1'b0;
    logic [11:0] adc = 12'd0;
    logic        rx_tready = 1'b1;
    logic        tx_tvalid = 1'b0;
    logic [5:0]  cmd_addr = 6'd0, n_addr = 6'd0;
    logic [31:0] cmd_data = 32'd0, n_data = 32'd0;
    logic        cmd_rqst = 1'b0, n_rqst = 1'b0;

    logic rst_all, rst_channels;
    assign rst_all = !run;
    assign rst_channels = !run | rst_channels_p;

    // Each core's transmit samples, from its own generator.
    int unsigned a_k = 0, b_k = 0;
    function automatic logic [31:0] sample(input int unsigned k);
        logic [31:0] h;
        h = k * 32'd2654435761;
        return {h[31:16], h[23:8]};
    endfunction

    // The new core.
    logic [23:0] a_rx_tdata;
    logic        a_rx_tlast, a_rx_tvalid;
    logic [1:0]  a_rx_tuser;
    logic signed [11:0] a_dac;
    logic        a_tx_on, a_cw_on, a_tx_tready;
    rbv_core #(.NR(NR)) dut (
        .clk(clk), .clk_2x(clk_2x),
        .rst_channels(rst_channels), .rst_all(rst_all), .rst_nco(1'b0),
        .run(run), .qmsec_pulse(qmsec_pulse),
        .ds_cmd_ptt(ptt), .ext_keydown(ext_keydown), .ext_ptt(ext_ptt),
        .tx_on(a_tx_on), .cw_on(a_cw_on),
        .tx_tdata(sample(a_k)), .tx_tptt(ptt), .tx_tvalid(tx_tvalid),
        .tx_tready(a_tx_tready), .tx_data_dac(a_dac),
        .rx_data_adc(adc),
        .rx_tdata(a_rx_tdata), .rx_tlast(a_rx_tlast), .rx_tready(rx_tready),
        .rx_tvalid(a_rx_tvalid), .rx_tuser(a_rx_tuser),
        .cmd_addr(n_addr), .cmd_data(n_data), .cmd_rqst(n_rqst));

    // The reference, connected as the images connected it.
    logic [23:0] b_rx_tdata;
    logic        b_rx_tlast, b_rx_tvalid;
    logic [1:0]  b_rx_tuser;
    logic [11:0] b_dac;
    logic        b_tx_on, b_cw_on, b_tx_tready, b_tx_twait, b_cmd_ack, b_lr_tready;
    logic [18:0] b_cw_profile;
    radio_ref #(.NR(NR), .NT(1), .CLK_FREQ(76800000), .LRDATA(0)) ref_i (
        .clk(clk), .clk_2x(clk_2x),
        .rst_channels(rst_channels), .rst_all(rst_all), .rst_nco(1'b0),
        .ds_cmd_ptt(ptt), .run(run), .qmsec_pulse(qmsec_pulse),
        .ext_keydown(ext_keydown), .ext_ptt(ext_ptt),
        .tx_on(b_tx_on), .cw_on(b_cw_on), .cw_profile(b_cw_profile),
        .tx_tdata(sample(b_k)), .tx_tlast(1'b1), .tx_tready(b_tx_tready),
        .tx_tvalid(tx_tvalid), .tx_tuser({ptt, 3'b000}), .tx_twait(b_tx_twait),
        .tx_data_dac(b_dac),
        .lr_tdata(32'd0), .lr_tid(3'd0), .lr_tlast(1'b0), .lr_tready(b_lr_tready),
        .lr_tvalid(1'b0),
        .rx_data_adc(adc),
        .rx_tdata(b_rx_tdata), .rx_tlast(b_rx_tlast), .rx_tready(rx_tready),
        .rx_tvalid(b_rx_tvalid), .rx_tuser(b_rx_tuser),
        .cmd_addr(cmd_addr), .cmd_data(cmd_data), .cmd_rqst(cmd_rqst), .cmd_ack(b_cmd_ack));

    // Samples taken; each generator restarts when PTT rises.
    logic ptt_d = 1'b0;
    always @(posedge clk) begin
        ptt_d <= ptt;
        if (ptt && !ptt_d) begin
            a_k <= 0;
            b_k <= 0;
        end else begin
            if (tx_tvalid && a_tx_tready) a_k <= a_k + 1;
            if (tx_tvalid && b_tx_tready) b_k <= b_k + 1;
        end
    end

    int errors = 0, cycles = 0, rx_words = 0, rx_nonzero = 0, dac_nonzero = 0;
    int tx_clocks = 0, cw_clocks = 0, tready_diff = 0;
    always @(negedge clk) begin
        cycles = cycles + 1;
        if ({a_rx_tdata, a_rx_tlast, a_rx_tvalid, a_rx_tuser, a_dac, a_tx_on, a_cw_on} !==
            {b_rx_tdata, b_rx_tlast, b_rx_tvalid, b_rx_tuser, b_dac, b_tx_on, b_cw_on}) begin
            if (errors < 10)
                $display("MISMATCH t=%0t: new rx %h %b%b dac %h on %b%b, ref rx %h %b%b dac %h on %b%b",
                         $time, a_rx_tdata, a_rx_tvalid, a_rx_tlast, a_dac, a_tx_on, a_cw_on,
                         b_rx_tdata, b_rx_tvalid, b_rx_tlast, b_dac, b_tx_on, b_cw_on);
            errors = errors + 1;
        end
        if (a_tx_tready !== b_tx_tready) tready_diff = tready_diff + 1;
        if (a_rx_tvalid) begin
            rx_words = rx_words + 1;
            if (a_rx_tdata !== 24'd0 && !$isunknown(a_rx_tdata)) rx_nonzero = rx_nonzero + 1;
        end
        if (a_dac !== 12'd0 && !$isunknown(a_dac)) dac_nonzero = dac_nonzero + 1;
        if (a_tx_on === 1'b1) tx_clocks = tx_clocks + 1;
        if (a_cw_on === 1'b1) cw_clocks = cw_clocks + 1;
    end

    // ADC samples (a tone near 7.1 MHz and noise), the quarter-millisecond
    // pulse every 64 clocks, the sender's ready, the FIFO's valid.
    localparam real TWO_PI = 6.283185307179586;
    logic [31:0] lfsr = 32'h5EED_2026;
    int unsigned adc_phase = 0;
    int qcount = 0;
    always @(posedge clk) begin
        real a;
        lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
        adc_phase <= adc_phase + 32'd397062997;  // 7.1 MHz at 76.8 MHz
        a = TWO_PI * real'(adc_phase) / 4294967296.0;
        adc <= 12'($rtoi(1500.0 * $sin(a)) + ($signed(lfsr[7:0]) >>> 1));
        qcount <= (qcount == 63) ? 0 : qcount + 1;
        qmsec_pulse <= (qcount == 63);
        rx_tready <= lfsr[13:11] != 3'd0;
        tx_tvalid <= lfsr[17:15] != 3'd0;
    end

    // The driver's translation (rb_hl2_translate(), duplex set): the
    // register write a Hermes-Lite 2 command becomes, if any.
    function automatic logic [31:0] phase_word(input logic [31:0] hz);
        logic [63:0] p;
        p = 64'(hz) * 64'd1876499845 + 64'd16777216;
        return p[56:25];
    endfunction
    // {write, address, data}.
    function automatic logic [38:0] translate(input logic [5:0] a, input logic [31:0] d);
        logic [5:0]  na;
        logic [31:0] nd;
        na = 6'd0;
        nd = 32'd0;
        if (a == 6'h00) begin
            na = 6'h20;
            nd = {22'd0, d[25:24], 4'd0, d[6:3]};
        end else if (a == 6'h01) begin
            na = 6'h21;
            nd = phase_word(d);
        end else if (a >= 6'h02 && a <= 6'h08) begin
            na = 6'h22 + (a - 6'h02);
            nd = phase_word(d);
        end else if (a >= 6'h12 && a <= 6'h16) begin
            na = 6'h29 + (a - 6'h12);
            nd = phase_word(d);
        end else if (a == 6'h10) begin
            na = 6'h33;
            nd = {22'd0, d[31:24], d[17:16]};
        end else if (a == 6'h17) begin
            na = 6'h32;
            nd = {19'd0, d[12:8], 1'b0, d[6:0]};
        end else return 39'd0;
        return {1'b1, na, nd};
    endfunction

    // One command to both cores on the same clock, held until the next.
    task automatic cmd(input logic [5:0] addr, input logic [31:0] data);
        logic [38:0] t;
        t = translate(addr, data);
        @(posedge clk);
        #1;
        cmd_addr = addr;
        cmd_data = data;
        cmd_rqst = 1'b1;
        n_addr   = t[37:32];
        n_data   = t[31:0];
        n_rqst   = t[38];
        @(posedge clk);
        #1;
        cmd_rqst = 1'b0;
        n_rqst   = 1'b0;
        repeat (6) @(posedge clk);
        #1;
    endtask

    function automatic logic [31:0] rnd();
        return $urandom;
    endfunction

    // Every address with random data, as clients send them: duplex set,
    // PureSignal and the VNA bit clear.
    task automatic all_registers();
        for (int k = 1; k < 64; k++) begin
            logic [31:0] d;
            d = rnd();
            if (k == 'h09) d[23] = 1'b0;
            if (k == 'h0a) d[22] = 1'b0;
            if (k == 'h17) d[6:0] = {4'd0, d[2:0]};          // short TX latency
            if (k == 'h10) d[31:24] = {4'd0, d[27:24]};      // short CW hang
            cmd(6'(k), d);
        end
    endtask

    task automatic tune();
        int addrs[] = '{'h01, 'h02, 'h03, 'h04, 'h05, 'h06, 'h07, 'h08,
                        'h12, 'h13, 'h14, 'h15, 'h16};
        foreach (addrs[k]) cmd(6'(addrs[k]), 32'd1000000 + (rnd() % 30000000));
    endtask

    task automatic wait_clocks(input int n);
        repeat (n) @(posedge clk);
        #1;
    endtask

    // As in sim/radio_eq_tb.sv: the reference's phase words start at zero,
    // as the FPGA's registers do, and every CIC register is zeroed in both
    // designs once the mixers give valid samples, before run rises.
    initial begin
        for (int k = 0; k < 10; k++) ref_i.rx_phase[k] = 32'd0;
        ref_i.tx_phase0 = 32'd0;
    end
`define ZERO_INT(R) \
    R.ci1 = '0; R.ci2 = '0; R.ci3 = '0; R.cq1 = '0; R.cq2 = '0; R.cq3 = '0; \
    R.di1 = '0; R.di2 = '0; R.di3 = '0; R.dq1 = '0; R.dq2 = '0; R.dq3 = '0; \
    R.pi1 = '0; R.pi2 = '0; R.pi3 = '0; R.pq1 = '0; R.pq2 = '0; R.pq3 = '0; \
    R.i1 = '0; R.i2 = '0; R.i3 = '0; R.i4 = '0; R.i5 = '0; \
    R.q1 = '0; R.q2 = '0; R.q3 = '0; R.q4 = '0; R.q5 = '0;
    task automatic zero_cic();
        `ZERO_INT(dut.RX[0].receiver)
        `ZERO_INT(dut.RX[1].receiver)
        `ZERO_INT(dut.RX[2].receiver)
        `ZERO_INT(dut.RX[3].receiver)
        `ZERO_INT(dut.RX[4].receiver)
        `ZERO_INT(dut.RX[5].receiver)
        `ZERO_INT(ref_i.receiver_0)
        `ZERO_INT(ref_i.RECEIVER1.receiver_1)
        `ZERO_INT(ref_i.RECEIVER2_g.receiver_2)
        `ZERO_INT(ref_i.RECEIVER3.receiver_3)
        `ZERO_INT(ref_i.RECEIVER4.receiver_4)
        `ZERO_INT(ref_i.RECEIVER5.receiver_5)
    endtask

    int phase_clocks;
    initial begin
        if (!$value$plusargs("PHASE=%d", phase_clocks)) phase_clocks = 120000;
        wait_clocks(20);
        // 384 ksps, one receiver, duplex, then the rest.
        cmd(6'h00, {6'd0, 2'd3, 16'd0, 1'b0, 4'd0, 1'b1, 2'b00});
        all_registers();
        tune();
        @(negedge clk);
        zero_cic();
        wait_clocks(1);
        run = 1'b1;
        for (int r = 3; r >= 0; r--) begin
            for (int lc = 0; lc < NR; lc += (r >= 2 ? 1 : 2)) begin
                logic [31:0] d;
                d = rnd();
                d[25:24] = 2'(r);
                d[6:3] = 4'(lc);
                d[2] = 1'b1;            // duplex
                cmd(6'h00, d);
                tune();
                wait_clocks(phase_clocks / NR);
            end
            all_registers();
            cmd(6'h00, {6'd0, 2'(r), 16'd0, 1'b0, 4'd0, 1'b1, 2'b00});
            run = 1'b0;
            wait_clocks(500);
            run = 1'b1;
            wait_clocks(2000);
            rst_channels_p = 1'b1;
            wait_clocks(1);
            rst_channels_p = 1'b0;
            // PTT with samples flowing, released, and keyed again.
            cmd(6'h17, {19'd0, 5'(r * 3), 1'b0, 7'(r + 1)});
            ptt = 1'b1;
            wait_clocks(phase_clocks / 4);
            ptt = 1'b0;
            wait_clocks(phase_clocks / 16);
            ptt = 1'b1;
            wait_clocks(phase_clocks / 16);
            ptt = 1'b0;
            wait_clocks(phase_clocks / 8);
            // CW from the jack: key down and up, then key with PTT held.
            cmd(6'h10, rnd() & 32'h0f03_0000);
            for (int k = 0; k < 3; k++) begin
                ext_keydown = 1'b1;
                wait_clocks(3000 + (rnd() % 3000));
                ext_keydown = 1'b0;
                wait_clocks(2000 + (rnd() % 3000));
            end
            ext_ptt = 1'b1;
            wait_clocks(1000);
            ext_keydown = 1'b1;
            wait_clocks(4000);
            ext_keydown = 1'b0;
            wait_clocks(1000);
            ext_ptt = 1'b0;
            wait_clocks(phase_clocks / 8);
        end
        run = 1'b0;
        wait_clocks(100);
        $display("%0d clocks, %0d receive words (%0d not zero), %0d clocks transmitting (%0d CW), %0d DAC samples not zero, %0d mismatches; tx_tready differed on %0d clocks (the dropped accounting)",
                 cycles, rx_words, rx_nonzero, tx_clocks, cw_clocks, dac_nonzero, errors, tready_diff);
        if (errors == 0 && rx_nonzero > 1000 && dac_nonzero > 1000 && cw_clocks > 1000)
            $display("PASS");
        else $display("FAIL");
        $finish;
    end
endmodule
