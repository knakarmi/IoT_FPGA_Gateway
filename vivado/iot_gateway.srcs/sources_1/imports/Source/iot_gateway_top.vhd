-- =============================================================================
-- IoT Gateway - Week 4
-- File   : iot_gateway_top.vhd
--
-- Architecture: TAP (monitor) mode
-- The parser sits alongside the existing switch->aes256gcm path.
-- It receives a copy of the data stream from axis_switch_0, parses headers,
-- and signals the PS via IRQ with classification results (o_pkt_valid,
-- o_pkt_drop, o_protocol_class, o_error_code etc.).
--
-- The original switch->aes256gcm connection is kept intact inside the BD,
-- avoiding any data-width conversion issues (AES expects 128-bit, switch
-- outputs 64-bit - the BD handles this internally as before).
--
-- Parser controls s_axis_tready back to axis_switch_0 for backpressure.
-- =============================================================================

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity iot_gateway_top is
    port (
        DDR_addr          : inout std_logic_vector(14 downto 0);
        DDR_ba            : inout std_logic_vector(2  downto 0);
        DDR_cas_n         : inout std_logic;
        DDR_ck_n          : inout std_logic;
        DDR_ck_p          : inout std_logic;
        DDR_cke           : inout std_logic;
        DDR_cs_n          : inout std_logic;
        DDR_dm            : inout std_logic_vector(3  downto 0);
        DDR_dq            : inout std_logic_vector(31 downto 0);
        DDR_dqs_n         : inout std_logic_vector(3  downto 0);
        DDR_dqs_p         : inout std_logic_vector(3  downto 0);
        DDR_odt           : inout std_logic;
        DDR_ras_n         : inout std_logic;
        DDR_reset_n       : inout std_logic;
        DDR_we_n          : inout std_logic;
        FIXED_IO_ddr_vrn  : inout std_logic;
        FIXED_IO_ddr_vrp  : inout std_logic;
        FIXED_IO_mio      : inout std_logic_vector(53 downto 0);
        FIXED_IO_ps_clk   : inout std_logic;
        FIXED_IO_ps_porb  : inout std_logic;
        FIXED_IO_ps_srstb : inout std_logic
    );
end entity iot_gateway_top;

