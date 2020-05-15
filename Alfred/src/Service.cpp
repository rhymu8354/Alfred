/**
 * @file Service.cpp
 *
 * This module contains the implementation of the Service class.
 */

#include "Service.hpp"
#include "TimeKeeper.hpp"

#include <AsyncData/Dispatcher.hpp>
#include <future>
#include <Http/Client.hpp>
#include <Http/Server.hpp>
#include <HttpNetworkTransport/HttpClientNetworkTransport.hpp>
#include <HttpNetworkTransport/HttpServerNetworkTransport.hpp>
#include <Json/Value.hpp>
#include <mutex>
#include <signal.h>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <SystemAbstractions/DiagnosticsStreamReporter.hpp>
#include <SystemAbstractions/File.hpp>
#include <time.h>

namespace {

    /**
     * This contains variables set through the operating system environment
     * or the command-line arguments.
     */
    struct Environment {
        /**
         * This is the path to the configuration file to use when
         * configuring Alfred.
         */
        std::string configFilePath;

        /**
         * This is the path to the file to which to write diagnostic messages
         * when running as a daemon.
         */
        std::string logFilePath = SystemAbstractions::File::GetExeParentDirectory() + "/log.txt";

        /**
         * This flag indicates whether or not Alfred is being
         * run as a system service.
         */
        bool daemon = false;

        /**
         * This is the minimum importance level of diagnostic messages that
         * will be reported to the log or terminal.
         */
        size_t diagnosticReportingThreshold = 2;
    };

    /**
     * This function configures and starts the web client.
     *
     * @param[in,out] http
     *     This is the web client to configure and start.
     *
     * @param[in] timeKeeper
     *     This is the object to be used to track time in the web client.
     *
     * @param[in] configuration
     *     This holds all of the configuration items for the entire system.
     *
     * @param[in] environment
     *     This contains variables set through the operating system
     *     environment or the command-line arguments.
     *
     * @param[in] diagnosticsSender
     *     This is the object to use to publish any diagnostic messages.
     *
     * @return
     *     An indication of whether or not the function succeeded is returned.
     */
    bool ConfigureAndStartHttpClient(
        Http::Client& http,
        std::shared_ptr< TimeKeeper > timeKeeper,
        const Json::Value& configuration,
        const Environment& environment,
        const SystemAbstractions::DiagnosticsSender& diagnosticsSender
    ) {
        auto clientTransport = std::make_shared< HttpNetworkTransport::HttpClientNetworkTransport >();
        clientTransport->SubscribeToDiagnostics(
            diagnosticsSender.Chain(),
            environment.diagnosticReportingThreshold
        );
        Http::Client::MobilizationDependencies httpClientDeps;
        httpClientDeps.timeKeeper = timeKeeper;
        httpClientDeps.transport = clientTransport;
        httpClientDeps.requestTimeoutSeconds = configuration["RequestTimeoutSeconds"];
        http.Mobilize(httpClientDeps);
        return true;
    }

    /**
     * This function configures and starts the web server.
     *
     * @param[in,out] http
     *     This is the web server to configure and start.
     *
     * @param[in] timeKeeper
     *     This is the object to be used to track time in the web server.
     *
     * @param[in] configuration
     *     This holds all of the configuration items for the entire system.
     *
     * @param[in] environment
     *     This contains variables set through the operating system
     *     environment or the command-line arguments.
     *
     * @param[in] diagnosticsSender
     *     This is the object to use to publish any diagnostic messages.
     *
     * @return
     *     An indication of whether or not the function succeeded is returned.
     */
    bool ConfigureAndStartHttpServer(
        Http::Server& http,
        const std::shared_ptr< TimeKeeper >& timeKeeper,
        const Json::Value& configuration,
        const Environment& environment,
        const SystemAbstractions::DiagnosticsSender& diagnosticMessageSender
    ) {
        Http::Server::MobilizationDependencies httpDeps;
        httpDeps.timeKeeper = std::make_shared< TimeKeeper >();
        auto transport = std::make_shared< HttpNetworkTransport::HttpServerNetworkTransport >();
        transport->SubscribeToDiagnostics(diagnosticMessageSender.Chain(), 0);
        httpDeps.transport = transport;
        http.SetConfigurationItem("Port", "8100");
        http.SetConfigurationItem("TooManyRequestsThreshold", "0.0");
        const auto& httpConfig = configuration["Http"];
        if (httpConfig.GetType() == Json::Value::Type::Object) {
            for (const auto keyValue: httpConfig) {
                const auto& key = keyValue.key();
                const auto& value = keyValue.value();
                http.SetConfigurationItem(key, value);
            }
        }
        return http.Mobilize(httpDeps);
    }

