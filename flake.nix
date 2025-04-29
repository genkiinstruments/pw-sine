{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs?shallow=1&ref=nixos-unstable";
  inputs.treefmt-nix.url = "github:numtide/treefmt-nix";
  inputs.treefmt-nix.inputs.nixpkgs.follows = "nixpkgs";

  outputs =
    {
      self,
      nixpkgs,
      treefmt-nix,
    }:
    let
      # Small tool to iterate over each systems
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      eachSystem = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Eval the treefmt modules from ./treefmt.nix
      treefmtEval = eachSystem (
        pkgs:
        treefmt-nix.lib.evalModule pkgs {
          programs.clang-format.enable = true;
          programs.nixfmt.enable = true;
          programs.deadnix.enable = true;
          programs.statix.enable = true;
        }
      );
    in
    {
      # for `nix fmt`
      formatter = eachSystem (pkgs: treefmtEval.${pkgs.system}.config.build.wrapper);

      # for `nix flake check`
      checks = eachSystem (
        pkgs:
        {
          formatting = treefmtEval.${pkgs.system}.config.build.check self;
        }
        // self.packages.${pkgs.system}
      );

      # for `nix build`
      packages = eachSystem (pkgs: {
        audio-src = pkgs.callPackage ./audio-src { }; # Sine generator from pw examples
        pw-sine = pkgs.callPackage ./pw-sine { }; # Another sine generator
        audio-src-midi-chromatic = pkgs.callPackage ./audio-src-midi-chromatic { }; # Another sine generator with chromatic midi scale
        audio-src-midi-synth = pkgs.callPackage ./audio-src-midi-synth { }; # Sine midi synth
        default = self.packages.${pkgs.system}.pw-sine;
      });

      # for `nix develop`
      devShells = eachSystem (pkgs: {
        default = pkgs.mkShell {
          # Add build dependencies
          packages = [ self.packages.${pkgs.system}.default ];

          # Add environment variables
          env = { };

          # Load custom bash code
          shellHook = ''

          '';
        };
      });
    };
}
