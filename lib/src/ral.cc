#include <libral/ral.hpp>

#include <config.hpp>

#include <fstream>

#include <libral/mount.hpp>
#include <libral/user.hpp>
#include <libral/group.hpp>
#include <libral/host.hpp>
#include <libral/file.hpp>
#include <libral/provider.hpp>

#include <libral/simple_provider.hpp>
#include <libral/json_provider.hpp>
#include <libral/target/base.hpp>

#include <leatherman/file_util/directory.hpp>
#include <leatherman/logging/logging.hpp>
#include <leatherman/util/environment.hpp>

#include <boost/filesystem.hpp>

using namespace leatherman::locale;
namespace fs = boost::filesystem;
namespace util = leatherman::util;

namespace libral {

  std::string absolute_path(const std::string& path) {
    return fs::absolute(fs::path(path)).native();
  }

  result<void> ral::connect(const std::string& target, bool sudo, bool keep) {
    auto tgt = target::make_ssh(target, sudo, keep);
    auto res = tgt->connect();
    if (res.is_ok()) {
      _local = false;
      _target = tgt;
    }
    return res;
  }

  environment ral::make_env() {
    return environment(shared_from_this());
  }

  void update_augeas_lens_lib(const std::vector<std::string>& data_dirs) {
    // Append our lenses to AUGEAS_LENS_LIB. We do this in the environment
    // rather than when we construct augeas handles because that ensures
    // that external providers also use that setting
    const std::string var = "AUGEAS_LENS_LIB";

    std::stringstream buf;
    bool first=true;

    for (auto dir : data_dirs) {
      if (!first)
        buf << ":";
      first=false;
      buf << dir << "/lenses";
    }

    std::string value;
    if (util::environment::get(var, value)) {
      util::environment::set(var, value + ":" + buf.str());
    } else {
      util::environment::set(var, buf.str());
    }
  }

  std::shared_ptr<ral> ral::create(std::vector<std::string> data_dirs) {
    std::string env_data_dir;

    // Convert each entry in data_dirs into an absolute path
    for (auto& dir : data_dirs) {
      dir = absolute_path(dir);
    }

    // Split RALSH_DATA_DIR on ':' and append absolute path for each
    // component
    if (util::environment::get("RALSH_DATA_DIR", env_data_dir)) {
      std::string::size_type start=0;
      while (1) {
        auto end = env_data_dir.find(':', start);
        auto dir = absolute_path(env_data_dir.substr(start, end - start));
        data_dirs.push_back(dir);
        if (end == std::string::npos) break;
        start = end+1;
      }
    }
    // Append default data dir
    data_dirs.push_back(absolute_path(LIBRAL_DATA_DIR));

    // Prepend RALSH_LIBEXEC_DIR to PATH
    std::string env_libexec_dir;
    if (!util::environment::get("RALSH_LIBEXEC_DIR", env_libexec_dir)) {
      env_libexec_dir = LIBRAL_LIBEXEC_DIR;
    }
    // FIXME: lop trailing '/' off env_libexec_dir
    std::string path;
    if (util::environment::get("PATH", path)) {
      util::environment::set("PATH", env_libexec_dir + ":" + path);
    } else {
      // This is extremely strange .. no path ?
      util::environment::set("PATH", env_libexec_dir);
    }
    util::environment::reload_search_paths();

    // FIXME: Check that mruby is there and warn otherwise

    update_augeas_lens_lib(data_dirs);

    auto handle = new ral(data_dirs);

    return std::shared_ptr<ral>(handle);
  }

  ral::ral(const std::vector<std::string>& data_dirs)
    : _data_dirs(data_dirs), _target(target::make_local()), _local(true) {
    _target->connect();
  }

  bool ral::init_provider(environment &env,
                          const std::string& name,
                          std::shared_ptr<provider>& prov) {
    auto res = prov->prepare(env);
    if (res.is_ok()) {
      if (prov->spec()->suitable()) {
        return true;
      } else {
        LOG_INFO("provider[{1}] for {2} is not suitable", prov->source(), name);
      }
    } else {
      LOG_WARNING("provider[{1}] failed to prepare: {2}", name, res.err().detail);
    }
    return false;
  }

