// lnv42, Jan 2020
// HF FSK reader (used for iso15)

// output is the frequence divider from 13,56 MHz

// (eg. for iso 15 two subcarriers mode (423,75 khz && 484,28 khz): it return 32 or 28)
// (423,75k = 13.56M / 32 and 484.28k = 13,56M / 28)

module hi_read_fsk(
    ck_1356meg,
    pwr_lo, pwr_hi, pwr_oe1, pwr_oe2, pwr_oe3, pwr_oe4,
    adc_d, adc_clk,
    ssp_frame, ssp_din, ssp_clk,
    output_frequency
);

    input ck_1356meg;
    output pwr_lo, pwr_hi, pwr_oe1, pwr_oe2, pwr_oe3, pwr_oe4;
    input [7:0] adc_d;
    output adc_clk;
    output ssp_frame, ssp_din, ssp_clk;
    input [1:0] output_frequency;

assign adc_clk = ck_1356meg;  // input sample frequency is 13,56 MHz

// Carrier is steady on through this, unless we're snooping.
assign pwr_hi  = ck_1356meg;
assign pwr_oe4 = 1'b0;
assign pwr_oe1 = 1'b0;
assign pwr_oe3 = 1'b0;

reg [7:0] adc_cnt = 8'd0;
reg [7:0] out1 = 8'd0;
reg [7:0] old = 8'd0;
reg [7:0] edge_id = 8'd0;
reg edge_started = 1'd0;
// Count clock edge between two signal edges
always @(negedge adc_clk)
begin
    adc_cnt <= adc_cnt + 1'd1;

    if (adc_d > old && adc_d - old > 8'd24) // edge detected
    begin
        if (edge_started == 1'd0) // new edge starting
        begin
            if (edge_id <= adc_cnt)
                out1 <= adc_cnt - edge_id;
            else
                out1 <= adc_cnt + 9'h100 - edge_id;
            edge_id <= adc_cnt;
            edge_started = 1'd1;
        end
    end
    else
        edge_started = 1'd0;
    old <= adc_d;
end

// agregate out values (depending on selected output frequency)
reg [10:0] out_tmp = 11'd0;
reg [7:0] out = 8'd0;
always @(adc_cnt)
begin
    out_tmp <= out_tmp + out1;
    if (output_frequency == `FPGA_HF_FSK_READER_OUTPUT_848_KHZ && adc_cnt[0] == 1'd0)
    begin // average on 2 values
        out <= out_tmp[8:1];
        out_tmp <= 11'd0;
    end
    else if (output_frequency == `FPGA_HF_FSK_READER_OUTPUT_424_KHZ && adc_cnt[1:0] == 2'd0)
    begin // average on 4 values
        out <= out_tmp[9:2];
        out_tmp <= 11'd0;
    end
    else if (output_frequency == `FPGA_HF_FSK_READER_OUTPUT_212_KHZ && adc_cnt[2:0] == 3'd0)
    begin // average on 8 values
        out <= out_tmp[10:3];
        out_tmp <= 11'd0;
    end
    else
        out <= out1;

    if (adc_cnt > 8'd192 && edge_id < 8'd64)
    begin
        out <= 8'd0;
        out_tmp <= 11'd0;
    end
end

   
// Set output (ssp) clock
(* clock_signal = "yes" *) reg ssp_clk;
always @(ck_1356meg)
begin
    if (output_frequency == `FPGA_HF_FSK_READER_OUTPUT_1695_KHZ)
        ssp_clk <= ~ck_1356meg;
    else if (output_frequency == `FPGA_HF_FSK_READER_OUTPUT_848_KHZ)
        ssp_clk <= ~adc_cnt[0];
    else if (output_frequency == `FPGA_HF_FSK_READER_OUTPUT_424_KHZ)
        ssp_clk <= ~adc_cnt[1];
    else                                           // 212 KHz
        ssp_clk <= ~adc_cnt[2];
end

// Transmit output
reg ssp_frame;
reg [7:0] ssp_out = 8'd0;
reg [2:0] ssp_cnt = 3'd0;
always @(posedge ssp_clk)
begin
    ssp_cnt <= ssp_cnt + 1'd1;
    if(ssp_cnt == 3'd7)
    begin
        ssp_out <= out;
        ssp_frame <= 1'b1;
    end
    else
    begin
        ssp_out <= {ssp_out[6:0], 1'b0};
        ssp_frame <= 1'b0;
    end
end

assign ssp_din = ssp_out[7];


// Unused.
assign pwr_lo = 1'b0;
assign pwr_oe2 = 1'b0;

endmodule

