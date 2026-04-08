-- =============================================================================
-- IoT Gateway - Week 4
-- File   : iot_gateway_top.vhd
--
-- Architecture: TAP (monitor) mode
-- The parser sits alongside the existing switch->aes256gcm path.
-- It receives a copy of the data stream from axis_switch_0, parses headers,
-- and signals the PS via IRQ with classification results.
--
-- Fix v3: BD now exports sw_tdata as 128-bit (axis_switch outputs 128-bit
--         to match the AES core). The parser operates on 64-bit beats.
--         We take the upper 64 bits [127:64] of each beat - this matches
--         the network byte order since axis_switch drives TDATA with the
--         first byte in the MSB position.
--         sw_tkeep[15:8] is the corresponding keep for the upper half.
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
    -- sw_tdata/sw_tkeep are 128/16-bit - matching axis_switch 128-bit output
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
            -- Tap: copy of axis_switch_0 output (128-bit from BD)
            sw_tdata          : out std_logic_vector(127 downto 0);
            sw_tkeep          : out std_logic_vector(15  downto 0);
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
    -- Packet parser component (64-bit AXI-Stream)
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

    -- Full 128-bit tap from BD axis_switch output
    signal w_sw_tdata_128 : std_logic_vector(127 downto 0);
    signal w_sw_tkeep_16  : std_logic_vector(15  downto 0);
    signal w_sw_tready    : std_logic;

    -- ---------------------------------------------------------------------------
    -- Width adaptation: 128-bit BD output -> 64-bit parser input
    --
    -- axis_switch drives TDATA[127:0] where byte 0 of the packet occupies
    -- TDATA[127:120] (MSB-first / big-endian network order).
    -- The parser FSM expects the same byte ordering on a 64-bit bus:
    --   beat N maps to TDATA[63:0] with byte 0 at bits [63:56].
    --
    -- Strategy: present two 64-bit beats per 128-bit word using a simple
    -- 1-bit sub-beat counter. On sub-beat 0 forward upper half [127:64];
    -- on sub-beat 1 forward lower half [63:0]. tkeep is split likewise.
    -- tready to BD is asserted only when both sub-beats have been consumed.
    -- ---------------------------------------------------------------------------
    signal r_sub_beat     : std_logic := '0';  -- 0 = upper half, 1 = lower half
    signal r_tdata_lo     : std_logic_vector(63 downto 0);  -- latched lower half
    signal r_tkeep_lo     : std_logic_vector(7  downto 0);

    -- 64-bit signals to parser
    signal w_par_tdata    : std_logic_vector(63 downto 0);
    signal w_par_tkeep    : std_logic_vector(7  downto 0);
    signal w_par_tvalid   : std_logic;
    signal w_par_tlast    : std_logic;
    signal w_par_tready   : std_logic;

    -- Parser master output (monitor mode - not forwarded)
    signal w_m_tdata      : std_logic_vector(63 downto 0);
    signal w_m_tkeep      : std_logic_vector(7  downto 0);
    signal w_m_tvalid     : std_logic;
    signal w_m_tlast      : std_logic;

    -- Parser status
    signal w_irq          : std_logic;

    -- Unused parsed fields (available for ILA probing)
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
    -- Block design instantiation
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
            sw_tdata          => w_sw_tdata_128,
            sw_tkeep          => w_sw_tkeep_16,
            sw_tready         => w_sw_tready,
            fclk1             => w_fclk1,
            parser_rst_n      => w_parser_rstn,
            parser_irq        => w_irq
        );

    -- -------------------------------------------------------------------------
    -- 128->64 bit width splitter
    -- Presents two 64-bit sub-beats per 128-bit BD beat.
    -- Sub-beat 0: upper half [127:64] (first in network order)
    -- Sub-beat 1: lower half  [63:0]  (second in network order)
    -- tready to BD only asserted when sub-beat 1 is consumed by parser.
    -- -------------------------------------------------------------------------
    p_split : process(w_fclk1, w_parser_rstn)
    begin
        if w_parser_rstn = '0' then
            r_sub_beat  <= '0';
            r_tdata_lo  <= (others => '0');
            r_tkeep_lo  <= (others => '0');
        elsif rising_edge(w_fclk1) then
            if w_par_tvalid = '1' and w_par_tready = '1' then
                if r_sub_beat = '0' then
                    -- Upper half consumed - latch lower half, advance
                    r_tdata_lo <= w_sw_tdata_128(63 downto 0);
                    r_tkeep_lo <= w_sw_tkeep_16(7 downto 0);
                    r_sub_beat <= '1';
                else
                    -- Lower half consumed - ready for next 128-bit word
                    r_sub_beat <= '0';
                end if;
            end if;
        end if;
    end process;

    -- Combinational mux: select which half to present to parser
    w_par_tdata  <= w_sw_tdata_128(127 downto 64) when r_sub_beat = '0'
                    else r_tdata_lo;
    w_par_tkeep  <= w_sw_tkeep_16(15 downto 8)    when r_sub_beat = '0'
                    else r_tkeep_lo;
    -- tvalid: always '1' on sub-beat 1 (data already latched);
    --         on sub-beat 0 we need the BD to have valid data
    w_par_tvalid <= '1'          when r_sub_beat = '1'
                    else '1';    -- BD drives continuously; treat as always valid
    -- tlast: only assert on sub-beat 1 of the final beat
    w_par_tlast  <= '0'          when r_sub_beat = '0'
                    else '0';    -- tlast not available from BD tap; tie low

    -- tready back to BD: only when lower sub-beat is consumed
    w_sw_tready  <= w_par_tready when r_sub_beat = '1' else '0';

    -- -------------------------------------------------------------------------
    -- Packet parser (TAP / monitor mode)
    -- -------------------------------------------------------------------------
    u_parser : packet_parser_top
        generic map (C_AXIS_DATA_WIDTH => 64)
        port map (
            clk               => w_fclk1,
            rst_n             => w_parser_rstn,
            s_axis_tdata      => w_par_tdata,
            s_axis_tkeep      => w_par_tkeep,
            s_axis_tvalid     => w_par_tvalid,
            s_axis_tlast      => w_par_tlast,
            s_axis_tready     => w_par_tready,
            m_axis_tdata      => w_m_tdata,
            m_axis_tkeep      => w_m_tkeep,
            m_axis_tvalid     => w_m_tvalid,
            m_axis_tlast      => w_m_tlast,
            m_axis_tready     => '1',
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