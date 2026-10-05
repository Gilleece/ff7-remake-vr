#pragma once
// Read console variables (as float) and GSystemResolution from the running game.

#include <string>
#include <vector>

namespace ff7vr::dev::cvars {

// "ok GSystemResolution=WxH r.ScreenPercentage=100 r.Foo=missing ..." or "err ...".
std::string read(const std::vector<std::string>& names);

// Sets a console variable at console priority (as typed into the console; not
// saved anywhere). "ok name=<value now> setby=0x..." or "err ...".
std::string set(const std::string& name, const std::string& value);

}  // namespace ff7vr::dev::cvars