    std::string FormatDateTime(double time) {
        char buffer[20];
        auto timeSeconds = (time_t)time;
        (void)strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", gmtime(&timeSeconds));
        return buffer;
    }

    /**
     * This function loads the contents of the file with the given path
     * into the given string.
     *
     * @param[in] filePath
     *     This is the path of the file to load.
     *
     * @param[in] fileDescription
     *     This is a description of the file being loaded, used in any
     *     diagnostic messages published by the function.
     *
     * @param[in] diagnosticsSender
     *     This is the object to use to publish any diagnostic messages.
     *
     * @param[out] fileContents
     *     This is where to store the file's contents.
     *
     * @return
     *     An indication of whether or not the function succeeded is returned.
     */
    bool LoadFile(
        const std::string& filePath,
        const std::string& fileDescription,
        const SystemAbstractions::DiagnosticsSender& diagnosticsSender,
        std::string& fileContents
    ) {
        SystemAbstractions::File file(filePath);
        if (
            !file.IsDirectory()
            && file.OpenReadOnly()
        ) {
            std::vector< uint8_t > fileContentsAsVector(file.GetSize());
            if (file.Read(fileContentsAsVector) != fileContentsAsVector.size()) {
                diagnosticsSender.SendDiagnosticInformationFormatted(
                    SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                    "Unable to read %s file '%s'",
                    fileDescription.c_str(),
                    filePath.c_str()
                );
                return false;
            }
            (void)fileContents.assign(
                (const char*)fileContentsAsVector.data(),
                fileContentsAsVector.size()
            );
        } else {
            diagnosticsSender.SendDiagnosticInformationFormatted(
                SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                "Unable to open %s file '%s'",
                fileDescription.c_str(),
                filePath.c_str()
            );
            return false;
        }
        return true;
    }

    /**
     * This function opens and reads the server's configuration file,
     * returning it.  The configuration is formatted as a JSON object.
     *
     * @param[in] environment
     *     This contains variables set through the operating system
     *     environment or the command-line arguments.
     *
     * @param[in] diagnosticsSender
     *     This is the object to use to publish any diagnostic messages.
     *
     * @return
     *     The server's configuration is returned as a JSON object.
     */
    Json::Value ReadConfiguration(
        const Environment& environment,
        const SystemAbstractions::DiagnosticsSender& diagnosticsSender
    ) {
        // Start with a default configuration, to be used if there are any
        // issues reading the actual configuration file.
        Json::Value configuration(Json::Value::Type::Object);

        // Attempt to load configuration file from various paths.
        std::vector< std::string > possibleConfigFilePaths = {
            SystemAbstractions::File::GetExeParentDirectory() + "/config.json",
            "config.json",
        };
        if (!environment.configFilePath.empty()) {
            possibleConfigFilePaths.insert(
                possibleConfigFilePaths.begin(),
                environment.configFilePath
            );
        }
        for (const auto& possibleConfigFilePath: possibleConfigFilePaths) {
            std::string encodedConfig;
            if (
                LoadFile(
                    possibleConfigFilePath,
                    "configuration",
                    diagnosticsSender,
                    encodedConfig
                )
            ) {
                configuration = Json::Value::FromEncoding(encodedConfig);
                diagnosticsSender.SendDiagnosticInformationFormatted(
                    3,
                    "Loaded configuration from file '%s'",
                    possibleConfigFilePath.c_str()
                );
                break;
            }
        }
        return configuration;
    }

