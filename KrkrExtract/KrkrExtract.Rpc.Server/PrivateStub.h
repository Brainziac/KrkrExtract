#pragma once

#include <my.h>
#include <Stubs.h>

enum class RaiseErrorType
{
	RAISE_ERROR_HEARTBEAT_TIMEOUT = 0,
	RAISE_ERROR_REMOTE_CRASH = 1,
	RAISE_ERROR_SECRET_DISMATCH = 2,
	RAISE_ERROR_INVALID_PID = 3,
	RAISE_ERROR_REMOTE_DEAD = 4,
	RAISE_ERROR_REMOTE_PRIVILEGED = 5,
	RAISE_ERROR_REMOTE_GENEROUS = 6
};




