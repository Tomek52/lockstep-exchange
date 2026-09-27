#pragma once
// Fixture for the architecture self-test: every include below violates the
// domain rules, so check_includes.cmake must FAIL on this directory.
#include <thread>
#include <grpcpp/grpcpp.h>
#include "lockstep/app/engine.hpp"
