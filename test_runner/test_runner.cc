// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is a TCP service and a fidl service. The TCP portion of this process
// accepts test commands, runs them, waits for completion or error, and reports
// back to the TCP client.
// The TCP protocol is as follows:
// - Client connects, sends a single line representing the test command to run:
//   run <test_id> <shell command to run>\n
// - Once the test is done, we reply to the TCP client:
//   <test_id> pass|fail\n
//
// The <test_id> is an unique ID string that the TCP client gives us per test;
// we tag our replies and device logs with it so the TCP client can identify
// device logs (and possibly if multiple tests are run at the same time).
//
// The shell command representing the running test is launched in a new
// ApplicationEnvironment for easy teardown. This ApplicationEnvironment
// contains a TestRunner service (see test_runner.fidl). The applications
// launched by the shell command (which may launch more than 1 process) may use
// the |TestRunner| service to signal completion of the test, and also provides
// a way to signal process crashes.

// TODO(vardhan): Make it possible to run multiple tests within the same test
// runner environment, without teardown; useful for testing modules, which may
// not need to tear down device_runner.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <functional>
#include <string>
#include <vector>

#include "apps/modular/lib/app/application_context.h"
#include "apps/modular/lib/fidl/scope.h"
#include "apps/modular/services/test_runner/test_runner.fidl.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/strings/split_string.h"
#include "lib/ftl/strings/string_view.h"
#include "lib/mtl/tasks/message_loop.h"

// TODO(vardhan): Make listen port command-line configurable.
constexpr uint16_t kListenPort = 8342;  // TCP port

namespace modular {
namespace {

// This is an ApplicationEnvironment under which our test runs. We expose a
// TestRunner service which the test can use to assert when it is complete.
class TestRunnerScope : public Scope {
 public:
  TestRunnerScope(
      ApplicationEnvironmentPtr parent_env,
      ServiceProviderPtr default_services,
      const std::string& label,
      ServiceProviderImpl::InterfaceRequestHandler<TestRunner> request_handler)
      : Scope(std::move(parent_env), label) {
    service_provider_.SetDefaultServiceProvider(std::move(default_services));
    service_provider_.AddService(request_handler);
  }

 private:
  // |ApplicationEnvironmentHost|:
  void GetApplicationEnvironmentServices(
      fidl::InterfaceRequest<ServiceProvider> environment_services) override {
    service_provider_.AddBinding(std::move(environment_services));
  }

  ServiceProviderImpl service_provider_;
};

class TestRunnerConnection;
// TestRunContext represents a single run of a test. Given a test to run, it
// runs it in a new ApplicationEnvironment and provides the environment a
// TestRunner
// service to report completion. When tests are done, their completion is
// reported back to TestRunnerConnection (which is responsible for deleting
// TestRunContext). If the child application stops without reporting anything,
// we declare the test a failure.
class TestRunContext : public TestRunner {
 public:
  TestRunContext(std::shared_ptr<ApplicationContext> app_context,
                 TestRunnerConnection* connection,
                 const std::string& test_id,
                 const std::string& url,
                 const std::vector<std::string>& args);

 private:
  // |TestRunner|:
  void Finish(bool success);

  ApplicationControllerPtr child_app_controller_;
  std::unique_ptr<TestRunnerScope> child_env_scope_;

  TestRunnerConnection* const test_runner_connection_;
  fidl::Binding<TestRunner> test_runner_binding_;
  // This is a tag that we use to identify the test that was run. For now, it
  // helps distinguish between multiple test outputs to the device log.
  const std::string test_id_;
};

// Represents a client connection, and is self-owned (it will exit the
// MessageLoop upon completion). TestRunnerConnection receives commands to run
// tests and runs them one at a time using TestRunContext.
class TestRunnerConnection {
 public:
  explicit TestRunnerConnection(int socket_fd,
                                std::shared_ptr<ApplicationContext> app_context)
      : app_context_(app_context), socket_(socket_fd) {}

  void Start() {
    FTL_CHECK(!test_context_);
    ReadAndRunCommand();
  }

  // Called by TestRunContext when it has finished running its test. This will
  // trigger reading more commands from TCP socket.
  void Finish(const std::string& test_id, bool success) {
    FTL_CHECK(test_context_);

    // IMPORTANT: leave this log here, exactly as it is. Currently, tests
    // launched from host (e.g. Linux) grep for this text to figure out the
    // amount of the log to associate with the test.
    FTL_LOG(INFO) << "test_runner: done " << test_id << " success=" << success;

    std::stringstream epilogue;
    epilogue << test_id << " ";
    epilogue << (success ? "pass" : "fail") << "\n";

    std::string bytes = epilogue.str();
    FTL_CHECK(write(socket_, bytes.data(), bytes.size()) > 0);

    test_context_.reset();
    Start();
  }

 private:
  ~TestRunnerConnection() {
    close(socket_);
    mtl::MessageLoop::GetCurrent()->PostQuitTask();
  }

