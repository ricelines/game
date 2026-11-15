#include "control.hpp"

#include <cerrno>
#include <fcntl.h>
#include <picosha2.h>
#include <stdexcept>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace amber::control {
KeyHash hash_key(std::string_view key) {
  return picosha2::hash256_hex_string(key.begin(), key.end());
}
void rebuild_key_index(Profiles &profiles) {
  profiles.by_key.clear();
  for (std::size_t index = 1; index < profiles.rows.size(); ++index) {
    const auto &row = profiles.rows[index];
    if (!row.id)
      continue;
    if (row.id != index || (row.connection_modes & ~all_modes))
      throw std::runtime_error("invalid profile identity or connection modes");
    for (const auto &hash : row.key_hashes) {
      if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != hash.npos)
        throw std::runtime_error("invalid profile key hash");
      if (!profiles.by_key.emplace(hash, row.id).second)
        throw std::runtime_error("duplicate profile key hash");
    }
  }
}
void put_profile(Profiles &profiles, Profile profile) {
  if (!profile.id)
    throw std::runtime_error("profile id must be nonzero");
  auto candidate = profiles;
  if (candidate.rows.size() <= profile.id)
    candidate.rows.resize(std::size_t(profile.id) + 1);
  candidate.rows[profile.id] = std::move(profile);
  rebuild_key_index(candidate);
  profiles = std::move(candidate);
}
const Profile *authenticate(const Profiles &profiles, std::string_view key) {
  const auto found = profiles.by_key.find(hash_key(key));
  return found == profiles.by_key.end() ? nullptr : &profiles.rows[found->second];
}
namespace {
struct File {
  int fd;
  ~File() {
    if (fd >= 0)
      ::close(fd);
  }
};
std::string read_private_key(const std::filesystem::path &path) {
  File file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (file.fd < 0)
    throw std::runtime_error("cannot open private key file: " + path.string());
  struct stat info{};
  if (::fstat(file.fd, &info) || !S_ISREG(info.st_mode) || (info.st_mode & 0777) != 0600)
    throw std::runtime_error("key file must be regular with mode 0600: " + path.string());
  std::string key;
  std::array<char, 256> bytes;
  ssize_t count;
  while ((count = ::read(file.fd, bytes.data(), bytes.size())) > 0)
    key.append(bytes.data(), std::size_t(count));
  if (count < 0)
    throw std::runtime_error("cannot read private key file: " + path.string());
  while (!key.empty() && (key.back() == '\n' || key.back() == '\r'))
    key.pop_back();
  if (key.empty())
    throw std::runtime_error("private key file is empty: " + path.string());
  return key;
}
} // namespace
KeyHash create_key_file(const std::filesystem::path &path) {
  std::array<std::uint8_t, 32> random{};
  std::size_t done = 0;
  while (done != random.size()) {
    const auto count = ::getrandom(random.data() + done, random.size() - done, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("OS random key generation failed");
    done += std::size_t(count);
  }
  const auto key = picosha2::bytes_to_hex_string(random.begin(), random.end());
  File file{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (file.fd < 0)
    throw std::runtime_error("cannot exclusively create private key file: " + path.string());
  if (::fchmod(file.fd, 0600)) {
    ::unlink(path.c_str());
    throw std::runtime_error("cannot set private key file permissions: " + path.string());
  }
  const auto output = key + '\n';
  done = 0;
  while (done != output.size()) {
    const auto count = ::write(file.fd, output.data() + done, output.size() - done);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      ::unlink(path.c_str());
      throw std::runtime_error("cannot write private key file: " + path.string());
    }
    done += std::size_t(count);
  }
  return hash_key(key);
}
KeyHash hash_key_file(const std::filesystem::path &path) {
  return hash_key(read_private_key(path));
}
} // namespace amber::control
