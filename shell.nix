{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    gcc
    gnumake
    pkg-config
    SDL2
    SDL2_ttf
    ffmpeg
    libGL
    dejavu_fonts
    (python3.withPackages (ps: [ ps.capstone ]))
  ];

  shellHook = ''
    export MHA2_FONT_PATH="${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf"
    export MHA2_GAME_FONT_PATH="${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans-Bold.ttf"
  '';
}
