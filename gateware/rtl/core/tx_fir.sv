// SPDX-License-Identifier: GPL-2.0-or-later
//
// The transmit interpolating filter: 48 ksps I/Q in, eight outputs for
// each input sample (384 ksps), a 1024-tap low-pass in polyphase form. It
// replaces the Hermes-Lite 2 core's FirInterp8_1024.v (James Ahlstrom
// N2ADR, Phil Harman VK6PH) with the same outputs on the same clocks (the
// images' build of it: its memories read in two clocks, address and output
// registered; TODO/2026-11-17-core-takeover.md stage 6a), and its
// coefficients (coefI8_1024).
//
// FirInterp8_1024.v, as here: the input samples are kept in a 128-word
// memory, sign extended to 18 bits. On calc the filter computes one output:
// from the newest sample back, each sample times coefficient phase + 8j
// (j = 0 to 127; phase counts the outputs, 0 to 7), the 36-bit products
// rounded to 24 bits by their bit 11 and summed; the output is the sum's
// bits 20..1 plus bit 1. After the eighth output the next input sample is
// written and requested.
//
// The difference: the coefficients are symmetric (coefficient k equals
// coefficient 1023 - k, checked against coefI8_1024.txt), so only the
// first 512 are stored, in one memory block instead of two: address k
// reads k, or 1023 - k (~k) from 512 on.
module tx_fir (
    input  logic               clock,                   // 76.8 MHz
    input  logic               calc,                    // compute an output sample
    output logic               req = 1'b0,              // the next input sample, please
    input  logic signed [15:0] x_real,
    input  logic signed [15:0] x_imag,
    output logic signed [19:0] y_real = '0,
    output logic signed [19:0] y_imag = '0
);
    typedef enum logic [3:0] {
        S_WAIT, S_ADDR, S_ADDR_A, S_ADDR_B, S_RUN, S_DONE, S_END1, S_END2, S_END3, S_END4
    } state_t;
    state_t state = S_WAIT;

    logic [6:0]         waddr = '0, raddr = '0;
    logic               we = 1'b0;
    logic signed [23:0] racc = '0, iacc = '0;
    logic [35:0]        q;
    logic [35:0]        reg_q = '0;
    logic signed [17:0] q_real, q_imag;
    assign q_real = reg_q[35:18];
    assign q_imag = reg_q[17:0];
    logic [9:0]         caddr = '0;
    logic signed [17:0] coef;
    logic signed [17:0] reg_coef = '0;
    logic signed [35:0] rmult = '0, imult = '0;
    logic [2:0]         phase = '0;
    logic [9:0]         counter = '0;

    // The sample memory: 18-bit I and Q, sign extended from 16.
    fir_sdpram #(.W(36), .D(128), .AW(7)) ram (
        .clk(clock), .we(we), .waddr(waddr),
        .wdata({{2{x_real[15]}}, x_real, {2{x_imag[15]}}, x_imag}),
        .raddr(raddr), .q(q)
    );

    // The coefficient memory, read in two clocks as the original's.
    logic [17:0] rom [0:511];
    logic [8:0]  rom_a = '0;
    logic [17:0] rom_q = '0;
    assign coef = rom_q;
    always_ff @(posedge clock) begin
        rom_a <= caddr[9] ? ~caddr[8:0] : caddr[8:0];
        rom_q <= rom[rom_a];
    end

    always_ff @(posedge clock) begin
        case (state)
            S_WAIT: begin
                if (calc) begin
                    state   <= S_ADDR;
                    raddr   <= waddr;           // the newest sample
                    caddr   <= {7'd0, phase};   // the first coefficient
                    counter <= 10'd129;         // 128 samples and the pipeline
                    racc    <= '0;
                    iacc    <= '0;
                    rmult   <= '0;
                    imult   <= '0;
                end
            end
            S_ADDR, S_ADDR_A, S_ADDR_B, S_RUN: begin
                // The next sample back and the next coefficient of this phase.
                raddr    <= raddr - 7'd1;
                caddr    <= caddr + 10'd8;
                reg_q    <= q;
                reg_coef <= coef;
                case (state)
                    S_ADDR:   state <= S_ADDR_A;
                    S_ADDR_A: state <= S_ADDR_B;
                    S_ADDR_B: state <= S_RUN;
                    default: begin
                        rmult   <= q_real * reg_coef;
                        imult   <= q_imag * reg_coef;
                        racc    <= racc + rmult[35:12] + 24'(rmult[11]);
                        iacc    <= iacc + imult[35:12] + 24'(imult[11]);
                        counter <= counter - 10'd1;
                        if (counter == 10'd0) state <= S_DONE;
                    end
                endcase
            end
            S_DONE: begin
                y_real <= racc[20:1] + 20'(racc[1]);
                y_imag <= iacc[20:1] + 20'(iacc[1]);
                state  <= (phase == 3'd7) ? S_END1 : S_WAIT;
                phase  <= phase + 3'd1;
            end
            S_END1: begin       // the last output for this input sample
                state <= S_END2;
                waddr <= waddr + 7'd1;
            end
            S_END2: begin       // write the waiting sample at the new address
                state <= S_END3;
                we    <= 1'b1;
            end
            S_END3: begin
                state <= S_END4;
                we    <= 1'b0;
                req   <= 1'b1;
            end
            S_END4: begin
                state <= S_WAIT;
                req   <= 1'b0;
            end
            default: state <= S_WAIT;
        endcase
    end

    // Coefficients 0 to 511 of coefI8_1024 (18-bit two's complement).
    initial begin
        rom[0] = 18'h3fff5; rom[1] = 18'h3fff6; rom[2] = 18'h3fff8; rom[3] = 18'h3fffa;
        rom[4] = 18'h3fffd; rom[5] = 18'h00001; rom[6] = 18'h00004; rom[7] = 18'h00007;
        rom[8] = 18'h00009; rom[9] = 18'h0000b; rom[10] = 18'h0000b; rom[11] = 18'h0000b;
        rom[12] = 18'h00009; rom[13] = 18'h00007; rom[14] = 18'h00004; rom[15] = 18'h00001;
        rom[16] = 18'h3fffd; rom[17] = 18'h3fffa; rom[18] = 18'h3fff8; rom[19] = 18'h3fff6;
        rom[20] = 18'h3fff5; rom[21] = 18'h3fff5; rom[22] = 18'h3fff6; rom[23] = 18'h3fff8;
        rom[24] = 18'h3fffa; rom[25] = 18'h3fffd; rom[26] = 18'h00001; rom[27] = 18'h00004;
        rom[28] = 18'h00007; rom[29] = 18'h0000a; rom[30] = 18'h0000b; rom[31] = 18'h0000c;
        rom[32] = 18'h0000b; rom[33] = 18'h0000a; rom[34] = 18'h00007; rom[35] = 18'h00004;
        rom[36] = 18'h00001; rom[37] = 18'h3fffd; rom[38] = 18'h3fffa; rom[39] = 18'h3fff7;
        rom[40] = 18'h3fff5; rom[41] = 18'h3fff4; rom[42] = 18'h3fff4; rom[43] = 18'h3fff5;
        rom[44] = 18'h3fff7; rom[45] = 18'h3fffa; rom[46] = 18'h3fffd; rom[47] = 18'h00001;
        rom[48] = 18'h00004; rom[49] = 18'h00008; rom[50] = 18'h0000a; rom[51] = 18'h0000b;
        rom[52] = 18'h0000c; rom[53] = 18'h0000b; rom[54] = 18'h0000a; rom[55] = 18'h00007;
        rom[56] = 18'h00004; rom[57] = 18'h00001; rom[58] = 18'h3fffd; rom[59] = 18'h3fffa;
        rom[60] = 18'h3fff7; rom[61] = 18'h3fff5; rom[62] = 18'h3fff4; rom[63] = 18'h3fff4;
        rom[64] = 18'h3fff5; rom[65] = 18'h3fff7; rom[66] = 18'h3fffa; rom[67] = 18'h3fffd;
        rom[68] = 18'h00001; rom[69] = 18'h00005; rom[70] = 18'h00008; rom[71] = 18'h0000a;
        rom[72] = 18'h0000c; rom[73] = 18'h0000d; rom[74] = 18'h0000c; rom[75] = 18'h0000b;
        rom[76] = 18'h00008; rom[77] = 18'h00005; rom[78] = 18'h00001; rom[79] = 18'h3fffd;
        rom[80] = 18'h3fff9; rom[81] = 18'h3fff6; rom[82] = 18'h3fff4; rom[83] = 18'h3fff3;
        rom[84] = 18'h3fff3; rom[85] = 18'h3fff4; rom[86] = 18'h3fff6; rom[87] = 18'h3fff9;
        rom[88] = 18'h3fffd; rom[89] = 18'h00001; rom[90] = 18'h00005; rom[91] = 18'h00008;
        rom[92] = 18'h0000b; rom[93] = 18'h0000d; rom[94] = 18'h0000e; rom[95] = 18'h0000d;
        rom[96] = 18'h0000b; rom[97] = 18'h00009; rom[98] = 18'h00005; rom[99] = 18'h00001;
        rom[100] = 18'h3fffd; rom[101] = 18'h3fff9; rom[102] = 18'h3fff6; rom[103] = 18'h3fff3;
        rom[104] = 18'h3fff2; rom[105] = 18'h3fff2; rom[106] = 18'h3fff3; rom[107] = 18'h3fff5;
        rom[108] = 18'h3fff9; rom[109] = 18'h3fffd; rom[110] = 18'h00001; rom[111] = 18'h00005;
        rom[112] = 18'h00009; rom[113] = 18'h0000c; rom[114] = 18'h0000e; rom[115] = 18'h0000f;
        rom[116] = 18'h0000e; rom[117] = 18'h0000d; rom[118] = 18'h0000a; rom[119] = 18'h00006;
        rom[120] = 18'h00001; rom[121] = 18'h3fffd; rom[122] = 18'h3fff8; rom[123] = 18'h3fff5;
        rom[124] = 18'h3fff2; rom[125] = 18'h3fff0; rom[126] = 18'h3fff0; rom[127] = 18'h3fff1;
        rom[128] = 18'h3fff4; rom[129] = 18'h3fff8; rom[130] = 18'h3fffc; rom[131] = 18'h00001;
        rom[132] = 18'h00006; rom[133] = 18'h0000a; rom[134] = 18'h0000e; rom[135] = 18'h00010;
        rom[136] = 18'h00011; rom[137] = 18'h00010; rom[138] = 18'h0000e; rom[139] = 18'h0000b;
        rom[140] = 18'h00007; rom[141] = 18'h00002; rom[142] = 18'h3fffd; rom[143] = 18'h3fff8;
        rom[144] = 18'h3fff3; rom[145] = 18'h3fff0; rom[146] = 18'h3ffee; rom[147] = 18'h3ffee;
        rom[148] = 18'h3fff0; rom[149] = 18'h3fff2; rom[150] = 18'h3fff6; rom[151] = 18'h3fffb;
        rom[152] = 18'h00001; rom[153] = 18'h00006; rom[154] = 18'h0000b; rom[155] = 18'h0000f;
        rom[156] = 18'h00012; rom[157] = 18'h00013; rom[158] = 18'h00013; rom[159] = 18'h00010;
        rom[160] = 18'h0000d; rom[161] = 18'h00008; rom[162] = 18'h00002; rom[163] = 18'h3fffd;
        rom[164] = 18'h3fff7; rom[165] = 18'h3fff2; rom[166] = 18'h3ffee; rom[167] = 18'h3ffec;
        rom[168] = 18'h3ffec; rom[169] = 18'h3ffed; rom[170] = 18'h3fff0; rom[171] = 18'h3fff4;
        rom[172] = 18'h3fffa; rom[173] = 18'h00000; rom[174] = 18'h00007; rom[175] = 18'h0000c;
        rom[176] = 18'h00011; rom[177] = 18'h00015; rom[178] = 18'h00016; rom[179] = 18'h00016;
        rom[180] = 18'h00013; rom[181] = 18'h0000f; rom[182] = 18'h0000a; rom[183] = 18'h00003;
        rom[184] = 18'h3fffc; rom[185] = 18'h3fff6; rom[186] = 18'h3fff0; rom[187] = 18'h3ffec;
        rom[188] = 18'h3ffe9; rom[189] = 18'h3ffe8; rom[190] = 18'h3ffe9; rom[191] = 18'h3ffed;
        rom[192] = 18'h3fff2; rom[193] = 18'h3fff8; rom[194] = 18'h00000; rom[195] = 18'h00007;
        rom[196] = 18'h0000e; rom[197] = 18'h00014; rom[198] = 18'h00018; rom[199] = 18'h0001a;
        rom[200] = 18'h0001a; rom[201] = 18'h00017; rom[202] = 18'h00013; rom[203] = 18'h0000c;
        rom[204] = 18'h00005; rom[205] = 18'h3fffd; rom[206] = 18'h3fff5; rom[207] = 18'h3ffee;
        rom[208] = 18'h3ffe8; rom[209] = 18'h3ffe5; rom[210] = 18'h3ffe3; rom[211] = 18'h3ffe5;
        rom[212] = 18'h3ffe9; rom[213] = 18'h3ffef; rom[214] = 18'h3fff6; rom[215] = 18'h3ffff;
        rom[216] = 18'h00008; rom[217] = 18'h00010; rom[218] = 18'h00017; rom[219] = 18'h0001c;
        rom[220] = 18'h0001f; rom[221] = 18'h0001f; rom[222] = 18'h0001c; rom[223] = 18'h00017;
        rom[224] = 18'h00010; rom[225] = 18'h00007; rom[226] = 18'h3fffd; rom[227] = 18'h3fff3;
        rom[228] = 18'h3ffea; rom[229] = 18'h3ffe3; rom[230] = 18'h3ffdf; rom[231] = 18'h3ffdd;
        rom[232] = 18'h3ffde; rom[233] = 18'h3ffe3; rom[234] = 18'h3ffea; rom[235] = 18'h3fff3;
        rom[236] = 18'h3fffd; rom[237] = 18'h00008; rom[238] = 18'h00013; rom[239] = 18'h0001c;
        rom[240] = 18'h00022; rom[241] = 18'h00026; rom[242] = 18'h00027; rom[243] = 18'h00024;
        rom[244] = 18'h0001e; rom[245] = 18'h00015; rom[246] = 18'h0000a; rom[247] = 18'h3fffe;
        rom[248] = 18'h3fff1; rom[249] = 18'h3ffe6; rom[250] = 18'h3ffdd; rom[251] = 18'h3ffd7;
        rom[252] = 18'h3ffd4; rom[253] = 18'h3ffd6; rom[254] = 18'h3ffda; rom[255] = 18'h3ffe3;
        rom[256] = 18'h3ffee; rom[257] = 18'h3fffb; rom[258] = 18'h00009; rom[259] = 18'h00016;
        rom[260] = 18'h00022; rom[261] = 18'h0002b; rom[262] = 18'h00030; rom[263] = 18'h00031;
        rom[264] = 18'h0002e; rom[265] = 18'h00027; rom[266] = 18'h0001c; rom[267] = 18'h0000e;
        rom[268] = 18'h3ffff; rom[269] = 18'h3ffef; rom[270] = 18'h3ffe1; rom[271] = 18'h3ffd5;
        rom[272] = 18'h3ffcc; rom[273] = 18'h3ffc8; rom[274] = 18'h3ffc9; rom[275] = 18'h3ffce;
        rom[276] = 18'h3ffd9; rom[277] = 18'h3ffe7; rom[278] = 18'h3fff7; rom[279] = 18'h00009;
        rom[280] = 18'h0001b; rom[281] = 18'h0002a; rom[282] = 18'h00037; rom[283] = 18'h0003e;
        rom[284] = 18'h00041; rom[285] = 18'h0003e; rom[286] = 18'h00035; rom[287] = 18'h00027;
        rom[288] = 18'h00015; rom[289] = 18'h00001; rom[290] = 18'h3ffec; rom[291] = 18'h3ffd9;
        rom[292] = 18'h3ffc8; rom[293] = 18'h3ffbc; rom[294] = 18'h3ffb5; rom[295] = 18'h3ffb5;
        rom[296] = 18'h3ffbc; rom[297] = 18'h3ffc9; rom[298] = 18'h3ffdb; rom[299] = 18'h3fff1;
        rom[300] = 18'h00009; rom[301] = 18'h00021; rom[302] = 18'h00037; rom[303] = 18'h00049;
        rom[304] = 18'h00054; rom[305] = 18'h00059; rom[306] = 18'h00055; rom[307] = 18'h0004a;
        rom[308] = 18'h00038; rom[309] = 18'h00020; rom[310] = 18'h00005; rom[311] = 18'h3ffe8;
        rom[312] = 18'h3ffcc; rom[313] = 18'h3ffb4; rom[314] = 18'h3ffa2; rom[315] = 18'h3ff98;
        rom[316] = 18'h3ff97; rom[317] = 18'h3ff9f; rom[318] = 18'h3ffb0; rom[319] = 18'h3ffc9;
        rom[320] = 18'h3ffe8; rom[321] = 18'h0000a; rom[322] = 18'h0002c; rom[323] = 18'h0004c;
        rom[324] = 18'h00066; rom[325] = 18'h00078; rom[326] = 18'h00080; rom[327] = 18'h0007c;
        rom[328] = 18'h0006d; rom[329] = 18'h00054; rom[330] = 18'h00032; rom[331] = 18'h0000a;
        rom[332] = 18'h3ffe0; rom[333] = 18'h3ffb7; rom[334] = 18'h3ff92; rom[335] = 18'h3ff77;
        rom[336] = 18'h3ff66; rom[337] = 18'h3ff63; rom[338] = 18'h3ff6e; rom[339] = 18'h3ff87;
        rom[340] = 18'h3ffac; rom[341] = 18'h3ffda; rom[342] = 18'h0000e; rom[343] = 18'h00042;
        rom[344] = 18'h00073; rom[345] = 18'h0009c; rom[346] = 18'h000b8; rom[347] = 18'h000c5;
        rom[348] = 18'h000c0; rom[349] = 18'h000a9; rom[350] = 18'h00081; rom[351] = 18'h0004c;
        rom[352] = 18'h0000d; rom[353] = 18'h3ffca; rom[354] = 18'h3ff89; rom[355] = 18'h3ff4f;
        rom[356] = 18'h3ff24; rom[357] = 18'h3ff0a; rom[358] = 18'h3ff06; rom[359] = 18'h3ff1a;
        rom[360] = 18'h3ff43; rom[361] = 18'h3ff81; rom[362] = 18'h3ffcd; rom[363] = 18'h00022;
        rom[364] = 18'h00079; rom[365] = 18'h000c8; rom[366] = 18'h00108; rom[367] = 18'h00133;
        rom[368] = 18'h00143; rom[369] = 18'h00136; rom[370] = 18'h0010a; rom[371] = 18'h000c3;
        rom[372] = 18'h00065; rom[373] = 18'h3fff9; rom[374] = 18'h3ff88; rom[375] = 18'h3ff1c;
        rom[376] = 18'h3fec0; rom[377] = 18'h3fe7e; rom[378] = 18'h3fe5d; rom[379] = 18'h3fe62;
        rom[380] = 18'h3fe90; rom[381] = 18'h3fee3; rom[382] = 18'h3ff57; rom[383] = 18'h3ffe2;
        rom[384] = 18'h00077; rom[385] = 18'h00109; rom[386] = 18'h00189; rom[387] = 18'h001ea;
        rom[388] = 18'h00221; rom[389] = 18'h00226; rom[390] = 18'h001f5; rom[391] = 18'h00191;
        rom[392] = 18'h00100; rom[393] = 18'h0004d; rom[394] = 18'h3ff88; rom[395] = 18'h3fec4;
        rom[396] = 18'h3fe14; rom[397] = 18'h3fd8b; rom[398] = 18'h3fd38; rom[399] = 18'h3fd27;
        rom[400] = 18'h3fd5d; rom[401] = 18'h3fdd9; rom[402] = 18'h3fe94; rom[403] = 18'h3ff7d;
        rom[404] = 18'h00081; rom[405] = 18'h00187; rom[406] = 18'h00274; rom[407] = 18'h00331;
        rom[408] = 18'h003a6; rom[409] = 18'h003c4; rom[410] = 18'h00383; rom[411] = 18'h002e4;
        rom[412] = 18'h001f0; rom[413] = 18'h000bd; rom[414] = 18'h3ff64; rom[415] = 18'h3fe08;
        rom[416] = 18'h3fcca; rom[417] = 18'h3fbce; rom[418] = 18'h3fb30; rom[419] = 18'h3fb07;
        rom[420] = 18'h3fb5d; rom[421] = 18'h3fc32; rom[422] = 18'h3fd76; rom[423] = 18'h3ff0f;
        rom[424] = 18'h000d9; rom[425] = 18'h002a7; rom[426] = 18'h0044a; rom[427] = 18'h00594;
        rom[428] = 18'h0065e; rom[429] = 18'h0068c; rom[430] = 18'h0060f; rom[431] = 18'h004eb;
        rom[432] = 18'h00334; rom[433] = 18'h0010f; rom[434] = 18'h3feae; rom[435] = 18'h3fc4c;
        rom[436] = 18'h3fa2a; rom[437] = 18'h3f884; rom[438] = 18'h3f78c; rom[439] = 18'h3f768;
        rom[440] = 18'h3f826; rom[441] = 18'h3f9c2; rom[442] = 18'h3fc1c; rom[443] = 18'h3ff02;
        rom[444] = 18'h0022d; rom[445] = 18'h0054e; rom[446] = 18'h0080f; rom[447] = 18'h00a20;
        rom[448] = 18'h00b41; rom[449] = 18'h00b44; rom[450] = 18'h00a17; rom[451] = 18'h007c9;
        rom[452] = 18'h00485; rom[453] = 18'h00096; rom[454] = 18'h3fc5a; rom[455] = 18'h3f840;
        rom[456] = 18'h3f4b9; rom[457] = 18'h3f22c; rom[458] = 18'h3f0ee; rom[459] = 18'h3f136;
        rom[460] = 18'h3f315; rom[461] = 18'h3f670; rom[462] = 18'h3fb05; rom[463] = 18'h0006b;
        rom[464] = 18'h0061e; rom[465] = 18'h00b86; rom[466] = 18'h0100e; rom[467] = 18'h01329;
        rom[468] = 18'h0146d; rom[469] = 18'h01397; rom[470] = 18'h0109a; rom[471] = 18'h00ba2;
        rom[472] = 18'h00514; rom[473] = 18'h3fd85; rom[474] = 18'h3f5b1; rom[475] = 18'h3ee6a;
        rom[476] = 18'h3e881; rom[477] = 18'h3e4af; rom[478] = 18'h3e386; rom[479] = 18'h3e559;
        rom[480] = 18'h3ea32; rom[481] = 18'h3f1c9; rom[482] = 18'h3fb8a; rom[483] = 18'h00697;
        rom[484] = 18'h011df; rom[485] = 18'h01c34; rom[486] = 18'h02465; rom[487] = 18'h02962;
        rom[488] = 18'h02a55; rom[489] = 18'h026bd; rom[490] = 18'h01e87; rom[491] = 18'h01215;
        rom[492] = 18'h0023f; rom[493] = 18'h3f050; rom[494] = 18'h3ddea; rom[495] = 18'h3cced;
        rom[496] = 18'h3bf4e; rom[497] = 18'h3b6f3; rom[498] = 18'h3b580; rom[499] = 18'h3bc37;
        rom[500] = 18'h3cbd1; rom[501] = 18'h3e468; rom[502] = 18'h0056a; rom[503] = 18'h02d9b;
        rom[504] = 18'h05b25; rom[505] = 18'h08bb1; rom[506] = 18'h0bc95; rom[507] = 18'h0eafb;
        rom[508] = 18'h1141d; rom[509] = 18'h13575; rom[510] = 18'h14cef; rom[511] = 18'h1590e;
    end
endmodule
