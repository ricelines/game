#include "catalog/catalog.hpp"
#include "react.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace amber;
  using namespace amber::catalog;
  try {
    std::filesystem::path request_path, output;
    for (int i = 1; i < argc; i += 2) {
      if (i + 1 >= argc)
        throw std::runtime_error("expected --request FILE --output DIR");
      const std::string flag = argv[i];
      if (flag == "--request")
        request_path = argv[i + 1];
      else if (flag == "--output")
        output = argv[i + 1];
      else
        throw std::runtime_error("unknown validator option");
    }
    if (request_path.empty() || output.empty())
      throw std::runtime_error("expected --request FILE --output DIR");
    std::ifstream input(request_path);
    auto request = nlohmann::json::parse(input);
    RuntimePins pins;
    const auto &p = request.at("pins");
    pins.luau = p.at("luau");
    pins.host_abi = p.at("host_abi");
    pins.bake = p.value("bake", std::string{});
    pins.jolt = p.value("jolt", std::string("5.6.0"));
    const auto registry_pin = request.at("registry_pin").get<std::string>();
    const auto registry = request.at("registry");
    if (sha256(registry.dump()) != registry_pin)
      throw std::runtime_error("registry pin does not match supplied registry");
    react::Terms terms;
    load_terms({registry_pin, registry}, terms);
    const react::RightsDeclaration rights{
        {"use", "take", "move", "edit", "copy", "transfer", "destroy"},
        {"spend", "build", "enter", "run_scripts", "return", "animate", "camera"}};
    const auto &limits = request.at("limits");
    const react::VmSettings settings{
        limits.at("script_kb").get<std::size_t>(), limits.at("declaration_ms").get<double>(),
        limits.at("handler_ms").get<double>(), limits.at("event_fields_max").get<std::size_t>()};
    auto script = prepare_script(request.at("source").get<std::string>(), terms, rights,
                                 registry_pin, pins, settings);
    write_script(output, script);
    std::cout << nlohmann::json{{"hash", script.hash},
                                {"source_hash", script.source_hash},
                                {"bytecode_hash", script.bytecode_hash},
                                {"declaration", declaration_json(script.declaration)},
                                {"files",
                                 {{"source", "source.luau"},
                                  {"bytecode", "bytecode.luau"},
                                  {"manifest", "script.json"}}}}
                     .dump()
              << '\n';
  } catch (const std::exception &error) {
    std::cout << nlohmann::json{{"errors", nlohmann::json::array({error.what()})}}.dump() << '\n';
    return 1;
  }
}
