-- =============================================================================
-- IoT Gateway - Week 4
-- File   : pkt_header_fsm.vhd
-- Purpose: AXI4-Stream header parser FSM
--
-- Packet Format (byte offsets from start of frame):
--
--  [ Ethernet Header  ]  14 bytes   (bytes  0-13)
--    dst_mac[47:0]    :  bytes  0- 5
--    src_mac[47:0]    :  bytes  6-11
--    eth_type[15:0]   :  bytes 12-13  (0x0800 = IPv4)
--
--  [ IPv4 Header      ]  20 bytes min (bytes 14-33)
--    ver_ihl[7:0]     :  byte  14
--    dscp_ecn[7:0]    :  byte  15
--    total_len[15:0]  :  bytes 16-17
--    id[15:0]         :  bytes 18-19
--    flags_foff[15:0] :  bytes 20-21  (fragmented packets dropped)
--    ttl[7:0]         :  byte  22
--    protocol[7:0]    :  byte  23     (0x11 = UDP)
--    checksum[15:0]   :  bytes 24-25
--    src_ip[31:0]     :  bytes 26-29
--    dst_ip[31:0]     :  bytes 30-33
--
--  [ UDP Header       ]  8 bytes    (bytes 34-41)
--    src_port[15:0]   :  bytes 34-35
--    dst_port[15:0]   :  bytes 36-37
--    length[15:0]     :  bytes 38-39
--    checksum[15:0]   :  bytes 40-41
--
--  [ IoT App Header   ]  8 bytes    (bytes 42-49)
--    device_id[15:0]  :  bytes 42-43
--    msg_type[7:0]    :  byte  44
--    flags[7:0]       :  byte  45
--    seq_num[15:0]    :  bytes 46-47
--    payload_len[15:0]:  bytes 48-49
--
--  [ Payload          ]  variable   (bytes 50+)
--
-- AXI4-Stream beat mapping (64-bit = 8 bytes/beat):
--   Beat 0  : bytes  0- 7
--   Beat 1  : bytes  8-15
--   Beat 2  : bytes 16-23
--   Beat 3  : bytes 24-31
--   Beat 4  : bytes 32-39
--   Beat 5  : bytes 40-47
--   Beat 6  : bytes 48-55  (last header beat + first 5 payload bytes)
--   Beat 7+ : full payload beats
-- =============================================================================

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity pkt_header_fsm is
    generic (
        C_AXIS_DATA_WIDTH : integer := 64
    );
    port (
        clk   : in std_logic;
        rst_n : in std_logic;

        -- Slave AXI4-Stream (raw frame from DMA)
        s_axis_tdata  : in  std_logic_vector(C_AXIS_DATA_WIDTH-1   downto 0);
        s_axis_tkeep  : in  std_logic_vector(C_AXIS_DATA_WIDTH/8-1 downto 0);
        s_axis_tvalid : in  std_logic;
        s_axis_tlast  : in  std_logic;
        s_axis_tready : out std_logic;

        -- Parsed header outputs (registered)
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

        -- Master AXI4-Stream (payload beats forwarded after header strip)
        m_axis_tdata  : out std_logic_vector(C_AXIS_DATA_WIDTH-1   downto 0);
        m_axis_tkeep  : out std_logic_vector(C_AXIS_DATA_WIDTH/8-1 downto 0);
        m_axis_tvalid : out std_logic;
        m_axis_tlast  : out std_logic;
        m_axis_tready : in  std_logic;

        -- Status
        o_parse_done  : out std_logic;
        o_parse_error : out std_logic;
        o_error_code  : out std_logic_vector(2 downto 0)
        -- Error codes:
        --   "000" = OK
        --   "001" = EtherType not IPv4
        --   "010" = IP proto not UDP
        --   "011" = IP fragmented
        --   "100" = Payload too short
        --   "101" = Truncated header (tlast during header)
    );
end entity pkt_header_fsm;

