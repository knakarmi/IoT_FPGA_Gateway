--------------------------------------------------------------------------------
--! @File name:     aes256gcm_wrapper
--! @Description:   Vivado IP packaging wrapper for AES-256-GCM.
--!                 All port widths are explicit integers so Vivado's IP
--!                 packager does not need to resolve package constants.
--!
--!                 Fixed configuration:
--!                   AES Mode    : 256
--!                   # Rounds    : 14
--!                   Size        : L (one-way pipeline)
--!                   Pipe stages : 0
--!                   GF-mul IPs  : 1
--------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

--------------------------------------------------------------------------------
entity aes256gcm_wrapper is
    port(
        -- Clock and reset
        rst_i                           : in  std_logic;
        clk_i                           : in  std_logic;

        -- Mode / control
        aes_gcm_mode_i                  : in  std_logic_vector(1 downto 0);
        aes_gcm_enc_dec_i               : in  std_logic;
        aes_gcm_pipe_reset_i            : in  std_logic;

        -- Key loading (256-bit key, left-aligned)
        aes_gcm_key_word_val_i          : in  std_logic_vector(3 downto 0);
        aes_gcm_key_word_i              : in  std_logic_vector(255 downto 0);

        -- IV / ICB
        aes_gcm_iv_val_i                : in  std_logic;
        aes_gcm_iv_i                    : in  std_logic_vector(95 downto 0);
        aes_gcm_icb_start_cnt_i         : in  std_logic;
        aes_gcm_icb_stop_cnt_i          : in  std_logic;

        -- GHASH / AAD
        aes_gcm_ghash_pkt_val_i         : in  std_logic;
        aes_gcm_ghash_aad_bval_i        : in  std_logic_vector(15 downto 0);
        aes_gcm_ghash_aad_i             : in  std_logic_vector(127 downto 0);

        -- Data input (PT for encrypt, CT for decrypt)
        aes_gcm_data_in_bval_i          : in  std_logic_vector(15 downto 0);
        aes_gcm_data_in_i               : in  std_logic_vector(127 downto 0);

        -- Data output (CT for encrypt, PT for decrypt)
        aes_gcm_ready_o                 : out std_logic;
        aes_gcm_data_out_val_o          : out std_logic;
        aes_gcm_data_out_bval_o         : out std_logic_vector(15 downto 0);
        aes_gcm_data_out_o              : out std_logic_vector(127 downto 0);

        -- Authentication tag
        aes_gcm_ghash_tag_val_o         : out std_logic;
        aes_gcm_ghash_tag_o             : out std_logic_vector(127 downto 0);

        -- Overflow flag
        aes_gcm_icb_cnt_overflow_o      : out std_logic
    );
end entity;

--------------------------------------------------------------------------------
architecture arch_aes256gcm_wrapper of aes256gcm_wrapper is

    component top_aes_gcm is
        port(
            rst_i                           : in  std_logic;
            clk_i                           : in  std_logic;
            aes_gcm_mode_i                  : in  std_logic_vector(1 downto 0);
            aes_gcm_enc_dec_i               : in  std_logic;
            aes_gcm_pipe_reset_i            : in  std_logic;
            aes_gcm_key_word_val_i          : in  std_logic_vector(3 downto 0);
            aes_gcm_key_word_i              : in  std_logic_vector(255 downto 0);
            aes_gcm_iv_val_i                : in  std_logic;
            aes_gcm_iv_i                    : in  std_logic_vector(95 downto 0);
            aes_gcm_icb_start_cnt_i         : in  std_logic;
            aes_gcm_icb_stop_cnt_i          : in  std_logic;
            aes_gcm_ghash_pkt_val_i         : in  std_logic;
            aes_gcm_ghash_aad_bval_i        : in  std_logic_vector(15 downto 0);
            aes_gcm_ghash_aad_i             : in  std_logic_vector(127 downto 0);
            aes_gcm_data_in_bval_i          : in  std_logic_vector(15 downto 0);
            aes_gcm_data_in_i               : in  std_logic_vector(127 downto 0);
            aes_gcm_ready_o                 : out std_logic;
            aes_gcm_data_out_val_o          : out std_logic;
            aes_gcm_data_out_bval_o         : out std_logic_vector(15 downto 0);
            aes_gcm_data_out_o              : out std_logic_vector(127 downto 0);
            aes_gcm_ghash_tag_val_o         : out std_logic;
            aes_gcm_ghash_tag_o             : out std_logic_vector(127 downto 0);
            aes_gcm_icb_cnt_overflow_o      : out std_logic
        );
    end component;

begin

    u_top_aes_gcm : top_aes_gcm
        port map(
            rst_i                           => rst_i,
            clk_i                           => clk_i,
            aes_gcm_mode_i                  => aes_gcm_mode_i,
            aes_gcm_enc_dec_i               => aes_gcm_enc_dec_i,
            aes_gcm_pipe_reset_i            => aes_gcm_pipe_reset_i,
            aes_gcm_key_word_val_i          => aes_gcm_key_word_val_i,
            aes_gcm_key_word_i              => aes_gcm_key_word_i,
            aes_gcm_iv_val_i                => aes_gcm_iv_val_i,
            aes_gcm_iv_i                    => aes_gcm_iv_i,
            aes_gcm_icb_start_cnt_i         => aes_gcm_icb_start_cnt_i,
            aes_gcm_icb_stop_cnt_i          => aes_gcm_icb_stop_cnt_i,
            aes_gcm_ghash_pkt_val_i         => aes_gcm_ghash_pkt_val_i,
            aes_gcm_ghash_aad_bval_i        => aes_gcm_ghash_aad_bval_i,
            aes_gcm_ghash_aad_i             => aes_gcm_ghash_aad_i,
            aes_gcm_data_in_bval_i          => aes_gcm_data_in_bval_i,
            aes_gcm_data_in_i               => aes_gcm_data_in_i,
            aes_gcm_ready_o                 => aes_gcm_ready_o,
            aes_gcm_data_out_val_o          => aes_gcm_data_out_val_o,
            aes_gcm_data_out_bval_o         => aes_gcm_data_out_bval_o,
            aes_gcm_data_out_o              => aes_gcm_data_out_o,
            aes_gcm_ghash_tag_val_o         => aes_gcm_ghash_tag_val_o,
            aes_gcm_ghash_tag_o             => aes_gcm_ghash_tag_o,
            aes_gcm_icb_cnt_overflow_o      => aes_gcm_icb_cnt_overflow_o
        );

end architecture;