    void RegisterTestResource(
        Http::Server& http
    ) {
        http.RegisterResource(
            {},
            [](
                const Http::Request& request,
                std::shared_ptr< Http::Connection > connection,
                const std::string& trailer
            ){
                Http::Response response;
                response.statusCode = 200;
                response.reasonPhrase = "OK";
                response.headers.SetHeader("Content-Type", "text/plain");
                response.body = "Hello, World!";
                return response;
            }
        );
    }

    /**
     * Encode the given JSON value and write it out to the given file.
     *
     * @param[in,out] file
     *     This is the file to write.
     *
     * @param[in] value
     *     This is the JSON value to encode and write to the file.
     */
    void WriteJsonToFile(
        SystemAbstractions::File& file,
        const Json::Value& value
    ) {
        if (!file.OpenReadWrite()) {
            return;
        }
        Json::EncodingOptions jsonEncodingOptions;
        jsonEncodingOptions.pretty = true;
        jsonEncodingOptions.reencode = true;
        const auto encoding = value.ToEncoding(jsonEncodingOptions);
        SystemAbstractions::IFile::Buffer buffer(
            encoding.begin(),
            encoding.end()
        );
        (void)file.Write(buffer);
        (void)file.SetSize(buffer.size());
        file.Close();
    }

    /**
     * This function prints to the standard error stream information
     * about how to use this program.
     */
    void PrintUsageInformation() {
        fprintf(stderr,
            (
                "Usage: Alfred [options]\n"
                "\n"
                "Launch Alfred, attached to the terminal\n"
                "unless -s or --service is specified.\n"
                "\n"
                "Options:\n"
                "  -c|--config PATH\n"
                "    Use configuration saved in the file at the given PATH.\n"
                "  -s|--service\n"
                "    Run Alfred as a service, rather than directly\n"
                "    in the terminal.  (NOTE: requires separate OS-specific installation steps.)\n"
            )
        );
    }

    /**
     * This flag is used when the service is attached to a terminal.
     * It indicates whether or not the server should shut down.
     */
    bool shutDown = false;

    /**
     * This function is used when the service is attached to a terminal.
     * It is set up to be called when the SIGINT signal is
     * received by the program.  It just sets the "shutDown" flag
     * and relies on the program to be polling the flag to detect
     * when it's been set.
     *
     * @param[in] sig
     *     This is the signal for which this function was called.
     */
    void InterruptHandler(int) {
        shutDown = true;
    }

}

/**
 * This contains the private properties of a Service class instance.
 */
