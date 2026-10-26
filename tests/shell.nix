let
  source = builtins.getFlake (builtins.getEnv "HYPERDHT_DIR");
  pkgs = import source.inputs.nixpkgs { system = builtins.currentSystem; };
in pkgs.mkShell {
  packages = [ pkgs.gcc14 pkgs.pkg-config ];
  buildInputs = [ pkgs.libsodium ];
}
