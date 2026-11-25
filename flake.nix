{
  description = "Moorhuhn Adventure 2: Der Fluch des Goldes — native Linux remaster (build & asset tooling)";

  # The game itself is built from your own mha2.exe (make lift && make), which a pure
  # flake build cannot see, so this flake provides development shells only:
  #   nix develop          -> build & play (same as shell.nix)
  #   nix develop .#assets -> HD asset pack tooling (PyTorch/CUDA, spandrel, OpenCV, …)
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      pkgsUnfree = import nixpkgs { inherit system; config.allowUnfree = true; };
    in {
      devShells.${system} = {
        default = import ./shell.nix { inherit pkgs; };
        assets = import ./nix/assets-shell.nix { pkgs = pkgsUnfree; };
      };
      formatter.${system} = pkgs.nixfmt;
    };
}
