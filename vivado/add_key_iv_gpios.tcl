# =============================================================================
# add_key_iv_gpios.tcl
#
# Surgical patch to add 8 key-word GPIOs + 3 IV GPIOs to the existing BD.
# Run this in Vivado's Tcl console with the BD open.
#
# What it does:
#   1. Creates 8 axi_gpio cells for KEY_W0..KEY_W7 (32-bit each)
#   2. Creates 3 axi_gpio cells for IV, IV1, IV2 (32-bit each)
#   3. Creates xlconcat_key (8x32 -> 256) and xlconcat_iv (3x32 -> 96)
#   4. Disconnects the zero-constant xlconstant_key and xlconstant_iv
#   5. Wires GPIOs -> concats -> AES core
#   6. Bumps smartconnect_1 NUM_MI from 4 to 15
#   7. Connects all new GPIO AXI-Lite slaves to smartconnect_1
#   8. Connects clocks/resets
#   9. Assigns address segments for all 11 GPIOs
#
# Safe to re-run: guarded with existence checks where possible.
# =============================================================================

puts "=== add_key_iv_gpios.tcl starting ==="

# ---------------------------------------------------------------------------
# 1. Bump smartconnect_1 to handle 11 more masters
#    (3 existing CTRL/KEY_VAL/STATUS + 11 new = 14; go to 15 for cushion)
# ---------------------------------------------------------------------------
puts "Step 1: Bumping smartconnect_1 NUM_MI to 15..."
set_property -dict [list CONFIG.NUM_MI {15}] [get_bd_cells smartconnect_1]

# ---------------------------------------------------------------------------
# 2. Create 11 axi_gpio cells, all 32-bit output
# ---------------------------------------------------------------------------
puts "Step 2: Creating 11 axi_gpio cells..."
set key_names [list axi_gpio_key_w0 axi_gpio_key_w1 axi_gpio_key_w2 axi_gpio_key_w3 \
                    axi_gpio_key_w4 axi_gpio_key_w5 axi_gpio_key_w6 axi_gpio_key_w7]
set iv_names  [list axi_gpio_iv axi_gpio_iv1 axi_gpio_iv2]

foreach name [concat $key_names $iv_names] {
    puts "  Creating $name"
    create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 $name
    set_property -dict [list \
        CONFIG.C_ALL_OUTPUTS {1} \
        CONFIG.C_GPIO_WIDTH  {32} \
    ] [get_bd_cells $name]
}

# ---------------------------------------------------------------------------
# 3. Create xlconcat_key (8 x 32 -> 256) and xlconcat_iv (3 x 32 -> 96)
# ---------------------------------------------------------------------------
puts "Step 3: Creating xlconcat cells..."
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 xlconcat_key
set_property -dict [list \
    CONFIG.NUM_PORTS {8} \
    CONFIG.IN0_WIDTH {32} CONFIG.IN1_WIDTH {32} CONFIG.IN2_WIDTH {32} CONFIG.IN3_WIDTH {32} \
    CONFIG.IN4_WIDTH {32} CONFIG.IN5_WIDTH {32} CONFIG.IN6_WIDTH {32} CONFIG.IN7_WIDTH {32} \
] [get_bd_cells xlconcat_key]

create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 xlconcat_iv
set_property -dict [list \
    CONFIG.NUM_PORTS {3} \
    CONFIG.IN0_WIDTH {32} CONFIG.IN1_WIDTH {32} CONFIG.IN2_WIDTH {32} \
] [get_bd_cells xlconcat_iv]

# ---------------------------------------------------------------------------
# 4. Wire GPIO outputs to concat inputs
#    Concat In0 is LSB: key_w0 -> In0 (low 32 bits), key_w7 -> In7 (high 32 bits)
#    Same for IV.
# ---------------------------------------------------------------------------
puts "Step 4: Wiring GPIOs -> concats..."
for {set i 0} {$i < 8} {incr i} {
    connect_bd_net [get_bd_pins axi_gpio_key_w${i}/gpio_io_o] \
                   [get_bd_pins xlconcat_key/In${i}]
}
connect_bd_net [get_bd_pins axi_gpio_iv/gpio_io_o]   [get_bd_pins xlconcat_iv/In0]
connect_bd_net [get_bd_pins axi_gpio_iv1/gpio_io_o]  [get_bd_pins xlconcat_iv/In1]
connect_bd_net [get_bd_pins axi_gpio_iv2/gpio_io_o]  [get_bd_pins xlconcat_iv/In2]

# ---------------------------------------------------------------------------
# 5. Disconnect the zero-constants from AES core, wire concats in their place
# ---------------------------------------------------------------------------
puts "Step 5: Replacing xlconstant_key and xlconstant_iv with live concats..."

# Remove the nets connecting zero constants to AES input pins
set key_nets [get_bd_nets -of_objects [get_bd_pins aes256gcm_0/aes_gcm_key_word_i]]
if {$key_nets ne ""} {
    puts "  Removing existing key net: $key_nets"
    delete_bd_objs $key_nets
}
set iv_nets [get_bd_nets -of_objects [get_bd_pins aes256gcm_0/aes_gcm_iv_i]]
if {$iv_nets ne ""} {
    puts "  Removing existing iv net: $iv_nets"
    delete_bd_objs $iv_nets
}

