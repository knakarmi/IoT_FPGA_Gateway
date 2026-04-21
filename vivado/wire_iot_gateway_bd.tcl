# =============================================================================
# wire_iot_gateway_bd.tcl  (v2 - with error handling)
# ELE598 IoT Gateway - Complete block design wiring script
# Skips connections that already exist instead of erroring out
# =============================================================================

puts "=== IoT Gateway BD Wiring Script v2 ==="

set connected 0
set skipped   0
set errors    0

# Safe net connect - skips if already connected
proc cn {args} {
    global connected skipped errors
    set pins {}
    foreach p $args {
        set pin [get_bd_pins -quiet $p]
        if {$pin eq ""} {
            puts "  WARN: pin not found: $p"
            incr errors
            return
        }
        lappend pins $pin
    }
    if {[catch {connect_bd_net {*}$pins} err]} {
        if {[string match "*already connected*" $err] || \
            [string match "*all ports/pins are already*" $err]} {
            incr skipped
        } else {
            puts "  ERR cn $args: $err"
            incr errors
        }
    } else {
        incr connected
    }
}

# Safe interface net connect - skips if already connected
proc ci {p1 p2} {
    global connected skipped errors
    set pin1 [get_bd_intf_pins -quiet $p1]
    set pin2 [get_bd_intf_pins -quiet $p2]
    if {$pin1 eq "" || $pin2 eq ""} {
        puts "  WARN: intf pin not found: $p1 or $p2"
        incr errors
        return
    }
    if {[catch {connect_bd_intf_net $pin1 $pin2} err]} {
        if {[string match "*already connected*" $err] || \
            [string match "*all ports/pins are already*" $err]} {
            incr skipped
        } else {
            puts "  ERR ci $p1 $p2: $err"
            incr errors
        }
    } else {
        incr connected
    }
}

# =============================================================================
# SECTION 1: CLOCKS AND RESETS
# =============================================================================
puts "\n--- Section 1: Clocks and Resets ---"

cn processing_system7_0/FCLK_CLK0      proc_sys_reset_0/slowest_sync_clk
cn processing_system7_0/FCLK_CLK1      proc_sys_reset_1/slowest_sync_clk
cn processing_system7_0/FCLK_RESET0_N  proc_sys_reset_0/ext_reset_in
cn processing_system7_0/FCLK_RESET0_N  proc_sys_reset_1/ext_reset_in
cn xlconstant_dcm/dout                  proc_sys_reset_0/dcm_locked
cn xlconstant_dcm/dout                  proc_sys_reset_1/dcm_locked

cn processing_system7_0/FCLK_CLK0      axi_dma_0/s_axi_lite_aclk
cn processing_system7_0/FCLK_CLK1      axi_dma_0/m_axi_mm2s_aclk
cn processing_system7_0/FCLK_CLK1      axi_dma_0/m_axi_s2mm_aclk
cn processing_system7_0/FCLK_CLK1      axi_dma_0/m_axi_sg_aclk
cn proc_sys_reset_1/peripheral_aresetn  axi_dma_0/axi_resetn

cn processing_system7_0/FCLK_CLK1      aes256gcm_0/clk_i
cn proc_sys_reset_1/peripheral_reset    aes256gcm_0/rst_i

cn processing_system7_0/FCLK_CLK0      axi_gpio_ctrl/s_axi_aclk
cn processing_system7_0/FCLK_CLK0      axi_gpio_key_val/s_axi_aclk
cn processing_system7_0/FCLK_CLK0      axi_gpio_status/s_axi_aclk
cn proc_sys_reset_0/peripheral_aresetn  axi_gpio_ctrl/s_axi_aresetn
cn proc_sys_reset_0/peripheral_aresetn  axi_gpio_key_val/s_axi_aresetn
cn proc_sys_reset_0/peripheral_aresetn  axi_gpio_status/s_axi_aresetn

cn processing_system7_0/FCLK_CLK1      smartconnect_0/aclk
cn processing_system7_0/FCLK_CLK0      smartconnect_1/aclk
cn proc_sys_reset_1/interconnect_aresetn  smartconnect_0/aresetn
cn proc_sys_reset_0/interconnect_aresetn  smartconnect_1/aresetn

cn processing_system7_0/FCLK_CLK1      processing_system7_0/S_AXI_HP0_ACLK
cn processing_system7_0/FCLK_CLK1      processing_system7_0/S_AXI_HP1_ACLK
cn processing_system7_0/FCLK_CLK0      processing_system7_0/M_AXI_GP0_ACLK

cn processing_system7_0/FCLK_CLK1      axis_dwidth_conv_0/aclk
cn processing_system7_0/FCLK_CLK1      axis_subset_converter_0/aclk
cn processing_system7_0/FCLK_CLK1      axis_data_fifo_0/s_axis_aclk
cn processing_system7_0/FCLK_CLK1      axis_switch_0/aclk
cn proc_sys_reset_1/peripheral_aresetn  axis_dwidth_conv_0/aresetn
cn proc_sys_reset_1/peripheral_aresetn  axis_subset_converter_0/aresetn
cn proc_sys_reset_1/peripheral_aresetn  axis_data_fifo_0/s_axis_aresetn
cn proc_sys_reset_1/peripheral_aresetn  axis_switch_0/aresetn

puts "  Clocks done"

# =============================================================================
# SECTION 2: AXI MEMORY BUS
# =============================================================================
puts "\n--- Section 2: AXI Memory Bus ---"

