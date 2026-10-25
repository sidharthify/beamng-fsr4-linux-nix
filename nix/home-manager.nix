self:
{ config, lib, pkgs, ... }:

let
  cfg = config.programs.bng-fsr4;
in
{
  options.programs.bng-fsr4 = {
    enable = lib.mkEnableOption "the FSR 4.1.1 Vulkan layer for native-Linux BeamNG.drive";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      description = "The bng-fsr4 package.";
    };

    menuMod = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Install the BeamNG mod that renames "FSR 3 (custom)" to "FSR 4.1.1 (Linux)" in the
        anti-aliasing menu while the layer runs.
      '';
    };

    userFolder = lib.mkOption {
      type = lib.types.str;
      default = "${config.xdg.dataHome}/BeamNG/BeamNG.drive/current";
      description = "BeamNG's user folder; the menu mod goes into its mods directory.";
    };
  };

  config = lib.mkIf cfg.enable {
    # Implicit layer: inert until the game is started with BNG_FSR4=1.
    xdg.dataFile."vulkan/implicit_layer.d/VkLayer_bng_fsr4.json".source =
      "${cfg.package}/share/vulkan/implicit_layer.d/VkLayer_bng_fsr4.json";

    home.file = lib.mkIf cfg.menuMod {
      "${lib.removePrefix "${config.home.homeDirectory}/" cfg.userFolder}/mods/bng_fsr4_linux.zip".source =
        "${cfg.package}/share/bng-fsr4/bng_fsr4_linux.zip";
    };
  };
}
