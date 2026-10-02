{
  mkPackage,
  defaultQQ,
}:
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.programs.linuxqq-wayland-fix;
in
{
  options.programs.linuxqq-wayland-fix = {
    enable = lib.mkEnableOption "linuxqq-wayland-fix（修复 Linux QQ 在 Wayland 下的屏幕共享、共享电脑声音、剪贴板和截图）";

    package = lib.mkOption {
      type = lib.types.package;
      default = mkPackage {
        inherit pkgs;
        overrides = { qqPackage = cfg.qq; };
      };
      defaultText = lib.literalExpression "linuxqq-wayland-fix.packages.\${system}.default";
      description = ''
        使用的 linuxqq-wayland-fix 包。默认用当前 pkgs 由 flake 里的
        {file}`nix/package.nix` 构建，因此跟随你系统的 nixpkgs。
      '';
    };

    qq = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = defaultQQ pkgs;
      defaultText = lib.literalExpression "(if pkgs.config.allowUnfree then pkgs.qq else null)";
      description = ''
        被修复的 QQ，即 nixpkgs 的 {option}`pkgs.qq`（unfree 许可，需要
        `nixpkgs.config.allowUnfree = true`）。它的路径会被写进启动器的
        `QQ_WAYLAND_FIX_QQ`，并装进 {option}`environment.systemPackages`
        —— 原生 QQ 的菜单项和 `Icon=qq` 用的图标都来自它。

        设为 `null` 时不安装 QQ，启动器退回自己在 PATH 里找 `linuxqq` 或 `qq`
        （或者 `/opt/QQ/qq`），或者读你设置的 `QQ_WAYLAND_FIX_QQ`。
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ] ++ lib.optional (cfg.qq != null) cfg.qq;

    warnings = lib.optional (!(config.xdg.portal.enable or false)) ''
      programs.linuxqq-wayland-fix 已启用，但没有启用 xdg-desktop-portal。
      QQ 的屏幕共享走 portal 的 ScreenCast 接口，缺了它共享会失败；
      niri / Hyprland 这类合成器一般还要额外启用对应的后端
      （xdg-desktop-portal-gnome / xdg-desktop-portal-hyprland）。
    '';
  };
}
