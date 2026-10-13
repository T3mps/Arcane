// LinkProbe, object B: a static registrar ALONE in its object (the
// ARC_COMPONENT / ARC_SYSTEM shape). Nothing references this object -- not
// main, not the anchor -- so only whole-archiving the library puts it in the
// link, and only its init-table entry keeps it past --gc-sections.

extern "C" void LinkProbe_Register(int bit);

namespace
{
    struct ComponentRegistrar
    {
        ComponentRegistrar() { LinkProbe_Register(2); }
    };
    ComponentRegistrar g_componentRegistrar;
}