struct Service::Impl
    : public std::enable_shared_from_this< Service::Impl >
{
    // Properties

    /**
     * This holds the current configuration of the service.
     */
    Json::Value configuration;

    /**
     * This is used to publish diagnostic messages generated by the service
     * or one of its components.
     */
    SystemAbstractions::DiagnosticsSender diagnosticsSender;

    AsyncData::Dispatcher dispatcher;

    /**
     * This contains variables set through the operating system environment or
     * the command-line arguments.
     */
    Environment environment;

    /**
     * This is used to receive requests for resources from clients via HTTP.
     */
    Http::Server httpServer;

    /**
     * This is used to make requests for resources from servers via HTTP.
     */
    Http::Client httpClient;

    /**
     * This is used to synchronize access to this structure.
     */
    std::mutex mutex;

    /**
     * This is set once the stopServer promise is set, in order to prevent
     * accidentally setting the promise twice.
     */
    bool stopServiceSet = false;

    /**
     * This is used to signal the service to stop.
     */
    std::promise< void > stopService;

    /**
     * This is used to track time.
     */
    const std::shared_ptr< TimeKeeper > timeKeeper = std::make_shared< TimeKeeper >();

    /**
     * This is the delegate provided by the diagnostics publisher, to be
     * called when we want to cancel our subscription.
     */
    SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate unsubscribeFromDiagnostics;

    // Constructor

    Impl()
        : diagnosticsSender("Alfred")
    {
    }

    // Methods

    /**
     * Create and return a delegate that will publish diagnostic messages
     * to the console.
     *
     * @param[in] normalOutputFile
     *     This is the file to which to write "normal" (not warning or error)
     *     messages.
     *
     * @param[in] errorOutputFile
     *     This is the file to which to write warning or error messages.
     *
     * @return
     *     A delegate that will publish diagnostic messages
     *     to the console is returned.
     */
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate MakeDiagnosticPublisher(
        std::shared_ptr< FILE > normalOutputFile,
        std::shared_ptr< FILE > errorOutputFile
    ) {
        auto mutex = std::make_shared< std::mutex >();
        std::weak_ptr< Service::Impl > selfWeak(shared_from_this());
        return [
            selfWeak,
            normalOutputFile,
            errorOutputFile,
            mutex
        ](
            std::string senderName,
            size_t level,
            std::string message
        ) {
            const auto self = selfWeak.lock();
            if (self == nullptr) {
                return;
            }
            const auto now = self->timeKeeper->GetCurrentTime();
            self->dispatcher.Post(
                [
                    now,
                    normalOutputFile,
                    errorOutputFile,
                    mutex,
                    senderName,
                    level,
                    message
                ]{
                    FILE* destination;
                    std::string prefix;
                    if (level >= SystemAbstractions::DiagnosticsSender::Levels::ERROR) {
                        destination = errorOutputFile.get();
                        prefix = "error: ";
                    } else if (level >= SystemAbstractions::DiagnosticsSender::Levels::WARNING) {
                        destination = errorOutputFile.get();
                        prefix = "warning: ";
                    } else {
                        destination = normalOutputFile.get();
                    }
                    std::lock_guard< decltype(*mutex) > lock(*mutex);
                    fprintf(
                        destination,
                        "[%s %s:%zu] %s%s\n",
                        FormatDateTime(now).c_str(),
                        senderName.c_str(),
                        level,
                        prefix.c_str(),
                        message.c_str()
                    );
                }
            );
        };
    }

    /**
     * This method updates the environment to incorporate
     * any applicable command-line arguments.
     *
     * @param[in] argc
     *     This is the number of command-line arguments given to the program.
     *
     * @param[in] argv
     *     This is the array of command-line arguments given to the program.
     *
     * @return
     *     An indication of whether or not the function succeeded is returned.
     */
    bool ProcessCommandLineArguments(
        int argc,
        char* argv[]
    ) {
        size_t state = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            switch (state) {
                case 0: { // next option
                    if ((arg == "-c") || (arg == "--config")) {
                        state = 1;
                    } else if ((arg == "-s") || (arg == "--service")) {
                        environment.daemon = true;
                    } else if (arg.substr(0, 1) == "-") {
                        diagnosticsSender.SendDiagnosticInformationString(
                            SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                            "unrecognized option: '" + arg + "'"
                        );
                        return false;
                    } else {
                        diagnosticsSender.SendDiagnosticInformationString(
                            SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                            "extra argument '" + arg + "' ignored"
                        );
                    }
                } break;

                case 1: { // -c|--config
                    if (!environment.configFilePath.empty()) {
                        diagnosticsSender.SendDiagnosticInformationString(
                            SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                            "multiple configuration file paths given"
                        );
                        return false;
                    }
                    environment.configFilePath = arg;
                    state = 0;
                } break;

                default: break;
            }
        }
        switch (state) {
            case 1: { // -c|--config
                diagnosticsSender.SendDiagnosticInformationString(
                    SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                    "configuration file path expected"
                );
            } return false;

            default: return true;
        }
    }

    /**
     * This method is called to run the service to completion, either by the
     * service being told to stop, or by the service terminating on its own.
     *
     * @return
     *     The exit code that should be returned from the main function
     *     of the program is returned.
     */
    int Run() {
        if (!SetUp()) {
            return EXIT_FAILURE;
        }
        const auto serviceToldToStop = stopService.get_future();
        while (!shutDown) {
            if (
                serviceToldToStop.wait_for(std::chrono::milliseconds(100))
                == std::future_status::ready
            ) {
                break;
            }
        }
        ShutDown(unsubscribeFromDiagnostics);
        return EXIT_SUCCESS;
    }

    /**
     * Set up the service in preparation for running.
     *
     * @return
     *     An indication of whether or not the service was successfully
     *     prepared for running is returned.
     */
    bool SetUp() {
        unsubscribeFromDiagnostics = httpServer.SubscribeToDiagnostics(
            diagnosticsSender.Chain(),
            environment.diagnosticReportingThreshold
        );
        if (
            !ConfigureAndStartHttpServer(
                httpServer,
                timeKeeper,
                configuration,
                environment,
                diagnosticsSender
            )
        ) {
            unsubscribeFromDiagnostics();
            return false;
        }
        if (
            !ConfigureAndStartHttpClient(
                httpClient,
                timeKeeper,
                configuration,
                environment,
                diagnosticsSender
            )
        ) {
            unsubscribeFromDiagnostics();
            return false;
        }
        RegisterTestResource(httpServer);
        diagnosticsSender.SendDiagnosticInformationString(
            3,
            "Alfred up and running."
        );
        return true;
    }

    /**
     * Clean up the service state now that it has finished running.
     *
     * @param[in,out] unsubscribeFromDiagnostics
     *     This is the delegate provided by the diagnostics publisher, to be
     *     called when we want to cancel our subscription.
     */
    void ShutDown(
        const SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate& unsubscribeFromDiagnostics
    ) {
        diagnosticsSender.SendDiagnosticInformationString(
            3,
            "Exiting..."
        );
        unsubscribeFromDiagnostics();
    }

    /**
     * This method is called to signal the service to stop.
     */
    void Stop() {
        if (!stopServiceSet) {
            stopService.set_value();
            stopServiceSet = true;
        }
    }

};

