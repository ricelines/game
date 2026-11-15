#pragma once
#include "catalog.hpp"
#include "snap/codec.hpp"

namespace amber::catalog {
// nullopt means full snapshot; an explicitly empty span means empty capture.
using ModelSelection = std::optional<std::span<const ModelId>>;
using ScriptSelection = std::optional<std::span<const ScriptId>>;
using ParameterSelection = std::optional<std::span<const std::uint32_t>>;
using ObjectSelection = std::optional<std::span<const Handle>>;
void write_models(snap::Output &, const Models &, ModelSelection = {});
void read_models(snap::Input &, Models &);
void write_parameters(snap::Output &, const ParameterBlocks &, ParameterSelection = {});
void read_parameters(snap::Input &, ParameterBlocks &);
void write_scripts(snap::Output &, const Scripts &, ScriptSelection = {});
void read_scripts(snap::Input &, Scripts &);
void write_text(snap::Output &, const ModelTexts &, const InstanceTexts &, ModelSelection = {},
                ObjectSelection = {});
void read_text(snap::Input &, ModelTexts &, InstanceTexts &);
void write_authored_texts(snap::Output &, const AuthoredTexts &, ObjectSelection = {});
void read_authored_texts(snap::Input &, AuthoredTexts &);
} // namespace amber::catalog