ci axi_dma_0/M_AXI_MM2S              smartconnect_0/S00_AXI
ci axi_dma_0/M_AXI_S2MM              smartconnect_0/S01_AXI
ci axi_dma_0/M_AXI_SG                smartconnect_0/S02_AXI
ci smartconnect_0/M00_AXI             processing_system7_0/S_AXI_HP0
ci smartconnect_0/M01_AXI             processing_system7_0/S_AXI_HP1

puts "  Memory bus done"

# =============================================================================
# SECTION 3: AXI CONTROL BUS
# =============================================================================
puts "\n--- Section 3: AXI Control Bus ---"

ci processing_system7_0/M_AXI_GP0    smartconnect_1/S00_AXI
ci smartconnect_1/M00_AXI             axi_dma_0/S_AXI_LITE
ci smartconnect_1/M01_AXI             axi_gpio_ctrl/S_AXI
ci smartconnect_1/M02_AXI             axi_gpio_key_val/S_AXI
ci smartconnect_1/M03_AXI             axi_gpio_status/S_AXI

puts "  Control bus done"

# =============================================================================
# SECTION 4: AXI-STREAM DATA PATH
# =============================================================================
puts "\n--- Section 4: AXI-Stream Data Path ---"

ci axi_dma_0/M_AXIS_MM2S              axis_dwidth_conv_0/S_AXIS

cn axis_dwidth_conv_0/m_axis_tdata    aes256gcm_0/aes_gcm_data_in_i
cn xlconstant_bval/dout               aes256gcm_0/aes_gcm_data_in_bval_i
cn xlconstant_one1/dout               axis_dwidth_conv_0/m_axis_tready

cn aes256gcm_0/aes_gcm_data_out_o       axis_data_fifo_0/s_axis_tdata
cn aes256gcm_0/aes_gcm_data_out_bval_o  axis_data_fifo_0/s_axis_tkeep
cn aes256gcm_0/aes_gcm_data_out_val_o   axis_data_fifo_0/s_axis_tvalid
cn aes256gcm_0/aes_gcm_ghash_tag_val_o  axis_data_fifo_0/s_axis_tlast

ci axis_data_fifo_0/M_AXIS              axis_subset_converter_0/S_AXIS
ci axis_subset_converter_0/M_AXIS       axis_switch_0/S00_AXIS
ci axis_switch_0/M00_AXIS               axi_dma_0/S_AXIS_S2MM

puts "  Data path done"

# =============================================================================
# SECTION 5: AES CONTROL SIGNALS
# =============================================================================
puts "\n--- Section 5: AES Control Signals ---"

cn axi_gpio_ctrl/gpio_io_o    aes256gcm_0/aes_gcm_enc_dec_i
cn axi_gpio_ctrl/gpio_io_o    aes256gcm_0/aes_gcm_ghash_pkt_val_i
cn axi_gpio_ctrl/gpio_io_o    aes256gcm_0/aes_gcm_icb_start_cnt_i
cn axi_gpio_ctrl/gpio_io_o    aes256gcm_0/aes_gcm_icb_stop_cnt_i
cn axi_gpio_ctrl/gpio_io_o    aes256gcm_0/aes_gcm_pipe_reset_i

cn axi_gpio_key_val/gpio_io_o  aes256gcm_0/aes_gcm_key_word_val_i
cn aes256gcm_0/aes_gcm_ready_o axi_gpio_status/gpio_io_i

cn xlconstant_key/dout  aes256gcm_0/aes_gcm_key_word_i
cn xlconstant_iv/dout   aes256gcm_0/aes_gcm_iv_i
cn xlconstant_6/dout    aes256gcm_0/aes_gcm_iv_val_i
cn xlconstant_7/dout    aes256gcm_0/aes_gcm_ghash_aad_i
cn xlconstant_6/dout    aes256gcm_0/aes_gcm_ghash_aad_bval_i
cn xlconstant_0/dout    aes256gcm_0/aes_gcm_mode_i

puts "  AES control done"

# =============================================================================
# SECTION 6: INTERRUPTS
# =============================================================================
puts "\n--- Section 6: Interrupts ---"

cn axi_dma_0/mm2s_introut  xlconcat_irq/In0
cn axi_dma_0/s2mm_introut  xlconcat_irq/In1
cn xlconstant_dcm/dout     xlconcat_irq/In2
cn xlconcat_irq/dout        processing_system7_0/IRQ_F2P

puts "  Interrupts done"

# =============================================================================
# SECTION 7: PS7 BOARD INTERFACES
# =============================================================================
puts "\n--- Section 7: PS7 Board Interfaces ---"

if {[get_bd_ports -quiet DDR] eq ""} {
    create_bd_intf_port -mode Master \
        -vlnv xilinx.com:interface:ddrx_rtl:1.0 DDR
}
if {[get_bd_ports -quiet FIXED_IO] eq ""} {
    create_bd_intf_port -mode Master \
        -vlnv xilinx.com:display_processing_system7:fixedio_rtl:1.0 FIXED_IO
}

ci processing_system7_0/DDR      DDR
ci processing_system7_0/FIXED_IO FIXED_IO

puts "  Board interfaces done"

# =============================================================================
# SUMMARY
# =============================================================================
puts "\n=== Connection Summary ==="
puts "  Connected : $connected"
puts "  Skipped   : $skipped (already existed)"
puts "  Errors    : $errors"

puts "\n--- Validating ---"
validate_bd_design
save_bd_design

puts "\n=== Script complete ==="
puts "Next: run fix_bd_ctrl_bus.tcl to add xlslice blocks"