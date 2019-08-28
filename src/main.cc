/**
 * sexec - Execute commands via SSH
 * Copyright (C) 2016-2017 Changli Gao <xiaosuo@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <config.h>
#include <getopt.h>
#include <unistd.h>
#include <pwd.h>

#include <libssh/libssh.h>
#include <libssh/callbacks.h>

#include <climits>
#include <iostream>
#include <sstream>
#include <cstdio>
#include <memory>
#include <cassert>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <list>
#include <future>
#include <mutex>
#include <csignal>
#include <tuple>

static std::mutex g_io_mutex;

static std::unordered_map<std::string, std::string> g_ext2interpreter = {
  { "sh",  "/bin/sh"             },
  { "py",  "/usr/bin/env python" },
  { "rb",  "/usr/bin/env ruby"   },
  { "pl",  "/usr/bin/env perl"   },
  { "php", "/usr/bin/env php"    },
};

std::function<void(FILE*)> g_start_log;
std::function<void(FILE*)> g_end_log;

static volatile sig_atomic_t g_stopping = false;

int SshUserauthPublicKey(ssh_session sess) {
  return ssh_userauth_publickey_auto(sess, nullptr, nullptr);
}

void StartLogToConsole(FILE *out) {
  const char *prefix;
  if (out == stdout) {
    prefix = "\033[32;1m";
  } else {
    prefix = "\033[31;1m";
  }
  fwrite(prefix, 7, 1, out);
}

void EndLogToConsole(FILE *out) {
  fwrite("\033[0m", 4, 1, out);
  fflush(out);
}

void StartLogToFile(FILE *out) {
  (void)out;
}

void EndLogToFile(FILE *out) {
  fflush(out);
}

void InstallLogHandler() {
  if (isatty(STDOUT_FILENO) && isatty(STDERR_FILENO)) {
    g_start_log = &StartLogToConsole;
    g_end_log = &EndLogToConsole;
  } else {
    g_start_log = &StartLogToFile;
    g_end_log = &EndLogToFile;
  }
}

std::string FileGetContents(std::string filename) {
  std::ifstream ifs(filename, std::ifstream::binary);
  if (!ifs) {
    throw std::runtime_error("open: " + filename);
  }
  if (!ifs.seekg(0, std::ios_base::end)) {
    throw std::runtime_error("seekg: " + filename);
  }
  auto size = ifs.tellg();
  if (!ifs.seekg(0, std::ios_base::beg)) {
    throw std::runtime_error("seekg: " + filename);
  }
  std::string contents;
  contents.resize(size);
  if (!ifs.read(&contents.front(), size)) {
    throw std::runtime_error("read: " + filename);
  }
  return contents;
}

struct Options {
  ~Options() {
    if (key) {
      ssh_key_free(key);
    }
  }

  void Parse(int argc, char *argv[]) {
    argv0 = argv[0];
    std::string short_opts = "a:c:de:f:hi:p:t:u:H:T:V";
    option long_opts[] = {
      { "auth",      required_argument, nullptr, 'a' },
      { "cmd",       required_argument, nullptr, 'c' },
      { "dedup",     no_argument,       nullptr, 'd' },
      { "env",       required_argument, nullptr, 'e' },
      { "file",      required_argument, nullptr, 'f' },
      { "help",      no_argument,       nullptr, 'h' },
      { "identity",  required_argument, nullptr, 'i' },
      { "parallel",  required_argument, nullptr, 'p' },
      { "timeout",   required_argument, nullptr, 't' },
      { "user",      required_argument, nullptr, 'u' },
      { "host",      required_argument, nullptr, 'H' },
      { "thread",    required_argument, nullptr, 'T' },
      { "version",   required_argument, nullptr, 'V' },
      { nullptr,     0,                 nullptr, 0   }
    };
    for (;;) {
      int opt = getopt_long(argc, argv, short_opts.c_str(), long_opts, nullptr);
      if (opt == -1) {
        break;
      }
      switch (opt) {
        case 'a': {
          static char * const tokens[] = {
            const_cast<char*>("gssapi"),
            const_cast<char*>("publickey"),
            nullptr,
          };
          char *value;
          char *subopts = optarg;
          while (*subopts) {
            switch (getsubopt(&subopts, tokens, &value)) {
              case 0:
                auth_methods.emplace_back(&ssh_userauth_gssapi);
                break;
              case 1:
                auth_methods.emplace_back(&SshUserauthPublicKey);
                break;
              default:
                ShowHelp(stderr);
                exit(EXIT_FAILURE);
            }
          }
          break;
        }
        case 'c':
          cmd = optarg;
          break;
        case 'd':
          dedup = true;
          break;
        case 'e': {
          if (!*optarg) {
            throw std::runtime_error("Empty env");
          }
          auto *val = strchr(optarg, '=');
          if (val == NULL) {
            envs[optarg] = "";
          } else if (val == optarg) {
            throw std::runtime_error("Empty env");
          } else {
            envs[std::string(optarg, val - optarg)] = val + 1;
          }
          break;
        }
        case 'f':
          script_name = optarg;
          script_contents = FileGetContents(optarg);
          break;
        case 'h':
          ShowHelp(stdout);
          exit(EXIT_SUCCESS);
          break;
        case 'i': {
          if (key) {
            throw std::runtime_error("Duplicate id");
          }
          int rc = ssh_pki_import_privkey_file(
              optarg, nullptr, nullptr, nullptr, &key);
          if (rc != SSH_OK) {
            throw std::runtime_error(
                std::string("Failed to import private key: ") + optarg +
                ": " + std::to_string(rc));
          }
          auth_methods.emplace_back([this](ssh_session sess) -> int {
            return ssh_userauth_publickey(sess, nullptr, this->key);
          });
          break;
        }
        case 'p':
          parallel = atoi(optarg);
          break;
        case 't':
          timeout = atoi(optarg);
          break;
        case 'u':
          user = optarg;
          break;
        case 'H':
          LoadHostsFromFile(optarg);
          break;
        case 'T':
          num_threads = atoi(optarg);
          break;
        case 'V':
          printf("%s\n", PACKAGE_STRING);
          exit(EXIT_SUCCESS);
          break;
        default:
          ShowHelp(stderr);
          exit(EXIT_FAILURE);
      }
    }
    for (; optind < argc; ++optind) {
      hosts.emplace_back(argv[optind]);
    }
    Validate();
  }

  void LoadHostsFromFile(const char *filename) {
    std::istream *is;
    std::unique_ptr<std::ifstream> ifs;
    if (strcmp(filename, "-") == 0) {
      is = &std::cin;
    } else {
      ifs.reset(new std::ifstream(filename));
      if (!*ifs) {
        throw std::runtime_error(std::string("Failed to open ") + filename);
      }
      is = ifs.get();
    }
    std::string line;
    while (std::getline(*is, line)) {
      if (!line.empty() && line.back() == '\n') {
        line.pop_back();
      }
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty()) {
        continue;
      }
      hosts.emplace_back(line);
    }
  }

  void ShowHelp(FILE *out) {
    fprintf(
        out,
        "Usage: %s [OPTION]... [HOST]...\n"
        "\n"
        "Options:\n"
        "  -a, --auth <METHODS>  Authentication methods separated by `,'\n"
        "                        `gssapi,publickey' by default\n"
        "                        `gssapi' and `publickey' are supported\n"
        "  -c, --cmd <CMD>       Execute <CMD>\n"
        "  -d, --dedup           Dedup hosts\n"
        "  -e, --env var=val     Set `val' to environment variable `var'\n"
        "  -f, --file <FILE>     Execute <FILE>\n"
        "  -h, --help            Show this message\n"
        "  -i, --identity <FILE> The identity (private key) file\n"
        "  -p, --parallel <N>    Max parallel sessions per thread,\n"
        "                        1 by default\n"
        "  -t, --timeout <SEC>   Timeout in seconds per session,\n"
        "                        -1 by default\n"
        "  -u, --user <USER>     Signed in as <USER>\n"
        "  -H, --host <FILE>     Use the hosts in <FILE>. A single dash(`-')\n"
        "                        means the standard input\n"
        "  -T, --threads <N>     Use <N> threads\n"
        "  -V, --version         Display the version of sexec and exit\n",
        argv0.c_str());
  }

  void Validate() {
    if (auth_methods.empty()) {
      auth_methods.emplace_back(&ssh_userauth_gssapi);
      auth_methods.emplace_back(&SshUserauthPublicKey);
    }
    if (hosts.empty()) {
      throw std::runtime_error("No host");
    }
    if (user.empty()) {
      auto size = sysconf(_SC_GETPW_R_SIZE_MAX);
      if (size == -1) {
        throw std::runtime_error(
            "Failed to get the size of the buffer for passwd");
      }
      std::unique_ptr<char[]> buf(new char[size]);
      passwd pwd_store;
      passwd *pwd;
      if (getpwuid_r(geteuid(), &pwd_store, buf.get(), size, &pwd)) {
        throw std::runtime_error("Failed to resolve username");
      }
      user = pwd->pw_name;
    }

    if (dedup) {
      std::vector<std::string> filtered;
      filtered.reserve(hosts.size());
      std::unordered_set<std::string> seen;
      for (const auto &host : hosts) {
        if (seen.count(host) != 0) {
          continue;
        }
        filtered.emplace_back(host);
        seen.emplace(host);
      }
      hosts = filtered;
    }

    // Parse shebang
    if (cmd.empty()) {
      if (script_contents.empty()) {
        throw std::runtime_error("No cmd");
      }
      if (script_contents.compare(0, 2, "#!") != 0) {
        // Lazy user. Try to derive the interpreter from the file extension.
        auto last_dot = script_name.rfind('.');
        if (last_dot == std::string::npos) {
          throw std::runtime_error("No shebang and file extension");
        }
        auto file_ext = script_name.substr(last_dot + 1);
        auto it = g_ext2interpreter.find(file_ext);
        if (it == g_ext2interpreter.end()) {
          throw std::runtime_error("Unknown file extension: " + file_ext);
        }
        cmd = it->second;
      } else {
        auto pos = script_contents.find('\n');
        if (pos == std::string::npos) {
          throw std::runtime_error("No shebang");
        }
        cmd = script_contents.substr(2, pos - 2);
        if (!cmd.empty() && cmd.back() == '\r') {
          cmd.pop_back();
        }
        if (cmd.empty()) {
          throw std::runtime_error("No shebang");
        }
      }
      cmd += " /dev/stdin";
    }

    if (!envs.empty()) {
      std::string prefix = "/usr/bin/env ";
      for (const auto &env : envs) {
        prefix += env.first + '=' + env.second + ' ';
      }
      cmd = prefix + cmd;
    }

    if (timeout == 0) {
      throw std::runtime_error("Zero timeout?");
    }

    if (parallel == 0) {
      throw std::runtime_error("Zero parallel?");
    }

    if (num_threads <= 0) {
      throw std::runtime_error("No threads?");
    }

    if (static_cast<size_t>(num_threads) > hosts.size()) {
      num_threads = hosts.size();
    }
  }

  const char *GetHost(size_t i) const {
    return hosts[i].c_str();
  }

  std::string argv0;
  std::string cmd;
  std::vector<std::string> hosts;
  std::string user;
  bool dedup = false;
  std::string script_name;
  std::string script_contents;
  int timeout = -1;
  int parallel = 1;
  int num_threads = 1;
  std::unordered_map<std::string, std::string> envs;
  std::vector<std::function<int(ssh_session)>> auth_methods;
  ssh_key key = nullptr;
};

class Session {
 public:
  Session(const Options &opts, size_t host_index, ssh_event event) :
      opts_(opts), host_index_(host_index), event_(event),
      sess_(ssh_new(), &ssh_free) {
    assert(event_);
    if (!sess_) {
      throw std::bad_alloc();
    }
    // parse host:port string;
    host_str = host();
    char *host;
    int port;
    tie(host, port) = divide(host_str);

    int rc = ssh_options_set(sess_.get(), SSH_OPTIONS_HOST, host);
    if (rc != 0) {
      Error("Set SSH_OPTIONS_HOST", rc);
    }
    int rc = ssh_options_set(sess_.get(), SSH_OPTIONS_PORT, port);
    if (rc != 0) {
      Error("Set SSH_OPTIONS_PORT", rc);
    }
    rc = ssh_options_set(sess_.get(), SSH_OPTIONS_COMPRESSION, "no");
    if (rc != 0) {
      Error("Set SSH_OPTIONS_COMPRESSION to no", rc);
    }
    int no = 0;
    rc = ssh_options_set(sess_.get(), SSH_OPTIONS_STRICTHOSTKEYCHECK, &no);
    if (rc != 0) {
      Error("Set SSH_OPTIONS_STRICTHOSTKEYCHECK to no", rc);
    }
    rc = ssh_options_set(sess_.get(), SSH_OPTIONS_KNOWNHOSTS, "nosuchfile");
    if (rc != 0) {
      Error("Set SSH_OPTIONS_KNOWNHOSTS to nosuchfile", rc);
    }
    ssh_set_blocking(sess_.get(), 0);
    Drive(&Session::Connect);
    deadline_ = std::chrono::steady_clock::now() +
        std::chrono::seconds(opts_.timeout);
  }

  ~Session() {
    ssh_event_remove_session(event_, sess_.get());
    if (chan_) {
      ssh_channel_free(chan_);
    }
  }

  // Should be called until false is returned.
  bool Drive() {
    if (do_) {
      (this->*do_)();
    }
    return do_ != nullptr;
  }

  // parse host:port string;
  std::tuple<char *, int> divide(char *input) {
      int port = 22; // default
      char *host = std::strtok(input, ":");
      char *port_str = std::strtok(NULL, " ");
      if (port_str != NULL){
          port = std::atoi(port_str);
      }
      return std::make_tuple(host, port);
  }
  int exit_status() const { return exit_status_; }
  bool exit_status_set() const { return exit_status_set_; }

  std::string exit_signal() const { return exit_signal_; }
  bool exit_signal_set() const { return exit_signal_set_; }

  const char *host() const { return opts_.GetHost(host_index_); }

  std::chrono::steady_clock::duration GetRemainingTime() const {
    return deadline_ - std::chrono::steady_clock::now();
  }

 private:
  void Error(std::string prefix, int rc) {
    throw std::runtime_error(
        prefix + '(' + std::to_string(rc) + "): " + ssh_get_error(sess_.get()));
  }

  void Drive(void (Session::*cb)()) {
    do_ = cb;
    Drive();
  }

  void AddEvent() {
    int rc = ssh_event_add_session(event_, sess_.get());
    if (rc != SSH_OK) {
      Error("Add session to event", rc);
    }
  }

  void Connect() {
    int rc = ssh_connect(sess_.get());
    if (!added_event_) {
      AddEvent();
      added_event_ = true;
    }
    switch (rc) {
      case SSH_OK:
        rc = ssh_options_set(sess_.get(), SSH_OPTIONS_USER, opts_.user.c_str());
        if (rc) {
          Error("Set user option", rc);
        }
        Drive(&Session::Authenticate);
        break;
      case SSH_AGAIN:
        break;
      default:
        Error("Connect", rc);
    }
  }

  void Authenticate() {
    int rc = opts_.auth_methods[auth_method_index_](sess_.get());
    switch (rc) {
      case SSH_AUTH_SUCCESS:
        assert(!chan_);
        chan_ = ssh_channel_new(sess_.get());
        if (!chan_) {
          throw std::bad_alloc();
        }
        memset(&cb_, 0, sizeof(cb_));
        ssh_callbacks_init(&cb_);
        cb_.userdata = this;
        cb_.channel_exit_status_function = &OnChannelExitStatus;
        cb_.channel_exit_signal_function = &OnChannelExitSignal;
        rc = ssh_set_channel_callbacks(chan_, &cb_);
        if (rc != 0) {
          Error("Set channel callbacks", rc);
        }
        Drive(&Session::OpenChannel);
        break;
      case SSH_AUTH_AGAIN:
        break;
      case SSH_AUTH_ERROR:
        Error("Authenticate", rc);
        break;
      default:
        if (++auth_method_index_ >= opts_.auth_methods.size()) {
          Error("Authenticate", rc);
        }
        Authenticate();
    }
  }

  void OpenChannel() {
    int rc = ssh_channel_open_session(chan_);
    switch (rc) {
      case SSH_OK:
        Drive(&Session::ExecuteCommand);
        break;
      case SSH_AGAIN:
        break;
      default:
        Error("OpenChannel", rc);
    }
  }

  void ExecuteCommand() {
    int rc = ssh_channel_request_exec(chan_, opts_.cmd.c_str());
    switch (rc) {
      case SSH_OK:
        Drive(&Session::Communicate);
        break;
      case SSH_AGAIN:
        break;
      default:
        Error("ExecuteCommand", rc);
    }
  }

  static bool IsSafe(int c) {
    return c >= 0x80 || isgraph(c) || c == '\t' || c == ' ' || c == '\n';
  }

  static void WriteSafe(const void *buf, size_t size, FILE *stream) {
    auto ptr = static_cast<const unsigned char *>(buf);
    auto end = ptr + size;
    bool safe = true;
    for (; ptr < end; ++ptr) {
      if (!IsSafe(*ptr)) {
        safe = false;
        break;
      }
    }
    if (safe) {
      fwrite(buf, size, 1, stream);
    } else {
      std::vector<uint8_t> enc;
      for (ptr = end - size; ptr < end; ++ptr) {
        auto c = *ptr;
        if (IsSafe(c)) {
          enc.push_back(c);
        } else {
          enc.push_back('\\');
          enc.push_back('0' + (c >> 6));
          enc.push_back('0' + ((c >> 3) & 0x7));
          enc.push_back('0' + (c & 0x7));
        }
      }
      fwrite(enc.data(), enc.size(), 1, stream);
    }
  }

  void Communicate() {
    if (script_contents_offset_ < opts_.script_contents.size()) {
      for (;;) {
        int rc = ssh_channel_write(
            chan_, opts_.script_contents.data() + script_contents_offset_,
            opts_.script_contents.size() - script_contents_offset_);
        if (rc < 0) {
          Error("Write to channel", rc);
        } else if (rc == 0) {
          break;
        }
        script_contents_offset_ += rc;
        if (script_contents_offset_ == opts_.script_contents.size()) {
          is_sending_eof_ = true;
          break;
        }
      }
    }

    if (is_sending_eof_) {
      int rc = ssh_channel_send_eof(chan_);
      if (rc != SSH_OK) {
        if (rc != SSH_AGAIN) {
          Error("Send EOF", rc);
        }
      } else {
        is_sending_eof_ = false;
      }
    }

    for (int is_stderr = 0; is_stderr < 2; ++is_stderr) {
      char buf[LINE_MAX];
      bool loop = true;
      do {
        int rc = ssh_channel_read_nonblocking(chan_, buf, sizeof(buf),
                                              is_stderr);
        switch (rc) {
          case 0:
          case SSH_AGAIN:
          case SSH_EOF:
            loop = false;
            break;
          default:
            if (rc < 0) {
              Error("Read channel", rc);
            }
            buf_[is_stderr].append(buf, rc);
            while (!buf_[is_stderr].empty()) {
              auto pos = buf_[is_stderr].find('\n');
              if (pos == std::string::npos) {
                break;
              }
              FILE *out = is_stderr ? stderr : stdout;
              {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                g_start_log(out);
                fprintf(out, "%s ", host());
                WriteSafe(buf_[is_stderr].data(), pos + 1, out);
                g_end_log(out);
              }
              buf_[is_stderr] = buf_[is_stderr].substr(pos + 1);
            }
        }
      } while (loop);
    }

    if (ssh_channel_is_eof(chan_)) {
      for (int is_stderr = 0; is_stderr < 2; ++is_stderr) {
        auto &buf = buf_[is_stderr];
        if (!buf.empty()) {
          FILE *out = is_stderr ? stderr : stdout;
          {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            g_start_log(out);
            fprintf(out, "%s ", host());
            WriteSafe(buf.data(), buf.size(), out);
            fputc('\n', out);
            g_end_log(out);
          }
          buf.clear();
        }
      }
      if (exit_status_set_ || exit_signal_set_) {
        Drive(nullptr);
      }
    }
  }

  static void OnChannelExitStatus(ssh_session sess, ssh_channel chan,
                                  int exit_status, void *userdata) {
    Session *sess_ = static_cast<Session *>(userdata);
    sess_->exit_status_ = exit_status;
    sess_->exit_status_set_ = true;
  }

  static void OnChannelExitSignal(
      ssh_session sess, ssh_channel chan, const char *signal, int core,
      const char *errmsg, const char *lang, void *userdata) {
    Session *sess_ = static_cast<Session *>(userdata);
    sess_->exit_signal_ = signal;
    sess_->exit_signal_set_ = true;
  }

  const Options &opts_;
  size_t host_index_;
  ssh_event event_;
  std::unique_ptr<ssh_session_struct, void(*)(ssh_session)> sess_;
  ssh_channel chan_ = nullptr;
  void (Session::*do_)() = nullptr;
  std::string buf_[2];
  bool exit_status_set_ = false;
  int exit_status_ = 0;
  std::string exit_signal_;
  bool exit_signal_set_ = false;
  size_t script_contents_offset_ = 0;
  ssh_channel_callbacks_struct cb_;
  bool added_event_ = false;
  std::chrono::steady_clock::time_point deadline_;
  bool is_sending_eof_ = false;
  size_t auth_method_index_ = 0;
};

class Sexec {
 public:
  Sexec(int argc, char *argv[]) {
    opts_.Parse(argc, argv);
  }

  static void Init() {
    if (ssh_init()) {
      throw std::runtime_error("Init libssh");
    }
  }

  static void Finalize() {
    if (ssh_finalize()) {
      throw std::runtime_error("Finalize libssh");
    }
  }

  static std::string PrettyTime(const std::chrono::milliseconds &duration) {
    int64_t d = 0, h = 0, m = 0, s = 0, ms = duration.count();
    assert(ms >= 0);
    if (ms >= 1000) {
      s = ms / 1000;
      ms %= 1000;
      if (s >= 60) {
        m = s / 60;
        s %= 60;
        if (m >= 60) {
          h = m / 60;
          m %= 60;
          if (h >= 24) {
            d = h / 24;
            h %= 24;
          }
        }
      }
    }
    std::ostringstream oss;
    if (d > 0) {
      oss << " " << d << " day(s)";
    }
    if (h > 0) {
      oss << " " << h << " hour(s)";
    }
    if (m > 0) {
      oss << " " << m << " minute(s)";
    }
    if (s > 0) {
      oss << " " << s << " second(s)";
    }
    if (ms > 0) {
      oss << " " << ms << " millisecond(s)";
    }
    auto str = oss.str();
    if (str.empty()) {
      return "0";
    } else {
      return str.substr(1);
    }
  }

  void Run() {
    auto start_time = std::chrono::system_clock::now();
    std::vector<std::future<void>> futures;
    futures.reserve(opts_.num_threads);
    size_t end_index = 0;
    for (size_t start_index = 0; start_index < opts_.hosts.size();
         start_index = end_index) {
      end_index = opts_.hosts.size() * (futures.size() + 1) / opts_.num_threads;
      futures.emplace_back(
          std::async(std::launch::async,
                     [=](){ this->Run(start_index, end_index); }));
    }
    for (auto &future : futures) {
      future.get();
    }
    auto end_time = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    g_start_log(stderr);
    fprintf(stderr, "- Finished %zu host(s) in %s\n",
            opts_.hosts.size() - failed_hosts_.size(),
            PrettyTime(duration).c_str());
    if (!failed_hosts_.empty()) {
      fprintf(stderr, "- Failed %zu host(s):", failed_hosts_.size());
      for (const auto &host : failed_hosts_) {
        fprintf(stderr, " %s", host.c_str());
      }
      fputc('\n', stderr);
    }
    if (!interrupted_hosts_.empty()) {
      fprintf(stderr, "- Interrupted %zu host(s):", interrupted_hosts_.size());
      for (const auto &host : interrupted_hosts_) {
        fprintf(stderr, " %s", host.c_str());
      }
      fputc('\n', stderr);
    }
    if (!unscheduled_hosts_.empty()) {
      fprintf(stderr, "- Unscheduled %zu host(s):", unscheduled_hosts_.size());
      for (const auto &host : unscheduled_hosts_) {
        fprintf(stderr, " %s", host.c_str());
      }
      fputc('\n', stderr);
    }
    g_end_log(stderr);
  }

  void Run(size_t start_index, size_t end_index) {
    std::unique_ptr<ssh_event_struct, void(*)(ssh_event)> event(
       ssh_event_new(), &ssh_event_free);
    if (!event) {
      throw std::runtime_error("New event");
    }
    std::list<std::unique_ptr<Session>> sessions;
    size_t host_index = start_index;
    while (host_index < end_index || !sessions.empty()) {
      while ((opts_.parallel < 0 ||
              sessions.size() < static_cast<size_t>(opts_.parallel)) &&
             host_index < end_index) {
        int index = host_index++;
        try {
          sessions.emplace_back(new Session(opts_, index, event.get()));
        } catch (const std::runtime_error &e) {
          std::lock_guard<std::mutex> lock(g_io_mutex);
          failed_hosts_.insert(opts_.GetHost(index));
          g_start_log(stderr);
          fprintf(stderr, "%s %s\n", opts_.GetHost(index), e.what());
          g_end_log(stderr);
        }
      }
      int timeout = -1;
      if (opts_.timeout > 0) {
        while (!sessions.empty()) {
          timeout = std::chrono::duration_cast<
              std::chrono::milliseconds>(
                  sessions.front()->GetRemainingTime()).count();
          if (timeout < 1) {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            failed_hosts_.insert(sessions.front()->host());
            g_start_log(stderr);
            fprintf(stderr, "%s timedout\n", sessions.front()->host());
            g_end_log(stderr);
            StepLocked();
            sessions.pop_front();
          } else {
            break;
          }
        }
        if (timeout == 0) {
          timeout = -1;
        }
      }
      if (sessions.empty()) {
        continue;
      }
      // Workaround a libssh issue:
      // In some cases, ssh_channel_request_exec() returns SSH_AGAIN, but
      // the following ssh_event_dopoll() will block forever.
      if (timeout < 0 || timeout > 1000) {
        timeout = 1000;
      }
      if (g_stopping) {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        for (const auto &sess : sessions) {
          interrupted_hosts_.emplace(sess->host());
        }
        for (; host_index < end_index; ++host_index) {
          unscheduled_hosts_.emplace(opts_.GetHost(host_index));
        }
        return;
      }
      int rc = ssh_event_dopoll(event.get(), timeout);
      if (rc == SSH_AGAIN) {  // Ignore timedout here and check later.
        rc = SSH_OK;
      }
      if (rc != SSH_OK) {
        g_start_log(stderr);
        fprintf(stderr, "- ssh_event_dopoll: %d\n", rc);
        g_end_log(stderr);
      }
      for (auto it = sessions.begin(); it != sessions.end(); ) {
        auto &sess = *it;
        try {
          if (!sess->Drive()) {
            if (sess->exit_status_set() &&
                sess->exit_status() != EXIT_SUCCESS) {
              std::lock_guard<std::mutex> lock(g_io_mutex);
              failed_hosts_.insert(sess->host());
              g_start_log(stderr);
              fprintf(stderr, "%s exit_status: %d\n", sess->host(),
                      sess->exit_status());
              g_end_log(stderr);
            }
            if (sess->exit_signal_set()) {
              std::lock_guard<std::mutex> lock(g_io_mutex);
              failed_hosts_.insert(sess->host());
              g_start_log(stderr);
              fprintf(stderr, "%s exit_signal: %s\n", sess->host(),
                      sess->exit_signal().c_str());
              g_end_log(stderr);
            }
            Step();
            it = sessions.erase(it);
          } else {
            ++it;
          }
        } catch (const std::runtime_error &e) {
          std::lock_guard<std::mutex> lock(g_io_mutex);
          failed_hosts_.insert(sess->host());
          g_start_log(stderr);
          fprintf(stderr, "%s %s\n", sess->host(), e.what());
          g_end_log(stderr);
          StepLocked();
          it = sessions.erase(it);
        }
      }
    }
  }

 private:
  void Step() {
    std::lock_guard<std::mutex> lock(g_io_mutex);
    StepLocked();
  }

  void StepLocked() {
    ++num_finished_hosts_;
    auto now = std::chrono::steady_clock::now();
    auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_show_step_ts_);
    if (delta.count() >= 1000) {
      last_show_step_ts_ = now;
      g_start_log(stderr);
      fprintf(stderr, "- Progress: %zu%%(%zu/%zu)\n",
              100 * num_finished_hosts_ / opts_.hosts.size(),
              num_finished_hosts_, opts_.hosts.size());
      g_end_log(stderr);
    }
  }

  std::chrono::steady_clock::time_point last_show_step_ts_ =
      std::chrono::steady_clock::now();
  size_t num_finished_hosts_ = 0;
  std::unordered_set<std::string> failed_hosts_;
  std::unordered_set<std::string> interrupted_hosts_;
  std::unordered_set<std::string> unscheduled_hosts_;
  Options opts_;
};

static void HandleInterrupt(int) {
  g_stopping = true;
}

int main(int argc, char *argv[]) {
  InstallLogHandler();
  try {
    if (signal(SIGINT, &HandleInterrupt) == SIG_ERR) {
      throw std::runtime_error("Failed to register the handler for SIGINT");
    }
    if (signal(SIGTERM, &HandleInterrupt) == SIG_ERR) {
      throw std::runtime_error("Failed to register the handler for SIGTERM");
    }
    Sexec::Init();
    Sexec sexec(argc, argv);
    sexec.Run();
    Sexec::Finalize();
  } catch (const std::runtime_error &e) {
    std::lock_guard<std::mutex> lock(g_io_mutex);
    g_start_log(stderr);
    fprintf(stderr, "- %s\n", e.what());
    g_end_log(stderr);
    exit(EXIT_FAILURE);
  }
  return EXIT_SUCCESS;
}
