# Copyright 0xB10C

{ pkgs ? import (fetchTarball "https://github.com/nixos/nixpkgs/archive/nixos-unstable.tar.gz") {},
  bdbVersion ? "",
  spareCores ? 0,
  withClang ? false,
  withDebug ? false,
  withGui ? false,
}:
let
  inherit (pkgs.lib) optionals strings;

  # Add mlc binary fetching
  mlcBinary = pkgs.fetchurl {
    url = "https://github.com/becheran/mlc/releases/download/v0.18.0/mlc-x86_64-linux";
    sha256 = "sha256-jbdp+UlFybBE+o567L398hbcWHsG8aQGqYYf5h9JRkw=";
  };

  # Create a derivation for mlc
  mlc = pkgs.runCommand "mlc" {} ''
    mkdir -p $out/bin
    cp ${mlcBinary} $out/bin/mlc
    chmod +x $out/bin/mlc
  '';

  binDirs =
    [ "\$PWD/src" ]
    ++ optionals withGui [ "\$PWD/src/qt" ];
  configureFlags =
    [ "--with-boost-libdir=$NIX_BOOST_LIB_DIR" ]
    ++ optionals ((builtins.elem bdbVersion ["" "db48" "db5"]) || abort "Unsupported bdbVersion value: ${bdbVersion}") []
    ++ optionals (bdbVersion == "") [ "--without-bdb" ]
    ++ optionals (!(builtins.elem bdbVersion ["" "db48"])) [ "--with-incompatible-bdb" ]
    ++ optionals withClang [ "CXX=clang++" "CC=clang" ]
    ++ optionals withDebug [ "--enable-debug" ]
    ++ optionals withGui [
      "--with-gui=qt5"
      "--with-qt-bindir=${pkgs.qt5.qtbase.dev}/bin:${pkgs.qt5.qttools.dev}/bin"
    ];
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

    ]
    ++ lib.optionals (bdbVersion == "db48") [
      db48
    ]
    ++ lib.optionals (bdbVersion == "db5") [
      db5
    ]
    ++ lib.optionals withGui [
      # bitcoin-qt
      qt5.qtbase
      # required for bitcoin-qt for "LRELEASE" etc
      qt5.qttools
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
    ];

    # Modifies the Nix clang++ wrapper to avoid warning:
    # "_FORTIFY_SOURCE requires compiling with optimization (-O)"
    hardeningDisable = if withDebug then [ "all" ] else [ ];

    # Fixes xcb plugin error when trying to launch bitcoin-qt
    QT_QPA_PLATFORM_PLUGIN_PATH = if withGui then "${pkgs.qt5.qtbase.bin}/lib/qt-${pkgs.qt5.qtbase.version}/plugins/platforms" else "";

    shellHook = ''
      echo "Bitcoin Core build nix-shell"
      echo ""
      echo "Setting up python venv"

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
