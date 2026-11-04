{
  lib,
  stdenv,
  fetchFromGitLab,
  cmake,
  ninja,
  pkg-config,
  kdePackages,
  qt6,
}:

let
  inherit (kdePackages) extra-cmake-modules ki18n;
in

stdenv.mkDerivation rec {
  pname = "kdecoration";
  version = "git-master";

  # Pinned to the same KDE master commit the AUR PKGBUILD and COPR spec use
  # (_kdecoration_commit=d13049250c0ea1afc279aa8dc99243565c0d83e8). kinetic-we
  # uses KDecoration3::Style (server-side drop shadows for CSD windows), which
  # only exists on kdecoration master — the released 6.7.x lacks it.
  src = fetchFromGitLab {
    domain = "invent.kde.org";
    owner = "plasma";
    repo = "kdecoration";
    rev = "d13049250c0ea1afc279aa8dc99243565c0d83e8";
    hash = "sha256-4oqhnoVQjVWtqIurZQ3qCjRy3UCdNBjEJNDk8jaUz6Q=";
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    extra-cmake-modules
  ];

  # Deps from the pinned kdecoration CMakeLists: Qt6 Core/Gui/Test + KF6 I18n.
  buildInputs = [
    qt6.qtbase
    ki18n
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
    "-DBUILD_TESTING=OFF"
  ];

  # Library only (libkdecoration), no applications to wrap. Without this the
  # Qt hook aborts: "this derivation depends on qtbase, but no wrapping
  # behavior was specified".
  dontWrapQtApps = true;

  meta = with lib; {
    description = "KDE Window Manager decorations library (git master, for KineticWE)";
    homepage = "https://invent.kde.org/plasma/kdecoration";
    license = licenses.lgpl2Plus;
    platforms = platforms.linux;
  };
}
