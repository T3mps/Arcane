// The ONE cgltf implementation TU in the whole tree (F2c Task 1) -- the same
// single-TU discipline StbImpl.cpp keeps beside it, and for the same reason: cgltf
// is a single-header library whose implementation is emitted by a macro, so a second
// definer anywhere would merge a second copy of every symbol (LNK2005). Every other
// consumer -- MeshImporter.cpp here, VendorSmokeTest.cpp in ArcaneTests -- includes
// <cgltf.h> for DECLARATIONS ONLY and links against this TU's symbols.
//
// CGLTF_IMPLEMENTATION is defined here and NOWHERE ELSE. Grep-verifiable:
//   grep -rn "CGLTF_IMPLEMENTATION" --include=*.cpp --include=*.hpp .
// must return exactly this file.

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
