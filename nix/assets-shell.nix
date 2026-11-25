# HD asset pack tooling (tools/upscale_transformer.py, tools/remaster_videos_4k.py,
# tools/polish_cursors.py, tools/compress_hd_lossless.py, tools/upscale_ai.py,
# tools/upscale_videos.py). Not needed to build or play the game; see shell.nix.
#
#   nix-shell nix/assets-shell.nix        (or: nix develop .#assets)
#
# PyTorch comes as the prebuilt CUDA wheel (torch-bin, unfree because of CUDA); without
# an NVIDIA GPU the scripts fall back to the CPU (very slow for the full pack).
{ pkgs ? import <nixpkgs> { config.allowUnfree = true; } }:

let
  py = pkgs.python3Packages;

  # spandrel loads the 4x-ClearRealityV1 (SPAN) model; not packaged in nixpkgs.
  spandrel = py.buildPythonPackage rec {
    pname = "spandrel";
    version = "0.4.2";
    format = "wheel";
    src = py.fetchPypi {
      inherit pname version format;
      dist = "py3";
      python = "py3";
      hash = "sha256-bJPj7L6w5Uj9LfRaYFRys0wWFCh8VrUbszze965SNbU=";
    };
    dependencies = with py; [ torch-bin torchvision-bin safetensors numpy einops typing-extensions ];
    pythonImportsCheck = [ "spandrel" ];
  };

  spandrel-extra-arches = py.buildPythonPackage rec {
    pname = "spandrel_extra_arches";
    version = "0.2.0";
    format = "wheel";
    src = py.fetchPypi {
      inherit pname version format;
      dist = "py3";
      python = "py3";
      hash = "sha256-FS5Ku9EIRwp3txYDACnD5/xvYWAOx2kmnJSkbUoTC08=";
    };
    dependencies = with py; [ spandrel torch-bin torchvision-bin numpy einops typing-extensions ];
    pythonImportsCheck = [ "spandrel_extra_arches" ];
  };

  python = pkgs.python3.withPackages (ps: [
    ps.torch-bin
    ps.torchvision-bin
    spandrel
    spandrel-extra-arches
    ps.numpy
    ps.pillow
    ps.opencv4
    ps.capstone
  ]);
in
pkgs.mkShell {
  packages = [
    python
    pkgs.ffmpeg                 # video decode/encode for the cutscene tools
    pkgs.realesrgan-ncnn-vulkan # Vulkan alternative (tools/upscale_ai.py, tools/upscale_videos.py)
  ];

  shellHook = ''
    if [ ! -f tools/models/sr/4x-ClearRealityV1.safetensors ]; then
      echo "note: put the 4x-ClearRealityV1 model at tools/models/sr/4x-ClearRealityV1.safetensors" >&2
    fi
  '';
}
