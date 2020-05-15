/**
 * @file TimeKeeper.cpp
 *
 * This module contains the implementations of the TimeKeeper class.
 */

#include "TimeKeeper.hpp"

#include <O9KClock/Clock.hpp>

/**
 * This contains the private properties of a TimeKeeper class instance.
 */
struct TimeKeeper::Impl {
    /**
     * This is used to interface with the operating system's notion of time.
     */
    O9K::Clock time;
};

TimeKeeper::~TimeKeeper() noexcept = default;

TimeKeeper::TimeKeeper()
    : impl_(new Impl())
{
}

double TimeKeeper::GetCurrentTime() {
    return impl_->time.GetTime();
}
