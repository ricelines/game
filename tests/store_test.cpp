#include "store/store.hpp"

#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void sparse_column_matches_reference_after_reuse() {
  amber::SparseColumn<std::string> column;
  std::unordered_map<std::uint32_t, std::string> reference;
  std::mt19937 random(704);
  for (int iteration = 0; iteration < 10000; ++iteration) {
    const auto slot = random() % 257;
    if (random() % 3 == 0) {
      require(column.erase(slot) == (reference.erase(slot) != 0), "erase presence differs");
    } else {
      auto value = std::to_string(random());
      column.set(slot, value);
      reference[slot] = value;
    }
    require(column.value.size() == reference.size(), "dense length differs");
    require(column.slot_of.size() == reference.size(), "slot length differs");
    for (std::size_t position = 0; position < column.slot_of.size(); ++position) {
      const auto key = column.slot_of[position];
      require(column.pos_of[key] == position, "reverse lookup was not repaired");
      require(reference.at(key) == column.value[position], "dense value differs");
    }
  }
  column.pos_of.clear();
  column.rebuild_positions(257);
  for (std::uint32_t slot = 0; slot < 257; ++slot) {
    const auto *value = column.get(slot);
    const auto found = reference.find(slot);
    require((value != nullptr) == (found != reference.end()), "rebuilt presence differs");
    if (value)
      require(*value == found->second, "rebuilt value differs");
  }
  column.clear();
  require(!column.get(0) && !column.get(256), "clear retained a value");
}

void handles_reject_destroyed_objects_and_views_survive_growth() {
  amber::Objects objects;
  const auto read = amber::identity(objects);
  const auto write = amber::transforms(objects);
  amber::ObjectInit init;
  init.model = 31;
  init.material = 14;
  init.owner = 7;
  init.maker = 9;
  init.flags = amber::Dynamic;
  const auto old = amber::create(objects, init);
  require(read.valid(old), "new object is invalid");
  amber::destroy(objects, old);
  require(!read.valid(old), "destroyed object remains valid");
  init.model = 42;
  init.owner = 17;
  init.parent = amber::Handle::make(13, 4);
  const auto replacement = amber::create(objects, init);
  require(replacement.slot() == old.slot(), "free slot not reused");
  require(replacement.generation() == old.generation() + 1, "generation did not advance");
  require(!read.valid(old) && read.valid(replacement), "stale handle aliases replacement");
  require(objects.owner[replacement.slot()] == 17 && objects.model[replacement.slot()] == 42,
          "replacement retained old columns");
  for (int count = 0; count < 10000; ++count)
    amber::create(objects, init);
  write.position[replacement.slot()] = {1, 2, 3};
  require(amber::transforms(std::as_const(objects)).position[replacement.slot()] ==
              amber::Vec3{1, 2, 3},
          "projection failed after vector growth");
  require(read.valid(replacement), "identity projection failed after vector growth");
  require(!read.valid({}), "none is a valid object");
}

void handle_text_is_unambiguous() {
  const auto handle = amber::Handle::make(832, 19);
  require(amber::parse_handle(amber::to_string(handle)) == handle, "handle text round trip failed");
  for (auto text : {"o:0.1", "o:1.0", "o:-1.2", "o:1.2junk", "o:1.", "o:1.2.3", "p:1",
                    "o:4294967296.1", "o:1.4294967296"})
    require(!amber::parse_handle(text), "malformed handle accepted");
}

void runtime_flag_writers_preserve_other_owners_bits() {
  amber::Objects objects;
  amber::ObjectInit init;
  init.model = 1;
  init.flags = amber::Body | amber::Dynamic | amber::Proxy | amber::NoCopy;
  const auto body = amber::create(objects, init);
  amber::space_flags(objects).set(body.slot(), amber::Held | amber::ScriptedMotion);
  require(objects.flags[body.slot()] ==
              (amber::Body | amber::Held | amber::ScriptedMotion | amber::Proxy | amber::NoCopy),
          "space writer changed a foreign flag");
  amber::permission_flags(objects).set(body.slot(), amber::Escrow);
  require(objects.flags[body.slot()] ==
              (amber::Body | amber::Held | amber::ScriptedMotion | amber::Proxy | amber::Escrow),
          "permission writer changed a foreign flag");
#ifdef NDEBUG
  // A release build must enforce the mask after assertions are compiled out.
  amber::space_flags(objects).set(body.slot(), amber::Static | amber::NoCopy);
  require(objects.flags[body.slot()] ==
              (amber::Body | amber::Static | amber::Escrow | amber::Proxy),
          "release writer accepted a foreign flag bit");
#endif
}
} // namespace

int main() {
  try {
    sparse_column_matches_reference_after_reuse();
    handles_reject_destroyed_objects_and_views_survive_growth();
    handle_text_is_unambiguous();
    runtime_flag_writers_preserve_other_owners_bits();
    std::cout << "store tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