architecture rtl of iot_gateway_top is

    -- -------------------------------------------------------------------------
    -- Block design wrapper component
    -- Ports match exactly what patch_bd_for_parser_v2.tcl creates
    -- -------------------------------------------------------------------------
    component iot_gateway_bd_wrapper is
        port (
            DDR_addr          : inout std_logic_vector(14 downto 0);
            DDR_ba            : inout std_logic_vector(2  downto 0);
            DDR_cas_n         : inout std_logic;
            DDR_ck_n          : inout std_logic;
            DDR_ck_p          : inout std_logic;
            DDR_cke           : inout std_logic;
            DDR_cs_n          : inout std_logic;
            DDR_dm            : inout std_logic_vector(3  downto 0);
            DDR_dq            : inout std_logic_vector(31 downto 0);
            DDR_dqs_n         : inout std_logic_vector(3  downto 0);
            DDR_dqs_p         : inout std_logic_vector(3  downto 0);
            DDR_odt           : inout std_logic;
            DDR_ras_n         : inout std_logic;
            DDR_reset_n       : inout std_logic;
            DDR_we_n          : inout std_logic;
            FIXED_IO_ddr_vrn  : inout std_logic;
            FIXED_IO_ddr_vrp  : inout std_logic;
            FIXED_IO_mio      : inout std_logic_vector(53 downto 0);
            FIXED_IO_ps_clk   : inout std_logic;
            FIXED_IO_ps_porb  : inout std_logic;
            FIXED_IO_ps_srstb : inout std_logic;
            -- Tap: copy of axis_switch_0 output (BD drives these out)
            sw_tdata          : out std_logic_vector(63 downto 0);
            sw_tkeep          : out std_logic_vector(7  downto 0);
            -- Parser drives tready back into the switch
            sw_tready         : in  std_logic;
            -- Clock and reset for parser
            fclk1             : out std_logic;
            parser_rst_n      : out std_logic;
            -- Parser IRQ into PS interrupt controller
            parser_irq        : in  std_logic
        );
    end component;

    -- -------------------------------------------------------------------------
    -- Packet parser component
    -- -------------------------------------------------------------------------
    component packet_parser_top is
        generic (
            C_AXIS_DATA_WIDTH  : integer := 64;
            C_S_AXI_ADDR_WIDTH : integer := 5
        );
        port (
            clk               : in  std_logic;
            rst_n             : in  std_logic;
            s_axis_tdata      : in  std_logic_vector(63 downto 0);
            s_axis_tkeep      : in  std_logic_vector(7  downto 0);
            s_axis_tvalid     : in  std_logic;
            s_axis_tlast      : in  std_logic;
            s_axis_tready     : out std_logic;
            m_axis_tdata      : out std_logic_vector(63 downto 0);
            m_axis_tkeep      : out std_logic_vector(7  downto 0);
            m_axis_tvalid     : out std_logic;
            m_axis_tlast      : out std_logic;
            m_axis_tready     : in  std_logic;
            o_eth_dst_mac     : out std_logic_vector(47 downto 0);
            o_eth_src_mac     : out std_logic_vector(47 downto 0);
            o_eth_type        : out std_logic_vector(15 downto 0);
            o_ip_src          : out std_logic_vector(31 downto 0);
            o_ip_dst          : out std_logic_vector(31 downto 0);
            o_ip_proto        : out std_logic_vector(7  downto 0);
            o_udp_src_port    : out std_logic_vector(15 downto 0);
            o_udp_dst_port    : out std_logic_vector(15 downto 0);
            o_udp_length      : out std_logic_vector(15 downto 0);
            o_iot_device_id   : out std_logic_vector(15 downto 0);
            o_iot_msg_type    : out std_logic_vector(7  downto 0);
            o_iot_seq_num     : out std_logic_vector(15 downto 0);
            o_iot_payload_len : out std_logic_vector(15 downto 0);
            o_protocol_class  : out std_logic_vector(2  downto 0);
            o_pkt_valid       : out std_logic;
            o_pkt_drop        : out std_logic;
            o_parse_done      : out std_logic;
            o_error_code      : out std_logic_vector(2 downto 0);
            o_irq             : out std_logic
        );
    end component;

    -- -------------------------------------------------------------------------
    -- Internal signals
    -- -------------------------------------------------------------------------
    signal w_fclk1       : std_logic;
    signal w_parser_rstn : std_logic;

    -- Tap signals from axis_switch_0
    signal w_sw_tdata    : std_logic_vector(63 downto 0);
    signal w_sw_tkeep    : std_logic_vector(7  downto 0);
    signal w_sw_tready   : std_logic;

    -- Parser master output (not connected to AES - monitored only)
    signal w_m_tdata     : std_logic_vector(63 downto 0);
    signal w_m_tkeep     : std_logic_vector(7  downto 0);
    signal w_m_tvalid    : std_logic;
    signal w_m_tlast     : std_logic;

    -- Parser status
    signal w_irq         : std_logic;

    -- Unused status outputs (available for ILA probe if desired)
    signal w_eth_dst_mac     : std_logic_vector(47 downto 0);
    signal w_eth_src_mac     : std_logic_vector(47 downto 0);
    signal w_eth_type        : std_logic_vector(15 downto 0);
    signal w_ip_src          : std_logic_vector(31 downto 0);
    signal w_ip_dst          : std_logic_vector(31 downto 0);
    signal w_ip_proto        : std_logic_vector(7  downto 0);
    signal w_udp_src_port    : std_logic_vector(15 downto 0);
    signal w_udp_dst_port    : std_logic_vector(15 downto 0);
    signal w_udp_length      : std_logic_vector(15 downto 0);
    signal w_iot_device_id   : std_logic_vector(15 downto 0);
    signal w_iot_msg_type    : std_logic_vector(7  downto 0);
    signal w_iot_seq_num     : std_logic_vector(15 downto 0);
    signal w_iot_payload_len : std_logic_vector(15 downto 0);
    signal w_protocol_class  : std_logic_vector(2  downto 0);
    signal w_pkt_valid       : std_logic;
    signal w_pkt_drop        : std_logic;
    signal w_parse_done      : std_logic;
    signal w_error_code      : std_logic_vector(2 downto 0);

