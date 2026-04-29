{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    # Сборка ядра
    gnumake
    gcc
    bc
    flex
    bison
    perl
    pkg-config
    ncurses
    openssl
    elfutils
    elfutils.dev
    zlib
    zlib.dev

    # Утилиты
    kmod
    cpio
    rsync
    wget

    # QEMU
    qemu

    # Отладка
    gdb
    pahole
  ];

  hardeningDisable = [ "all" ];

  shellHook = ''
    export KERNEL_DIR="$PWD/linux-6.13.9"
    export C_INCLUDE_PATH="${pkgs.elfutils.dev}/include:${pkgs.zlib.dev}/include''${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}"
    export LIBRARY_PATH="${pkgs.elfutils}/lib:${pkgs.zlib}/lib''${LIBRARY_PATH:+:$LIBRARY_PATH}"
    export NIX_CFLAGS_COMPILE=""
  '';
}
