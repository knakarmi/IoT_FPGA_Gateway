-- =============================================================================
-- IoT Gateway - Week 4
-- File   : tb_packet_parser.vhd
-- Purpose: Self-checking VHDL testbench for packet_parser_top
--
-- Fix v2: Added one extra clock cycle after wait_done before checking
--         o_pkt_valid / o_pkt_drop. The protocol_classifier registers its
--         output on the cycle AFTER i_parse_done fires, so we must wait
--         one extra cycle for those signals to be stable.
--
-- Test Cases:
--  TC1 - Valid Raw IoT UDP packet              -> CLASS_RAW_IOT, pkt_valid
--  TC2 - Valid MQTT UDP packet (port 1883)     -> CLASS_MQTT,    pkt_valid
--  TC3 - Valid CoAP UDP packet (port 5683)     -> CLASS_COAP,    pkt_valid
--  TC4 - Non-IPv4 EtherType (ARP 0x0806)      -> pkt_drop,  error_code="001"
--  TC5 - IP proto not UDP (TCP=0x06)           -> pkt_drop,  error_code="010"
--  TC6 - Fragmented IP packet (MF bit set)     -> pkt_drop,  error_code="011"
--  TC7 - Frame truncated during IP header      -> parse_done, error_code="101"
--  TC8 - Back-pressure from downstream         -> correct data despite stalls
-- =============================================================================

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb_packet_parser is
end entity tb_packet_parser;

