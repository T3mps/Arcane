// A game TU must not reach World's solver. The member is private.
#include <Arcane/Physics2D.hpp>
void Probe(Arcane::Physics2D::World& w) { (void)w.world; }
int main() { return 0; }
