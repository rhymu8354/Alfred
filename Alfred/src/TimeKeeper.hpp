#pragma once

/**
 * @file TimeKeeper.hpp
 *
 * This module declares the TimeKeeper implementation.
 */

#include <Http/TimeKeeper.hpp>
#include <Timekeeping/Clock.hpp>
#include <memory>

/**
 * This is an implementation of Http::TimeKeeper which uses SystemAbstractions
 * and the standard library to track time.
 */
class TimeKeeper
    : public Http::TimeKeeper
    , public Timekeeping::Clock
{
    // Lifecycle Methods
public:
    ~TimeKeeper() noexcept;
    TimeKeeper(const TimeKeeper&) = delete;
    TimeKeeper(TimeKeeper&&) noexcept = delete;
    TimeKeeper& operator=(const TimeKeeper&) = delete;
    TimeKeeper& operator=(TimeKeeper&&) noexcept = delete;

    // Public Methods
public:
    /**
     * This is the constructor of the class.
     */
    TimeKeeper();

    // Http::TimeKeeper
    // Timekeeping::Clock
public:
    virtual double GetCurrentTime() override;

    // Private properties
private:
    /**
     * This is the type of structure that contains the private
     * properties of the instance.  It is defined in the implementation
     * and declared here to ensure that it is scoped inside the class.
     */
    struct Impl;

    /**
     * This contains the private properties of the instance.
     */
    std::unique_ptr< Impl > impl_;
};
