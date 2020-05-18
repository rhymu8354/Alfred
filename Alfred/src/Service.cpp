/**
 * @file Service.cpp
 *
 * This module contains the implementation of the Service class.
 */

#include "ApiHttp.hpp"
#include "LoadFile.hpp"
#include "Service.hpp"
#include "Store.hpp"
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
#include <TlsDecorator/TlsDecorator.hpp>

namespace {

    /**
     * This contains variables set through the operating system environment
     * or the command-line arguments.
     */
    struct Environment {
        /**
         * This is the path to the store file to use for Alfred.
         */
        std::string storeFilePath;

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

    std::string FormatDateTime(double time) {
        char buffer[20];
        auto timeSeconds = (time_t)time;
        (void)strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", gmtime(&timeSeconds));
        return buffer;
    }

    /**
     * Encode the given JSON value and write it out to the given file.
     *
     * @param[in,out] file
     *     This is the file to write.
     *
     * @param[in] value
     *     This is the JSON value to encode and write to the file.
     *
     * @param[in] diagnosticsSender
     *     This is the object to use to publish any diagnostic messages.
     */
    void WriteJsonToFile(
        SystemAbstractions::File& file,
        const Json::Value& value,
        const SystemAbstractions::DiagnosticsSender& diagnosticsSender
    ) {
        if (!file.OpenReadWrite()) {
            diagnosticsSender.SendDiagnosticInformationFormatted(
                SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                "unable to open file \"%s\" for writing",
                file.GetPath().c_str()
            );
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
        if (file.Write(buffer) != buffer.size()) {
            diagnosticsSender.SendDiagnosticInformationFormatted(
                SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                "unable to write to file \"%s\"",
                file.GetPath().c_str()
            );
            file.Close();
            return;
        }
        if (!file.SetSize(buffer.size())) {
            diagnosticsSender.SendDiagnosticInformationFormatted(
                SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                "unable to set size of file \"%s\"",
                file.GetPath().c_str()
            );
            file.Close();
            return;
        }
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
                "unless -d or --daemon is specified.\n"
                "\n"
                "Options:\n"
                "  -s|--store PATH\n"
                "    Use configuration saved in the file at the given PATH.\n"
                "  -d|--daemon\n"
                "    Run Alfred as a daemon, rather than directly\n"
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
     * This holds all of the information managed by the service, along
     * with its metadata.
     */
    std::shared_ptr< Store > store = std::make_shared< Store >();

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
        (void)store->SubscribeToDiagnostics(diagnosticsSender.Chain());
    }

    // Methods

    /**
     * This function configures and starts the web client.
     */
    bool ConfigureAndStartHttpClient() {
        auto clientTransport = std::make_shared< HttpNetworkTransport::HttpClientNetworkTransport >();
        clientTransport->SubscribeToDiagnostics(
            diagnosticsSender.Chain(),
            environment.diagnosticReportingThreshold
        );
        Http::Client::MobilizationDependencies httpClientDeps;
        httpClientDeps.timeKeeper = timeKeeper;
        httpClientDeps.transport = clientTransport;
        const auto configuration = store->GetData("Configuration");
        httpClientDeps.requestTimeoutSeconds = configuration["RequestTimeoutSeconds"];
        httpClient.Mobilize(httpClientDeps);
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
    bool ConfigureAndStartHttpServer() {
        Http::Server::MobilizationDependencies httpDeps;
        httpDeps.timeKeeper = std::make_shared< TimeKeeper >();
        auto transport = std::make_shared< HttpNetworkTransport::HttpServerNetworkTransport >();
        transport->SubscribeToDiagnostics(diagnosticsSender.Chain(), 0);
        const auto configuration = store->GetData("Configuration");
        std::string cert, key;
        auto certPath = (std::string)configuration["SslCertificate"];
        if (!SystemAbstractions::File::IsAbsolutePath(certPath)) {
            certPath = SystemAbstractions::File::GetExeParentDirectory() + "/" + certPath;
        }
        if (!LoadFile(certPath, "SSL certificate", diagnosticsSender, cert)) {
            return false;
        }
        auto keyPath = (std::string)configuration["SslKey"];
        if (!SystemAbstractions::File::IsAbsolutePath(keyPath)) {
            keyPath = SystemAbstractions::File::GetExeParentDirectory() + "/" + keyPath;
        }
        if (!LoadFile(keyPath, "SSL private key", diagnosticsSender, key)) {
            return false;
        }
        const auto passphrase = (std::string)configuration["SslKeyPassphrase"];
        const auto connectionDecoratorFactory = [
            cert,
            key,
            passphrase
        ](
            std::shared_ptr< SystemAbstractions::INetworkConnection > connection
        ){
            const auto tlsDecorator = std::make_shared< TlsDecorator::TlsDecorator >();
            tlsDecorator->ConfigureAsServer(
                connection,
                cert,
                key,
                passphrase
            );
            return tlsDecorator;
        };
        transport->SetConnectionDecoratorFactory(connectionDecoratorFactory);
        httpDeps.transport = transport;
        httpServer.SetConfigurationItem("Port", "8100");
        httpServer.SetConfigurationItem("TooManyRequestsThreshold", "0.0");
        const auto& httpConfig = configuration["Http"];
        if (httpConfig.GetType() == Json::Value::Type::Object) {
            for (const auto keyValue: httpConfig) {
                const auto& key = keyValue.key();
                const auto& value = keyValue.value();
                httpServer.SetConfigurationItem(key, value);
            }
        }
        return httpServer.Mobilize(httpDeps);
    }

    bool LoadStore() {
        std::vector< std::string > possibleStoreFilePaths = {
            SystemAbstractions::File::GetExeParentDirectory() + "/Alfred.json",
            "Alfred.json",
        };
        if (!environment.storeFilePath.empty()) {
            possibleStoreFilePaths.insert(
                possibleStoreFilePaths.begin(),
                environment.storeFilePath
            );
        }
        for (const auto& possibleStoreFilePath: possibleStoreFilePaths) {
            if (store->Mobilize(possibleStoreFilePath, timeKeeper)) {
                return true;
            }
        }
        return false;
    }

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
                    if ((arg == "-s") || (arg == "--store")) {
                        state = 1;
                    } else if ((arg == "-d") || (arg == "--daemon")) {
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

                case 1: { // -s|--store
                    if (!environment.storeFilePath.empty()) {
                        diagnosticsSender.SendDiagnosticInformationString(
                            SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                            "multiple store file paths given"
                        );
                        return false;
                    }
                    environment.storeFilePath = arg;
                    state = 0;
                } break;

                default: break;
            }
        }
        switch (state) {
            case 1: { // -s|--store
                diagnosticsSender.SendDiagnosticInformationString(
                    SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                    "store file path expected"
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
        if (!ConfigureAndStartHttpServer()) {
            unsubscribeFromDiagnostics();
            return false;
        }
        if (!ConfigureAndStartHttpClient()) {
            unsubscribeFromDiagnostics();
            return false;
        }
        ApiHttp::RegisterResources(store, httpServer);
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
    if (!impl_->LoadStore()) {
        return EXIT_FAILURE;
    }
    unsubscribeDiagnosticsDelegate();
    const auto configuration = impl_->store->GetData("Configuration");
    if (configuration.Has("LogFile")) {
        impl_->environment.logFilePath = (std::string)configuration["LogFile"];
    }
    if (configuration.Has("DiagnosticReportingThreshold")) {
        impl_->environment.diagnosticReportingThreshold = (size_t)configuration["DiagnosticReportingThreshold"];
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
