{ lib, stdenv, vulkan-headers, zip, version }:

stdenv.mkDerivation {
  pname = "bng-fsr4";
  inherit version;

  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [ ./Makefile ./layer ./fsr411 ./packaging ./game-mod ./assets ];
  };

  nativeBuildInputs = [ zip ];
  buildInputs = [ vulkan-headers ];

  makeFlags = [ "PREFIX=${placeholder "out"}" ];
  enableParallelBuilding = true;

  meta = {
    description = "Vulkan layer that runs FSR 4.1.1 in place of BeamNG.drive's FSR 3 (custom) upscaler";
    license = with lib.licenses; [ gpl2Plus mit ];
    platforms = [ "x86_64-linux" ];
  };
}
