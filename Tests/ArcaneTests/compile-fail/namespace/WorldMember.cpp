// A game TU must not reach PhysicsWorld2D's solver. The member is private.
#include <Arcane/Physics2D.hpp>
void Probe(Arcane::PhysicsWorld2D& w) { (void)w.world; }
int main() { return 0; }