# Now connect the concats' outputs to the AES core
connect_bd_net [get_bd_pins xlconcat_key/dout] [get_bd_pins aes256gcm_0/aes_gcm_key_word_i]
connect_bd_net [get_bd_pins xlconcat_iv/dout]  [get_bd_pins aes256gcm_0/aes_gcm_iv_i]

# Delete the orphaned constants (optional, but keeps BD clean)
if {[llength [get_bd_cells -quiet xlconstant_key]] > 0} {
    puts "  Deleting orphaned xlconstant_key"
    delete_bd_objs [get_bd_cells xlconstant_key]
}
if {[llength [get_bd_cells -quiet xlconstant_iv]] > 0} {
    puts "  Deleting orphaned xlconstant_iv"
    delete_bd_objs [get_bd_cells xlconstant_iv]
}

# ---------------------------------------------------------------------------
# 6. Connect AXI-Lite interfaces: smartconnect_1 -> each new GPIO S_AXI
#    smartconnect_1 already has NUM_MI=15 from step 1.
#    We'll use ports M04 through M14 (M00-M03 are already used).
# ---------------------------------------------------------------------------
puts "Step 6: Connecting AXI-Lite to new GPIOs..."

# Build list of new GPIO cells in a fixed order matching expected address map
set new_gpios [list axi_gpio_key_w0 axi_gpio_key_w1 axi_gpio_key_w2 axi_gpio_key_w3 \
                    axi_gpio_key_w4 axi_gpio_key_w5 axi_gpio_key_w6 axi_gpio_key_w7 \
                    axi_gpio_iv axi_gpio_iv1 axi_gpio_iv2]

# Figure out which smartconnect_1 master ports are already in use.
# We scan existing intf_nets from smartconnect_1 to find used MI ports.
set used_ports [list]
foreach intf [get_bd_intf_pins -of_objects [get_bd_cells smartconnect_1] -filter {MODE == Master}] {
    set intf_name [get_property NAME $intf]
    # Check if this master is actually connected to something
    set connected_nets [get_bd_intf_nets -of_objects $intf]
    if {$connected_nets ne ""} {
        lappend used_ports $intf_name
    }
}
puts "  Currently used MI ports on smartconnect_1: $used_ports"

# Now connect each new GPIO to the next free MI port
set mi_index 0
foreach gpio $new_gpios {
    # Find next free MI slot
    while {1} {
        set candidate [format "M%02d_AXI" $mi_index]
        if {[lsearch $used_ports $candidate] == -1} {
            break
        }
        incr mi_index
        if {$mi_index >= 15} {
            error "Ran out of MI ports on smartconnect_1"
        }
    }
    puts "  smartconnect_1/$candidate -> $gpio/S_AXI"
    connect_bd_intf_net [get_bd_intf_pins smartconnect_1/$candidate] \
                        [get_bd_intf_pins $gpio/S_AXI]
    lappend used_ports $candidate
    incr mi_index
}

# ---------------------------------------------------------------------------
# 7. Connect clocks and resets for all new GPIOs
# ---------------------------------------------------------------------------
puts "Step 7: Wiring clocks and resets..."

# We use the same clock/reset that axi_gpio_ctrl uses (proc_sys_reset_0)
set clk_net [get_bd_nets -of_objects [get_bd_pins axi_gpio_ctrl/s_axi_aclk]]
set rst_net [get_bd_nets -of_objects [get_bd_pins axi_gpio_ctrl/s_axi_aresetn]]
puts "  Clock net: $clk_net"
puts "  Reset net: $rst_net"

foreach gpio $new_gpios {
    connect_bd_net -net $clk_net [get_bd_pins $gpio/s_axi_aclk]
    connect_bd_net -net $rst_net [get_bd_pins $gpio/s_axi_aresetn]
}

# ---------------------------------------------------------------------------
# 8. Assign address segments to each GPIO at the expected addresses
# ---------------------------------------------------------------------------
puts "Step 8: Assigning addresses..."

# List of (cell_name, offset) pairs.
set addr_map [list \
    {axi_gpio_key_w0 0x41230000} \
    {axi_gpio_key_w1 0x41240000} \
    {axi_gpio_key_w2 0x41250000} \
    {axi_gpio_key_w3 0x41260000} \
    {axi_gpio_key_w4 0x41270000} \
    {axi_gpio_key_w5 0x41280000} \
    {axi_gpio_key_w6 0x41290000} \
    {axi_gpio_key_w7 0x412A0000} \
    {axi_gpio_iv     0x412C0000} \
    {axi_gpio_iv1    0x412D0000} \
    {axi_gpio_iv2    0x412E0000} \
]

foreach pair $addr_map {
    set cell [lindex $pair 0]
    set offset [lindex $pair 1]
    puts "  $cell -> $offset"
    assign_bd_address -offset $offset -range 0x10000 \
        -target_address_space [get_bd_addr_spaces processing_system7_0/Data] \
        [get_bd_addr_segs ${cell}/S_AXI/Reg] -force
}

# ---------------------------------------------------------------------------
# 9. Done. Validate.
# ---------------------------------------------------------------------------
puts "Step 9: Validating design..."
validate_bd_design

puts "=== add_key_iv_gpios.tcl complete ==="
puts ""
puts "If validation returned no errors:"
puts "  1. Ctrl+S to save the BD"
puts "  2. Sources -> right-click BD -> Generate Output Products"
puts "  3. Flow -> Generate Bitstream"