Service::~Service() noexcept = default;
Service::Service(Service&&) noexcept = default;
Service& Service::operator=(Service&&) noexcept = default;

Service::Service()
    : impl_(new Impl())
{
}

int Service::Main(int argc, char* argv[]) {
    setbuf(stdout, NULL);
    auto diagnosticsReporter = impl_->MakeDiagnosticPublisher(
        std::shared_ptr< FILE >(stdout, [](FILE*){}),
        std::shared_ptr< FILE >(stderr, [](FILE*){})
    );
    auto unsubscribeDiagnosticsDelegate = impl_->diagnosticsSender.SubscribeToDiagnostics(
        diagnosticsReporter,
        impl_->environment.diagnosticReportingThreshold
    );
    if (!impl_->ProcessCommandLineArguments(argc, argv)) {
        PrintUsageInformation();
        return EXIT_FAILURE;
    }
    unsubscribeDiagnosticsDelegate();
    impl_->configuration = ReadConfiguration(
        impl_->environment,
        impl_->diagnosticsSender
    );
    if (impl_->configuration.Has("LogFile")) {
        impl_->environment.logFilePath = (std::string)impl_->configuration["LogFile"];
    }
    if (impl_->configuration.Has("DiagnosticReportingThreshold")) {
        impl_->environment.diagnosticReportingThreshold = (size_t)impl_->configuration["DiagnosticReportingThreshold"];
    }
    if (impl_->environment.daemon) {
        std::shared_ptr< FILE > logFile(
            fopen(impl_->environment.logFilePath.c_str(), "a"),
            [](FILE* p){ (void)fclose(p); }
        );
        setbuf(logFile.get(), NULL);
        diagnosticsReporter = impl_->MakeDiagnosticPublisher(
            logFile,
            logFile
        );
    }
    unsubscribeDiagnosticsDelegate = impl_->diagnosticsSender.SubscribeToDiagnostics(
        diagnosticsReporter,
        impl_->environment.diagnosticReportingThreshold
    );
    int result = EXIT_SUCCESS;
    if (impl_->environment.daemon) {
        result = SystemAbstractions::Service::Main();
    } else {
        const auto previousInterruptHandler = signal(SIGINT, InterruptHandler);
        result = Run();
        (void)signal(SIGINT, previousInterruptHandler);
    }
    shutDown = true;
    return result;
}

int Service::Run() {
    return impl_->Run();
}

void Service::Stop() {
    impl_->Stop();
}

std::string Service::GetServiceName() const {
    static const std::string name = "Alfred";
    return name;
}