architecture sim of tb_packet_parser is

    constant CLK_PERIOD : time    := 5 ns;  -- 200 MHz
    constant DATA_WIDTH : integer := 64;
    constant KEEP_WIDTH : integer := DATA_WIDTH / 8;

    -- -------------------------------------------------------------------------
    -- DUT component declaration
    -- -------------------------------------------------------------------------
    component packet_parser_top is
        generic (
            C_AXIS_DATA_WIDTH  : integer := 64;
            C_S_AXI_ADDR_WIDTH : integer := 5
        );
        port (
            clk               : in  std_logic;
            rst_n             : in  std_logic;
            s_axis_tdata      : in  std_logic_vector(DATA_WIDTH-1 downto 0);
            s_axis_tkeep      : in  std_logic_vector(KEEP_WIDTH-1 downto 0);
            s_axis_tvalid     : in  std_logic;
            s_axis_tlast      : in  std_logic;
            s_axis_tready     : out std_logic;
            m_axis_tdata      : out std_logic_vector(DATA_WIDTH-1 downto 0);
            m_axis_tkeep      : out std_logic_vector(KEEP_WIDTH-1 downto 0);
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
    -- Signals
    -- -------------------------------------------------------------------------
    signal clk           : std_logic := '0';
    signal rst_n         : std_logic := '0';

    signal s_axis_tdata  : std_logic_vector(DATA_WIDTH-1 downto 0) := (others => '0');
    signal s_axis_tkeep  : std_logic_vector(KEEP_WIDTH-1 downto 0) := (others => '0');
    signal s_axis_tvalid : std_logic := '0';
    signal s_axis_tlast  : std_logic := '0';
    signal s_axis_tready : std_logic;

    signal m_axis_tdata  : std_logic_vector(DATA_WIDTH-1 downto 0);
    signal m_axis_tkeep  : std_logic_vector(KEEP_WIDTH-1 downto 0);
    signal m_axis_tvalid : std_logic;
    signal m_axis_tlast  : std_logic;
    signal m_axis_tready : std_logic := '1';

    signal o_eth_dst_mac     : std_logic_vector(47 downto 0);
    signal o_eth_src_mac     : std_logic_vector(47 downto 0);
    signal o_eth_type        : std_logic_vector(15 downto 0);
    signal o_ip_src          : std_logic_vector(31 downto 0);
    signal o_ip_dst          : std_logic_vector(31 downto 0);
    signal o_ip_proto        : std_logic_vector(7  downto 0);
    signal o_udp_src_port    : std_logic_vector(15 downto 0);
    signal o_udp_dst_port    : std_logic_vector(15 downto 0);
    signal o_udp_length      : std_logic_vector(15 downto 0);
    signal o_iot_device_id   : std_logic_vector(15 downto 0);
    signal o_iot_msg_type    : std_logic_vector(7  downto 0);
    signal o_iot_seq_num     : std_logic_vector(15 downto 0);
    signal o_iot_payload_len : std_logic_vector(15 downto 0);
    signal o_protocol_class  : std_logic_vector(2  downto 0);
    signal o_pkt_valid       : std_logic;
    signal o_pkt_drop        : std_logic;
    signal o_parse_done      : std_logic;
    signal o_error_code      : std_logic_vector(2 downto 0);
    signal o_irq             : std_logic;

    signal pass_cnt : integer := 0;
    signal fail_cnt : integer := 0;

    -- -------------------------------------------------------------------------
    -- Helper: pack 8 bytes big-endian into a 64-bit word
    -- -------------------------------------------------------------------------
    function pack8 (
        b0,b1,b2,b3,b4,b5,b6,b7 : std_logic_vector(7 downto 0)
    ) return std_logic_vector is
    begin
        return b0 & b1 & b2 & b3 & b4 & b5 & b6 & b7;
    end function;

begin

    -- -------------------------------------------------------------------------
    -- DUT instantiation
    -- -------------------------------------------------------------------------
    dut : packet_parser_top
        generic map (C_AXIS_DATA_WIDTH => DATA_WIDTH)
        port map (
            clk               => clk,
            rst_n             => rst_n,
            s_axis_tdata      => s_axis_tdata,
            s_axis_tkeep      => s_axis_tkeep,
            s_axis_tvalid     => s_axis_tvalid,
            s_axis_tlast      => s_axis_tlast,
            s_axis_tready     => s_axis_tready,
            m_axis_tdata      => m_axis_tdata,
            m_axis_tkeep      => m_axis_tkeep,
            m_axis_tvalid     => m_axis_tvalid,
            m_axis_tlast      => m_axis_tlast,
            m_axis_tready     => m_axis_tready,
            o_eth_dst_mac     => o_eth_dst_mac,
            o_eth_src_mac     => o_eth_src_mac,
            o_eth_type        => o_eth_type,
            o_ip_src          => o_ip_src,
            o_ip_dst          => o_ip_dst,
            o_ip_proto        => o_ip_proto,
            o_udp_src_port    => o_udp_src_port,
            o_udp_dst_port    => o_udp_dst_port,
            o_udp_length      => o_udp_length,
            o_iot_device_id   => o_iot_device_id,
            o_iot_msg_type    => o_iot_msg_type,
            o_iot_seq_num     => o_iot_seq_num,
            o_iot_payload_len => o_iot_payload_len,
            o_protocol_class  => o_protocol_class,
            o_pkt_valid       => o_pkt_valid,
            o_pkt_drop        => o_pkt_drop,
            o_parse_done      => o_parse_done,
            o_error_code      => o_error_code,
            o_irq             => o_irq
        );

    -- -------------------------------------------------------------------------
    -- Clock generation
    -- -------------------------------------------------------------------------
    clk <= not clk after CLK_PERIOD / 2;

    -- -------------------------------------------------------------------------
    -- Stimulus
    -- -------------------------------------------------------------------------
    p_stim : process

        variable v_last : std_logic;  -- set before each drive_beat, avoids
                                      -- inline conditionals in procedure args

        -- ------------------------------------------------------------
        -- drive_beat: assert one AXIS beat, wait until s_axis_tready
        -- ------------------------------------------------------------
        procedure drive_beat (
            constant data : in std_logic_vector(DATA_WIDTH-1 downto 0);
            constant keep : in std_logic_vector(KEEP_WIDTH-1 downto 0);
            constant last : in std_logic
        ) is
        begin
            wait until rising_edge(clk);
            s_axis_tdata  <= data;
            s_axis_tkeep  <= keep;
            s_axis_tvalid <= '1';
            s_axis_tlast  <= last;
            loop
                wait until rising_edge(clk);
                exit when s_axis_tready = '1';
            end loop;
            s_axis_tvalid <= '0';
            s_axis_tlast  <= '0';
        end procedure;

        -- ------------------------------------------------------------
        -- check_cond: print [PASS] or [FAIL] and update counters
        -- ------------------------------------------------------------
        procedure check_cond (
            constant test_name : in string;
            constant cond      : in boolean
        ) is
        begin
            if cond then
                report "[PASS] " & test_name severity note;
                pass_cnt <= pass_cnt + 1;
            else
                report "[FAIL] " & test_name severity error;
                fail_cnt <= fail_cnt + 1;
            end if;
            wait until rising_edge(clk);
        end procedure;

        -- ------------------------------------------------------------
        -- wait_done: spin until o_parse_done pulses (or timeout)
        -- Then waits ONE extra cycle so the protocol_classifier output
        -- (o_pkt_valid / o_pkt_drop) has time to register.
        -- ------------------------------------------------------------
        procedure wait_done is
            variable cnt : integer := 0;
        begin
            loop
                wait until rising_edge(clk);
                cnt := cnt + 1;
                exit when o_parse_done = '1' or cnt >= 300;
            end loop;
            -- ONE extra cycle: classifier registers its output on the
            -- rising edge AFTER it sees i_parse_done = '1'
            wait until rising_edge(clk);
        end procedure;

        -- ------------------------------------------------------------
        -- idle_gap: small pause between test cases
        -- ------------------------------------------------------------
        procedure idle_gap is
        begin
            for i in 0 to 3 loop
                wait until rising_edge(clk);
            end loop;
        end procedure;

        -- ------------------------------------------------------------
        -- send_iot_pkt: build and drive a full IoT/UDP/IPv4/Eth frame
        --   trunc_after = 0  -> full 9-beat packet
        --   trunc_after = N  -> assert tlast on beat N and stop
        -- ------------------------------------------------------------
        procedure send_iot_pkt (
            constant dst_port     : in std_logic_vector(15 downto 0);
            constant ip_proto_in  : in std_logic_vector(7  downto 0);
            constant ip_flags     : in std_logic_vector(7  downto 0);
            constant iot_msg_type : in std_logic_vector(7  downto 0);
            constant trunc_after  : in integer;
            constant pay_byte     : in std_logic_vector(7  downto 0)
        ) is
        begin
            -- Beat 0: eth_dst[47:0] | eth_src[47:32]
            if trunc_after = 1 then v_last := '1'; else v_last := '0'; end if;
            drive_beat(
                pack8(x"FF",x"FF",x"FF",x"FF",x"FF",x"FF",x"AA",x"BB"),
                x"FF", v_last);
            if trunc_after = 1 then return; end if;

            -- Beat 1: eth_src[31:0] | EtherType=0x0800 | ip_ver_ihl=0x45 | ip_dscp
            if trunc_after = 2 then v_last := '1'; else v_last := '0'; end if;
            drive_beat(
                pack8(x"CC",x"DD",x"EE",x"FF",x"08",x"00",x"45",x"00"),
                x"FF", v_last);
            if trunc_after = 2 then return; end if;

            -- Beat 2: ip_total_len | ip_id | ip_flags | ip_ttl | ip_proto
            if trunc_after = 3 then v_last := '1'; else v_last := '0'; end if;
            drive_beat(
                pack8(x"00",x"4A",x"12",x"34",ip_flags,x"00",x"40",ip_proto_in),
                x"FF", v_last);
            if trunc_after = 3 then return; end if;

            -- Beat 3: ip_cksum | ip_src=192.168.1.10 | ip_dst[31:16]=10.0
            if trunc_after = 4 then v_last := '1'; else v_last := '0'; end if;
            drive_beat(
                pack8(x"00",x"00",x"C0",x"A8",x"01",x"0A",x"0A",x"00"),
                x"FF", v_last);
            if trunc_after = 4 then return; end if;

            -- Beat 4: ip_dst[15:0] | udp_sport=4000 | udp_dport | udp_len_hi
            if trunc_after = 5 then v_last := '1'; else v_last := '0'; end if;
            drive_beat(
                pack8(x"00",x"01",x"0F",x"A0",
                      dst_port(15 downto 8), dst_port(7 downto 0),
                      x"00", x"38"),
                x"FF", v_last);
            if trunc_after = 5 then return; end if;

            -- Beat 5: udp_len_lo | udp_cksum | iot_dev_id=0xBEEF | iot_msg_type
            --         iot_flags  | iot_seq_hi
            if trunc_after = 6 then v_last := '1'; else v_last := '0'; end if;
            drive_beat(
                pack8(x"38",x"00",x"00",x"BE",x"EF",
                      iot_msg_type, x"00", x"00"),
                x"FF", v_last);
            if trunc_after = 6 then return; end if;

            -- Beat 6: iot_seq_lo | iot_payload_len=0x0018 | 5 payload bytes
            if trunc_after = 7 then v_last := '1'; else v_last := '0'; end if;
            drive_beat(
                pack8(x"07",x"00",x"18",
                      pay_byte, pay_byte, pay_byte, pay_byte, pay_byte),
                x"FF", v_last);
            if trunc_after = 7 then return; end if;

            -- Beat 7: 8 payload bytes (not last)
            drive_beat(
                pay_byte & pay_byte & pay_byte & pay_byte &
                pay_byte & pay_byte & pay_byte & pay_byte,
                x"FF", '0');

            -- Beat 8: final 8 payload bytes (last)
            drive_beat(
                pay_byte & pay_byte & pay_byte & pay_byte &
                pay_byte & pay_byte & pay_byte & pay_byte,
                x"FF", '1');
        end procedure;

    begin
        -- Reset sequence
        rst_n         <= '0';
        s_axis_tvalid <= '0';
        s_axis_tlast  <= '0';
        m_axis_tready <= '1';
        for i in 0 to 7 loop wait until rising_edge(clk); end loop;
        rst_n <= '1';
        for i in 0 to 3 loop wait until rising_edge(clk); end loop;

        -- ============================================================
        -- TC1: Valid Raw IoT UDP (port 9000, msg_type = 0x01)
        -- ============================================================
        report "--- TC1: Valid Raw IoT UDP ---" severity note;
        send_iot_pkt(
            dst_port     => std_logic_vector(to_unsigned(9000, 16)),
            ip_proto_in  => x"11",
            ip_flags     => x"00",
            iot_msg_type => x"01",
            trunc_after  => 0,
            pay_byte     => x"AB");
        wait_done;  -- waits for parse_done + 1 extra cycle for classifier
        check_cond("TC1 pkt_valid",    o_pkt_valid = '1');
        check_cond("TC1 no drop",      o_pkt_drop  = '0');
        check_cond("TC1 class RAW",    o_protocol_class = "000");
        check_cond("TC1 ip_src",       o_ip_src = x"C0A8010A");
        check_cond("TC1 ip_dst",       o_ip_dst = x"0A000001");
        check_cond("TC1 udp_dport",    o_udp_dst_port    = std_logic_vector(to_unsigned(9000,16)));
        check_cond("TC1 iot_dev_id",   o_iot_device_id   = x"BEEF");
        check_cond("TC1 iot_msg_type", o_iot_msg_type    = x"01");
        check_cond("TC1 iot_seq",      o_iot_seq_num     = x"0007");
        check_cond("TC1 pay_len",      o_iot_payload_len = x"0018");
        idle_gap;

        -- ============================================================
        -- TC2: MQTT (port 1883)
        -- ============================================================
        report "--- TC2: MQTT (port 1883) ---" severity note;
        send_iot_pkt(
            dst_port     => std_logic_vector(to_unsigned(1883, 16)),
            ip_proto_in  => x"11",
            ip_flags     => x"00",
            iot_msg_type => x"00",
            trunc_after  => 0,
            pay_byte     => x"30");
        wait_done;
        check_cond("TC2 pkt_valid",  o_pkt_valid = '1');
        check_cond("TC2 class MQTT", o_protocol_class = "001");
        idle_gap;

        -- ============================================================
        -- TC3: CoAP (port 5683)
        -- ============================================================
        report "--- TC3: CoAP (port 5683) ---" severity note;
        send_iot_pkt(
            dst_port     => std_logic_vector(to_unsigned(5683, 16)),
            ip_proto_in  => x"11",
            ip_flags     => x"00",
            iot_msg_type => x"00",
            trunc_after  => 0,
            pay_byte     => x"64");
        wait_done;
        check_cond("TC3 pkt_valid",  o_pkt_valid = '1');
        check_cond("TC3 class CoAP", o_protocol_class = "010");
        idle_gap;

        -- ============================================================
        -- TC4: ARP EtherType (0x0806) -- expect pkt_drop, error_code="001"
        -- Note: for error cases the FSM drains the packet without calling
        -- S_DONE, so o_parse_done fires when drain completes and
        -- o_pkt_drop is set by the classifier one cycle later.
        -- ============================================================
        report "--- TC4: Non-IPv4 EtherType (ARP) ---" severity note;
        drive_beat(x"FFFFFFFFFFFFAABB", x"FF", '0');
        drive_beat(pack8(x"CC",x"DD",x"EE",x"FF",x"08",x"06",x"00",x"01"),
                   x"FF", '0');
        drive_beat((others => '0'), x"FF", '0');
        drive_beat((others => '0'), x"FF", '1');
        wait_done;
        check_cond("TC4 pkt_drop",     o_pkt_drop   = '1');
        check_cond("TC4 error_code=1", o_error_code = "001");
        idle_gap;

        -- ============================================================
        -- TC5: IP protocol TCP (0x06) -- expect pkt_drop, error_code="010"
        -- ============================================================
        report "--- TC5: IP proto TCP (not UDP) ---" severity note;
        send_iot_pkt(
            dst_port     => std_logic_vector(to_unsigned(9000, 16)),
            ip_proto_in  => x"06",
            ip_flags     => x"00",
            iot_msg_type => x"01",
            trunc_after  => 0,
            pay_byte     => x"CC");
        wait_done;
        check_cond("TC5 pkt_drop",     o_pkt_drop   = '1');
        check_cond("TC5 error_code=2", o_error_code = "010");
        idle_gap;

        -- ============================================================
        -- TC6: Fragmented IP (MF=1) -- expect pkt_drop, error_code="011"
        -- ============================================================
        report "--- TC6: Fragmented IP ---" severity note;
        send_iot_pkt(
            dst_port     => std_logic_vector(to_unsigned(9000, 16)),
            ip_proto_in  => x"11",
            ip_flags     => x"20",
            iot_msg_type => x"01",
            trunc_after  => 0,
            pay_byte     => x"DD");
        wait_done;
        check_cond("TC6 pkt_drop",     o_pkt_drop   = '1');
        check_cond("TC6 error_code=3", o_error_code = "011");
        idle_gap;

        -- ============================================================
        -- TC7: Truncated frame (tlast at beat 3) -- expect error_code="101"
        -- ============================================================
        report "--- TC7: Truncated header ---" severity note;
        send_iot_pkt(
            dst_port     => std_logic_vector(to_unsigned(9000, 16)),
            ip_proto_in  => x"11",
            ip_flags     => x"00",
            iot_msg_type => x"01",
            trunc_after  => 3,
            pay_byte     => x"EE");
        wait_done;
        check_cond("TC7 parse_done",   o_parse_done = '1' or o_error_code /= "000");
        check_cond("TC7 error_code=5", o_error_code = "101");
        idle_gap;

        -- ============================================================
        -- TC8: Back-pressure (m_axis_tready held low during header)
        -- ============================================================
        report "--- TC8: Back-pressure ---" severity note;
        m_axis_tready <= '0';

        drive_beat(pack8(x"FF",x"FF",x"FF",x"FF",x"FF",x"FF",x"AA",x"BB"), x"FF", '0');
        drive_beat(pack8(x"CC",x"DD",x"EE",x"FF",x"08",x"00",x"45",x"00"), x"FF", '0');
        drive_beat(pack8(x"00",x"4A",x"12",x"34",x"00",x"00",x"40",x"11"), x"FF", '0');
        drive_beat(pack8(x"00",x"00",x"C0",x"A8",x"01",x"0A",x"0A",x"00"), x"FF", '0');
        -- UDP dst port = 1883 = 0x075B
        drive_beat(pack8(x"00",x"01",x"0F",x"A0",x"07",x"5B",x"00",x"38"), x"FF", '0');
        drive_beat(pack8(x"38",x"00",x"00",x"BE",x"EF",x"00",x"00",x"00"), x"FF", '0');
        drive_beat(pack8(x"07",x"00",x"18",x"99",x"99",x"99",x"99",x"99"), x"FF", '0');
        -- Release back-pressure then send remaining payload
        for i in 0 to 9 loop wait until rising_edge(clk); end loop;
        m_axis_tready <= '1';
        drive_beat(x"9999999999999999", x"FF", '0');
        drive_beat(x"9999999999999999", x"FF", '1');
        wait_done;
        check_cond("TC8 pkt_valid",  o_pkt_valid = '1');
        check_cond("TC8 class MQTT", o_protocol_class = "001");
        idle_gap;

        -- ============================================================
        -- Summary
        -- ============================================================
        report "==============================================" severity note;
        report "PASSED: " & integer'image(pass_cnt) &
               "   FAILED: " & integer'image(fail_cnt) severity note;
        report "==============================================" severity note;

        if fail_cnt = 0 then
            report "ALL TESTS PASSED" severity note;
        else
            report "SOME TESTS FAILED - review output above" severity failure;
        end if;

        wait;
    end process p_stim;

    -- -------------------------------------------------------------------------
    -- Watchdog
    -- -------------------------------------------------------------------------
    p_watchdog : process
    begin
        wait for 1000 us;
        report "SIMULATION TIMEOUT" severity failure;
    end process p_watchdog;

end architecture sim;