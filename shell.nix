# Copyright 0xB10C, willcl-ark
{ pkgs ? import (fetchTarball "https://github.com/nixos/nixpkgs/archive/nixos-24.11.tar.gz") {},
  spareCores ? 0,
  withClang ? false,
  withDebug ? false,
}:
let
  inherit (pkgs.lib) optionals strings;
  inherit (pkgs) stdenv;

  # Add mlc binary fetching
  mlcBinary = pkgs.fetchurl {
    url = "https://github.com/becheran/mlc/releases/download/v0.18.0/mlc-x86_64-linux";
    sha256 = "sha256-jbdp+UlFybBE+o567L398hbcWHsG8aQGqYYf5h9JRkw=";
  };
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

  # Create a derivation for mlc
  mlc = pkgs.runCommand "mlc" {} ''
    mkdir -p $out/bin
    cp ${mlcBinary} $out/bin/mlc
    chmod +x $out/bin/mlc
  '';

  binDirs =
    [ "\$PWD/build/src" ];
  configureFlags =
    [ "--with-boost-libdir=$NIX_BOOST_LIB_DIR" ]
    ++ optionals withClang [ "CXX=clang++" "CC=clang" ]
    ++ optionals withDebug [ "--enable-debug" ];
  jobs =
    if (strings.hasSuffix "linux" builtins.currentSystem) then "$(($(nproc)-${toString spareCores}))"
    else if (strings.hasSuffix "darwin" builtins.currentSystem) then "$(($(sysctl -n hw.physicalcpu)-${toString spareCores}))"
    else "6";
in pkgs.mkShell {
    nativeBuildInputs = with pkgs; [
      autoconf
      automake
      libtool
      pkg-config
      boost
      libevent
      zeromq
      sqlite
      clang_18

      # tests
      hexdump

      # compiler output caching per
      # https://github.com/bitcoin/bitcoin/blob/master/doc/productivity.md#cache-compilations-with-ccache
      ccache

      # for newer cmake building
      cmake

      # depends
      byacc

      # debugging
      gdb

      # tracing
      libsystemtap
      linuxPackages.bpftrace
      linuxPackages.bcc

    ];
    buildInputs = with pkgs; [
      just
      bash

      # lint requirements
      cargo
      git
      mlc
      ruff
      rustc
      rustup
      shellcheck
      python310
      uv

      # Benchmarking
      flamegraph
      hyperfine
      jq
      linuxKernel.packages.linux_6_6.perf
      perf-tools
      util-linux
    ];

    # Modifies the Nix clang++ wrapper to avoid warning:
    # "_FORTIFY_SOURCE requires compiling with optimization (-O)"
    hardeningDisable = if withDebug then [ "all" ] else [ ];

    shellHook = ''
      echo "Bitcoin Core build nix-shell"
      echo ""
      echo "Setting up python venv"

      # fixes libstdc++ issues and libgl.so issues
      export LD_LIBRARY_PATH=${stdenv.cc.cc.lib}/lib/:$LD_LIBRARY_PATH

      uv venv --python 3.10
      source .venv/bin/activate
      uv pip install -r pyproject.toml

      BCC_EGG=${pkgs.linuxPackages.bcc}/${pkgs.python3.sitePackages}/bcc-${pkgs.linuxPackages.bcc.version}-py3.${pkgs.python3.sourceVersion.minor}.egg

      echo "adding bcc egg to PYTHONPATH: $BCC_EGG"
      if [ -f $BCC_EGG ]; then
        export PYTHONPATH="$PYTHONPATH:$BCC_EGG"
        echo ""
      else
        echo "The bcc egg $BCC_EGG does not exist. Maybe the python or bcc version is different?"
      fi

      echo "adding ${builtins.concatStringsSep ":" binDirs} to \$PATH to make running built binaries more natural"
      export PATH=$PATH:${builtins.concatStringsSep ":" binDirs};

      rustup default stable
      rustup component add rustfmt
    '';
}
