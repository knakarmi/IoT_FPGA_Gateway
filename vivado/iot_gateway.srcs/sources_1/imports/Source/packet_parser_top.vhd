-- =============================================================================
-- IoT Gateway - Week 4: Packet Parsing and Processing Module
-- File   : packet_parser_top.vhd
-- Target : Xilinx Zynq ZC706 (xc7z045ffg900-2)
-- Clock  : FCLK_CLK1 @ 200 MHz (fast datapath domain)
-- Bus    : AXI4-Stream (64-bit, matching axi_dma_0 MM2S width)
-- =============================================================================
-- Architecture
-- ============
--   S_AXIS (from DMA MM2S)
--      |
--      v
--  +-----------------------------------------------------+
--  |              PACKET PARSER TOP                       |
--  |                                                      |
--  |  +--------------+   +--------------+                |
--  |  |  Eth/IP/UDP  |   |  IoT Proto   |                |
--  |  |  Header FSM  |-->|  Classifier  |                |
--  |  +--------------+   +--------------+                |
--  |          |                   |                       |
--  |          v                   v                       |
--  |  +----------------------------------+               |
--  |  |        Header Register Bank      |               |
--  |  |  eth_dst, eth_src, eth_type      |               |
--  |  |  ip_src, ip_dst, ip_proto        |               |
--  |  |  udp_sport, udp_dport, udp_len   |               |
--  |  |  iot_dev_id, iot_msg_type        |               |
--  |  |  iot_seq, iot_payload_len        |               |
--  |  +----------------------------------+               |
--  |          |                                           |
--  |          v                                           |
--  |  +----------------------------------+               |
--  |  |     M_AXIS Payload Forward       |               |
--  |  |  (to aes256gcm pipeline)         |               |
--  |  +----------------------------------+               |
--  +-----------------------------------------------------+
-- =============================================================================

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity packet_parser_top is
    generic (
        -- AXI4-Stream data width (must match DMA: c_m_axis_mm2s_tdata_width=64)
        C_AXIS_DATA_WIDTH  : integer := 64;
        -- AXI-Lite address width for status registers
        C_S_AXI_ADDR_WIDTH : integer := 5
    );
    port (
        -- Clock / Reset
        clk      : in  std_logic;   -- 200 MHz FCLK_CLK1
        rst_n    : in  std_logic;   -- active-low, from proc_sys_reset_1

        -- Slave AXI4-Stream - from AXI DMA MM2S (raw Ethernet frame)
        s_axis_tdata  : in  std_logic_vector(C_AXIS_DATA_WIDTH-1 downto 0);
        s_axis_tkeep  : in  std_logic_vector(C_AXIS_DATA_WIDTH/8-1 downto 0);
        s_axis_tvalid : in  std_logic;
        s_axis_tlast  : in  std_logic;
        s_axis_tready : out std_logic;

        -- Master AXI4-Stream - to AES-256-GCM core (payload only)
        m_axis_tdata  : out std_logic_vector(C_AXIS_DATA_WIDTH-1 downto 0);
        m_axis_tkeep  : out std_logic_vector(C_AXIS_DATA_WIDTH/8-1 downto 0);
        m_axis_tvalid : out std_logic;
        m_axis_tlast  : out std_logic;
        m_axis_tready : in  std_logic;

        -- Parsed Header Fields (registered, valid after parse_done)
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

        -- Protocol Classification Output
        -- 000=Raw IoT, 001=MQTT, 010=CoAP, 011=HTTP, 111=Unknown/Drop
        o_protocol_class  : out std_logic_vector(2 downto 0);

        -- Status / Error Flags
        o_pkt_valid  : out std_logic;  -- header parse success
        o_pkt_drop   : out std_logic;  -- malformed / unknown
        o_parse_done : out std_logic;  -- one-cycle pulse
        o_error_code : out std_logic_vector(2 downto 0);
        o_irq        : out std_logic   -- to xlconcat_irq
    );
end entity packet_parser_top;