begin

    -- -------------------------------------------------------------------------
    -- Block design
    -- -------------------------------------------------------------------------
    u_bd : iot_gateway_bd_wrapper
        port map (
            DDR_addr          => DDR_addr,
            DDR_ba            => DDR_ba,
            DDR_cas_n         => DDR_cas_n,
            DDR_ck_n          => DDR_ck_n,
            DDR_ck_p          => DDR_ck_p,
            DDR_cke           => DDR_cke,
            DDR_cs_n          => DDR_cs_n,
            DDR_dm            => DDR_dm,
            DDR_dq            => DDR_dq,
            DDR_dqs_n         => DDR_dqs_n,
            DDR_dqs_p         => DDR_dqs_p,
            DDR_odt           => DDR_odt,
            DDR_ras_n         => DDR_ras_n,
            DDR_reset_n       => DDR_reset_n,
            DDR_we_n          => DDR_we_n,
            FIXED_IO_ddr_vrn  => FIXED_IO_ddr_vrn,
            FIXED_IO_ddr_vrp  => FIXED_IO_ddr_vrp,
            FIXED_IO_mio      => FIXED_IO_mio,
            FIXED_IO_ps_clk   => FIXED_IO_ps_clk,
            FIXED_IO_ps_porb  => FIXED_IO_ps_porb,
            FIXED_IO_ps_srstb => FIXED_IO_ps_srstb,
            sw_tdata          => w_sw_tdata,
            sw_tkeep          => w_sw_tkeep,
            sw_tready         => w_sw_tready,
            fclk1             => w_fclk1,
            parser_rst_n      => w_parser_rstn,
            parser_irq        => w_irq
        );

    -- -------------------------------------------------------------------------
    -- Packet parser (tap/monitor mode)
    -- Receives the same stream as aes256gcm_0.
    -- Master output is dropped (m_axis_tready tied high).
    -- PS is notified via IRQ when parse_done fires.
    -- -------------------------------------------------------------------------
    u_parser : packet_parser_top
        generic map (C_AXIS_DATA_WIDTH => 64)
        port map (
            clk               => w_fclk1,
            rst_n             => w_parser_rstn,
            s_axis_tdata      => w_sw_tdata,
            s_axis_tkeep      => w_sw_tkeep,
            s_axis_tvalid     => '1',        -- switch has no tvalid pin; always valid
            s_axis_tlast      => '0',        -- switch has no tlast pin
            s_axis_tready     => w_sw_tready,
            m_axis_tdata      => w_m_tdata,
            m_axis_tkeep      => w_m_tkeep,
            m_axis_tvalid     => w_m_tvalid,
            m_axis_tlast      => w_m_tlast,
            m_axis_tready     => '1',        -- master output not used
            o_eth_dst_mac     => w_eth_dst_mac,
            o_eth_src_mac     => w_eth_src_mac,
            o_eth_type        => w_eth_type,
            o_ip_src          => w_ip_src,
            o_ip_dst          => w_ip_dst,
            o_ip_proto        => w_ip_proto,
            o_udp_src_port    => w_udp_src_port,
            o_udp_dst_port    => w_udp_dst_port,
            o_udp_length      => w_udp_length,
            o_iot_device_id   => w_iot_device_id,
            o_iot_msg_type    => w_iot_msg_type,
            o_iot_seq_num     => w_iot_seq_num,
            o_iot_payload_len => w_iot_payload_len,
            o_protocol_class  => w_protocol_class,
            o_pkt_valid       => w_pkt_valid,
            o_pkt_drop        => w_pkt_drop,
            o_parse_done      => w_parse_done,
            o_error_code      => w_error_code,
            o_irq             => w_irq
        );

end architecture rtl;