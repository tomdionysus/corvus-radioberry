// SPDX-License-Identifier: GPL-2.0-or-later
//
// A simple dual-port RAM: one write port, one read port with two
// registers (the memory blocks' address and output registers), one clock,
// in the form synthesis tools document for inferring a memory block with
// registered output. The shared receive filters (firshared.sv, firsharedfold.sv) keep
// their sample memories in these: Quartus did not infer a memory declared
// directly inside their generate loops and built it from registers.
// A read of the address being written returns the old word or the new;
// the filters never do one.
module fir_sdpram #(
    parameter int W = 32,       // word width
    parameter int D = 768,      // words
    parameter int AW = 10       // address width, 2^AW >= D
) (
    input  logic          clk,
    input  logic          we,
    input  logic [AW-1:0] waddr,
    input  logic [W-1:0]  wdata,
    input  logic [AW-1:0] raddr,
    output logic [W-1:0]  q             // two cycles after raddr
);
    logic [W-1:0] mem [0:D-1];
`ifdef FIR_SHARED_SIM
    // The FPGA's memory blocks power up at zero; a simulator needs telling.
    initial for (int i = 0; i < D; i++) mem[i] = '0;
`endif
    logic [W-1:0] q_r;
    always_ff @(posedge clk) begin
        if (we) mem[waddr] <= wdata;
        q_r <= mem[raddr];
        q   <= q_r;
    end
endmodule
