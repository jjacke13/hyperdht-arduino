{
  description = "Bounded HyperDHT endpoint development";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/b6018f87da91d19d0ab4cf979885689b469cdd41";
  outputs = { self, nixpkgs }: {
    devShells.x86_64-linux.default = let
      pkgs = import nixpkgs { system = "x86_64-linux"; };
    in pkgs.mkShell {
      packages = [ pkgs.gcc14 pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.binutils pkgs.gcc-arm-embedded pkgs.python3 pkgs.qemu ];
      buildInputs = [ pkgs.libsodium pkgs.libuv ];
    };
  };
}