architecture rtl of packet_parser_top is

    -- -------------------------------------------------------------------------
    -- Component: pkt_header_fsm
    -- -------------------------------------------------------------------------
    component pkt_header_fsm is
        generic (C_AXIS_DATA_WIDTH : integer := 64);
        port (
            clk               : in  std_logic;
            rst_n             : in  std_logic;
            s_axis_tdata      : in  std_logic_vector(C_AXIS_DATA_WIDTH-1   downto 0);
            s_axis_tkeep      : in  std_logic_vector(C_AXIS_DATA_WIDTH/8-1 downto 0);
            s_axis_tvalid     : in  std_logic;
            s_axis_tlast      : in  std_logic;
            s_axis_tready     : out std_logic;
            o_eth_dst_mac     : out std_logic_vector(47 downto 0);
            o_eth_src_mac     : out std_logic_vector(47 downto 0);
            o_eth_type        : out std_logic_vector(15 downto 0);
            o_ip_src          : out std_logic_vector(31 downto 0);
            o_ip_dst          : out std_logic_vector(31 downto 0);
            o_ip_proto        : out std_logic_vector(7  downto 0);
            o_ip_ihl          : out std_logic_vector(7  downto 0);
            o_udp_src_port    : out std_logic_vector(15 downto 0);
            o_udp_dst_port    : out std_logic_vector(15 downto 0);
            o_udp_length      : out std_logic_vector(15 downto 0);
            o_iot_device_id   : out std_logic_vector(15 downto 0);
            o_iot_msg_type    : out std_logic_vector(7  downto 0);
            o_iot_seq_num     : out std_logic_vector(15 downto 0);
            o_iot_payload_len : out std_logic_vector(15 downto 0);
            m_axis_tdata      : out std_logic_vector(C_AXIS_DATA_WIDTH-1   downto 0);
            m_axis_tkeep      : out std_logic_vector(C_AXIS_DATA_WIDTH/8-1 downto 0);
            m_axis_tvalid     : out std_logic;
            m_axis_tlast      : out std_logic;
            m_axis_tready     : in  std_logic;
            o_parse_done      : out std_logic;
            o_parse_error     : out std_logic;
            o_error_code      : out std_logic_vector(2 downto 0)
        );
    end component;

    -- -------------------------------------------------------------------------
    -- Component: protocol_classifier
    -- -------------------------------------------------------------------------
    component protocol_classifier is
        port (
            clk              : in  std_logic;
            rst_n            : in  std_logic;
            i_parse_done  : in  std_logic;
            i_parse_error : in  std_logic;
            i_eth_type       : in  std_logic_vector(15 downto 0);
            i_ip_proto       : in  std_logic_vector(7  downto 0);
            i_udp_dst_port   : in  std_logic_vector(15 downto 0);
            i_iot_msg_type   : in  std_logic_vector(7  downto 0);
            o_protocol_class : out std_logic_vector(2  downto 0);
            o_pkt_valid      : out std_logic;
            o_pkt_drop       : out std_logic
        );
    end component;

    -- -------------------------------------------------------------------------
    -- Internal signals
    -- -------------------------------------------------------------------------
    signal w_eth_dst_mac     : std_logic_vector(47 downto 0);
    signal w_eth_src_mac     : std_logic_vector(47 downto 0);
    signal w_eth_type        : std_logic_vector(15 downto 0);
    signal w_ip_src          : std_logic_vector(31 downto 0);
    signal w_ip_dst          : std_logic_vector(31 downto 0);
    signal w_ip_proto        : std_logic_vector(7  downto 0);
    signal w_ip_ihl          : std_logic_vector(7  downto 0);
    signal w_udp_src_port    : std_logic_vector(15 downto 0);
    signal w_udp_dst_port    : std_logic_vector(15 downto 0);
    signal w_udp_length      : std_logic_vector(15 downto 0);
    signal w_iot_device_id   : std_logic_vector(15 downto 0);
    signal w_iot_msg_type    : std_logic_vector(7  downto 0);
    signal w_iot_seq_num     : std_logic_vector(15 downto 0);
    signal w_iot_payload_len : std_logic_vector(15 downto 0);

    signal w_parse_done  : std_logic;
    signal w_parse_error : std_logic;
    signal w_error_code  : std_logic_vector(2 downto 0);
    signal w_proto_class : std_logic_vector(2 downto 0);
    signal w_pkt_valid   : std_logic;
    signal w_pkt_drop    : std_logic;

    signal pay_tdata  : std_logic_vector(C_AXIS_DATA_WIDTH-1   downto 0);
    signal pay_tkeep  : std_logic_vector(C_AXIS_DATA_WIDTH/8-1 downto 0);
    signal pay_tvalid : std_logic;
    signal pay_tlast  : std_logic;

