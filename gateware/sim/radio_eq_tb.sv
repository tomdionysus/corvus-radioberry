// SPDX-License-Identifier: GPL-2.0-or-later
//
// The radio core (rtl/core/radio.sv) against a reference copy of an
// earlier version (module radio_ref, built from git by tools/sim.sh) with
// the same ports, from bfeb533 on, side by side, both as the images build
// them (six receivers, one transmitter, LRDATA 0): the same ADC samples,
// commands, keying and transmit samples, every output compared on every
// clock. (The version of this file at bfeb533 compares bfeb533 against
// ad4726b, with the ports removed there.)
//
// The commands are random writes to every address (address 0x09 with the
// VNA bit clear, as the daemon and the SoapySDR module send it), with
// each receive rate and receiver count in turn and every receiver tuned;
// between them the run bit drops (which resets the receivers and the
// sender), PTT keys with transmit samples flowing, and the jack's key and
// PTT inputs key CW. The quarter-millisecond pulse comes every 64 clocks,
// so that the transmit timings (latency, hang times) run out within the
// test.
//
//   +PHASE=<clocks>   clocks for each receive rate (default 120000)
//   +TX_NEW           DAC words not compared, PureSignal off (see below)
`timescale 1ps / 1ps

module radio_eq_tb;
    localparam int NR = 6;
    localparam int HALF_2X = 3255;  // ps
    logic clk = 1'b0, clk_2x = 1'b1;
    always #(HALF_2X) clk_2x = ~clk_2x;
    always #(2 * HALF_2X) clk = ~clk;

    // Inputs, changed just after each rising edge of clk.
    logic        run = 1'b0;
    logic        rst_channels_p = 1'b0;
    logic        ptt = 1'b0;
    logic        ext_keydown = 1'b0, ext_ptt = 1'b0;
    logic        qmsec_pulse = 1'b0;
    logic [11:0] adc = 12'd0;
    logic        rx_tready = 1'b1;
    logic        tx_tvalid = 1'b0;
    logic [31:0] tx_tdata = 32'd0;
    logic [5:0]  cmd_addr = 6'd0;
    logic [31:0] cmd_data = 32'd0;
    logic        cmd_rqst = 1'b0;

    logic rst_all, rst_channels;
    assign rst_all = !run;
    assign rst_channels = !run | rst_channels_p;

    // The design under test.
    logic [23:0] a_rx_tdata;
    logic        a_rx_tlast, a_rx_tvalid;
    logic [1:0]  a_rx_tuser;
    logic [11:0] a_dac;
    logic        a_tx_on, a_cw_on, a_tx_tready, a_tx_twait, a_cmd_ack, a_lr_tready;
    logic [18:0] a_cw_profile;
    radio #(.NR(NR), .NT(1), .CLK_FREQ(76800000), .LRDATA(0)) dut (
        .clk(clk), .clk_2x(clk_2x),
        .rst_channels(rst_channels), .rst_all(rst_all), .rst_nco(1'b0),
        .ds_cmd_ptt(ptt), .run(run), .qmsec_pulse(qmsec_pulse),
        .ext_keydown(ext_keydown), .ext_ptt(ext_ptt),
        .tx_on(a_tx_on), .cw_on(a_cw_on), .cw_profile(a_cw_profile),
        .tx_tdata(tx_tdata), .tx_tlast(1'b1), .tx_tready(a_tx_tready),
        .tx_tvalid(tx_tvalid), .tx_tuser({ptt, 3'b000}), .tx_twait(a_tx_twait),
        .tx_data_dac(a_dac),
        .lr_tdata(32'd0), .lr_tid(3'd0), .lr_tlast(1'b0), .lr_tready(a_lr_tready),
        .lr_tvalid(1'b0),
        .rx_data_adc(adc),
        .rx_tdata(a_rx_tdata), .rx_tlast(a_rx_tlast), .rx_tready(rx_tready),
        .rx_tvalid(a_rx_tvalid), .rx_tuser(a_rx_tuser),
        .cmd_addr(cmd_addr), .cmd_data(cmd_data), .cmd_rqst(cmd_rqst), .cmd_ack(a_cmd_ack)
    );

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
        .tx_tdata(tx_tdata), .tx_tlast(1'b1), .tx_tready(b_tx_tready),
        .tx_tvalid(tx_tvalid), .tx_tuser({ptt, 3'b000}), .tx_twait(b_tx_twait),
        .tx_data_dac(b_dac),
        .lr_tdata(32'd0), .lr_tid(3'd0), .lr_tlast(1'b0), .lr_tready(b_lr_tready),
        .lr_tvalid(1'b0),
        .rx_data_adc(adc),
        .rx_tdata(b_rx_tdata), .rx_tlast(b_rx_tlast), .rx_tready(rx_tready),
        .rx_tvalid(b_rx_tvalid), .rx_tuser(b_rx_tuser),
        .cmd_addr(cmd_addr), .cmd_data(cmd_data), .cmd_rqst(cmd_rqst), .cmd_ack(b_cmd_ack)
    );

    // Every output on every clock. X in the same places is equal (both
    // designs start with the same unwritten registers); the counts below
    // show that the comparison saw real values.
    // With +RX_TOL=<n>, for a design whose receive arithmetic is no longer
    // the reference's bit for bit (the pruned CICs, from 45a42e9; the
    // reference at bfeb533): every other output must match exactly; the
    // CIC outputs at the receive filter's input (the reference's
    // cic_rdy[0], cic_i, cic_q; the design's fs_x, fs_xr, fs_xi) must come
    // on the same clock and differ by at most one 16-bit LSB, except in
    // the few outputs after a change of rate, where both CICs give a
    // transient of different scale (at most 2 % of the values); the
    // receive words, which the filter's memory keeps those transients in
    // for 122 outputs, are reported (rms, largest, how many further apart
    // than n), not judged.
    // With +TX_NEW, for a design whose transmit up-converter is no longer
    // the reference's (the multiplier mixer and dither, from stage 6c; the
    // reference before it): the DAC words are not compared (their spectra
    // are judged by tools/sim.sh txupconv), PureSignal stays off (with it,
    // the second receiver hears the DAC words), and every other output must
    // match.
    int   tx_new = 0;
    initial if ($test$plusargs("TX_NEW")) tx_new = 1;
    int   rx_tol = 0;
    int   rx_far = 0, rx_dmax = 0;
    real  rx_dse = 0.0;
    int   cic_n = 0, cic_far = 0, cic_dmax = 0;
    initial if (!$value$plusargs("RX_TOL=%d", rx_tol)) rx_tol = 0;
`ifdef REF_PRUNED
    // A reference from 19c1441 on: the filter's input as in the design.
    logic ref_x;
    logic [15:0] ref_xi [0:NR-1];
    logic [15:0] ref_xq [0:NR-1];
    assign ref_x = ref_i.fs_x;
    for (genvar g = 0; g < NR; g++) begin : REFX
        assign ref_xi[g] = ref_i.fs_xr[16*g +: 16];
        assign ref_xq[g] = ref_i.fs_xi[16*g +: 16];
    end
