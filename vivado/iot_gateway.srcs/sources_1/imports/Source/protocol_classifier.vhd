-- =============================================================================
-- IoT Gateway - Week 4
-- File   : protocol_classifier.vhd
-- Fix v2 : Added i_parse_error port. Classification is now gated so that any
--          packet the FSM flagged as erroneous (fragmented, wrong proto, etc.)
--          is forced to CLASS_UNKNOWN / pkt_drop='1' regardless of header
--          field values. Without this, a fragmented IPv4/UDP packet would
--          pass classification because EtherType and ip_proto look valid.
-- =============================================================================

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity protocol_classifier is
    port (
        clk   : in std_logic;
        rst_n : in std_logic;

        -- Trigger: one-cycle pulse when header parse completes
        i_parse_done  : in std_logic;
        -- Error flag from the FSM (set same cycle as i_parse_done for errors)
        i_parse_error : in std_logic;

        -- Parsed fields from header FSM
        i_eth_type     : in std_logic_vector(15 downto 0);
        i_ip_proto     : in std_logic_vector(7  downto 0);
        i_udp_dst_port : in std_logic_vector(15 downto 0);
        i_iot_msg_type : in std_logic_vector(7  downto 0);

        -- Classification output (registered, stable until next parse_done)
        o_protocol_class : out std_logic_vector(2 downto 0);
        o_pkt_valid      : out std_logic;
        o_pkt_drop       : out std_logic
    );
end entity protocol_classifier;

architecture rtl of protocol_classifier is

    constant CLASS_RAW_IOT : std_logic_vector(2 downto 0) := "000";
    constant CLASS_MQTT    : std_logic_vector(2 downto 0) := "001";
    constant CLASS_COAP    : std_logic_vector(2 downto 0) := "010";
    constant CLASS_HTTP    : std_logic_vector(2 downto 0) := "011";
    constant CLASS_UNKNOWN : std_logic_vector(2 downto 0) := "111";

    constant PORT_MQTT      : std_logic_vector(15 downto 0) := std_logic_vector(to_unsigned(1883, 16));
    constant PORT_MQTT_TLS  : std_logic_vector(15 downto 0) := std_logic_vector(to_unsigned(8883, 16));
    constant PORT_COAP      : std_logic_vector(15 downto 0) := std_logic_vector(to_unsigned(5683, 16));
    constant PORT_COAP_DTLS : std_logic_vector(15 downto 0) := std_logic_vector(to_unsigned(5684, 16));
    constant PORT_HTTP      : std_logic_vector(15 downto 0) := std_logic_vector(to_unsigned(80,   16));
    constant PORT_HTTP_ALT  : std_logic_vector(15 downto 0) := std_logic_vector(to_unsigned(8080, 16));

    constant IOT_MSG_RAW  : std_logic_vector(7 downto 0) := x"01";
    constant IOT_MSG_BULK : std_logic_vector(7 downto 0) := x"02";
    constant IOT_MSG_CMD  : std_logic_vector(7 downto 0) := x"03";
    constant IOT_MSG_ACK  : std_logic_vector(7 downto 0) := x"04";

    constant ETHERTYPE_IPV4 : std_logic_vector(15 downto 0) := x"0800";
    constant IPPROTO_UDP    : std_logic_vector(7  downto 0) := x"11";

begin

    p_classify : process(clk, rst_n)
    begin
        if rst_n = '0' then
            o_protocol_class <= CLASS_UNKNOWN;
            o_pkt_valid      <= '0';
            o_pkt_drop       <= '0';

        elsif rising_edge(clk) then
            if i_parse_done = '1' then
                -- Default: unknown, drop
                o_protocol_class <= CLASS_UNKNOWN;
                o_pkt_valid      <= '0';
                o_pkt_drop       <= '1';

                -- Gate: if FSM flagged a parse error, force drop regardless of
                -- header contents. A fragmented IPv4/UDP packet still has valid
                -- EtherType and ip_proto so without this guard it would be
                -- incorrectly classified as a forwadable packet.
                if i_parse_error = '0' then
                    if i_eth_type = ETHERTYPE_IPV4 and i_ip_proto = IPPROTO_UDP then

                        if i_udp_dst_port = PORT_MQTT or i_udp_dst_port = PORT_MQTT_TLS then
                            o_protocol_class <= CLASS_MQTT;
                            o_pkt_valid      <= '1';
                            o_pkt_drop       <= '0';

                        elsif i_udp_dst_port = PORT_COAP or i_udp_dst_port = PORT_COAP_DTLS then
                            o_protocol_class <= CLASS_COAP;
                            o_pkt_valid      <= '1';
                            o_pkt_drop       <= '0';

                        elsif i_udp_dst_port = PORT_HTTP or i_udp_dst_port = PORT_HTTP_ALT then
                            o_protocol_class <= CLASS_HTTP;
                            o_pkt_valid      <= '1';
                            o_pkt_drop       <= '0';

                        elsif i_iot_msg_type = IOT_MSG_RAW  or
                              i_iot_msg_type = IOT_MSG_BULK or
                              i_iot_msg_type = IOT_MSG_CMD  or
                              i_iot_msg_type = IOT_MSG_ACK  then
                            o_protocol_class <= CLASS_RAW_IOT;
                            o_pkt_valid      <= '1';
                            o_pkt_drop       <= '0';
                        end if;
                    end if;
                end if;
                -- parse_error='1', non-IPv4/UDP, or unknown port/type:
                -- stays CLASS_UNKNOWN, pkt_drop='1'
            end if;
        end if;
    end process p_classify;

end architecture rtl;