  std::vector<std::shared_ptr<provider>> ral::providers(void) {
    std::vector<std::shared_ptr<provider>> result;
    environment env = make_env();

    std::vector<std::pair<std::string, provider *>> builtin = {
      { "mount", new mount_provider() },
      { "user",  new user_provider()  },
      { "group", new group_provider() },
      { "host",  new host_provider()  },
      { "file",  new file_provider()  }
    };

    for (auto& p : builtin) {
      std::shared_ptr<provider> prov(p.second);
      if (init_provider(env, p.first, prov)) {
        result.push_back(std::move(prov));
      }
    }

    // Find external providers
    auto cb = [&result,&env,this](std::string const &path) {
      auto cmd = env.script(path);
      auto res = get_metadata(*cmd, path);

      if (! res) {
        LOG_WARNING("provider[{1}]: {2}", path, res.err().detail);
        return true;
      }

      auto name = fs::path(cmd->path()).filename().stem().native();
      auto spec = env.parse_spec(name, *res);
      if (!spec) {
        LOG_ERROR("provider[{1}]: {2}", path, spec.err().detail);
        return true;
      }

      provider *raw_prov;
      auto invoke = spec->invoke();
      if (invoke == "simple") {
        raw_prov = new simple_provider(cmd, *spec);
      } else if (invoke == "json") {
        raw_prov = new json_provider(cmd, *spec);
      } else {
        LOG_ERROR("provider[{1}]: unknown calling convention '{2}', expected 'simple' or 'json'", path, invoke);
        return true;
      }

      auto prov = std::shared_ptr<provider>(raw_prov);
      if (init_provider(env, spec->type_name(), prov)) {
        LOG_INFO(_("provider[{1}] ({2}) for {3} loaded",
                   path, invoke, spec->type_name()));
        result.push_back(std::move(prov));
      }
      return true;
    };

    for (auto dir : _data_dirs) {
      leatherman::file_util::each_file(dir + "/providers", cb , "\\.prov$");
    }

    std::sort(result.begin(), result.end(),
              [](const std::shared_ptr<provider>& a,
                 const std::shared_ptr<provider>& b)
              { return a->qname() < b->qname(); });

    return result;
  }

  boost::optional<std::shared_ptr<provider>>
  ral::find_provider(const std::string& name) {
    auto provs = providers();


    for (auto& p : provs) {
      // FIXME: We assume that qname is unique amongst all providers, but
      // do not enforce that
      if (p->qname() == name) {
        return p;
      }
    }

    std::shared_ptr<provider> prov;
    for (auto& p : provs) {
      if (p->type_name() == name) {
        if (!prov) {
          prov = p;
        } else {
          // At least two providers with the same name
          return boost::none;
        }
      }
    }
    if (prov)
      return prov;
    else
      return boost::none;
  }

  result<std::string> ral::get_metadata(command& cmd,
                                        const std::string& path) const {
    std::string yaml(path);
    size_t pos = path.length() - strlen(".prov");
    yaml.replace(pos, std::string::npos, ".yaml");

    if (access(yaml.c_str(), R_OK) == 0) {
      // External metadata
      std::ifstream file(yaml);
      std::ostringstream buf;
      buf << file.rdbuf();
      return buf.str();
    } else {
      return run_describe(cmd);
    }
  }

  result<std::string> ral::run_describe(command& cmd) const {
    if (cmd.executable()) {
      auto res = cmd.execute({ "ral_action=describe" });
      if (!res.success) {
        if (res.output.empty()) {
          return error(_("ignored as it exited with status {1}", res.exit_code));
        } else {
          return error(_("ignored as it exited with status {1}. Output was '{2}'",
                         res.exit_code, res.output));
        }
      }
      if (! res.error.empty()) {
        return error(_("ignored as it produced stderr '{1}'", res.error));
      }
      return res.output;
    }
    // We only get here if path is not executable
    return error(_("file {1} looks like a provider but is not executable",
                   cmd.path()));
  }

  boost::optional<std::string>
  ral::find_in_data_dirs(const std::string& file) const {
    for (const auto& dir : _data_dirs) {
      boost::system::error_code ec;
      auto p = fs::canonical(file, dir, ec);
      if (! ec) {
        return p.native();
      }
    }
    return boost::none;
  }
}
