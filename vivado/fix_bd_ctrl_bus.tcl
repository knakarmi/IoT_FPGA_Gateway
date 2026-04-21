# =============================================================================
# fix_bd_ctrl_bus.tcl
# ELE598 IoT Gateway - Week 8
#
# ROOT CAUSE: axi_gpio_ctrl is a 5-bit bus driving 4 separate 1-bit AES pins
# via a SINGLE net. Vivado connects all 4 pins to the same signal, meaning
# every ctrl write simultaneously affects enc_dec, icb_start, icb_stop AND
# pipe_reset. The core is being reset on every control write.
#
# FIX: Replace the single ctrl GPIO with individual GPIOs per control signal,
# OR use a single 5-bit GPIO with proper xlslice to extract individual bits.
#
# We use xlslice (bit extraction) - requires no new AXI slaves.
# =============================================================================

open_bd_design [get_files iot_gateway_bd.bd]

puts "=== Fixing ctrl GPIO bit mapping ==="

# -----------------------------------------------------------------------------
# Step 1: Disconnect the single-net ctrl connection
# -----------------------------------------------------------------------------
puts "Step 1: Disconnecting ctrl GPIO from AES pins..."
disconnect_bd_net /axi_gpio_ctrl_gpio_io_o [get_bd_pins aes256gcm_0/aes_gcm_enc_dec_i]
disconnect_bd_net /axi_gpio_ctrl_gpio_io_o [get_bd_pins aes256gcm_0/aes_gcm_icb_start_cnt_i]
disconnect_bd_net /axi_gpio_ctrl_gpio_io_o [get_bd_pins aes256gcm_0/aes_gcm_icb_stop_cnt_i]
disconnect_bd_net /axi_gpio_ctrl_gpio_io_o [get_bd_pins aes256gcm_0/aes_gcm_pipe_reset_i]

# -----------------------------------------------------------------------------
# Step 2: Create xlslice instances to extract individual bits from 5-bit GPIO
#   bit[0] = enc_dec_i
#   bit[1] = ghash_pkt_val_i  (we will handle separately)
#   bit[2] = icb_start_cnt_i
#   bit[3] = icb_stop_cnt_i
#   bit[4] = pipe_reset_i
# -----------------------------------------------------------------------------
puts "Step 2: Creating xlslice for each control bit..."

# enc_dec_i = bit 0
set slice_enc [create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 xlslice_enc_dec]
set_property -dict [list CONFIG.DIN_WIDTH {5} CONFIG.DIN_FROM {0} CONFIG.DIN_TO {0} CONFIG.DOUT_WIDTH {1}] $slice_enc

# icb_start_cnt_i = bit 2
set slice_icb_start [create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 xlslice_icb_start]
set_property -dict [list CONFIG.DIN_WIDTH {5} CONFIG.DIN_FROM {2} CONFIG.DIN_TO {2} CONFIG.DOUT_WIDTH {1}] $slice_icb_start

# icb_stop_cnt_i = bit 3
set slice_icb_stop [create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 xlslice_icb_stop]
set_property -dict [list CONFIG.DIN_WIDTH {5} CONFIG.DIN_FROM {3} CONFIG.DIN_TO {3} CONFIG.DOUT_WIDTH {1}] $slice_icb_stop

# pipe_reset_i = bit 4
set slice_rst [create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 xlslice_pipe_reset]
set_property -dict [list CONFIG.DIN_WIDTH {5} CONFIG.DIN_FROM {4} CONFIG.DIN_TO {4} CONFIG.DOUT_WIDTH {1}] $slice_rst

# -----------------------------------------------------------------------------
# Step 3: Connect GPIO output to all slice inputs
# -----------------------------------------------------------------------------
puts "Step 3: Connecting GPIO to slices..."
connect_bd_net [get_bd_pins axi_gpio_ctrl/gpio_io_o] [get_bd_pins xlslice_enc_dec/Din]
connect_bd_net [get_bd_pins axi_gpio_ctrl/gpio_io_o] [get_bd_pins xlslice_icb_start/Din]
connect_bd_net [get_bd_pins axi_gpio_ctrl/gpio_io_o] [get_bd_pins xlslice_icb_stop/Din]
connect_bd_net [get_bd_pins axi_gpio_ctrl/gpio_io_o] [get_bd_pins xlslice_pipe_reset/Din]

