// Top level of the AV box: HDMI in -> NTSC/RGB out.
module av_top (
    input  wire       clk,       // recovered pixel clock (54 MHz)
    input  wire       rst,
    input  wire       ypbpr,     // slide switch
    input  wire [9:0] raw0, raw1, raw2,
    output wire [7:0] comp, luma, chroma,
    output wire [5:0] ro, go, bo,
    output wire       csync_n,
    output wire       locked,
    output wire       de_o, hs_o, vs_o,   // for the testbench
    output wire       field2
);
    wire [7:0] r, g, b;
    hdmi_rx  rx  (clk, rst, raw0, raw1, raw2, de_o, hs_o, vs_o, r, g, b, locked);
    ntsc_enc enc (clk, rst, ypbpr, de_o, hs_o, vs_o, r, g, b,
                  comp, luma, chroma, ro, go, bo, csync_n, field2);
endmodule
