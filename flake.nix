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
      # `command`, not a bare store path in ProgramArguments: nix-darwin wraps it
      # in `wait4path /nix/store`. The Nix Store volume is mounted after
      # LaunchDaemons load at boot, so a bare path fails with EX_CONFIG and
      # launchd never retries. The cost is that System Settings lists the
      # background item as `sh` rather than by name.
      darwinModules.default = { pkgs, ... }: {
        launchd.daemons.capslock-daemon = {
          command = "${mkPackage pkgs}/bin/capslock-daemon";
          serviceConfig = {
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