# -----------------------------------------------------------------------------
# Step 4: Connect slice outputs to AES pins
# -----------------------------------------------------------------------------
puts "Step 4: Connecting slice outputs to AES pins..."
connect_bd_net [get_bd_pins xlslice_enc_dec/Dout]    [get_bd_pins aes256gcm_0/aes_gcm_enc_dec_i]
connect_bd_net [get_bd_pins xlslice_icb_start/Dout]  [get_bd_pins aes256gcm_0/aes_gcm_icb_start_cnt_i]
connect_bd_net [get_bd_pins xlslice_icb_stop/Dout]   [get_bd_pins aes256gcm_0/aes_gcm_icb_stop_cnt_i]
connect_bd_net [get_bd_pins xlslice_pipe_reset/Dout] [get_bd_pins aes256gcm_0/aes_gcm_pipe_reset_i]

# -----------------------------------------------------------------------------
# Step 5: Fix ghash_pkt_val_i
# Currently driven by axis_dwidth_conv_0/m_axis_tvalid (only high during DMA)
# Should be driven by ctrl GPIO bit[1] so software controls it explicitly
# -----------------------------------------------------------------------------
puts "Step 5: Fixing ghash_pkt_val_i..."

# Disconnect from dwidth_conv tvalid
# disconnect_bd_net /axis_dwidth_conv_0_m_axis_tvalid [get_bd_pins aes256gcm_0/aes_gcm_ghash_pkt_val_i]

# Create slice for bit 1
set slice_ghash [create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 xlslice_ghash_pkt_val]
set_property -dict [list CONFIG.DIN_WIDTH {5} CONFIG.DIN_FROM {1} CONFIG.DIN_TO {1} CONFIG.DOUT_WIDTH {1}] $slice_ghash
connect_bd_net [get_bd_pins axi_gpio_ctrl/gpio_io_o]      [get_bd_pins xlslice_ghash_pkt_val/Din]
connect_bd_net [get_bd_pins xlslice_ghash_pkt_val/Dout]   [get_bd_pins aes256gcm_0/aes_gcm_ghash_pkt_val_i]

# axis_dwidth_conv_0/m_axis_tvalid is now unconnected - that's OK,
# the AES core has no tvalid input so it was unused anyway

puts "Step 5 done"

# -----------------------------------------------------------------------------
# Step 6: Validate and save
# -----------------------------------------------------------------------------
puts "Step 6: Validating..."
validate_bd_design
save_bd_design

puts ""
puts "=== Fix complete ==="
puts ""
puts "New ctrl GPIO bit mapping (write to 0x41200000):"
puts "  bit[0] = enc_dec_i       (1=encrypt, 0=decrypt)"
puts "  bit[1] = ghash_pkt_val_i (assert during packet processing)"
puts "  bit[2] = icb_start_cnt_i (pulse to start GCM counter)"
puts "  bit[3] = icb_stop_cnt_i  (pulse to stop GCM counter)"
puts "  bit[4] = pipe_reset_i    (active high reset - assert then deassert)"
puts ""
puts "Key loading sequence:"
puts "  1. Write 0x10 -> assert pipe_reset"
puts "  2. Write 0x00 -> deassert pipe_reset"
puts "  3. Load key words, assert key_word_val=0x7, deassert"
puts "  4. Load IV words, assert iv_val, deassert"
puts "  5. Write 0x01 -> set enc_dec=1 (encrypt mode)"
puts "  6. Write 0x05 -> set enc_dec=1 + icb_start=1 (start counter)"
puts "  7. Write 0x01 -> deassert icb_start (enc_dec stays 1)"
puts "  8. Write 0x03 -> set enc_dec=1 + ghash_pkt_val=1"
puts "  9. Poll ready_o (should assert within microseconds)"
puts ""
puts "Next: Generate Output Products -> Bitstream -> Export -> Vitis rebuild"

