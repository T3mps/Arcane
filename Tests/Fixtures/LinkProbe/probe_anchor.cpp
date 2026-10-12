// LinkProbe, object A (engine modularization P5, ruling R11's WSL2 probe): the
// module anchor ArcModule_LinkProbe -- the symbol a package's forced reference
// (-u / /INCLUDE:) names -- plus a static registrar in the SAME object. main
// references nothing here, so only the forced reference pulls this object in.

extern "C" void LinkProbe_Register(int bit);

extern "C" void ArcModule_LinkProbe() {}

namespace
{
    struct AnchorRegistrar
    {
        AnchorRegistrar() { LinkProbe_Register(1); }
    };
    AnchorRegistrar g_anchorRegistrar;
}
