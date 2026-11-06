#pragma once
#include <string>


namespace core::runtime::SDL {
	// A supplied result owns the original failure text; the host reports it once.
	bool Init(std::string* error = nullptr);
	void Shutdown();
}
