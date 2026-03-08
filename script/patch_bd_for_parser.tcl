# =============================================================================
# patch_bd_for_parser_v2.tcl
#
# Revised approach: the parser monitors (taps) the axis_switch_0 output
# rather than intercepting it. The original switch->aes256gcm connection
# stays intact inside the BD (avoiding the 64->128 bit width problem).
#
# The parser receives a copy of the data stream via exported ports, parses
# headers, and signals the PS via IRQ with classification results.
# The PS can then decide to abort/reset the AES pipeline if o_pkt_drop=1.
#
# What this script does:
#   1. Exports axis_switch_0 output signals as read-only taps (sw_tdata etc.)
#      The original switch->aes256gcm net is KEPT intact.
#   2. Exports fclk1 and parser_rst_n for the parser clock/reset
#   3. Expands xlconcat_irq to 3 ports for the parser IRQ
#   4. Validates and saves
#
# Run from Vivado Tcl console (project must be open):
#   source C:/URI/ELE598/iot_gateway_project/vivado/patch_bd_for_parser_v2.tcl
# =============================================================================

puts "=========================================================="
puts "  patch_bd_for_parser_v2.tcl"
puts "=========================================================="

set bd_file [get_files -quiet iot_gateway_bd.bd]
if {$bd_file eq ""} {
    error "Cannot find iot_gateway_bd.bd - is the project open?"
}
open_bd_design $bd_file
current_bd_design iot_gateway_bd
puts "INFO: Opened iot_gateway_bd"

# =============================================================================
# Step 1: Export axis_switch_0 output as tap ports (original net stays intact)
#
# We add the external ports onto the EXISTING nets - Vivado allows a net to
# drive both an internal pin and an external port simultaneously.
# =============================================================================
puts "INFO: Step 1 - Creating tap ports on axis_switch_0 output..."

# These are outputs from the BD (driven by axis_switch_0, read by parser)
create_bd_port -dir O -from 63 -to 0  sw_tdata
create_bd_port -dir O -from  7 -to 0  sw_tkeep

# tready: parser drives this back in. We must break the xlconstant_one1
# connection and replace it with an external port.
# (The parser's s_axis_tready output goes here)
create_bd_port -dir I                  sw_tready

# =============================================================================
# Step 2: Disconnect m_axis_tready from xlconstant_one1, wire to external port
# =============================================================================
puts "INFO: Step 2 - Rerouting m_axis_tready to external port..."

disconnect_bd_net [get_bd_nets xlconstant_one1_dout] \
                  [get_bd_pins axis_switch_0/m_axis_tready]

connect_bd_net [get_bd_pins axis_switch_0/m_axis_tready] [get_bd_ports sw_tready]

# =============================================================================
# Step 3: Tap the existing switch->aes data nets (add port onto existing net)
# The aes256gcm still gets its data - we just also export it for the parser.
# =============================================================================
puts "INFO: Step 3 - Tapping axis_switch_0_m_axis_tdata and tkeep nets..."

connect_bd_net [get_bd_nets axis_switch_0_m_axis_tdata] [get_bd_ports sw_tdata]
connect_bd_net [get_bd_nets axis_switch_0_m_axis_tkeep] [get_bd_ports sw_tkeep]

# =============================================================================
# Step 4: Export clock and reset
# =============================================================================
puts "INFO: Step 4 - Exposing clock and reset..."

create_bd_port -dir O -type clk  fclk1
create_bd_port -dir O -type rst  parser_rst_n

connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK1]     [get_bd_ports fclk1]
connect_bd_net [get_bd_pins proc_sys_reset_1/peripheral_aresetn] [get_bd_ports parser_rst_n]

# =============================================================================
# Step 5: Expand xlconcat_irq to 3 inputs, wire parser IRQ to In2
# =============================================================================
puts "INFO: Step 5 - Expanding IRQ concat and adding parser_irq port..."

create_bd_port -dir I  parser_irq

set_property CONFIG.NUM_PORTS {3} [get_bd_cells xlconcat_irq]
connect_bd_net [get_bd_ports parser_irq] [get_bd_pins xlconcat_irq/In2]

# =============================================================================
# Step 6: Validate and save
# =============================================================================
puts "INFO: Step 6 - Validating and saving..."

validate_bd_design
save_bd_design

puts ""
puts "=========================================================="
puts "  PATCH COMPLETE"
puts ""
puts "  The BD now has these new ports:"
puts "    sw_tdata  [63:0]  - out - copy of axis_switch_0 data"
puts "    sw_tkeep  [7:0]   - out - copy of axis_switch_0 keep"  
puts "    sw_tready         - in  - parser tready back to switch"
puts "    fclk1             - out - 200MHz clock for parser"
puts "    parser_rst_n      - out - active-low reset for parser"
puts "    parser_irq        - in  - parser IRQ to PS"
puts ""
puts "  The original switch->aes256gcm connection is KEPT INTACT."
puts "  The parser monitors the stream and signals PS via IRQ."
puts ""
puts "  Next steps:"
puts "    1. Right-click iot_gateway_bd -> Generate HDL Wrapper"
puts "       -> 'Let Vivado manage wrapper and auto-update'"
puts "    2. Add iot_gateway_top.vhd as a Design Source"
puts "    3. Right-click iot_gateway_top -> Set as Top"  
puts "    4. Generate Bitstream"
puts "=========================================================="