begin

    -- -------------------------------------------------------------------------
    -- Header FSM
    -- -------------------------------------------------------------------------
    u_hdr_fsm : pkt_header_fsm
        generic map (C_AXIS_DATA_WIDTH => C_AXIS_DATA_WIDTH)
        port map (
            clk               => clk,
            rst_n             => rst_n,
            s_axis_tdata      => s_axis_tdata,
            s_axis_tkeep      => s_axis_tkeep,
            s_axis_tvalid     => s_axis_tvalid,
            s_axis_tlast      => s_axis_tlast,
            s_axis_tready     => s_axis_tready,
            o_eth_dst_mac     => w_eth_dst_mac,
            o_eth_src_mac     => w_eth_src_mac,
            o_eth_type        => w_eth_type,
            o_ip_src          => w_ip_src,
            o_ip_dst          => w_ip_dst,
            o_ip_proto        => w_ip_proto,
            o_ip_ihl          => w_ip_ihl,
            o_udp_src_port    => w_udp_src_port,
            o_udp_dst_port    => w_udp_dst_port,
            o_udp_length      => w_udp_length,
            o_iot_device_id   => w_iot_device_id,
            o_iot_msg_type    => w_iot_msg_type,
            o_iot_seq_num     => w_iot_seq_num,
            o_iot_payload_len => w_iot_payload_len,
            m_axis_tdata      => pay_tdata,
            m_axis_tkeep      => pay_tkeep,
            m_axis_tvalid     => pay_tvalid,
            m_axis_tlast      => pay_tlast,
            m_axis_tready     => m_axis_tready,
            o_parse_done      => w_parse_done,
            o_parse_error     => w_parse_error,
            o_error_code      => w_error_code
        );

    -- -------------------------------------------------------------------------
    -- Protocol Classifier
    -- -------------------------------------------------------------------------
    u_classifier : protocol_classifier
        port map (
            clk              => clk,
            rst_n            => rst_n,
            i_parse_done  => w_parse_done,
            i_parse_error => w_parse_error,
            i_eth_type       => w_eth_type,
            i_ip_proto       => w_ip_proto,
            i_udp_dst_port   => w_udp_dst_port,
            i_iot_msg_type   => w_iot_msg_type,
            o_protocol_class => w_proto_class,
            o_pkt_valid      => w_pkt_valid,
            o_pkt_drop       => w_pkt_drop
        );

    -- -------------------------------------------------------------------------
    -- Payload AXIS passthrough (gated on valid packet)
    -- -------------------------------------------------------------------------
    m_axis_tdata  <= pay_tdata;
    m_axis_tkeep  <= pay_tkeep;
    m_axis_tvalid <= pay_tvalid and w_pkt_valid;
    m_axis_tlast  <= pay_tlast;

    -- -------------------------------------------------------------------------
    -- Output Assignments
    -- -------------------------------------------------------------------------
    o_eth_dst_mac     <= w_eth_dst_mac;
    o_eth_src_mac     <= w_eth_src_mac;
    o_eth_type        <= w_eth_type;
    o_ip_src          <= w_ip_src;
    o_ip_dst          <= w_ip_dst;
    o_ip_proto        <= w_ip_proto;
    o_udp_src_port    <= w_udp_src_port;
    o_udp_dst_port    <= w_udp_dst_port;
    o_udp_length      <= w_udp_length;
    o_iot_device_id   <= w_iot_device_id;
    o_iot_msg_type    <= w_iot_msg_type;
    o_iot_seq_num     <= w_iot_seq_num;
    o_iot_payload_len <= w_iot_payload_len;
    o_protocol_class  <= w_proto_class;
    o_pkt_valid       <= w_pkt_valid;
    o_pkt_drop        <= w_pkt_drop;
    o_parse_done      <= w_parse_done;
    o_error_code      <= w_error_code;

    -- IRQ on parse error or drop (feeds xlconcat_irq)
    o_irq <= w_parse_error or w_pkt_drop;

end architecture rtl;