#include <Arcane/Ecs.hpp>
using Probe = Arcane::View<int>;
int main() { return static_cast<int>(sizeof(Probe)); }
