{
  description = "Hold off sleep while Caps Lock is on";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";

  outputs = { self, nixpkgs }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" ];
      forEach = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});

      mkPackage = pkgs: pkgs.stdenv.mkDerivation {
        pname = "capslock-daemon";
        version = "1.0.0";
        src = ./.;
        dontConfigure = true;

        buildPhase = ''
          runHook preBuild
          $CC -O2 -Wall -Wextra -o capslock-daemon src/capslock-daemon.c \
            -framework IOKit -framework CoreFoundation
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          install -D -m 0755 capslock-daemon $out/bin/capslock-daemon
          runHook postInstall
        '';

        meta = {
          description = "Hold off sleep while Caps Lock is on";
          license = nixpkgs.lib.licenses.mit;
          platforms = nixpkgs.lib.platforms.darwin;
          mainProgram = "capslock-daemon";
        };
      };
    in
    {
      packages = forEach (pkgs: {
        default = mkPackage pkgs;
        capslock-daemon = mkPackage pkgs;
      });

      # nix-darwin module. Runs as root — SleepDisabled is a privileged setting.
      # ProgramArguments points at the store path directly rather than through a
      # shell, so the background item is named for itself in System Settings.
      darwinModules.default = { pkgs, ... }: {
        launchd.daemons.capslock-daemon = {
          serviceConfig = {
            ProgramArguments = [ "${mkPackage pkgs}/bin/capslock-daemon" ];
            RunAtLoad = true;
            KeepAlive = true;
            ThrottleInterval = 10;
            StandardOutPath = "/var/log/capslock-daemon.log";
            StandardErrorPath = "/var/log/capslock-daemon.log";
          };
        };
      };
    };
}
