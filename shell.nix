# Copyright 0xB10C, willcl-ark
{ pkgs ? import
  (fetchTarball "https://github.com/nixos/nixpkgs/archive/nixos-24.11.tar.gz")
  { }, }:
let
  inherit (pkgs.lib) optionals strings;
  inherit (pkgs) stdenv;

  # Hyperfine
  # Included here because we need master for the `--conclude` flag from pr 719
  hyperfine = pkgs.rustPlatform.buildRustPackage rec {
    pname = "hyperfine";
    name = "hyperfine";
    version = "e3e86174d9e11dd3a8951990f279c3b85f5fc0b9";

    src = pkgs.fetchFromGitHub {
      owner = "sharkdp";
      repo = "hyperfine";
      rev = version;
      sha256 = "sha256-WCc7gURd8dFgUC8moxB7y16e1jNKtImwsfXnqU36IrE=";
    };

    nativeBuildInputs = with pkgs; [ sqlite ];

    cargoHash = "sha256-E46//75Dgg+XClhD2iV86PYYwEE7bLeYMLK5UkyRpyg=";

    meta = with pkgs.lib; {
      description = "A command-line benchmarking tool.";
      homepage = "https://github.com/sharkdp/hyperfine";
      license = licenses.mit;
    };
  };

in pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    autoconf
    automake
    boost
    ccache
    clang_18
    cmake
    libevent
    libtool
    pkg-config
    sqlite
    zeromq
  ];
  buildInputs = with pkgs; [
    just
    bash
    git
    shellcheck
    python310
    uv

    # Benchmarking
    cargo-flamegraph
    flamegraph
    hyperfine
    jq
    linuxKernel.packages.linux_6_6.perf
    perf-tools
    util-linux
  ];

  shellHook = ''
    echo "Bitcoin Core build nix-shell"
    echo ""
    echo "Setting up python venv"

    # fixes libstdc++ issues and libgl.so issues
    export LD_LIBRARY_PATH=${stdenv.cc.cc.lib}/lib/:$LD_LIBRARY_PATH

    uv venv --python 3.10
    source .venv/bin/activate
    uv pip install -r pyproject.toml
  '';
}