  // Read and entire command (which consists of one line) and return it.
  // Can be called again to read the next command. Blocks until an entire line
  // has been read.
  std::string ReadCommand() {
    char buf[1024];
    // Read until we see a new line.
    auto newline_pos = command_buffer_.find("\n");
    while (newline_pos == std::string::npos) {
      ssize_t n = read(socket_, buf, sizeof(buf));
      if (n <= 0) {
        return std::string();
      }
      command_buffer_.append(buf, n);
      newline_pos = command_buffer_.find("\n");
    }

    // Consume only until the new line (and leave the rest of the bytes for
    // subsequent ReadCommand()s.
    auto retval = command_buffer_.substr(0, newline_pos + 1);
    command_buffer_.erase(0, newline_pos + 1);
    return retval;
  }

  // Read an entire line representing the command to run and run it. When the
  // test has finished running, TestRunnerConnection::Finish is invoked. We do
  // not read any further commands until that has happened.
  void ReadAndRunCommand() {
    std::string command = ReadCommand();
    if (command.empty()) {
      delete this;
      return;
    }

    // command_parse[0] = "run"
    // command_parse[1] = test_id
    // command_parse[2] = url
    // command_parse[3..] = args (optional)
    std::vector<std::string> command_parse = ftl::SplitStringCopy(
        command, " ", ftl::kTrimWhitespace, ftl::kSplitWantNonEmpty);

    FTL_CHECK(command_parse.size() >= 3)
        << "Not enough args. Must be: `run <test id> <command to run>`";
    FTL_CHECK(command_parse[0] == "run") << "Only supported command is `run`.";

    FTL_LOG(INFO) << "test_runner: run " << command_parse[1];

    std::vector<std::string> args;
    for (size_t i = 3; i < command_parse.size(); i++) {
      args.push_back(std::move(command_parse[i]));
    }

    // When TestRunContext is done with the test, it calls
    // TestRunnerConnection::Finish().
    test_context_.reset(new TestRunContext(app_context_, this, command_parse[1],
                                           command_parse[2], args));
  }

  std::shared_ptr<ApplicationContext> app_context_;
  std::unique_ptr<TestRunContext> test_context_;

  // Posix fd for the TCP connection.
  const int socket_;
  std::string command_buffer_;
};

TestRunContext::TestRunContext(std::shared_ptr<ApplicationContext> app_context,
                               TestRunnerConnection* connection,
                               const std::string& test_id,
                               const std::string& url,
                               const std::vector<std::string>& args)
    : test_runner_connection_(connection),
      test_runner_binding_(this),
      test_id_(test_id) {
  // 1. Make a child environment to run the command.
  ApplicationEnvironmentPtr parent_env;
  app_context->environment()->Duplicate(parent_env.NewRequest());

  ServiceProviderPtr parent_env_services;
  parent_env->GetServices(parent_env_services.NewRequest());

  test_runner_binding_.set_connection_error_handler(
      std::bind(&TestRunContext::Finish, this, false));

  child_env_scope_ = std::make_unique<TestRunnerScope>(
      std::move(parent_env), std::move(parent_env_services), "test_runner_env",
      [this](fidl::InterfaceRequest<TestRunner> request) {
        test_runner_binding_.Bind(std::move(request));
      });

  // 2. Launch the test command.
  ApplicationLauncherPtr launcher;
  child_env_scope_->environment()->GetApplicationLauncher(
      launcher.NewRequest());

  ApplicationLaunchInfoPtr info = ApplicationLaunchInfo::New();
  info->url = url;
  info->arguments = fidl::Array<fidl::String>::From(args);
  launcher->CreateApplication(std::move(info),
                              child_app_controller_.NewRequest());
  child_app_controller_.set_connection_error_handler(
      std::bind(&TestRunContext::Finish, this, false));
}

void TestRunContext::Finish(bool success) {
  test_runner_connection_->Finish(test_id_, success);
}

// TestRunnerTCPServer is a TCP server that accepts connections and launches
// them as TestRunnerConnection.
class TestRunnerTCPServer {
 public:
  TestRunnerTCPServer(uint16_t port)
      : app_context_(ApplicationContext::CreateFromStartupInfo()) {
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // 1. Make a TCP socket.
    listener_ = socket(addr.sin_family, SOCK_STREAM, IPPROTO_TCP);
    FTL_CHECK(listener_ != -1);

    // 2. Bind it to an address.
    FTL_CHECK(bind(listener_, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) != -1);

    // 3. Make it a listening socket.
    FTL_CHECK(listen(listener_, 100) != -1);
  }

  ~TestRunnerTCPServer() { close(listener_); }

  // Blocks until there is a new connection.
  TestRunnerConnection* AcceptConnection() {
    int sockfd = accept(listener_, nullptr, nullptr);
    if (sockfd == -1) {
      FTL_LOG(INFO) << "accept() oops";
    }
    return new TestRunnerConnection(sockfd, app_context_);
  }

 private:
  int listener_;
  std::shared_ptr<ApplicationContext> app_context_;
};

}  // namespace
}  // namespace modular

int main() {
  mtl::MessageLoop loop;
  modular::TestRunnerTCPServer server(kListenPort);
  while (1) {
    // TODO(vardhan): Because our sockets are POSIX fds, they don't work with
    // our message loop, so we do some synchronous operations and have to do
    // manipulate the message loop to pass control back and forth. Consider
    // using separate threads for handle message loop vs. fd polling.
    auto* runner = server.AcceptConnection();
    loop.task_runner()->PostTask(
        std::bind(&modular::TestRunnerConnection::Start, runner));
    loop.Run();
  }
  return 0;
}