architecture rtl of pkt_header_fsm is

    constant BEAT_BYTES : integer := C_AXIS_DATA_WIDTH / 8;  -- 8

    -- EtherType / IP protocol constants
    constant ETHERTYPE_IPV4 : std_logic_vector(15 downto 0) := x"0800";
    constant IPPROTO_UDP    : std_logic_vector(7  downto 0) := x"11";

    -- FSM state type
    type t_state is (
        S_IDLE,
        S_ETH_1,
        S_IP_0,
        S_IP_1,
        S_UDP_IOT_0,
        S_UDP_IOT_1,
        S_IOT_PAY,
        S_PAYLOAD,
        S_DONE,
        S_ERROR,
        S_DRAIN
    );

    signal state : t_state;

    -- Registered header fields
    signal r_eth_dst_mac     : std_logic_vector(47 downto 0);
    signal r_eth_src_mac     : std_logic_vector(47 downto 0);
    signal r_eth_type        : std_logic_vector(15 downto 0);
    signal r_ip_src          : std_logic_vector(31 downto 0);
    signal r_ip_dst          : std_logic_vector(31 downto 0);
    signal r_ip_proto        : std_logic_vector(7  downto 0);
    signal r_ip_ihl          : std_logic_vector(7  downto 0);
    signal r_udp_src_port    : std_logic_vector(15 downto 0);
    signal r_udp_dst_port    : std_logic_vector(15 downto 0);
    signal r_udp_length      : std_logic_vector(15 downto 0);
    signal r_iot_device_id   : std_logic_vector(15 downto 0);
    signal r_iot_msg_type    : std_logic_vector(7  downto 0);
    signal r_iot_seq_num     : std_logic_vector(15 downto 0);
    signal r_iot_payload_len : std_logic_vector(15 downto 0);

    -- Cross-beat staging registers
    signal r_ip_dst_hi    : std_logic_vector(15 downto 0);
    signal r_udp_len_hi   : std_logic_vector(7  downto 0);
    signal r_iot_seq_hi   : std_logic_vector(7  downto 0);

    -- Registered outputs
    signal r_tready     : std_logic;
    signal r_m_tdata    : std_logic_vector(C_AXIS_DATA_WIDTH-1   downto 0);
    signal r_m_tkeep    : std_logic_vector(C_AXIS_DATA_WIDTH/8-1 downto 0);
    signal r_m_tvalid   : std_logic;
    signal r_m_tlast    : std_logic;
    signal r_parse_done : std_logic;
    signal r_parse_err  : std_logic;
    signal r_error_code : std_logic_vector(2 downto 0);

    -- Convenience: individual bytes of the current beat (network order)
    -- b(0) = tdata[63:56], b(7) = tdata[7:0]
    type t_byte_arr is array (0 to BEAT_BYTES-1) of std_logic_vector(7 downto 0);
    signal b : t_byte_arr;

    signal fire : std_logic;  -- beat accepted this cycle

