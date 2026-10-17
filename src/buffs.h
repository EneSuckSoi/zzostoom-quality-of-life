#pragma once
#include <string>
#include <vector>

// Timed buffs on the player (SpEffects that the game shows with an icon), with the time left.
namespace zz::buffs {

struct Active {
    int id = 0;
    std::string name;
    float remaining = 0;  // seconds
    float duration = 0;
};

// Render thread; refreshes about 10 times per second.
void Update(bool show_unnamed);
const std::vector<Active>& List();
std::string Status();

}  // namespace zz::buffs
