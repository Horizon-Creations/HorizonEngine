#pragma once
#include "Types/Defines.h"
#include "Types/Enums.h"
#include <string>

class HE_API Logger
{
public:
	using LogLevel = HE::LogLevel;
	static void Log(LogLevel level, const char* message);
private:
	std::string logfile;
};