begin

    -- -------------------------------------------------------------------------
    -- Byte extraction (big-endian network order)
    -- -------------------------------------------------------------------------
    GEN_BYTES : for i in 0 to BEAT_BYTES-1 generate
        b(i) <= s_axis_tdata(C_AXIS_DATA_WIDTH-1 - i*8 downto
                             C_AXIS_DATA_WIDTH-8 - i*8);
    end generate;

    fire <= s_axis_tvalid and r_tready;

    -- -------------------------------------------------------------------------
    -- FSM
    -- -------------------------------------------------------------------------
    p_fsm : process(clk, rst_n)
    begin
        if rst_n = '0' then
            state            <= S_IDLE;
            r_tready         <= '0';
            r_m_tvalid       <= '0';
            r_m_tlast        <= '0';
            r_parse_done     <= '0';
            r_parse_err      <= '0';
            r_error_code     <= "000";
            r_eth_dst_mac    <= (others => '0');
            r_eth_src_mac    <= (others => '0');
            r_eth_type       <= (others => '0');
            r_ip_src         <= (others => '0');
            r_ip_dst         <= (others => '0');
            r_ip_proto       <= (others => '0');
            r_ip_ihl         <= (others => '0');
            r_udp_src_port   <= (others => '0');
            r_udp_dst_port   <= (others => '0');
            r_udp_length     <= (others => '0');
            r_iot_device_id  <= (others => '0');
            r_iot_msg_type   <= (others => '0');
            r_iot_seq_num    <= (others => '0');
            r_iot_payload_len<= (others => '0');
            r_ip_dst_hi      <= (others => '0');
            r_udp_len_hi     <= (others => '0');
            r_iot_seq_hi     <= (others => '0');
            r_m_tdata        <= (others => '0');
            r_m_tkeep        <= (others => '0');

        elsif rising_edge(clk) then
            -- Default: deassert single-cycle pulses.
            -- r_parse_err is NOT cleared here - it is sticky and held high
            -- from error detection until the next packet starts in S_IDLE.
            -- This ensures the classifier, which samples one cycle after
            -- parse_done, still sees i_parse_error = '1'.
            r_parse_done <= '0';
            r_m_tvalid   <= '0';
            r_m_tlast    <= '0';

            case state is

                -- --------------------------------------------------------------
                when S_IDLE =>
                    r_tready    <= '1';
                    r_parse_err <= '0';  -- clear error at start of each new packet
                    if fire = '1' then
                        -- Beat 0: eth_dst[47:0] | eth_src[47:32]
                        r_eth_dst_mac        <= b(0) & b(1) & b(2) & b(3) & b(4) & b(5);
                        r_eth_src_mac(47 downto 32) <= b(6) & b(7);
                        if s_axis_tlast = '1' then
                            r_parse_err  <= '1';
                            r_error_code <= "101";
                            state        <= S_DONE;
                        else
                            state <= S_ETH_1;
                        end if;
                    end if;

                -- --------------------------------------------------------------
                when S_ETH_1 =>
                    -- Beat 1: eth_src[31:0] | eth_type | ip_ver_ihl | ip_dscp
                    if fire = '1' then
                        r_eth_src_mac(31 downto 0) <= b(0) & b(1) & b(2) & b(3);
                        r_eth_type                 <= b(4) & b(5);
                        -- IHL = lower nibble of b(6), in 32-bit words => *4 for bytes
                        r_ip_ihl <= std_logic_vector(
                            shift_left(resize(unsigned(b(6)(3 downto 0)), 8), 2));

                        if s_axis_tlast = '1' then
                            r_parse_err  <= '1';
                            r_error_code <= "101";
                            state        <= S_DONE;
                        elsif (b(4) & b(5)) /= ETHERTYPE_IPV4 then
                            r_parse_err  <= '1';
                            r_error_code <= "001";
                            state        <= S_DRAIN;
                        else
                            state <= S_IP_0;
                        end if;
                    end if;

                -- --------------------------------------------------------------
                when S_IP_0 =>
                    -- Beat 2: ip_total_len | ip_id | ip_flags_foff | ip_ttl | ip_proto
                    if fire = '1' then
                        r_ip_proto <= b(7);

                        -- Check fragmentation: MF bit = b(4)[5], offset = b(4)[4:0] & b(5)
                        if (b(4)(5) = '1') or
                           (b(4)(4 downto 0) /= "00000") or
                           (b(5) /= x"00") then
                            r_parse_err  <= '1';
                            r_error_code <= "011";
                            state        <= S_DRAIN;
                        elsif s_axis_tlast = '1' then
                            r_parse_err  <= '1';
                            r_error_code <= "101";
                            state        <= S_DONE;
                        else
                            state <= S_IP_1;
                        end if;
                    end if;

                -- --------------------------------------------------------------
                when S_IP_1 =>
                    -- Beat 3: ip_cksum | ip_src[31:0] | ip_dst[31:16]
                    if fire = '1' then
                        r_ip_src    <= b(2) & b(3) & b(4) & b(5);
                        r_ip_dst_hi <= b(6) & b(7);

                        if s_axis_tlast = '1' then
                            r_parse_err  <= '1';
                            r_error_code <= "101";
                            state        <= S_DONE;
                        elsif r_ip_proto /= IPPROTO_UDP then
                            r_parse_err  <= '1';
                            r_error_code <= "010";
                            state        <= S_DRAIN;
                        else
                            state <= S_UDP_IOT_0;
                        end if;
                    end if;

                -- --------------------------------------------------------------
                when S_UDP_IOT_0 =>
                    -- Beat 4: ip_dst[15:0] | udp_sport | udp_dport | udp_len[15:8]
                    if fire = '1' then
                        r_ip_dst       <= r_ip_dst_hi & b(0) & b(1);
                        r_udp_src_port <= b(2) & b(3);
                        r_udp_dst_port <= b(4) & b(5);
                        r_udp_len_hi   <= b(6);
                        -- b(7) = udp_len LSB, consumed next beat

                        if s_axis_tlast = '1' then
                            r_parse_err  <= '1';
                            r_error_code <= "101";
                            state        <= S_DONE;
                        else
                            state <= S_UDP_IOT_1;
                        end if;
                    end if;

                -- --------------------------------------------------------------
                when S_UDP_IOT_1 =>
                    -- Beat 5: udp_len_lo | udp_cksum | iot_dev_id | iot_msg_type
                    --         iot_flags | iot_seq[15:8]
                    if fire = '1' then
                        r_udp_length    <= r_udp_len_hi & b(0);
                        -- b(1),b(2) = udp checksum (not stored)
                        r_iot_device_id <= b(3) & b(4);
                        r_iot_msg_type  <= b(5);
                        -- b(6) = iot_flags (not stored)
                        r_iot_seq_hi    <= b(7);

                        if s_axis_tlast = '1' then
                            r_parse_err  <= '1';
                            r_error_code <= "101";
                            state        <= S_DONE;
                        else
                            state <= S_IOT_PAY;
                        end if;
                    end if;

                -- --------------------------------------------------------------
                when S_IOT_PAY =>
                    -- Beat 6: iot_seq_lo | iot_payload_len | first 5 payload bytes
                    if fire = '1' then
                        r_iot_seq_num     <= r_iot_seq_hi & b(0);
                        r_iot_payload_len <= b(1) & b(2);

                        -- Forward first payload beat (bytes 3-7), top-aligned
                        r_m_tdata  <= b(3) & b(4) & b(5) & b(6) & b(7) & x"000000";
                        r_m_tkeep  <= "11111000";   -- 5 valid bytes
                        r_m_tvalid <= '1';
                        r_m_tlast  <= s_axis_tlast;

                        if s_axis_tlast = '1' then
                            state <= S_DONE;
                        else
                            state <= S_PAYLOAD;
                        end if;
                    end if;

                -- --------------------------------------------------------------
                when S_PAYLOAD =>
                    -- Pass through remaining payload beats with back-pressure
                    r_tready <= m_axis_tready;

                    if fire = '1' then
                        r_m_tdata  <= s_axis_tdata;
                        r_m_tkeep  <= s_axis_tkeep;
                        r_m_tvalid <= '1';
                        r_m_tlast  <= s_axis_tlast;

                        if s_axis_tlast = '1' then
                            state <= S_DONE;
                        end if;
                    end if;

                -- --------------------------------------------------------------
                when S_DONE =>
                    r_tready     <= '0';
                    r_parse_done <= '1';
                    state        <= S_IDLE;

                -- --------------------------------------------------------------
                when S_ERROR =>
                    r_parse_err <= '1';
                    r_tready    <= '1';
                    state       <= S_DRAIN;

                -- --------------------------------------------------------------
                when S_DRAIN =>
                    -- Consume and discard remaining beats of a bad packet
                    r_tready <= '1';
                    if fire = '1' and s_axis_tlast = '1' then
                        r_parse_done <= '1';
                        state        <= S_IDLE;
                    end if;

                when others =>
                    state <= S_IDLE;

            end case;
        end if;
    end process p_fsm;

    -- -------------------------------------------------------------------------
    -- Output port connections
    -- -------------------------------------------------------------------------
    s_axis_tready     <= r_tready;
    m_axis_tdata      <= r_m_tdata;
    m_axis_tkeep      <= r_m_tkeep;
    m_axis_tvalid     <= r_m_tvalid;
    m_axis_tlast      <= r_m_tlast;
    o_parse_done      <= r_parse_done;
    o_parse_error     <= r_parse_err;
    o_error_code      <= r_error_code;
    o_eth_dst_mac     <= r_eth_dst_mac;
    o_eth_src_mac     <= r_eth_src_mac;
    o_eth_type        <= r_eth_type;
    o_ip_src          <= r_ip_src;
    o_ip_dst          <= r_ip_dst;
    o_ip_proto        <= r_ip_proto;
    o_ip_ihl          <= r_ip_ihl;
    o_udp_src_port    <= r_udp_src_port;
    o_udp_dst_port    <= r_udp_dst_port;
    o_udp_length      <= r_udp_length;
    o_iot_device_id   <= r_iot_device_id;
    o_iot_msg_type    <= r_iot_msg_type;
    o_iot_seq_num     <= r_iot_seq_num;
    o_iot_payload_len <= r_iot_payload_len;

end architecture rtl;