#pragma once

/**
 * @file Service.hpp
 *
 * This module declares the Service implementation.
 */

#include <memory>
#include <SystemAbstractions/Service.hpp>

/**
 * This is an implementation of SystemAbstraction::Service which runs
 * Alfred.
 */
class Service
    : public SystemAbstractions::Service
{
    // Lifecycle Methods
public:
    ~Service() noexcept;
    Service(const Service&) = delete;
    Service(Service&&) noexcept;
    Service& operator=(const Service&) = delete;
    Service& operator=(Service&&) noexcept;

    // Public Methods
public:
    /**
     * This is the constructor of the class.
     */
    Service();

    /**
     * This method is called from the main program to run the service either
     * as a daemon or attached to the user's terminal directly.
     *
     * @param[in] argc
     *     This is the number of command-line arguments given
     *     to the program.
     *
     * @param[in] argv
     *     This points to an array of pointers to the command-line
     *     arguments given to the program.
     *
     * @return
     *     The exit code that should be returned from the main function
     *     of the program is returned.
     */
    int Main(int argc, char* argv[]);

    // SystemAbstractions::Service
protected:
    virtual int Run() override;
    virtual void Stop() override;
    virtual std::string GetServiceName() const override;

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
    std::shared_ptr< Impl > impl_;
};
