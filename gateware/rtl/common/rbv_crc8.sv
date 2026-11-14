// SPDX-License-Identifier: GPL-2.0-or-later
//
// CRC-8, polynomial 0x07, initial value 0, one bit at a time, most
// significant bit first. The driver computes the same over whole bytes.
package rbv_crc8;
    function automatic logic [7:0] step(input logic [7:0] crc, input logic bit_in);
        logic fb;
        fb = crc[7] ^ bit_in;
        return {crc[6:0], 1'b0} ^ (fb ? 8'h07 : 8'h00);
    endfunction
endpackage
