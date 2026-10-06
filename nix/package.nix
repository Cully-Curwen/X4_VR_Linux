# The X4 VR mod's native Linux build (README.md, section Linux). Classic Nix, no flakes:
#   nix-build                          (from the repository root, via default.nix)
#   pkgs.callPackage ./nix/package.nix {}   (from configuration.nix)
# Build it from the same nixpkgs as the system's Steam, so the libraries it loads into X4 match
# the glibc X4 runs with (on NixOS, X4's Steam runtime container uses the host's glibc).
{ lib, stdenv, cmake, vulkan-headers, openvr }:

stdenv.mkDerivation {
  pname = "x4vr";
  version = "0.2.0-phase1";

  src = lib.cleanSourceWith {
    src = ../.;
    # Only what the Linux build reads; Windows-only parts and local build trees stay out.
    filter = path: type:
      let rel = lib.removePrefix (toString ../. + "/") (toString path);
      in lib.cleanSourceFilter path type
         && !(lib.hasPrefix "build" rel) && !(lib.hasPrefix "result" rel)
         && !(lib.hasPrefix "reports" rel) && !(lib.hasPrefix "external" rel);
  };

  nativeBuildInputs = [ cmake ];
  buildInputs = [ vulkan-headers ];
  # OpenVR's client library is compiled into the mod from source (see linux/CMakeLists.txt).
  cmakeFlags = [ "-DX4VR_LINUX=ON" "-DOPENVR_SOURCE_DIR=${openvr.src}" ];
  doCheck = true;

  meta = {
    description = "Stereo VR for the native Linux build of X4: Foundations (work in progress)";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
    mainProgram = "x4vr";
  };
}