`else
    logic ref_x;
    logic [15:0] ref_xi [0:NR-1];
    logic [15:0] ref_xq [0:NR-1];
    assign ref_x = ref_i.cic_rdy[0];
    for (genvar g = 0; g < NR; g++) begin : REFX
        assign ref_xi[g] = ref_i.cic_i[g];
        assign ref_xq[g] = ref_i.cic_q[g];
    end
`endif
    always @(posedge clk) begin
        // (Before its first clock the reference's strobe register is
        // unknown in a simulator.)
        if (rx_tol > 0 && (ref_x === 1'b0 || ref_x === 1'b1)) begin
            if (dut.fs_x !== ref_x) begin
                if (errors < 10) $display("MISMATCH t=%0t: CIC strobe new %b ref %b", $time,
                                          dut.fs_x, ref_x);
                errors = errors + 1;
            end else if (dut.fs_x === 1'b1) begin
                for (int r = 0; r < NR; r++) begin
                    int di, dq;
                    di = $signed(16'(dut.fs_xr[16*r +: 16] - ref_xi[r]));
                    dq = $signed(16'(dut.fs_xi[16*r +: 16] - ref_xq[r]));
                    if (di < 0) di = -di;
                    if (dq < 0) dq = -dq;
                    cic_n = cic_n + 2;
                    if (di > 1) cic_far = cic_far + 1;
                    if (dq > 1) cic_far = cic_far + 1;
                    if (di > cic_dmax) cic_dmax = di;
                    if (dq > cic_dmax) cic_dmax = dq;
                end
            end
        end
    end
    int errors = 0, cycles = 0, rx_words = 0, rx_nonzero = 0, dac_nonzero = 0;
    int tx_clocks = 0, cw_clocks = 0, acks = 0;
    always @(negedge clk) begin
        logic [23:0] ra, rb;
        cycles = cycles + 1;
        ra = a_rx_tdata;
        rb = b_rx_tdata;
        if (rx_tol > 0 && a_rx_tvalid === 1'b1 && b_rx_tvalid === 1'b1 &&
            !$isunknown(ra) && !$isunknown(rb)) begin
            int d;
            d = $signed(ra - rb);
            if (d < 0) d = -d;
            rx_dse = rx_dse + real'(d) * real'(d);
            if (d > rx_dmax) rx_dmax = d;
            if (d > rx_tol) rx_far = rx_far + 1;
            rb = ra;
        end
        if ({ra, a_rx_tlast, a_rx_tvalid, a_rx_tuser, (tx_new ? b_dac : a_dac), a_tx_on, a_cw_on,
             a_cw_profile, a_tx_tready, a_tx_twait, a_cmd_ack, a_lr_tready} !==
            {rb, b_rx_tlast, b_rx_tvalid, b_rx_tuser, b_dac, b_tx_on, b_cw_on,
             b_cw_profile, b_tx_tready, b_tx_twait, b_cmd_ack, b_lr_tready}) begin
            if (errors < 10)
                $display("MISMATCH t=%0t: new rx %h %b%b dac %h on %b%b rdy %b ack %b, ref rx %h %b%b dac %h on %b%b rdy %b ack %b",
                         $time, a_rx_tdata, a_rx_tvalid, a_rx_tlast, a_dac, a_tx_on, a_cw_on,
                         a_tx_tready, a_cmd_ack, b_rx_tdata, b_rx_tvalid, b_rx_tlast, b_dac,
                         b_tx_on, b_cw_on, b_tx_tready, b_cmd_ack);
            errors = errors + 1;
        end
        if (a_rx_tvalid) begin
            rx_words = rx_words + 1;
            if (a_rx_tdata !== 24'd0 && !$isunknown(a_rx_tdata)) rx_nonzero = rx_nonzero + 1;
        end
        if (a_dac !== 12'd0 && !$isunknown(a_dac)) dac_nonzero = dac_nonzero + 1;
        if (a_tx_on === 1'b1) tx_clocks = tx_clocks + 1;
        if (a_cw_on === 1'b1) cw_clocks = cw_clocks + 1;
        if (a_cmd_ack === 1'b1) acks = acks + 1;
    end

    // Free-running stimulus: ADC samples (a tone near 7.1 MHz and noise),
    // the quarter-millisecond pulse, the sender's ready, transmit samples.
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
        // The transmit FIFO: a new random sample each time the core takes
        // one, and now and then empty.
        if (tx_tvalid && b_tx_tready) tx_tdata <= {lfsr[20:5], lfsr[31:16]};
        tx_tvalid <= lfsr[17:15] != 3'd0;
    end

    // One command, held until the next, as the SPI block presents them.
    task automatic cmd(input logic [5:0] addr, input logic [31:0] data);
        @(posedge clk);
        #1;
        cmd_addr = addr;
        cmd_data = data;
        cmd_rqst = 1'b1;
        @(posedge clk);
        #1;
        cmd_rqst = 1'b0;
        repeat (6) @(posedge clk);
        // Off the edge, so whatever the caller changes next (run, ptt) is
        // not set in the same time step as the clock both designs sample.
        #1;
    endtask

    function automatic logic [31:0] rnd();
        return $urandom;
    endfunction

    // Every address with random data (0x09 with the VNA bit clear).
    task automatic all_registers();
        // Address 0 (rate and receivers) is set by the caller.
        for (int k = 1; k < 64; k++) begin
            logic [31:0] d;
            d = rnd();
            if (k == 'h09) d[23] = 1'b0;
            if (k == 'h0a && tx_new) d[22] = 1'b0;  // PureSignal off
            if (k == 'h17) d[6:0] = {4'd0, d[2:0]};  // short TX latency for the test
            if (k == 'h10) d[31:24] = {4'd0, d[27:24]};  // short CW hang time
            cmd(6'(k), d);
        end
    endtask

    // Tune the transmitter and every receiver address.
    task automatic tune();
        int addrs[] = '{'h01, 'h02, 'h03, 'h04, 'h05, 'h06, 'h07, 'h08,
                        'h12, 'h13, 'h14, 'h15, 'h16};
        foreach (addrs[k]) cmd(6'(addrs[k]), 32'd1000000 + (rnd() % 30000000));
    endtask

    task automatic wait_clocks(input int n);
        repeat (n) @(posedge clk);
        #1;
    endtask

    // The phase words have no initial value in the source; the FPGA's
    // registers power up at zero. Unset, a simulator's X would reach the
    // mixers' and the transmit CORDIC's phase accumulators, which are never
    // reset, and stay there. Both designs alike, at time zero.
    initial begin
        for (int k = 0; k < 10; k++) begin
            dut.rx_phase[k] = 32'd0;
            ref_i.rx_phase[k] = 32'd0;
        end
        dut.tx_phase0 = 32'd0;
        ref_i.tx_phase0 = 32'd0;
    end

    // Likewise the mixers' pipeline registers and the CIC stages' output
    // registers: their first values are X, and the CIC integrators, which
    // are never reset, keep any X they add. The first CIC's integrators add
    // on every clock; everything after them moves only on strobes, held off
    // while run is low. So once the mixers give valid samples, and before
    // run rises, every CIC register is set to zero in both designs.
`define ZERO_STAGE(C, I) \
    C.cic_stages[I].cic_integrator_inst.out_data = '0; \
    C.cic_stages[I].cic_comb_inst.out_data = '0; \
    C.cic_stages[I].cic_comb_inst.prev_data = '0;
`define ZERO_CIC3(C) `ZERO_STAGE(C, 0) `ZERO_STAGE(C, 1) `ZERO_STAGE(C, 2)
`define ZERO_CIC5(C) `ZERO_CIC3(C) `ZERO_STAGE(C, 3) `ZERO_STAGE(C, 4)
`define ZERO_CIC(R) \
    `ZERO_CIC3(R.cic_inst_I2) `ZERO_CIC3(R.cic_inst_Q2) \
    `ZERO_CIC5(R.varcic_inst_I1) `ZERO_CIC5(R.varcic_inst_Q1)
    // The design under test: each receiver's first CIC and integrators
    // (rx_cic_int, its own registers); its shared combs' memory starts at
    // zero (fir_sdpram with FIR_SHARED_SIM).
`define ZERO_INT(R) \
    R.ci1 = '0; R.ci2 = '0; R.ci3 = '0; R.cq1 = '0; R.cq2 = '0; R.cq3 = '0; \
    R.di1 = '0; R.di2 = '0; R.di3 = '0; R.dq1 = '0; R.dq2 = '0; R.dq3 = '0; \
    R.pi1 = '0; R.pi2 = '0; R.pi3 = '0; R.pq1 = '0; R.pq2 = '0; R.pq3 = '0; \
    R.i1 = '0; R.i2 = '0; R.i3 = '0; R.i4 = '0; R.i5 = '0; \
    R.q1 = '0; R.q2 = '0; R.q3 = '0; R.q4 = '0; R.q5 = '0;
    task automatic zero_cic();
        `ZERO_INT(dut.receiver_0)
        `ZERO_INT(dut.RECEIVER1.receiver_1)
        `ZERO_INT(dut.RECEIVER2_g.receiver_2)
        `ZERO_INT(dut.RECEIVER3.receiver_3)
        `ZERO_INT(dut.RECEIVER4.receiver_4)
        `ZERO_INT(dut.RECEIVER5.receiver_5)
        // A reference from 19c1441 on has the same receivers as the design
        // (tools/sim.sh defines REF_PRUNED for it); an earlier one has the
        // Hermes-Lite 2 CICs.
`ifdef REF_PRUNED
        `ZERO_INT(ref_i.receiver_0)
        `ZERO_INT(ref_i.RECEIVER1.receiver_1)
        `ZERO_INT(ref_i.RECEIVER2_g.receiver_2)
        `ZERO_INT(ref_i.RECEIVER3.receiver_3)
        `ZERO_INT(ref_i.RECEIVER4.receiver_4)
        `ZERO_INT(ref_i.RECEIVER5.receiver_5)
`else
        `ZERO_CIC(ref_i.receiver_0)
        `ZERO_CIC(ref_i.RECEIVER1.receiver_1)
        `ZERO_CIC(ref_i.RECEIVER2_g.receiver_2)
        `ZERO_CIC(ref_i.RECEIVER3.receiver_3)
        `ZERO_CIC(ref_i.RECEIVER4.receiver_4)
        `ZERO_CIC(ref_i.RECEIVER5.receiver_5)
`endif
    endtask

    int phase_clocks;
    initial begin
        if (!$value$plusargs("PHASE=%d", phase_clocks)) phase_clocks = 120000;
        wait_clocks(20);
        // General register: 384 ksps, one receiver, no duplex, then the rest.
        cmd(6'h00, {6'd0, 2'd3, 16'd0, 1'b0, 4'd0, 1'b0, 2'b00});
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
                cmd(6'h00, d);
                tune();
                wait_clocks(phase_clocks / NR);
            end
            // Registers changed, run dropped and raised, a sender reset.
            all_registers();
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
        $display("%0d clocks, %0d receive words (%0d not zero), %0d clocks transmitting (%0d CW), %0d DAC samples not zero, %0d acks, %0d mismatches",
                 cycles, rx_words, rx_nonzero, tx_clocks, cw_clocks, dac_nonzero, acks, errors);
        if (rx_tol > 0) begin
            $display("CIC outputs against the reference: %0d values, %0d more than one LSB apart, largest %0d",
                     cic_n, cic_far, cic_dmax);
            $display("receive words against the reference: rms difference %f, largest %0d, %0d further apart than %0d",
                     $sqrt(rx_dse / rx_words), rx_dmax, rx_far, rx_tol);
        end
        if (errors == 0 && rx_nonzero > 1000 && dac_nonzero > 1000 && cw_clocks > 1000 &&
            (rx_tol == 0 || (cic_n > 1000 && cic_far * 50 <= cic_n)))
            $display("PASS");
        else $display("FAIL");
        $finish;
    end
endmodule
